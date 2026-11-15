//! USB streaming engine, thread coordination, and capture runner.

mod decimator;
mod reader;
mod report;
mod session;

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender};

use crate::config::{DEFAULT_ROWS_PER_HOUR, DEFAULT_SLOPE_NA_PER_LSB, SAMPLE_RATE_HZ};
use crate::csv_writer::{WindowWriter, create_csv_file, utc_now_string};
use crate::data_series::PlotPoint;
use crate::usb::{ProbeError, Xds110};
use session::{SessionEnv, SessionError, stream_session};

/// ADC sampling frequency requested from the probe during setup.
const PROBE_SETUP_FREQ: u32 = 48000;

/// Control commands that can be sent to the active probe during a live capture session
#[derive(Debug, Clone, Copy)]
pub enum ProbeControlCommand {
    RestartMcu,
}

/// Statistics across the entire capture session
#[derive(Debug)]
pub struct CaptureStats {
    pub total_samples: u64,
    pub adc_sum: i64,
    pub adc_min: i32,
    pub adc_max: i32,
    pub frames_lost: u32,
    pub reconnects: u32,
}

impl Default for CaptureStats {
    fn default() -> Self {
        Self {
            total_samples: 0,
            adc_sum: 0,
            adc_min: i32::MAX,
            adc_max: i32::MIN,
            frames_lost: 0,
            reconnects: 0,
        }
    }
}

impl CaptureStats {
    fn record(&mut self, adc: i32) {
        self.total_samples += 1;
        self.adc_sum += adc as i64;
        self.adc_min = self.adc_min.min(adc);
        self.adc_max = self.adc_max.max(adc);
    }

    fn adc_avg(&self) -> f64 {
        self.adc_sum as f64 / self.total_samples as f64
    }
}

pub struct CaptureConfig {
    pub time_limit_sec: f64,
    pub rows_per_hour: u32,
    pub output_path: PathBuf,
    pub slope: f64,
    pub retry_delay_sec: f64,
    pub max_retries: u32,
}

impl CaptureConfig {
    /// Default log location: `logs/long_YYYYMMDD_HHMMSS.csv`
    pub fn default_output_path() -> PathBuf {
        let timestamp = chrono::Local::now().format("%Y%m%d_%H%M%S");
        PathBuf::from(format!("logs/long_{timestamp}.csv"))
    }
}

impl Default for CaptureConfig {
    fn default() -> Self {
        Self {
            time_limit_sec: 0.0,
            rows_per_hour: DEFAULT_ROWS_PER_HOUR,
            output_path: Self::default_output_path(),
            slope: DEFAULT_SLOPE_NA_PER_LSB,
            retry_delay_sec: 5.0,
            max_retries: 0,
        }
    }
}

/// Tracks consecutive failures against the configured retry budget.
struct RetryPolicy {
    failures: u32,
    max_retries: u32,
    delay: Duration,
}

impl RetryPolicy {
    fn new(config: &CaptureConfig) -> Self {
        Self {
            failures: 0,
            max_retries: config.max_retries,
            // Guard against negative/NaN values which would panic in from_secs_f64
            delay: Duration::from_secs_f64(config.retry_delay_sec.max(0.0)),
        }
    }

    fn reset(&mut self) {
        self.failures = 0;
    }

    /// Record a failure; returns false once the retry budget is exhausted.
    fn record_failure(&mut self, what: &str) -> bool {
        self.failures += 1;
        eprintln!("\n{} {what}; retry {}...", utc_now_string(), self.failures);
        if self.max_retries > 0 && self.failures > self.max_retries {
            eprintln!("Too many consecutive failures, giving up.");
            return false;
        }
        true
    }

    /// Wait before the next attempt, returning early if a stop is requested.
    fn wait(&self, stop_signal: &AtomicBool) {
        let deadline = Instant::now() + self.delay;
        while !stop_signal.load(Ordering::Relaxed) {
            let Some(left) = deadline.checked_duration_since(Instant::now()) else {
                return;
            };
            thread::sleep(left.min(Duration::from_millis(100)));
        }
    }
}

