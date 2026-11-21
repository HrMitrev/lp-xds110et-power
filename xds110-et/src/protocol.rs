//! Protocol commands and frame encoding/decoding for XDS110 EnergyTrace.

// The command table documents the full protocol; not every command is used by this tool.
#![allow(dead_code)]

use std::fmt;

// Protocol command bytes
pub const CMD_DISCONNECT: u8 = 0x02;
pub const CMD_VERSION: u8 = 0x03;
pub const CMD_SET_SRST: u8 = 0x0E;
pub const CMD_SETUP: u8 = 0x1D;
pub const CMD_CALIBRATE: u8 = 0x1E;
pub const CMD_START: u8 = 0x1F;
pub const CMD_STOP: u8 = 0x20;
pub const CMD_CLEANUP: u8 = 0x21;
pub const CMD_DCDC_GET_MCU_VERSION: u8 = 0x22;
pub const CMD_DCDC_POWER_DOWN: u8 = 0x23;
pub const CMD_DCDC_SET_VCC: u8 = 0x24;
pub const CMD_DCDC_RESTART: u8 = 0x25;
pub const CMD_CONNECT: u8 = 0x28;
pub const CMD_SET_RANGE: u8 = 0x30;
pub const CMD_SET_DIG: u8 = 0x31;
pub const CMD_HWINFO: u8 = 0x46;
/// OpenFW extension: query or switch probe mode (0x00 = Normal, 0x01 = BMP, 0xFF/empty = Query)
pub const CMD_PROBE_MODE: u8 = 0x63;

pub const PROBE_MODE_CAP_SWITCH: u8 = 1 << 0;
pub const PROBE_MODE_CAP_GDB_SERVER: u8 = 1 << 1;
pub const PROBE_MODE_CAP_DEFAULT_BMP: u8 = 1 << 2;

/// Protocol frame magic byte '*'
pub const MAGIC_BYTE: u8 = b'*';

#[derive(Debug)]
pub enum ProtocolError {
    FrameTooShort(usize),
    InvalidMagic(u8),
    LengthMismatch { declared: usize, actual: usize },
    CommandFailed { cmd: u8, code: i32 },
    InvalidResponse(String),
}

impl fmt::Display for ProtocolError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::FrameTooShort(len) => write!(f, "Response frame too short: {len} bytes"),
            Self::InvalidMagic(b) => write!(f, "Invalid magic byte in response: 0x{b:02x}"),
            Self::LengthMismatch { declared, actual } => {
                write!(
                    f,
                    "Declared payload len {declared} does not match buffer len {actual}"
                )
            }
            Self::CommandFailed { cmd, code } => {
                write!(f, "Command 0x{cmd:02x} failed with error code {code}")
            }
            Self::InvalidResponse(msg) => write!(f, "Invalid response: {msg}"),
        }
    }
}

impl std::error::Error for ProtocolError {}

/// Encode a command request frame:
/// `*` (0x2A) | `u16le length` | `cmd` | `args`
/// where length = 1 + args.len()
pub fn encode_command(cmd: u8, args: &[u8]) -> Vec<u8> {
    let payload_len = (1 + args.len()) as u16;
    let mut frame = Vec::with_capacity(3 + 1 + args.len());
    frame.push(MAGIC_BYTE);
    frame.extend_from_slice(&payload_len.to_le_bytes());
    frame.push(cmd);
    frame.extend_from_slice(args);
    frame
}

