//! Implementations of the CLI subcommands.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;

use crossbeam_channel::bounded;

use crate::capture::{self, CaptureConfig};
use crate::cleanup::{ProbeExitGuard, register_exit_hooks};
use crate::cli::ModeAction;
use crate::csv_loader::load_csv_file;
use crate::gui::{self, EnergyTraceApp};
use crate::protocol::{CMD_CONNECT, ProbeMode};
use crate::usb::Xds110;

pub type CmdResult = Result<(), Box<dyn std::error::Error>>;

/// Number of decimated display batches buffered between the USB thread and the GUI.
const GUI_CHANNEL_DEPTH: usize = 256;
/// Number of pending control commands (MCU reset, ...) buffered from the GUI to the probe.
const CTRL_CHANNEL_DEPTH: usize = 16;

pub fn info() -> CmdResult {
    let probe = Xds110::open()?;
    probe.cmd(CMD_CONNECT, &[], 2)?;
    let ver = probe.version()?;
    let (hw, extra) = probe.hw_info()?;
    println!("Firmware Version: {ver}");
    println!("Hardware Info:    0x{hw:08x} (extra: 0x{extra:02x})");
    probe.shutdown();
    Ok(())
}

pub fn version() -> CmdResult {
    let probe = Xds110::open()?;
    probe.cmd(CMD_CONNECT, &[], 2)?;
    let ver = probe.version()?;
    println!("{ver}");
    probe.shutdown();
    Ok(())
}

pub fn probe_mode(action: ModeAction) -> CmdResult {
    let probe = Xds110::open()?;
    let status = match action {
        ModeAction::Status => probe.probe_mode_get()?,
        ModeAction::Normal => probe.probe_mode_set(ProbeMode::Normal)?,
        ModeAction::Bmp => probe.probe_mode_set(ProbeMode::Bmp)?,
    };
    println!("Probe Mode: {status}");
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

pub fn dfu_info() -> CmdResult {
    let dfu = crate::dfu::ensure_dfu_mode()?;
    let status = dfu.get_status()?;
    let (serial, config) = dfu.read_info_block()?;

    println!("XDS110 DFU Device Information:");
    println!("  DFU State:      {}", status.state);
    println!("  DFU Status:     {}", status.status);
    println!("  Flash Size:     {} KB", dfu.info.total_flash_size / 1024);
    println!("  Block Size:     {} bytes", dfu.info.block_size);
    println!("  App Start Addr: 0x{:08x}", dfu.info.app_start_addr);
    println!("  Serial Number:  {serial}");
    println!("  Configuration:  {config}");
    Ok(())
}

pub fn dfu_switch() -> CmdResult {
    println!("Switching XDS110 to DFU bootloader mode...");
    crate::dfu::trigger_dfu_mode_switch()?;
    println!("Command sent. Device is re-enumerating into DFU mode.");
    Ok(())
}

pub fn dfu_reset() -> CmdResult {
    let dfu = crate::dfu::DfuDevice::open()?;
    println!("Resetting XDS110 back to runtime mode...");
    dfu.reset_to_runtime()?;
    println!("Reset command sent.");
    Ok(())
}

pub fn dfu_serial(serial: Option<String>, suffix_only: bool, reset: bool) -> CmdResult {
    let dfu = crate::dfu::ensure_dfu_mode()?;
    let (curr_serial, _curr_config) = dfu.read_info_block()?;

    match serial {
        None => {
            println!("Current Serial Number: {curr_serial}");
        }
        Some(s) => {
            let final_serial = if suffix_only {
                if s.len() != 4 {
                    return Err(format!("Suffix must be exactly 4 characters, got '{}'", s).into());
                }
                let prefix = if curr_serial.len() >= 4 {
                    &curr_serial[0..4]
                } else {
                    "LS42"
                };
                format!("{prefix}{s}")
            } else {
                if s.len() != 8 {
                    return Err(
                        format!("Serial number must be exactly 8 characters, got '{}'", s).into(),
                    );
                }
                s
            };

            println!("Setting serial number to '{final_serial}'...");
            let (new_serial, _) = dfu.write_info_block(Some(&final_serial), None)?;
            println!("Serial number successfully updated to '{new_serial}'.");

            if reset {
                println!("Resetting probe back to runtime mode...");
                dfu.reset_to_runtime()?;
            }
        }
    }

    Ok(())
}

pub fn dfu_config(config_num: u32, reset: bool) -> CmdResult {
    let dfu = crate::dfu::ensure_dfu_mode()?;
    println!("Setting configuration to {config_num}...");
    let (_, new_cfg) = dfu.write_info_block(None, Some(config_num))?;
    println!("Configuration successfully updated to {new_cfg}.");

    if reset {
        println!("Resetting probe back to runtime mode...");
        dfu.reset_to_runtime()?;
    }

    Ok(())
}

pub fn dfu_flash(path: &Path, reset: bool) -> CmdResult {
    crate::dfu::flash_firmware_file(path, reset)?;
    Ok(())
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
