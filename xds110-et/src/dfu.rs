//! USB Device Firmware Upgrade (DFU) implementation for XDS110 debug probes.
//!
//! Reverse engineered from Texas Instruments `xdsdfu.exe` and verified on hardware.
//! Supports:
//! - Switching runtime probe (`0451:bef3`) into DFU bootloader mode (`1cbe:00ff` / `0451:bef4`)
//! - Querying DFU status and flash geometry (16 KB blocks, 1 MB flash)
//! - Reading and setting probe serial number (stored in last flash block 0xFC000)
//! - Reading and setting probe configuration (1 = standard, 2 = drag-n-drop, 4 = CMSIS-DAP 2.0, 5 = EVM)
//! - Flashing application firmware `.bin` (starting at 0x4000)
//! - Resetting probe back to runtime mode

#![allow(dead_code, unused_variables)]

use std::path::Path;
use std::thread;
use std::time::{Duration, Instant};

use rusb::{Context, Device, DeviceHandle, UsbContext};

use crate::config::{CMD_IF, CMD_OUT_EP, PID as RUNTIME_PID, VID as RUNTIME_VID};

#[allow(dead_code)]
/// Known USB IDs for XDS110 in DFU mode
pub const DFU_DEVICE_IDS: &[(u16, u16)] = &[
    (0x1CBE, 0x00FF), // Luminary Micro / TI Stellaris ROM DFU Bootloader
    (0x0451, 0xBEF4), // TI XDS110 DFU mode
    (0x1CBE, 0x029E), // Tiva DFU fallback
];

#[allow(dead_code)]
/// USB DFU Request Codes (Control Endpoint 0)
pub const DFU_DETACH: u8 = 0;
pub const DFU_DNLOAD: u8 = 1;
pub const DFU_UPLOAD: u8 = 2;
pub const DFU_GETSTATUS: u8 = 3;
pub const DFU_CLRSTATUS: u8 = 4;
pub const DFU_GETSTATE: u8 = 5;
pub const DFU_ABORT: u8 = 6;

#[allow(dead_code)]
/// TI Stellaris / Luminary Micro DFU Commands (sent via DFU_DNLOAD block 0)
pub const DFU_CMD_PROG: u8 = 0x01;
pub const DFU_CMD_READ: u8 = 0x02;
pub const DFU_CMD_CHECK: u8 = 0x03;
pub const DFU_CMD_ERASE: u8 = 0x04;
pub const DFU_CMD_INFO: u8 = 0x05;
pub const DFU_CMD_BIN: u8 = 0x06;
pub const DFU_CMD_RESET: u8 = 0x07;

#[allow(dead_code)]
/// DFU States according to USB DFU specification
pub const DFU_STATE_APP_IDLE: u8 = 0;
pub const DFU_STATE_DFU_IDLE: u8 = 2;
pub const DFU_STATE_DFU_DNLOAD_SYNC: u8 = 3;
pub const DFU_STATE_DFU_DNBUSY: u8 = 4;
pub const DFU_STATE_DFU_DNLOAD_IDLE: u8 = 5;
pub const DFU_STATE_DFU_MANIFEST: u8 = 7;
pub const DFU_STATE_DFU_UPLOAD_IDLE: u8 = 9;
pub const DFU_STATE_DFU_ERROR: u8 = 10;

/// Default application start address in flash (after 16 KB bootloader)
pub const APP_START_ADDR: u32 = 0x0000_4000;
/// Transfer block size (maximum bytes per DFU_DNLOAD/UPLOAD)
pub const TRANSFER_CHUNK_SIZE: usize = 1024;

/// Configuration magic value mask: 0xAA550000 | (config & 0xFFFF)
pub const CONFIG_MAGIC_MASK: u32 = 0xAA55_0000;

#[allow(dead_code)]
#[derive(Debug)]
pub enum DfuError {
    Usb(rusb::Error),
    DeviceNotFound,
    DeviceNotDfu,
    Protocol(String),
    Timeout,
    InvalidSerial(String),
    InvalidConfig(u32),
    Io(std::io::Error),
}

