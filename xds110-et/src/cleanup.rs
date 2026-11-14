//! Process exit hooks and probe disconnect (0x02) management.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use rusb::Context;

use crate::config::{CMD_IF, CMD_IN_EP, CMD_OUT_EP, DATA_IF};
use crate::protocol::{CMD_CLEANUP, CMD_DISCONNECT, CMD_STOP, encode_command};
use crate::usb::{Xds110, find_device};

/// Send the STOP (0x20), CLEANUP (0x21), and DISCONNECT (0x02) command sequence
/// directly to the XDS110 probe.
///
/// This opens its own handle so it can run from signal/panic hooks while another
/// thread still owns the main probe session.
pub fn send_probe_disconnect() {
    let Ok(context) = Context::new() else { return };
    let Ok(dev) = find_device(&context) else {
        return;
    };
    let Ok(handle) = dev.open() else { return };

    let _ = handle.claim_interface(CMD_IF);

    let timeout = Duration::from_millis(500);
    let mut resp = [0u8; 64];

    for &cmd in &[CMD_STOP, CMD_CLEANUP, CMD_DISCONNECT] {
        let req = encode_command(cmd, &[]);
        if handle.write_bulk(CMD_OUT_EP, &req, timeout).is_ok() {
            let _ = handle.read_bulk(CMD_IN_EP, &mut resp, timeout);
        }
    }

    let _ = handle.release_interface(CMD_IF);
    let _ = handle.release_interface(DATA_IF);
}

/// Directly send Reset MCU toggle (Cmd 0x0E: XDS_SetSrst) when no live session owns the probe.
pub fn restart_mcu_direct() {
    if let Ok(probe) = Xds110::open() {
        let _ = probe.reset_mcu_toggle(50);
        probe.shutdown();
    }
}

/// RAII Exit Guard that ensures the probe disconnect command (0x02)
/// is always sent when the guard is dropped.
pub struct ProbeExitGuard {
    stop_signal: Arc<AtomicBool>,
}

impl ProbeExitGuard {
    pub fn new(stop_signal: Arc<AtomicBool>) -> Self {
        Self { stop_signal }
    }
}

impl Drop for ProbeExitGuard {
    fn drop(&mut self) {
        self.stop_signal.store(true, Ordering::SeqCst);
        send_probe_disconnect();
    }
}

/// Register system exit hooks (Ctrl-C and panic hook) to guarantee
/// that CMD 0x02 (DISCONNECT) is sent upon any application exit.
pub fn register_exit_hooks(stop_signal: Arc<AtomicBool>) {
    // Ctrl-C / SIGINT hook
    let _ = ctrlc::set_handler(move || {
        stop_signal.store(true, Ordering::SeqCst);
        send_probe_disconnect();
    });

    // Panic hook: ensure probe is not left locked if a panic occurs
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |panic_info| {
        send_probe_disconnect();
        prev_hook(panic_info);
    }));
}
