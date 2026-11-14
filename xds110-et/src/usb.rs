//! USB communication interface for the Texas Instruments XDS110 EnergyTrace probe.

use rusb::{Context, Device, DeviceHandle, UsbContext};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::config::{CMD_IF, CMD_IN_EP, CMD_OUT_EP, CMD_TIMEOUT_MS, DATA_IF, DATA_IN_EP, PID, VID};
use crate::protocol::{
    CMD_CALIBRATE, CMD_CLEANUP, CMD_CONNECT, CMD_DISCONNECT, CMD_HWINFO, CMD_SET_DIG,
    CMD_SET_RANGE, CMD_SET_SRST, CMD_SETUP, CMD_STOP, Calibration, ProtocolError, decode_response,
    encode_command,
};

#[derive(Debug)]
pub enum ProbeError {
    DeviceNotFound,
    Usb(rusb::Error),
    Protocol(ProtocolError),
    Timeout,
    ConfigurationFailed(String),
}

impl std::fmt::Display for ProbeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::DeviceNotFound => write!(f, "XDS110 probe (0451:bef3) not found"),
            Self::Usb(e) => write!(f, "USB error: {e}"),
            Self::Protocol(e) => write!(f, "Protocol error: {e}"),
            Self::Timeout => write!(f, "USB transfer timed out"),
            Self::ConfigurationFailed(msg) => write!(f, "USB configuration error: {msg}"),
        }
    }
}

impl std::error::Error for ProbeError {}

/// Locate the XDS110 probe on the USB bus.
pub fn find_device(context: &Context) -> Result<Device<Context>, ProbeError> {
    context
        .devices()?
        .iter()
        .find(|d| {
            d.device_descriptor()
                .is_ok_and(|desc| desc.vendor_id() == VID && desc.product_id() == PID)
        })
        .ok_or(ProbeError::DeviceNotFound)
}

impl From<rusb::Error> for ProbeError {
    fn from(e: rusb::Error) -> Self {
        Self::Usb(e)
    }
}

impl From<ProtocolError> for ProbeError {
    fn from(e: ProtocolError) -> Self {
        Self::Protocol(e)
    }
}

pub struct Xds110 {
    handle: Arc<DeviceHandle<Context>>,
    shut_down: AtomicBool,
}

impl Xds110 {
    /// Open the XDS110 probe, set its configuration, and claim interfaces.
    pub fn open() -> Result<Self, ProbeError> {
        let context = Context::new()?;
        let handle = Self::find_device(&context)?;

        // Ensure active configuration is set
        if let Err(e) = handle.set_active_configuration(1) {
            // Some platforms / OS drivers return Busy if already configured, which is fine
            if e != rusb::Error::Busy {
                // If it's not busy, check active config
                let active = handle.active_configuration().unwrap_or(0);
                if active == 0 {
                    return Err(ProbeError::ConfigurationFailed(format!(
                        "Failed to set configuration: {e}"
                    )));
                }
            }
        }

        // Try detaching kernel drivers (supported on Linux, no-op/ignored on Windows WinUSB)
        let _ = handle.detach_kernel_driver(CMD_IF);
        let _ = handle.detach_kernel_driver(DATA_IF);

        // Claim command and data interfaces
        handle.claim_interface(CMD_IF)?;
        handle.claim_interface(DATA_IF)?;

        Ok(Self {
            handle: Arc::new(handle),
            shut_down: AtomicBool::new(false),
        })
    }

    fn find_device(context: &Context) -> Result<DeviceHandle<Context>, ProbeError> {
        Ok(find_device(context)?.open()?)
    }

    /// Clone the thread-safe device handle reference
    pub fn clone_handle(&self) -> Arc<DeviceHandle<Context>> {
        Arc::clone(&self.handle)
    }

    /// Send a command and wait for the response, with automatic retries.
    pub fn cmd(&self, cmd: u8, args: &[u8], retries: usize) -> Result<Vec<u8>, ProbeError> {
        let request = encode_command(cmd, args);
        let timeout = Duration::from_millis(CMD_TIMEOUT_MS);
        let mut resp_buf = [0u8; 0x1100];

        let mut last_err = None;
        for _ in 0..retries.max(1) {
            match self.handle.write_bulk(CMD_OUT_EP, &request, timeout) {
                Ok(_) => match self.handle.read_bulk(CMD_IN_EP, &mut resp_buf, timeout) {
                    Ok(n) => return Ok(decode_response(cmd, &resp_buf[..n])?),
                    Err(e) => last_err = Some(e),
                },
                Err(e) => last_err = Some(e),
            }
        }

        Err(last_err
            .map(ProbeError::from)
            .unwrap_or(ProbeError::Timeout))
    }