impl std::fmt::Display for DfuError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Usb(e) => write!(f, "USB error: {e}"),
            Self::DeviceNotFound => write!(f, "XDS110 device not found"),
            Self::DeviceNotDfu => write!(f, "Device is not in DFU mode"),
            Self::Protocol(msg) => write!(f, "DFU protocol error: {msg}"),
            Self::Timeout => write!(f, "DFU operation timed out"),
            Self::InvalidSerial(msg) => write!(f, "Invalid serial number: {msg}"),
            Self::InvalidConfig(cfg) => write!(f, "Invalid configuration: {cfg}"),
            Self::Io(e) => write!(f, "I/O error: {e}"),
        }
    }
}

impl std::error::Error for DfuError {}

impl From<rusb::Error> for DfuError {
    fn from(e: rusb::Error) -> Self {
        Self::Usb(e)
    }
}

impl From<std::io::Error> for DfuError {
    fn from(e: std::io::Error) -> Self {
        Self::Io(e)
    }
}

/// Status structure returned by DFU_GETSTATUS
#[derive(Debug, Clone, Copy)]
pub struct DfuStatus {
    pub status: u8,
    pub poll_timeout_ms: u32,
    pub state: u8,
    pub string_idx: u8,
}

/// Device info returned by DFU_CMD_INFO
#[derive(Debug, Clone, Copy)]
pub struct DfuDeviceInfo {
    pub block_size: u32,
    pub flash_start_addr: u32,
    pub total_flash_size: u32,
    pub app_start_addr: u32,
    pub num_blocks: u32,
    pub last_block_addr: u32,
}

pub struct DfuDevice {
    handle: DeviceHandle<Context>,
    pub info: DfuDeviceInfo,
}

impl DfuDevice {
    /// Attempt to find an XDS110 already in DFU mode.
    pub fn find_dfu_device(context: &Context) -> Result<Device<Context>, DfuError> {
        for device in context.devices()?.iter() {
            if let Ok(desc) = device.device_descriptor() {
                for &(vid, pid) in DFU_DEVICE_IDS {
                    if desc.vendor_id() == vid && desc.product_id() == pid {
                        return Ok(device);
                    }
                }
            }
        }
        Err(DfuError::DeviceNotFound)
    }

    /// Open a device already in DFU mode.
    pub fn open() -> Result<Self, DfuError> {
        let context = Context::new()?;
        let dev = Self::find_dfu_device(&context)?;
        let handle = dev.open()?;

        let _ = handle.set_active_configuration(1);
        let _ = handle.detach_kernel_driver(0);
        handle.claim_interface(0)?;

        let mut dfu = Self {
            handle,
            info: DfuDeviceInfo {
                block_size: 16384,
                flash_start_addr: 0x4000,
                total_flash_size: 0x100000,
                app_start_addr: 0x4000,
                num_blocks: 64,
                last_block_addr: 63 * 16384,
            },
        };

        // Query device geometry parameters
        if let Ok(info) = dfu.query_device_info() {
            dfu.info = info;
        }

        Ok(dfu)
    }

    /// Query DFU status (DFU_GETSTATUS).
    pub fn get_status(&self) -> Result<DfuStatus, DfuError> {
        let mut buf = [0u8; 6];
        let timeout = Duration::from_millis(1000);
        self.handle
            .read_control(0xA1, DFU_GETSTATUS, 0, 0, &mut buf, timeout)?;
        let status = buf[0];
        let poll_timeout_ms = (buf[1] as u32) | ((buf[2] as u32) << 8) | ((buf[3] as u32) << 16);
        let state = buf[4];
        let string_idx = buf[5];
        Ok(DfuStatus {
            status,
            poll_timeout_ms,
            state,
            string_idx,
        })
    }

    /// Clear DFU error status (DFU_CLRSTATUS).
    pub fn clear_status(&self) -> Result<(), DfuError> {
        let timeout = Duration::from_millis(1000);
        let _ = self
            .handle
            .write_control(0x21, DFU_CLRSTATUS, 0, 0, &[], timeout);
        Ok(())
    }

    /// Abort current operation and return to dfuIDLE.
    pub fn abort(&self) -> Result<(), DfuError> {
        let timeout = Duration::from_millis(1000);
        let _ = self
            .handle
            .write_control(0x21, DFU_ABORT, 0, 0, &[], timeout);
        Ok(())
    }

