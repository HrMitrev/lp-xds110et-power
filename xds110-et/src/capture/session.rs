//! Streaming of a single connected probe session.

use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, RecvTimeoutError, Sender};

use super::decimator::GuiDecimator;
use super::reader::ReaderThread;
use super::{CaptureStats, ProbeControlCommand, report};
use crate::csv_writer::WindowWriter;
use crate::data_series::PlotPoint;
use crate::frame::FrameParser;
use crate::protocol::CMD_START;
use crate::usb::{ProbeError, Xds110};

const STATUS_INTERVAL: Duration = Duration::from_millis(500);
const POLL_INTERVAL: Duration = Duration::from_millis(100);

/// Why a session ended abnormally.
#[derive(Debug)]
pub enum SessionError {
    /// The probe failed or disappeared; reconnecting may help.
    Probe(ProbeError),
    /// Writing the CSV log failed; retrying would only lose data.
    Log(std::io::Error),
}

impl std::fmt::Display for SessionError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Probe(e) => write!(f, "{e}"),
            Self::Log(e) => write!(f, "CSV log write failed: {e}"),
        }
    }
}

impl std::error::Error for SessionError {}

impl From<ProbeError> for SessionError {
    fn from(e: ProbeError) -> Self {
        Self::Probe(e)
    }
}

impl From<rusb::Error> for SessionError {
    fn from(e: rusb::Error) -> Self {
        Self::Probe(e.into())
    }
}

/// Parameters and channels shared by every session of one capture run.
pub struct SessionEnv<'a> {
    pub stop_signal: &'a AtomicBool,
    pub gui_tx: Option<&'a Sender<Vec<PlotPoint>>>,
    pub ctrl_rx: Option<&'a Receiver<ProbeControlCommand>>,
    pub max_samples: Option<u64>,
    pub time_limit: Option<Duration>,
    pub slope: f64,
    pub run_start: Instant,
}

/// Stream one connected session until error, stop signal, or `max_samples`.
/// Returns Ok(true) if `max_samples` was reached, Ok(false) if the stop signal ended it.
pub fn stream_session<W: Write>(
    probe: &Xds110,
    writer: &mut WindowWriter<W>,
    stats: &mut CaptureStats,
    env: &SessionEnv,
) -> Result<bool, SessionError> {
    let reader = ReaderThread::spawn(probe.clone_handle());
    probe.cmd(CMD_START, &[], 2)?;
    let stream_start = Instant::now();

    let mut parser = FrameParser::new();
    let mut decimator = GuiDecimator::new(stats.total_samples);
    let mut last_status = Instant::now();
    let mut timeline_samples =
        stats.total_samples + (stats.frames_lost as u64 * crate::config::SAMPLES_PER_FRAME as u64);

    while !env.stop_signal.load(Ordering::Relaxed) {
        handle_control_commands(probe, env.ctrl_rx);

        // Safety watchdog: abort if probe communication stalled far past target duration
        if let Some(limit) = env.time_limit {
            if stream_start.elapsed() >= limit + Duration::from_secs(5) {
                return Ok(true);
            }
        }

        if env.max_samples.is_some_and(|max| timeline_samples >= max) {
            return Ok(true);
        }

        let chunk = match reader.chunks.recv_timeout(POLL_INTERVAL) {
            Ok(chunk) => chunk?,
            Err(RecvTimeoutError::Timeout) => continue,
            Err(RecvTimeoutError::Disconnected) => return Err(rusb::Error::Other.into()),
        };

        parser.push_chunk(&chunk);
        for (lost, samples) in parser.parse_next_frames() {
            if lost > 0 {
                stats.frames_lost += lost;
                let lost_samples = lost as u64 * crate::config::SAMPLES_PER_FRAME as u64;
                timeline_samples += lost_samples;
                writer.add_lost_frames(lost).map_err(SessionError::Log)?;
            }

            // Never take more samples than the requested capture length
            let take = match env.max_samples {
                Some(max) => {
                    let remaining = max.saturating_sub(timeline_samples) as usize;
                    remaining.min(samples.len())
                }
                None => samples.len(),
            };
            let taken = &samples[..take];

            let mut gui_batch = Vec::new();
            for s in taken {
                stats.record(s.adc);
                if env.gui_tx.is_some() {
                    gui_batch.extend(decimator.push(s.adc, writer.offset, env.slope));
                }
            }
            timeline_samples += taken.len() as u64;

            if let (Some(tx), false) = (env.gui_tx, gui_batch.is_empty()) {
                // A full channel means the GUI is behind; dropping a batch only thins the plot
                let _ = tx.try_send(gui_batch);
            }

            writer.add_samples(taken).map_err(SessionError::Log)?;

            if env.max_samples.is_some_and(|max| timeline_samples >= max) {
                return Ok(true);
            }
        }

        if last_status.elapsed() >= STATUS_INTERVAL {
            last_status = Instant::now();
            writer.flush().map_err(SessionError::Log)?;
            report::print_status(
                env.run_start.elapsed(),
                stats,
                writer.rows_written,
                writer.offset,
                env.slope,
            );
        }
    }

    Ok(false)
}

/// Apply pending probe control commands (e.g. MCU reset or VCC adjustment).
fn handle_control_commands(probe: &Xds110, ctrl_rx: Option<&Receiver<ProbeControlCommand>>) {
    let Some(rx) = ctrl_rx else { return };
    while let Ok(cmd) = rx.try_recv() {
        match cmd {
            ProbeControlCommand::RestartMcu => {
                let _ = probe.reset_mcu_toggle(50);
            }
        }
    }
}
