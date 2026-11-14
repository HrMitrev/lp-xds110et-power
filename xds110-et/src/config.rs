//! Configuration constants for XDS110 EnergyTrace.

/// USB Vendor ID for Texas Instruments
pub const VID: u16 = 0x0451;
/// USB Product ID for XDS110
pub const PID: u16 = 0xBEF3;

/// Interface number for commands (WinUSB)
pub const CMD_IF: u8 = 2;
/// Bulk OUT endpoint for command requests
pub const CMD_OUT_EP: u8 = 0x02;
/// Bulk IN endpoint for command responses
pub const CMD_IN_EP: u8 = 0x83;

/// Interface number for raw EnergyTrace data stream
pub const DATA_IF: u8 = 6;
/// Bulk IN endpoint for EnergyTrace sample stream
pub const DATA_IN_EP: u8 = 0x87;

/// USB read/write timeout in milliseconds for command interface
pub const CMD_TIMEOUT_MS: u64 = 4000;

/// Default slope in nanoamperes per ADC Least Significant Bit (nA/LSB).
/// Derived from EnergyTrace calibration points.
pub const DEFAULT_SLOPE_NA_PER_LSB: f64 = 14.6626;

/// Hardware sampling rate in Hz (256 kS/s)
pub const SAMPLE_RATE_HZ: u32 = 256_000;

/// Fixed wire frame length in bytes for bulk stream transfers
pub const FRAME_LEN: usize = 1030;

/// Number of 32-bit samples per data frame
pub const SAMPLES_PER_FRAME: usize = 256;

/// Default rows written to CSV log per hour (1 row/sec)
pub const DEFAULT_ROWS_PER_HOUR: u32 = 3600;