    /// Wait until device state reaches dfuIDLE or dfuDNLOAD_IDLE, respecting poll timeout.
    pub fn wait_status_idle(&self) -> Result<DfuStatus, DfuError> {
        let start = Instant::now();
        let timeout = Duration::from_secs(10);
        while start.elapsed() < timeout {
            let s = self.get_status()?;
            if s.state == DFU_STATE_DFU_ERROR {
                self.clear_status()?;
                continue;
            }
            if s.poll_timeout_ms > 0 {
                thread::sleep(Duration::from_millis(s.poll_timeout_ms.max(5) as u64));
            }
            if s.state == DFU_STATE_DFU_IDLE || s.state == DFU_STATE_DFU_DNLOAD_IDLE {
                return Ok(s);
            }
            thread::sleep(Duration::from_millis(5));
        }
        Err(DfuError::Timeout)
    }

    /// Send a vendor command packet (DFU_DNLOAD with wValue = 0).
    fn send_command_packet(&self, packet: &[u8]) -> Result<(), DfuError> {
        let timeout = Duration::from_millis(2000);
        self.handle
            .write_control(0x21, DFU_DNLOAD, 0, 0, packet, timeout)?;
        let status = self.wait_status_idle()?;
        if status.status != 0 {
            return Err(DfuError::Protocol(format!(
                "Command failed with status {}",
                status.status
            )));
        }
        Ok(())
    }

    /// Query device flash parameters (DFU_CMD_INFO = 0x05).
    pub fn query_device_info(&mut self) -> Result<DfuDeviceInfo, DfuError> {
        let cmd = [DFU_CMD_INFO, 0, 0, 0, 0, 0, 0, 0];
        self.send_command_packet(&cmd)?;

        let mut buf = [0u8; 20];
        let timeout = Duration::from_millis(1000);
        self.handle
            .read_control(0xA1, DFU_UPLOAD, 2, 0, &mut buf, timeout)?;

        let block_size = u16::from_le_bytes([buf[0], buf[1]]) as u32;
        let total_flash = u32::from_le_bytes([buf[12], buf[13], buf[14], buf[15]]);
        let app_start = u32::from_le_bytes([buf[16], buf[17], buf[18], buf[19]]);

        let block_sz = if block_size > 0 { block_size } else { 16384 };
        let flash_sz = if total_flash > 0 {
            total_flash
        } else {
            0x100000
        };
        let app_addr = if app_start > 0 {
            app_start
        } else {
            APP_START_ADDR
        };
        let num_blocks = flash_sz / block_sz;
        let last_block_addr = (num_blocks - 1) * block_sz;

        let info = DfuDeviceInfo {
            block_size: block_sz,
            flash_start_addr: app_addr,
            total_flash_size: flash_sz,
            app_start_addr: app_addr,
            num_blocks,
            last_block_addr,
        };
        self.info = info;
        Ok(info)
    }

    /// Read raw flash memory using DFU_CMD_READ + DFU_CMD_BIN + DFU_UPLOAD.
    /// Returns data without the 8-byte LMDFU prefix.
    pub fn read_flash(&self, start_addr: u32, len: usize) -> Result<Vec<u8>, DfuError> {
        let block_idx = (start_addr >> 10) as u16;
        let mut cmd_read = [0u8; 8];
        cmd_read[0] = DFU_CMD_READ;
        cmd_read[2..4].copy_from_slice(&block_idx.to_le_bytes());
        cmd_read[4..8].copy_from_slice(&(len as u32).to_le_bytes());
        self.send_command_packet(&cmd_read)?;

        let mut cmd_bin = [0u8; 8];
        cmd_bin[0] = DFU_CMD_BIN;
        self.send_command_packet(&cmd_bin)?;

        let total_to_read = len + 8; // 8-byte header prepended by bootloader
        let mut raw_buf = Vec::with_capacity(total_to_read);
        let mut chunk_buf = [0u8; TRANSFER_CHUNK_SIZE];
        let mut block_seq = 2u16;

        while raw_buf.len() < total_to_read {
            let left = total_to_read - raw_buf.len();
            let req_len = left.min(TRANSFER_CHUNK_SIZE);
            let n = self.handle.read_control(
                0xA1,
                DFU_UPLOAD,
                block_seq,
                0,
                &mut chunk_buf[..req_len],
                Duration::from_millis(2000),
            )?;
            if n == 0 {
                break;
            }
            raw_buf.extend_from_slice(&chunk_buf[..n]);
            block_seq = block_seq.wrapping_add(1);
        }

        if raw_buf.len() < 8 {
            return Err(DfuError::Protocol("Flash read returned no data".into()));
        }

        Ok(raw_buf[8..].to_vec())
    }

