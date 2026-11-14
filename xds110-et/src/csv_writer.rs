//! Decoupled CSV logging and decimated window accumulation.

use chrono::Utc;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

use crate::config::SAMPLE_RATE_HZ;
use crate::frame::Sample;
use crate::protocol::Calibration;

pub const CSV_HEADER: &str = "index,time_s,utc,samples,frames_lost,adc_min,adc_max,adc_avg,current_min_nA,current_max_nA,current_avg_nA,status_or";

/// Formats UTC timestamp in ISO 8601 string
pub fn utc_now_string() -> String {
    Utc::now().format("%Y-%m-%dT%H:%M:%SZ").to_string()
}

/// Accumulates raw samples into decimated windows and flushes them to CSV.
pub struct WindowWriter<W: Write> {
    writer: W,
    pub window_size: u64,
    pub offset: f64,
    pub slope: f64,

    pub rows_written: u64,
    start_sample: u64,
    lost_in_window: u32,

    // Window accumulator
    n: u64,
    adc_sum: i64,
    adc_min: i32,
    adc_max: i32,
    status_or: u8,
}

impl<W: Write> WindowWriter<W> {
    pub fn new(writer: W, window_size: u64, offset: f64, slope: f64) -> Self {
        Self {
            writer,
            window_size: window_size.max(1),
            offset,
            slope,
            rows_written: 0,
            start_sample: 0,
            lost_in_window: 0,
            n: 0,
            adc_sum: 0,
            adc_min: i32::MAX,
            adc_max: i32::MIN,
            status_or: 0,
        }
    }

    /// Add lost frames count to current window
    pub fn add_lost_frames(&mut self, lost: u32) {
        self.lost_in_window += lost;
    }

    /// Advance time axis over a reconnect gap (in seconds)
    pub fn skip_gap(&mut self, seconds: f64) {
        self.start_sample += (seconds * SAMPLE_RATE_HZ as f64).round() as u64;
    }

    /// Write reconnect comment line
    pub fn write_reconnect_comment(
        &mut self,
        reconnect_num: u32,
        cal: &Calibration,
    ) -> std::io::Result<()> {
        writeln!(
            self.writer,
            "# reconnect {} (#{reconnect_num})",
            utc_now_string()
        )?;
        Self::write_cal_comment(
            &mut self.writer,
            cal,
            self.offset,
            self.slope,
            SAMPLE_RATE_HZ,
        )?;
        self.writer.flush()?;
        Ok(())
    }

    /// Write calibration comment metadata
    pub fn write_cal_comment(
        w: &mut W,
        cal: &Calibration,
        offset: f64,
        slope: f64,
        sample_rate: u32,
    ) -> std::io::Result<()> {
        writeln!(
            w,
            "# calibration (ADC average per point): idx0={:.3}, idx4={:.3}, idx1={:.3}",
            cal.idx0_offset, cal.idx4, cal.idx1
        )?;
        writeln!(
            w,
            "# offset_adc={offset:.3} slope_nA_per_lsb={slope:.4} sample_rate_hz={sample_rate}"
        )?;
        Ok(())
    }

    /// Add a slice of samples, automatically flushing windows when `window_size` is reached.
    pub fn add_samples(&mut self, samples: &[Sample]) -> std::io::Result<()> {
        let mut pos = 0;
        while pos < samples.len() {
            let room = (self.window_size - self.n) as usize;
            let take = room.min(samples.len() - pos);

            for sample in &samples[pos..pos + take] {
                self.n += 1;
                self.adc_sum += sample.adc as i64;
                if sample.adc < self.adc_min {
                    self.adc_min = sample.adc;
                }
                if sample.adc > self.adc_max {
                    self.adc_max = sample.adc;
                }
                self.status_or |= sample.status;
            }

            pos += take;

            if self.n >= self.window_size {
                self.flush_window()?;
            }
        }
        Ok(())
    }

    /// Flush the accumulated window into a single CSV row.
    pub fn flush_window(&mut self) -> std::io::Result<()> {
        if self.n == 0 {
            return Ok(());
        }

        let time_s = self.start_sample as f64 / SAMPLE_RATE_HZ as f64;
        let avg_adc = self.adc_sum as f64 / self.n as f64;
        let cur = |adc: f64| (adc - self.offset) * self.slope;

        writeln!(
            self.writer,
            "{},\"{:.3}\",{},{},{},{},{},{:.2},{:.1},{:.1},{:.1},{:02x}",
            self.rows_written,
            time_s,
            utc_now_string(),
            self.n,
            self.lost_in_window,
            self.adc_min,
            self.adc_max,
            avg_adc,
            cur(self.adc_min as f64),
            cur(self.adc_max as f64),
            cur(avg_adc),
            self.status_or
        )?;
        self.writer.flush()?;

        self.rows_written += 1;
        self.start_sample += self.n;
        self.lost_in_window = 0;

        // Reset accumulator
        self.n = 0;
        self.adc_sum = 0;
        self.adc_min = i32::MAX;
        self.adc_max = i32::MIN;
        self.status_or = 0;

        Ok(())
    }
}

/// Create a WindowWriter writing to a file on disk
pub fn create_csv_file(
    path: &Path,
    window_size: u64,
    rows_per_hour: u32,
    cal: &Calibration,
    slope: f64,
) -> std::io::Result<WindowWriter<BufWriter<File>>> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let file = File::create(path)?;
    let mut writer = BufWriter::new(file);

    WindowWriter::write_cal_comment(&mut writer, cal, cal.idx0_offset, slope, SAMPLE_RATE_HZ)?;
    writeln!(
        writer,
        "# rows_per_hour={rows_per_hour} samples_per_row={window_size} (status_or = bitwise OR of all status bytes in the row)"
    )?;
    writeln!(writer, "{CSV_HEADER}")?;
    writer.flush()?;

    Ok(WindowWriter::new(
        writer,
        window_size,
        cal.idx0_offset,
        slope,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_window_accumulation_and_decoupling() {
        let mut output = Vec::new();
        // Window size of 4 samples
        let mut wr = WindowWriter::new(&mut output, 4, 100.0, 10.0);

        let s1 = [
            Sample {
                adc: 110,
                status: 0x10,
            },
            Sample {
                adc: 90,
                status: 0x20,
            },
            Sample {
                adc: 120,
                status: 0x00,
            },
        ];
        wr.add_samples(&s1).unwrap();
        assert_eq!(wr.rows_written, 0); // Not reached 4 yet

        let s2 = [
            Sample {
                adc: 80,
                status: 0x04,
            }, // 4th sample -> triggers row 0
            Sample {
                adc: 105,
                status: 0x01,
            }, // 1st sample of row 1
        ];
        wr.add_samples(&s2).unwrap();
        assert_eq!(wr.rows_written, 1);

        wr.flush_window().unwrap(); // Flush partial row 1
        assert_eq!(wr.rows_written, 2);

        let text = String::from_utf8(output).unwrap();
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines.len(), 2);

        // Row 0: adc min 80, max 120, avg (110+90+120+80)/4 = 100.0
        // Current: (100 - 100)*10 = 0.0 nA, min: (80-100)*10 = -200.0, max: (120-100)*10 = 200.0
        // status_or: 0x10 | 0x20 | 0x04 = 0x34
        assert!(lines[0].starts_with("0,"));
        assert!(lines[0].contains(",80,120,100.00,-200.0,200.0,0.0,34"));

        // Row 1 (partial): adc 105, status 0x01
        assert!(lines[1].starts_with("1,"));
        assert!(lines[1].contains(",105,105,105.00,50.0,50.0,50.0,01"));
    }
}
