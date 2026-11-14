//! High-speed frame parser for the XDS110 EnergyTrace 256 kS/s stream.

use crate::config::{FRAME_LEN, SAMPLES_PER_FRAME};
use crate::protocol::signed24;

/// A single sample from the EnergyTrace stream
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Sample {
    pub adc: i32,
    pub status: u8,
}

/// Parses bulk IN frames from the stream byte buffer.
pub struct FrameParser {
    buf: Vec<u8>,
    preamble_seen: bool,
    last_seq: Option<u8>,
}

impl FrameParser {
    pub fn new() -> Self {
        Self {
            buf: Vec::with_capacity(32 * 1024),
            preamble_seen: false,
            last_seq: None,
        }
    }

    /// Reset sequence tracking (e.g., on reconnect)
    pub fn reset_seq(&mut self) {
        self.last_seq = None;
    }

    /// Push incoming chunk of bytes
    pub fn push_chunk(&mut self, chunk: &[u8]) {
        self.buf.extend_from_slice(chunk);
    }

    /// Parse available frames from internal buffer.
    /// Returns a list of (lost_frames_before_this_frame, [Sample; 256]).
    pub fn parse_next_frames(&mut self) -> Vec<(u32, [Sample; SAMPLES_PER_FRAME])> {
        let mut results = Vec::new();
        let mut pos = 0;

        while pos < self.buf.len() {
            // Check for initial 3-byte preamble '35 80 bb'
            if !self.preamble_seen && self.buf[pos] == 0x35 {
                if self.buf.len() - pos >= 3 {
                    if self.buf[pos + 1] == 0x80 && self.buf[pos + 2] == 0xbb {
                        self.preamble_seen = true;
                        pos += 3;
                        continue;
                    }
                } else {
                    // Need more bytes to verify preamble
                    break;
                }
            }

            // Check if we have a full 1030-byte frame
            if self.buf.len() - pos < FRAME_LEN {
                break;
            }

            // Frame must start with magic 0x68
            if self.buf[pos] != 0x68 {
                pos += 1; // Resync
                continue;
            }

            let seq = self.buf[pos + 1];

            // Check sequence continuity
            let lost = match self.last_seq {
                Some(last) => {
                    let expected = (last.wrapping_add(1)) & 0xFF;
                    if seq != expected {
                        (seq.wrapping_sub(last).wrapping_sub(1)) as u32
                    } else {
                        0
                    }
                }
                None => 0,
            };
            self.last_seq = Some(seq);

            // Parse 256 big-endian 32-bit words starting at offset pos + 6
            let payload_start = pos + 6;
            let mut samples = [Sample { adc: 0, status: 0 }; SAMPLES_PER_FRAME];

            for i in 0..SAMPLES_PER_FRAME {
                let off = payload_start + i * 4;
                let word = u32::from_be_bytes([
                    self.buf[off],
                    self.buf[off + 1],
                    self.buf[off + 2],
                    self.buf[off + 3],
                ]);
                let raw_adc = word >> 8;
                let status = (word & 0xFF) as u8;
                samples[i] = Sample {
                    adc: signed24(raw_adc),
                    status,
                };
            }

            results.push((lost, samples));
            pos += FRAME_LEN;
        }

        if pos > 0 {
            self.buf.drain(..pos);
        }

        results
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_preamble_and_frame_parsing() {
        let mut parser = FrameParser::new();

        // Push 3-byte preamble
        parser.push_chunk(&[0x35, 0x80, 0xbb]);

        // Construct a 1030-byte frame
        let mut frame = vec![0u8; 1030];
        frame[0] = 0x68; // Magic
        frame[1] = 0x01; // Seq
        frame[2] = 0x00; // 0x0400 u16le
        frame[3] = 0x04;
        frame[4] = 0x00;
        frame[5] = 0x00;

        // Sample 0: ADC = 1000, Status = 0xd0
        let w0: u32 = (1000 << 8) | 0xd0;
        frame[6..10].copy_from_slice(&w0.to_be_bytes());

        parser.push_chunk(&frame);

        let parsed = parser.parse_next_frames();
        assert_eq!(parsed.len(), 1);
        let (lost, samples) = &parsed[0];
        assert_eq!(*lost, 0);
        assert_eq!(samples[0].adc, 1000);
        assert_eq!(samples[0].status, 0xd0);
    }

    #[test]
    fn test_sequence_loss_detection() {
        let mut parser = FrameParser::new();

        let mut frame1 = vec![0u8; 1030];
        frame1[0] = 0x68;
        frame1[1] = 10;
        parser.push_chunk(&frame1);
        let p1 = parser.parse_next_frames();
        assert_eq!(p1[0].0, 0);

        // Frame 2 has seq 13 (2 frames lost: 11 and 12)
        let mut frame2 = vec![0u8; 1030];
        frame2[0] = 0x68;
        frame2[1] = 13;
        parser.push_chunk(&frame2);
        let p2 = parser.parse_next_frames();
        assert_eq!(p2[0].0, 2);
    }
}