    /// Program a flash memory region with provided data.
    pub fn write_flash<F>(
        &self,
        start_addr: u32,
        data: &[u8],
        mut progress_cb: F,
    ) -> Result<(), DfuError>
    where
        F: FnMut(usize, usize),
    {
        let block_idx = (start_addr >> 10) as u16;
        let mut cmd_prog = [0u8; 8];
        cmd_prog[0] = DFU_CMD_PROG;
        cmd_prog[2..4].copy_from_slice(&block_idx.to_le_bytes());
        cmd_prog[4..8].copy_from_slice(&(data.len() as u32).to_le_bytes());
        self.send_command_packet(&cmd_prog)?;

        let mut offset = 0;
        let mut block_seq = 1u16;
        let total = data.len();

        while offset < total {
            let chunk_len = (total - offset).min(TRANSFER_CHUNK_SIZE);
            let chunk = &data[offset..offset + chunk_len];

            self.handle.write_control(
                0x21,
                DFU_DNLOAD,
                block_seq,
                0,
                chunk,
                Duration::from_millis(4000),
            )?;

            let status = self.wait_status_idle()?;
            if status.status != 0 {
                return Err(DfuError::Protocol(format!(
                    "Download chunk at 0x{:08x} failed: status {}",
                    start_addr + offset as u32,
                    status.status
                )));
            }

            offset += chunk_len;
            block_seq = block_seq.wrapping_add(1);
            progress_cb(offset, total);
        }

        // Finalize with zero-length DFU_DNLOAD
        self.handle.write_control(
            0x21,
            DFU_DNLOAD,
            block_seq,
            0,
            &[],
            Duration::from_millis(2000),
        )?;
        let _ = self.wait_status_idle();

        Ok(())
    }

    /// Read the serial number and configuration from the probe info block.
    pub fn read_info_block(&self) -> Result<(String, u32), DfuError> {
        let last_addr = self.info.last_block_addr;
        let block = self.read_flash(last_addr, 64)?;
        if block.len() < 20 {
            return Err(DfuError::Protocol("Info block is too short".into()));
        }

        let serial_raw = &block[0..8];
        let serial = String::from_utf8_lossy(serial_raw)
            .trim_matches(char::from(0))
            .to_string();

        let cfg_raw = u32::from_le_bytes([block[16], block[17], block[18], block[19]]);
        let config = if (cfg_raw & 0xFFFF_0000) == CONFIG_MAGIC_MASK {
            cfg_raw & 0xFFFF
        } else {
            1
        };

        Ok((serial, config))
    }

    /// Write updated serial number and/or configuration to the last flash block.
    pub fn write_info_block(
        &self,
        new_serial: Option<&str>,
        new_config: Option<u32>,
    ) -> Result<(String, u32), DfuError> {
        let last_addr = self.info.last_block_addr;
        // Read full 1024-byte block to preserve anything else in the sector
        let mut block = self.read_flash(last_addr, TRANSFER_CHUNK_SIZE)?;
        if block.len() < TRANSFER_CHUNK_SIZE {
            block.resize(TRANSFER_CHUNK_SIZE, 0xFF);
        }

        if let Some(s) = new_serial {
            let bytes = s.as_bytes();
            if bytes.len() != 8 {
                return Err(DfuError::InvalidSerial(
                    "Serial number must be exactly 8 ASCII characters".into(),
                ));
            }
            block[0..8].copy_from_slice(bytes);
        }

        if let Some(cfg) = new_config {
            let encoded = CONFIG_MAGIC_MASK | (cfg & 0xFFFF);
            block[16..20].copy_from_slice(&encoded.to_le_bytes());
        }

        self.write_flash(last_addr, &block, |_, _| {})?;

        let serial = String::from_utf8_lossy(&block[0..8])
            .trim_matches(char::from(0))
            .to_string();
        let config = u32::from_le_bytes([block[16], block[17], block[18], block[19]]) & 0xFFFF;
        Ok((serial, config))
    }