/// Decode a command response frame:
/// `*` (0x2A) | `u16le length` | `i32le error` | `data`
/// Returns the slice of data following the error code if error == 0.
pub fn decode_response(cmd: u8, resp: &[u8]) -> Result<Vec<u8>, ProtocolError> {
    if resp.len() < 7 {
        return Err(ProtocolError::FrameTooShort(resp.len()));
    }
    if resp[0] != MAGIC_BYTE {
        return Err(ProtocolError::InvalidMagic(resp[0]));
    }
    let payload_len = u16::from_le_bytes([resp[1], resp[2]]) as usize;
    if payload_len < 4 || resp.len() < 3 + payload_len {
        return Err(ProtocolError::LengthMismatch {
            declared: payload_len,
            actual: resp.len() - 3,
        });
    }
    let err_code = i32::from_le_bytes([resp[3], resp[4], resp[5], resp[6]]);
    if err_code != 0 {
        return Err(ProtocolError::CommandFailed {
            cmd,
            code: err_code,
        });
    }
    // Data starts at byte 7 up to 3 + payload_len
    Ok(resp[7..3 + payload_len].to_vec())
}

/// Helper to convert signed 24-bit raw ADC to signed 32-bit integer.
#[inline]
pub fn signed24(v: u32) -> i32 {
    let raw = v & 0x00FF_FFFF;
    if (raw & 0x0080_0000) != 0 {
        (raw as i32) - (1 << 24)
    } else {
        raw as i32
    }
}

/// Calibration results from the probe
#[derive(Debug, Clone, Copy)]
pub struct Calibration {
    pub idx0_offset: f64,
    pub idx4: f64,
    pub idx1: f64,
}

/// Firmware version and hardware ID returned by CMD_VERSION (0x03)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProbeVersion {
    pub major: u8,
    pub minor: u8,
    pub patch: u8,
    pub build: u8,
    pub hw_id: u8,
    pub extra: u8,
}

impl ProbeVersion {
    pub fn parse(bytes: &[u8]) -> Result<Self, ProtocolError> {
        if bytes.len() < 5 {
            return Err(ProtocolError::InvalidResponse(format!(
                "Version response too short: {} bytes (expected >= 5)",
                bytes.len()
            )));
        }
        Ok(Self {
            major: bytes[3],
            minor: bytes[2],
            patch: bytes[1],
            build: bytes[0],
            hw_id: bytes[4],
            extra: if bytes.len() >= 6 { bytes[5] } else { 0 },
        })
    }
}

impl fmt::Display for ProbeVersion {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}.{}.{}.{:02x} (HW: 0x{:02x})",
            self.major, self.minor, self.patch, self.build, self.hw_id
        )
    }
}

/// Runtime probe operating mode for CMD_PROBE_MODE (0x63)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ProbeMode {
    Normal = 0x00,
    Bmp = 0x01,
}

impl ProbeMode {
    pub fn from_byte(val: u8) -> Result<Self, ProtocolError> {
        match val {
            0x00 => Ok(Self::Normal),
            0x01 => Ok(Self::Bmp),
            other => Err(ProtocolError::InvalidResponse(format!(
                "Unknown probe mode byte: 0x{other:02x}"
            ))),
        }
    }
}

impl fmt::Display for ProbeMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Normal => write!(f, "normal"),
            Self::Bmp => write!(f, "bmp"),
        }
    }
}

/// Status payload returned by CMD_PROBE_MODE (0x63): `[mode, caps]`
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProbeModeStatus {
    pub mode: ProbeMode,
    pub caps: u8,
}

impl ProbeModeStatus {
    pub fn parse(bytes: &[u8]) -> Result<Self, ProtocolError> {
        if bytes.len() < 2 {
            return Err(ProtocolError::InvalidResponse(format!(
                "Probe mode response too short: {} bytes (expected >= 2)",
                bytes.len()
            )));
        }
        Ok(Self {
            mode: ProbeMode::from_byte(bytes[0])?,
            caps: bytes[1],
        })
    }

    pub fn gdb_server_compiled(&self) -> bool {
        (self.caps & PROBE_MODE_CAP_GDB_SERVER) != 0
    }

    pub fn default_is_bmp(&self) -> bool {
        (self.caps & PROBE_MODE_CAP_DEFAULT_BMP) != 0
    }
}