    /// Stop a stream possibly left running by a killed process, then drain stale sample data.
    /// Bounded in time: a probe that is still streaming would otherwise never run dry.
    pub fn drain_data(&self) {
        let _ = self.cmd(CMD_STOP, &[], 1);
        let mut buf = [0u8; 0x2000];
        let timeout = Duration::from_millis(100);
        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline {
            match self.handle.read_bulk(DATA_IN_EP, &mut buf, timeout) {
                Ok(n) if n > 0 => {}
                _ => break,
            }
        }
    }

    /// Query hardware info from the probe (Cmd 0x46).
    /// Returns (hw_version, extra_byte).
    pub fn hw_info(&self) -> Result<(u32, u8), ProbeError> {
        let resp = self.cmd(CMD_HWINFO, &[], 2)?;
        if resp.len() < 5 {
            return Err(ProbeError::Protocol(ProtocolError::InvalidResponse(
                "Hardware info response too short".into(),
            )));
        }
        let hw_ver = u32::from_le_bytes([resp[0], resp[1], resp[2], resp[3]]);
        let extra = resp[4];
        Ok((hw_ver, extra))
    }

    /// Set up the EnergyTrace probe for streaming.
    pub fn setup(&self, freq: u32) -> Result<(), ProbeError> {
        self.cmd(CMD_CONNECT, &[], 2)?;
        self.cmd(CMD_SET_RANGE, &[0x00], 2)?;
        self.cmd(CMD_SET_DIG, &[0x01], 2)?;

        let mut setup_args = Vec::with_capacity(7);
        setup_args.push(1); // p2
        setup_args.push(0); // p4
        setup_args.extend_from_slice(&freq.to_le_bytes()); // p3 (freq u32le)
        setup_args.push(0); // p5
        self.cmd(CMD_SETUP, &setup_args, 2)?;

        Ok(())
    }

    /// Read calibration points from the probe.
    /// Indices queried: 0 (zero offset), 4 (~1.5mA), 1 (~40.2mA).
    pub fn calibrate(&self) -> Result<Calibration, ProbeError> {
        let mut values = [0.0f64; 3];
        let indices = [0u16, 4, 1];

        for (i, &idx) in indices.iter().enumerate() {
            let resp = self.cmd(CMD_CALIBRATE, &idx.to_le_bytes(), 2)?;
            if resp.len() < 8 {
                return Err(ProbeError::Protocol(ProtocolError::InvalidResponse(
                    format!("Calibration index {idx} response too short"),
                )));
            }
            let total = i32::from_le_bytes([resp[0], resp[1], resp[2], resp[3]]);
            let count = u32::from_le_bytes([resp[4], resp[5], resp[6], resp[7]]);
            values[i] = if count > 0 {
                total as f64 / count as f64
            } else {
                f64::NAN
            };
        }

        Ok(Calibration {
            idx0_offset: values[0],
            idx4: values[1],
            idx1: values[2],
        })
    }

    /// Toggle target MCU nSRST pin (logic from TI's xds110reset.exe)
    /// 1. CMD_SET_SRST (0x0E) with arg 0x00: asserts nSRST (pull low)
    /// 2. Sleep for delay_ms (default 50 ms)
    /// 3. CMD_SET_SRST (0x0E) with arg 0x01: deasserts nSRST (release high)
    pub fn reset_mcu_toggle(&self, delay_ms: u64) -> Result<(), ProbeError> {
        self.cmd(CMD_SET_SRST, &[0x00], 2)?;
        std::thread::sleep(Duration::from_millis(delay_ms.max(10)));
        self.cmd(CMD_SET_SRST, &[0x01], 2)?;
        Ok(())
    }

    /// Perform clean probe shutdown sequence (STOP, CLEANUP, DISCONNECT).
    /// Idempotent: only the first call talks to the probe.
    pub fn shutdown(&self) {
        if self.shut_down.swap(true, Ordering::SeqCst) {
            return;
        }
        let _ = self.cmd(CMD_STOP, &[], 1);
        let _ = self.cmd(CMD_CLEANUP, &[], 1);
        let _ = self.cmd(CMD_DISCONNECT, &[], 1);
    }
}

impl Drop for Xds110 {
    fn drop(&mut self) {
        self.shutdown();
        let _ = self.handle.release_interface(CMD_IF);
        let _ = self.handle.release_interface(DATA_IF);
    }
}
