//! Process exit hooks and probe disconnect (0x02) management.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use rusb::{Context, UsbContext};

use crate::config::{CMD_IF, CMD_IN_EP, CMD_OUT_EP, DATA_IF, PID, VID};
use crate::protocol::{
    CMD_CLEANUP, CMD_CONNECT, CMD_DCDC_SET_VCC, CMD_DISCONNECT, CMD_STOP, encode_command,
};

/// Send the STOP (0x20), CLEANUP (0x21), and DISCONNECT (0x02) command sequence
/// directly to the XDS110 probe.
pub fn send_probe_disconnect() {
    let context = match Context::new() {
        Ok(c) => c,
        Err(_) => return,
    };

    let device = match context.devices() {
        Ok(devs) => devs.iter().find(|d| {
            if let Ok(desc) = d.device_descriptor() {
                desc.vendor_id() == VID && desc.product_id() == PID
            } else {
                false
            }
        }),
        Err(_) => None,
    };

    let dev = match device {
        Some(d) => d,
        None => return,
    };

    let handle = match dev.open() {
        Ok(h) => h,
        Err(_) => return,
    };

    // Claim command interface
    let _ = handle.claim_interface(CMD_IF);

    // Send STOP (0x20), CLEANUP (0x21), and DISCONNECT (0x02)
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

/// Directly send Reset MCU toggle (Cmd 0x0E: XDS_SetSrst) from xds110reset.exe
pub fn restart_mcu_direct() {
    if let Ok(probe) = crate::usb::Xds110::open(false) {
        let _ = probe.reset_mcu_toggle(50);
        probe.shutdown();
    }
}

/// Directly send SetVcc (Cmd 0x24) when probe is not currently in a live session
pub fn set_vcc_direct(mv: u16) {
    let context = match Context::new() {
        Ok(c) => c,
        Err(_) => return,
    };
    if let Ok(devs) = context.devices() {
        if let Some(dev) = devs.iter().find(|d| {
            d.device_descriptor()
                .map(|desc| desc.vendor_id() == VID && desc.product_id() == PID)
                .unwrap_or(false)
        }) {
            if let Ok(handle) = dev.open() {
                let _ = handle.claim_interface(CMD_IF);
                let timeout = Duration::from_millis(500);
                let mut resp = [0u8; 64];
                let _ = handle.write_bulk(CMD_OUT_EP, &encode_command(CMD_CONNECT, &[]), timeout);
                let _ = handle.read_bulk(CMD_IN_EP, &mut resp, timeout);
                let _ = handle.write_bulk(
                    CMD_OUT_EP,
                    &encode_command(CMD_DCDC_SET_VCC, &mv.to_le_bytes()),
                    timeout,
                );
                let _ = handle.read_bulk(CMD_IN_EP, &mut resp, timeout);
                let _ = handle.release_interface(CMD_IF);
            }
        }
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
    let signal_clone = Arc::clone(&stop_signal);

    // Ctrl-C / SIGINT hook
    let _ = ctrlc::set_handler(move || {
        signal_clone.store(true, Ordering::SeqCst);
        send_probe_disconnect();
    });

    // Panic hook: ensure probe is not left locked if a panic occurs
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |panic_info| {
        send_probe_disconnect();
        prev_hook(panic_info);
    }));
}