impl fmt::Display for ProbeModeStatus {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let default_str = if self.default_is_bmp() {
            "bmp"
        } else {
            "normal"
        };
        let gdb_str = if self.gdb_server_compiled() {
            "ready"
        } else {
            "not compiled"
        };
        write!(
            f,
            "{} (default boot: {default_str}, BMP GDB server: {gdb_str}, caps: 0x{:02x})",
            self.mode, self.caps
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_encode_command() {
        let frame = encode_command(CMD_HWINFO, &[]);
        assert_eq!(frame, vec![b'*', 0x01, 0x00, 0x46]);

        let frame_args = encode_command(CMD_SET_RANGE, &[0x00]);
        assert_eq!(frame_args, vec![b'*', 0x02, 0x00, 0x30, 0x00]);
    }

    #[test]
    fn test_decode_response_success() {
        // Response format: '*' | len (u16le) | err (i32le) | data
        // For len = 9 (4 bytes err + 5 bytes data):
        let mut resp = vec![b'*', 0x09, 0x00];
        resp.extend_from_slice(&0i32.to_le_bytes()); // err = 0
        resp.extend_from_slice(&[0x02, 0x00, 0x00, 0x00, 0x07]); // data
        let decoded = decode_response(CMD_HWINFO, &resp).expect("should succeed");
        assert_eq!(decoded, vec![0x02, 0x00, 0x00, 0x00, 0x07]);
    }

    #[test]
    fn test_decode_response_error() {
        let mut resp = vec![b'*', 0x04, 0x00];
        resp.extend_from_slice(&(-644i32).to_le_bytes());
        let err = decode_response(CMD_STOP, &resp).unwrap_err();
        match err {
            ProtocolError::CommandFailed { cmd, code } => {
                assert_eq!(cmd, CMD_STOP);
                assert_eq!(code, -644);
            }
            _ => panic!("Expected CommandFailed"),
        }
    }

    #[test]
    fn test_decode_response_short_declared_length_does_not_panic() {
        // Declared payload (2) is smaller than the 4-byte error code
        let resp = [b'*', 0x02, 0x00, 0, 0, 0, 0, 0xAA];
        assert!(matches!(
            decode_response(CMD_HWINFO, &resp),
            Err(ProtocolError::LengthMismatch { .. })
        ));
    }

    #[test]
    fn test_signed24() {
        assert_eq!(signed24(0), 0);
        assert_eq!(signed24(100), 100);
        assert_eq!(signed24(0x7FFFFF), 8388607);
        assert_eq!(signed24(0x800000), -8388608);
        assert_eq!(signed24(0xFFFFFF), -1);
    }

    #[test]
    fn test_parse_probe_version() {
        let bytes = [0x41, 0x00, 0x00, 0x03, 0x23, 0x00];
        let ver = ProbeVersion::parse(&bytes).expect("parse version");
        assert_eq!(ver.major, 3);
        assert_eq!(ver.minor, 0);
        assert_eq!(ver.patch, 0);
        assert_eq!(ver.build, 0x41);
        assert_eq!(ver.hw_id, 0x23);
        assert_eq!(format!("{ver}"), "3.0.0.41 (HW: 0x23)");
    }

    #[test]
    fn test_parse_probe_mode_status() {
        let status = ProbeModeStatus::parse(&[0x01, 0x07]).expect("parse mode");
        assert_eq!(status.mode, ProbeMode::Bmp);
        assert!(status.gdb_server_compiled());
        assert!(status.default_is_bmp());
        assert_eq!(
            format!("{status}"),
            "bmp (default boot: bmp, BMP GDB server: ready, caps: 0x07)"
        );

        let normal = ProbeModeStatus::parse(&[0x00, 0x01]).expect("parse normal");
        assert_eq!(normal.mode, ProbeMode::Normal);
        assert!(!normal.gdb_server_compiled());
        assert!(!normal.default_is_bmp());

        assert!(ProbeModeStatus::parse(&[0x00]).is_err());
        assert!(ProbeModeStatus::parse(&[0x02, 0x01]).is_err());
    }
}