    /// Reset the probe back into runtime mode (DFU_CMD_RESET = 0x07).
    pub fn reset_to_runtime(&self) -> Result<(), DfuError> {
        let cmd_reset = [DFU_CMD_RESET, 0, 0, 0, 0, 0, 0, 0];
        let _ = self.handle.write_control(
            0x21,
            DFU_DNLOAD,
            0,
            0,
            &cmd_reset,
            Duration::from_millis(500),
        );
        Ok(())
    }
}

/// Send the command to switch runtime probe into DFU mode.
pub fn trigger_dfu_mode_switch() -> Result<(), DfuError> {
    let context = Context::new()?;
    let dev = context
        .devices()?
        .iter()
        .find(|d| {
            d.device_descriptor().is_ok_and(|desc| {
                desc.vendor_id() == RUNTIME_VID && desc.product_id() == RUNTIME_PID
            })
        })
        .ok_or(DfuError::DeviceNotFound)?;

    let handle = dev.open()?;
    let _ = handle.detach_kernel_driver(CMD_IF);
    handle.claim_interface(CMD_IF)?;

    let switch_cmd = [b'*', 0x01, 0x00, 0x26];
    let _ = handle.write_bulk(CMD_OUT_EP, &switch_cmd, Duration::from_millis(500));

    Ok(())
}

/// Maximum time to wait for DFU bootloader USB re-enumeration (covers WSL/usbipd attach latency)
const DFU_REENUM_TIMEOUT: Duration = Duration::from_secs(15);
const DFU_REENUM_POLL_INTERVAL: Duration = Duration::from_millis(250);

/// Connect to DFU mode, switching from runtime mode if needed.
pub fn ensure_dfu_mode() -> Result<DfuDevice, DfuError> {
    let context = Context::new()?;
    if DfuDevice::find_dfu_device(&context).is_ok() {
        return DfuDevice::open();
    }

    // Try switching from runtime mode
    eprintln!("Switching XDS110 to DFU bootloader mode (cmd 0x26)...");
    trigger_dfu_mode_switch()?;

    // Wait for USB re-enumeration (including WSL / usbipd auto-attach delay)
    eprintln!(
        "Waiting for DFU bootloader re-enumeration (up to {}s)...",
        DFU_REENUM_TIMEOUT.as_secs()
    );
    let deadline = Instant::now() + DFU_REENUM_TIMEOUT;
    let mut last_err: Option<DfuError> = None;
    while Instant::now() < deadline {
        thread::sleep(DFU_REENUM_POLL_INTERVAL);
        let ctx = Context::new()?;
        if DfuDevice::find_dfu_device(&ctx).is_ok() {
            thread::sleep(Duration::from_millis(200));
            match DfuDevice::open() {
                Ok(dev) => return Ok(dev),
                Err(err) => last_err = Some(err),
            }
        }
    }

    Err(last_err.unwrap_or(DfuError::Timeout))
}

/// Flash a firmware binary file to the probe.
pub fn flash_firmware_file(path: &Path, reset_after: bool) -> Result<(), DfuError> {
    let firmware_bytes = std::fs::read(path)?;
    if firmware_bytes.len() < 32 {
        return Err(DfuError::Protocol("Firmware binary is too small".into()));
    }

    let dfu = ensure_dfu_mode()?;
    println!(
        "Flashing {} ({} bytes) starting at 0x{:08x}...",
        path.display(),
        firmware_bytes.len(),
        dfu.info.app_start_addr
    );

    let start = Instant::now();
    dfu.write_flash(dfu.info.app_start_addr, &firmware_bytes, |done, total| {
        let pct = (done as f64 / total as f64) * 100.0;
        eprint!("\rProgress: [{done}/{total}] {pct:.1}%");
    })?;
    eprintln!();
    println!(
        "Flashing complete in {:.2}s.",
        start.elapsed().as_secs_f64()
    );

    if reset_after {
        println!("Resetting probe back to runtime mode...");
        dfu.reset_to_runtime()?;
    }

    Ok(())
}
