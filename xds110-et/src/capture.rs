//! USB streaming engine, thread coordination, and capture runner.

use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use crossbeam_channel::{bounded, Receiver, Sender};

use crate::config::{
    CMD_TIMEOUT_MS, DATA_IN_EP, DEFAULT_ROWS_PER_HOUR, DEFAULT_SLOPE_NA_PER_LSB, SAMPLE_RATE_HZ,
};
use crate::csv_writer::{create_csv_file, utc_now_string, WindowWriter};
use crate::data_series::PlotPoint;
use crate::frame::FrameParser;
use crate::protocol::{CMD_DCDC_SET_VCC, CMD_START};
use crate::usb::{ProbeError, Xds110};

/// Control commands that can be sent to the active probe during a live capture session
#[derive(Debug, Clone, Copy)]
pub enum ProbeControlCommand {
    RestartMcu,
    SetVcc(u16),
}

/// Number of hardware samples per live GUI plot point (20 ms @ 256 kS/s = 50 Hz refresh)
const SAMPLES_PER_GUI_POINT: u64 = 5120;



/// Human-readable formatting of current with automatic unit selection
pub fn fmt_current(na: f64) -> String {
    let abs_na = na.abs();
    if abs_na >= 1_000_000.0 {
        format!("{:.3} mA", na / 1_000_000.0)
    } else if abs_na >= 1_000.0 {
        format!("{:.3} uA", na / 1_000.0)
    } else {
        format!("{:.1} nA", na)
    }
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

pub struct CaptureConfig {
    pub time_limit_sec: f64,
    pub rows_per_hour: u32,
    pub output_path: PathBuf,
    pub slope: f64,
    pub retry_delay_sec: f64,
    pub max_retries: u32,
}

impl Default for CaptureConfig {
    fn default() -> Self {
        let timestamp = chrono::Local::now().format("%Y%m%d_%H%M%S");
        let output_path = PathBuf::from(format!("logs/long_{timestamp}.csv"));
        Self {
            time_limit_sec: 0.0,
            rows_per_hour: DEFAULT_ROWS_PER_HOUR,
            output_path,
            slope: DEFAULT_SLOPE_NA_PER_LSB,
            retry_delay_sec: 5.0,
            max_retries: 0,
        }
    }
}

/// Run the capture session with automatic reconnect and decoupled CSV writing.
pub fn run_capture(
    config: CaptureConfig,
    stop_signal: Arc<AtomicBool>,
    gui_tx: Option<Sender<Vec<PlotPoint>>>,
    ctrl_rx: Option<Receiver<ProbeControlCommand>>,
) -> Result<(), Box<dyn std::error::Error>> {
    let window_size =
        ((SAMPLE_RATE_HZ as f64 * 3600.0) / config.rows_per_hour as f64).round() as u64;
    let max_samples = if config.time_limit_sec > 0.0 {
        Some((config.time_limit_sec * SAMPLE_RATE_HZ as f64).round() as u64)
    } else {
        None
    };

    let mut stats = CaptureStats::default();
    let mut writer: Option<WindowWriter<std::io::BufWriter<std::fs::File>>> = None;
    let mut failures = 0u32;
    let session_start = Instant::now();

    eprintln!(
        "Starting capture: {} rows/hour (window size: {} samples)",
        config.rows_per_hour, window_size
    );
    eprintln!("Output file: {}", config.output_path.display());

    while !stop_signal.load(Ordering::Relaxed) {
        let mut probe = match Xds110::open(stats.reconnects > 0) {
            Ok(p) => p,
            Err(e) => {
                if writer.is_none() {
                    return Err(format!("Initial probe connection failed: {e}").into());
                }
                failures += 1;
                eprintln!(
                    "\n{} probe connect error: {e}; retry {failures}...",
                    utc_now_string()
                );
                if config.max_retries > 0 && failures > config.max_retries {
                    eprintln!("Too many consecutive reconnect failures, giving up.");
                    break;
                }
                thread::sleep(Duration::from_secs_f64(config.retry_delay_sec));
                stats.reconnects += 1;
                continue;
            }
        };

        probe.drain_data();
        if let Err(e) = probe.setup(48000) {
            eprintln!("Setup failed: {e}");
            probe.shutdown();
            thread::sleep(Duration::from_secs_f64(config.retry_delay_sec));
            continue;
        }

        let cal = match probe.calibrate() {
            Ok(c) => c,
            Err(e) => {
                eprintln!("Calibration failed: {e}");
                probe.shutdown();
                thread::sleep(Duration::from_secs_f64(config.retry_delay_sec));
                continue;
            }
        };

        eprintln!(
            "Calibration: idx0={:.3}, idx4={:.3}, idx1={:.3} (offset: {:.3})",
            cal.idx0_offset, cal.idx4, cal.idx1, cal.idx0_offset
        );

        if writer.is_none() {
            let wr = create_csv_file(
                &config.output_path,
                window_size,
                config.rows_per_hour,
                &cal,
                config.slope,
            )?;
            writer = Some(wr);
        } else if let Some(wr) = writer.as_mut() {
            wr.offset = cal.idx0_offset;
            let _ = wr.write_reconnect_comment(stats.reconnects, &cal);
        }

        failures = 0;
        let stream_result = stream_session(
            &mut probe,
            writer.as_mut().unwrap(),
            &mut stats,
            max_samples,
            config.slope,
            session_start,
            Arc::clone(&stop_signal),
            gui_tx.as_ref(),
            ctrl_rx.as_ref(),
        );

        probe.shutdown();

        match stream_result {
            Ok(completed) => {
                if completed {
                    break; // Target time / max samples reached
                }
            }
            Err(e) => {
                failures += 1;
                let lost_at = Instant::now();
                if let Some(wr) = writer.as_mut() {
                    let _ = wr.flush_window();
                }
                eprintln!(
                    "\n{} stream error: {e}; reconnecting (attempt {failures})...",
                    utc_now_string()
                );
                if config.max_retries > 0 && failures > config.max_retries {
                    eprintln!("Too many failed reconnects, giving up.");
                    break;
                }
                thread::sleep(Duration::from_secs_f64(config.retry_delay_sec));
                stats.reconnects += 1;
                if let Some(wr) = writer.as_mut() {
                    wr.skip_gap(lost_at.elapsed().as_secs_f64());
                }
            }
        }
    }

    if let Some(wr) = writer.as_mut() {
        let _ = wr.flush_window();
    }

    let elapsed = session_start.elapsed().as_secs_f64();
    eprintln!();
    eprintln!(
        "Capture finished: {} samples, {} rows written, {} frames lost, {} reconnects",
        stats.total_samples,
        writer.as_ref().map(|w| w.rows_written).unwrap_or(0),
        stats.frames_lost,
        stats.reconnects
    );
    eprintln!("Elapsed time: {:.2} s", elapsed);

    if stats.total_samples > 0 {
        let offset = writer.as_ref().map(|w| w.offset).unwrap_or(0.0);
        let cur = |adc: f64| (adc - offset) * config.slope;
        let avg_adc = stats.adc_sum as f64 / stats.total_samples as f64;
        eprintln!(
            "Current: min {}, max {}, avg {}",
            fmt_current(cur(stats.adc_min as f64)),
            fmt_current(cur(stats.adc_max as f64)),
            fmt_current(cur(avg_adc)),
        );
    }

    Ok(())
}

/// Stream one connected session until error, stop signal, or max_samples.
/// Returns Ok(true) if max_samples was reached, Ok(false) if stop_signal stopped it.
fn stream_session<W: Write>(
    probe: &mut Xds110,
    writer: &mut WindowWriter<W>,
    stats: &mut CaptureStats,
    max_samples: Option<u64>,
    slope: f64,
    session_start: Instant,
    stop_signal: Arc<AtomicBool>,
    gui_tx: Option<&Sender<Vec<PlotPoint>>>,
    ctrl_rx: Option<&Receiver<ProbeControlCommand>>,
) -> Result<bool, ProbeError> {
    // Channel for passing raw data chunks from dedicated USB reader thread
    let (chunk_tx, chunk_rx): (Sender<Result<Vec<u8>, rusb::Error>>, Receiver<_>) =
        bounded(128);

    // Dedicated reader thread to prevent USB endpoint FIFO overrun
    let thread_stop = Arc::new(AtomicBool::new(false));
    let thread_stop_clone = Arc::clone(&thread_stop);

    // DeviceHandle is Arc-cloned to allow the reader thread to read from DATA_IN_EP
    // while the main thread coordinates
    let dev_handle = probe.clone_handle();
    let reader_handle = thread::spawn(move || {
        let mut buf = vec![0u8; 0x4000];
        let timeout = Duration::from_millis(200);
        let mut idle_ms = 0u64;

        while !thread_stop_clone.load(Ordering::Relaxed) {
            match dev_handle.read_bulk(DATA_IN_EP, &mut buf, timeout) {
                Ok(n) => {
                    idle_ms = 0;
                    if n > 0 {
                        if chunk_tx.send(Ok(buf[..n].to_vec())).is_err() {
                            break;
                        }
                    }
                }
                Err(rusb::Error::Timeout) => {
                    idle_ms += 200;
                    if idle_ms >= CMD_TIMEOUT_MS {
                        let _ = chunk_tx.send(Err(rusb::Error::Timeout));
                        break;
                    }
                }
                Err(rusb::Error::Pipe) => {
                    // Endpoint stall/halt: clear halt and attempt to resume
                    let _ = dev_handle.clear_halt(DATA_IN_EP);
                    idle_ms += 200;
                    if idle_ms >= CMD_TIMEOUT_MS {
                        let _ = chunk_tx.send(Err(rusb::Error::Pipe));
                        break;
                    }
                }
                Err(e) => {
                    idle_ms += 200;
                    if idle_ms >= CMD_TIMEOUT_MS {
                        let _ = chunk_tx.send(Err(e));
                        break;
                    }
                    thread::sleep(Duration::from_millis(50));
                }
            }
        }
    });

    probe.cmd(CMD_START, &[], 2)?;

    let mut parser = FrameParser::new();
    let mut last_status_report = Instant::now();
    let mut target_reached = false;

    // GUI accumulator (decoupled from CSV window accumulator)
    let mut gui_n = 0u64;
    let mut gui_sum = 0i64;
    let mut gui_min = i32::MAX;
    let mut gui_max = i32::MIN;
    let mut gui_start_sample = stats.total_samples;
    let mut gui_batch = Vec::new();

    while !stop_signal.load(Ordering::Relaxed) {
        // Process any pending probe control commands (e.g. MCU reset or VCC adjustment)
        if let Some(rx) = ctrl_rx {
            while let Ok(cmd) = rx.try_recv() {
                match cmd {
                    ProbeControlCommand::RestartMcu => {
                        let _ = probe.reset_mcu_toggle(50);
                    }
                    ProbeControlCommand::SetVcc(mv) => {
                        let _ = probe.cmd(CMD_DCDC_SET_VCC, &mv.to_le_bytes(), 2);
                    }
                }
            }
        }

        if let Some(max_s) = max_samples {
            if stats.total_samples >= max_s {
                target_reached = true;
                break;
            }
        }

        // Wait for next chunk with short timeout
        match chunk_rx.recv_timeout(Duration::from_millis(100)) {
            Ok(Ok(chunk)) => {
                parser.push_chunk(&chunk);
                let frames = parser.parse_next_frames();

                for (lost, samples) in frames {
                    if lost > 0 {
                        stats.frames_lost += lost;
                        writer.add_lost_frames(lost);
                    }

                    // Check if taking all samples would exceed max_samples
                    let take = if let Some(max_s) = max_samples {
                        let rem = (max_s - stats.total_samples) as usize;
                        rem.min(samples.len())
                    } else {
                        samples.len()
                    };

                    let slice = &samples[..take];
                    for s in slice {
                        stats.total_samples += 1;
                        stats.adc_sum += s.adc as i64;
                        if s.adc < stats.adc_min {
                            stats.adc_min = s.adc;
                        }
                        if s.adc > stats.adc_max {
                            stats.adc_max = s.adc;
                        }

                        // Feed GUI accumulator
                        if gui_tx.is_some() {
                            gui_n += 1;
                            gui_sum += s.adc as i64;
                            if s.adc < gui_min {
                                gui_min = s.adc;
                            }
                            if s.adc > gui_max {
                                gui_max = s.adc;
                            }

                            if gui_n >= SAMPLES_PER_GUI_POINT {
                                let time_s = gui_start_sample as f64 / SAMPLE_RATE_HZ as f64;
                                let cur = |adc: f64| (adc - writer.offset) * slope;
                                let avg_adc = gui_sum as f64 / gui_n as f64;
                                gui_batch.push(PlotPoint {
                                    time_s,
                                    avg_na: cur(avg_adc),
                                    min_na: cur(gui_min as f64),
                                    max_na: cur(gui_max as f64),
                                });

                                gui_start_sample += gui_n;
                                gui_n = 0;
                                gui_sum = 0;
                                gui_min = i32::MAX;
                                gui_max = i32::MIN;
                            }
                        }
                    }

                    // Send accumulated GUI batch if any
                    if !gui_batch.is_empty() {
                        if let Some(tx) = gui_tx {
                            let _ = tx.try_send(std::mem::take(&mut gui_batch));
                        }
                    }

                    let _ = writer.add_samples(slice);

                    if take < samples.len() {
                        target_reached = true;
                        break;
                    }
                }

                if target_reached {
                    break;
                }

                // Live terminal reporting every 0.5s
                if last_status_report.elapsed() >= Duration::from_millis(500) {
                    last_status_report = Instant::now();
                    if stats.total_samples > 0 {
                        let cur = |adc: f64| (adc - writer.offset) * slope;
                        let avg_adc = stats.adc_sum as f64 / stats.total_samples as f64;
                        eprint!(
                            "\r{:9.1} s  {:>13} samples  {:>8} rows  rc={}  min {:>10}  max {:>10}  avg {:>10}   ",
                            session_start.elapsed().as_secs_f64(),
                            stats.total_samples,
                            writer.rows_written,
                            stats.reconnects,
                            fmt_current(cur(stats.adc_min as f64)),
                            fmt_current(cur(stats.adc_max as f64)),
                            fmt_current(cur(avg_adc)),
                        );
                        let _ = std::io::stderr().flush();
                    }
                }
            }
            Ok(Err(e)) => {
                thread_stop.store(true, Ordering::Relaxed);
                let _ = reader_handle.join();
                return Err(ProbeError::from(e));
            }
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => {
                // Continue checking stop_signal
            }
            Err(crossbeam_channel::RecvTimeoutError::Disconnected) => {
                break;
            }
        }
    }

    thread_stop.store(true, Ordering::Relaxed);
    let _ = reader_handle.join();

    Ok(target_reached)
}