/// Open the probe, configure it for streaming and read its calibration.
fn connect_probe() -> Result<(Xds110, crate::protocol::Calibration), ConnectError> {
    let probe = Xds110::open().map_err(ConnectError::Open)?;
    probe.drain_data();
    let cal = probe
        .setup(PROBE_SETUP_FREQ)
        .and_then(|()| probe.calibrate())
        .map_err(ConnectError::Setup)?;

    // A NaN offset would turn every current value in the log into NaN
    if cal.idx0_offset.is_nan() {
        return Err(ConnectError::Setup(ProbeError::ConfigurationFailed(
            "calibration returned no samples".into(),
        )));
    }

    eprintln!(
        "Calibration: idx0={:.3}, idx4={:.3}, idx1={:.3} (offset: {:.3})",
        cal.idx0_offset, cal.idx4, cal.idx1, cal.idx0_offset
    );
    Ok((probe, cal))
}

enum ConnectError {
    Open(ProbeError),
    Setup(ProbeError),
}

/// Run the capture session with automatic reconnect and decoupled CSV writing.
pub fn run_capture(
    config: CaptureConfig,
    stop_signal: Arc<AtomicBool>,
    gui_tx: Option<Sender<Vec<PlotPoint>>>,
    ctrl_rx: Option<Receiver<ProbeControlCommand>>,
) -> Result<(), Box<dyn std::error::Error>> {
    if config.rows_per_hour == 0 {
        return Err("rows_per_hour must be >= 1".into());
    }
    let window_size =
        (((SAMPLE_RATE_HZ as f64 * 3600.0) / config.rows_per_hour as f64).round() as u64).max(1);
    let max_samples = (config.time_limit_sec > 0.0)
        .then(|| (config.time_limit_sec * SAMPLE_RATE_HZ as f64).round() as u64);
    let time_limit =
        (config.time_limit_sec > 0.0).then(|| Duration::from_secs_f64(config.time_limit_sec));

    let mut stats = CaptureStats::default();
    let mut writer: Option<WindowWriter<std::io::BufWriter<std::fs::File>>> = None;
    let mut retry = RetryPolicy::new(&config);
    let run_start = Instant::now();
    let mut gave_up = false;

    eprintln!(
        "Starting capture: {} rows/hour (window size: {} samples)",
        config.rows_per_hour, window_size
    );
    eprintln!("Output file: {}", config.output_path.display());

    let env = SessionEnv {
        stop_signal: &stop_signal,
        gui_tx: gui_tx.as_ref(),
        ctrl_rx: ctrl_rx.as_ref(),
        max_samples,
        time_limit,
        slope: config.slope,
        run_start,
    };

    while !stop_signal.load(Ordering::Relaxed) {
        let (probe, cal) = match connect_probe() {
            Ok(connected) => connected,
            Err(ConnectError::Open(e)) if writer.is_none() => {
                return Err(format!("Initial probe connection failed: {e}").into());
            }
            Err(ConnectError::Open(e) | ConnectError::Setup(e)) => {
                if !retry.record_failure(&format!("probe connect error: {e}")) {
                    gave_up = true;
                    break;
                }
                retry.wait(&stop_signal);
                stats.reconnects += 1;
                continue;
            }
        };

        let wr = match writer.as_mut() {
            Some(wr) => {
                wr.offset = cal.idx0_offset;
                let _ = wr.write_reconnect_comment(stats.reconnects, &cal);
                wr
            }
            None => writer.insert(create_csv_file(
                &config.output_path,
                window_size,
                config.rows_per_hour,
                &cal,
                config.slope,
            )?),
        };

        retry.reset();
        let result = stream_session(&probe, wr, &mut stats, &env);
        probe.shutdown();

        match result {
            Ok(true) => break, // Target time / max samples reached
            Ok(false) => {}
            Err(e @ SessionError::Log(_)) => return Err(e.into()),
            Err(SessionError::Probe(e)) => {
                let lost_at = Instant::now();
                let _ = wr.flush_window();
                let _ = wr.flush();
                if !retry.record_failure(&format!("stream error: {e}; reconnecting")) {
                    gave_up = true;
                    break;
                }
                retry.wait(&stop_signal);
                stats.reconnects += 1;
                wr.skip_gap(lost_at.elapsed().as_secs_f64());
            }
        }
    }

    if let Some(wr) = writer.as_mut() {
        let _ = wr.flush_window();
        let _ = wr.flush();
    }

    let (rows, offset) = writer
        .as_ref()
        .map_or((0, 0.0), |w| (w.rows_written, w.offset));
    report::print_summary(run_start.elapsed(), &stats, rows, offset, config.slope);

    if gave_up && writer.is_none() {
        return Err("Could not connect to the probe".into());
    }
    Ok(())
}
