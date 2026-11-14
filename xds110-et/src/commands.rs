//! Implementations of the CLI subcommands.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;

use crossbeam_channel::bounded;

use crate::capture::{self, CaptureConfig};
use crate::cleanup::{ProbeExitGuard, register_exit_hooks};
use crate::csv_loader::load_csv_file;
use crate::gui::{self, EnergyTraceApp};
use crate::protocol::CMD_CONNECT;
use crate::usb::Xds110;

pub type CmdResult = Result<(), Box<dyn std::error::Error>>;

/// Number of decimated display batches buffered between the USB thread and the GUI.
const GUI_CHANNEL_DEPTH: usize = 256;
/// Number of pending control commands (MCU reset, ...) buffered from the GUI to the probe.
const CTRL_CHANNEL_DEPTH: usize = 16;

pub fn info() -> CmdResult {
    let probe = Xds110::open()?;
    probe.cmd(CMD_CONNECT, &[], 2)?;
    let (hw, extra) = probe.hw_info()?;
    println!("HardwareInfo: 0x{hw:08x} 0x{extra:02x}");
    probe.shutdown();
    Ok(())
}

/// Parse a hex byte with an optional `0x` prefix.
fn parse_hex_byte(s: &str) -> Result<u8, std::num::ParseIntError> {
    let clean = s
        .strip_prefix("0x")
        .or_else(|| s.strip_prefix("0X"))
        .unwrap_or(s);
    u8::from_str_radix(clean, 16)
}

pub fn raw(bytes: &[String]) -> CmdResult {
    let parsed = bytes
        .iter()
        .map(|b| parse_hex_byte(b).map_err(|e| format!("invalid hex byte '{b}': {e}")))
        .collect::<Result<Vec<u8>, _>>()?;
    let (&cmd, args) = parsed.split_first().ok_or("no command byte given")?;

    let probe = Xds110::open()?;
    let rsp = probe.cmd(cmd, args, 2)?;
    let hex: String = rsp.iter().map(|b| format!("{b:02x}")).collect();
    println!("{hex}");
    probe.shutdown();
    Ok(())
}

pub fn capture_headless(
    time: f64,
    rows_per_hour: u32,
    output: Option<PathBuf>,
    slope: f64,
    retry_delay: f64,
    max_retries: u32,
) -> CmdResult {
    if rows_per_hour == 0 {
        return Err("--rows-per-hour must be >= 1".into());
    }
    if !(time.is_finite() && time >= 0.0) {
        return Err("--time must be a non-negative number".into());
    }
    if !(retry_delay.is_finite() && retry_delay >= 0.0) {
        return Err("--retry-delay must be a non-negative number".into());
    }

    let config = CaptureConfig {
        time_limit_sec: time,
        rows_per_hour,
        output_path: output.unwrap_or_else(CaptureConfig::default_output_path),
        slope,
        retry_delay_sec: retry_delay,
        max_retries,
    };

    let stop_signal = Arc::new(AtomicBool::new(false));
    register_exit_hooks(Arc::clone(&stop_signal));
    let _exit_guard = ProbeExitGuard::new(Arc::clone(&stop_signal));

    capture::run_capture(config, stop_signal, None, None)
}

pub fn view_file(path: &Path) -> CmdResult {
    let filename = path.file_name().map_or_else(
        || "capture.csv".into(),
        |s| s.to_string_lossy().into_owned(),
    );

    let points = load_csv_file(path)?;
    if points.is_empty() {
        return Err(format!("No data points found in {}", path.display()).into());
    }

    gui::launch_gui(EnergyTraceApp::new_file(filename, points))
}

pub fn live_gui() -> CmdResult {
    let stop_signal = Arc::new(AtomicBool::new(false));

    // Guarantee CMD 0x02 (disconnect) is sent however the process exits
    register_exit_hooks(Arc::clone(&stop_signal));
    let _exit_guard = ProbeExitGuard::new(Arc::clone(&stop_signal));

    // Decimated display points flow USB -> GUI; control commands (Reset MCU) flow GUI -> USB
    let (tx, rx) = bounded(GUI_CHANNEL_DEPTH);
    let (ctrl_tx, ctrl_rx) = bounded(CTRL_CHANNEL_DEPTH);

    let capture_stop = Arc::clone(&stop_signal);
    let capture_handle = thread::spawn(move || {
        capture::run_capture(
            CaptureConfig::default(),
            capture_stop,
            Some(tx),
            Some(ctrl_rx),
        )
        .map_err(|e| e.to_string())
    });

    let app = EnergyTraceApp::new_live(Arc::clone(&stop_signal), rx, capture_handle, ctrl_tx);
    let gui_res = gui::launch_gui(app);

    stop_signal.store(true, Ordering::SeqCst);
    gui_res
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_hex_byte() {
        assert_eq!(parse_hex_byte("1d").unwrap(), 0x1d);
        assert_eq!(parse_hex_byte("0x46").unwrap(), 0x46);
        assert_eq!(parse_hex_byte("0XFF").unwrap(), 0xff);
        assert!(parse_hex_byte("zz").is_err());
        assert!(parse_hex_byte("100").is_err());
    }
}
