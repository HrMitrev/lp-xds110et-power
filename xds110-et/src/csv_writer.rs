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
    // Formatted UTC second, so very high row rates don't format a timestamp per row
    utc_cache: (i64, String),
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
            utc_cache: (i64::MIN, String::new()),
        }
    }

    /// Add lost frames count to current window and advance the time axis.
    pub fn add_lost_frames(&mut self, lost: u32) -> std::io::Result<()> {
        if self.n > 0 {
            self.flush_window()?;
        }
        self.lost_in_window += lost;
        self.start_sample += (lost as u64) * crate::config::SAMPLES_PER_FRAME as u64;
        Ok(())
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
        if self.window_size == 1 {
            return self.add_samples_raw(samples);
        }

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

    /// Optimized path for raw sample logging (1 sample per row, window_size == 1)
    fn add_samples_raw(&mut self, samples: &[Sample]) -> std::io::Result<()> {
        use std::fmt::Write as FmtWrite;
        let mut batch_buf = String::with_capacity(32 * 1024);

        for sample in samples {
            if (self.rows_written & 0x1FF) == 0 || self.utc_cache.0 == i64::MIN {
                let now = Utc::now();
                if now.timestamp() != self.utc_cache.0 {
                    self.utc_cache = (now.timestamp(), utc_now_string());
                }
            }

            let time_whole = self.start_sample / (SAMPLE_RATE_HZ as u64);
            let time_frac = ((self.start_sample % (SAMPLE_RATE_HZ as u64)) * 10_000_000)
                / (SAMPLE_RATE_HZ as u64);
            let cur_val = ((sample.adc as f64 - self.offset) * self.slope * 10.0).round() as i64;
            let cur_whole = cur_val / 10;
            let cur_frac = (cur_val % 10).abs();

            let _ = write!(
                batch_buf,
                "{},\"{time_whole}.{time_frac:07}\",{},1,{},{},{},{}.00,{cur_whole}.{cur_frac},{cur_whole}.{cur_frac},{cur_whole}.{cur_frac},{:02x}\n",
                self.rows_written,
                self.utc_cache.1,
                self.lost_in_window,
                sample.adc,
                sample.adc,
                sample.adc,
                sample.status
            );

            self.rows_written += 1;
            self.start_sample += 1;
            self.lost_in_window = 0;
        }

        self.writer.write_all(batch_buf.as_bytes())?;
        Ok(())
    }

    /// Flush buffered rows to the underlying writer.
    pub fn flush(&mut self) -> std::io::Result<()> {
        self.writer.flush()
    }

    /// Flush the accumulated window into a single CSV row.
    pub fn flush_window(&mut self) -> std::io::Result<()> {
        if self.n == 0 {
            return Ok(());
        }

        // Avoid querying system clock (clock_gettime) on every single row when sampling at high rates
        if (self.rows_written & 0x1FF) == 0 || self.utc_cache.0 == i64::MIN {
            let now = Utc::now();
            if now.timestamp() != self.utc_cache.0 {
                self.utc_cache = (now.timestamp(), utc_now_string());
            }
        }
        let time_s = self.start_sample as f64 / SAMPLE_RATE_HZ as f64;
        let avg_adc = self.adc_sum as f64 / self.n as f64;
        let cur = |adc: f64| (adc - self.offset) * self.slope;

        writeln!(
            self.writer,
            "{},\"{:.7}\",{},{},{},{},{},{:.2},{:.1},{:.1},{:.1},{:02x}",
            self.rows_written,
            time_s,
            self.utc_cache.1,
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
    let mut writer = BufWriter::with_capacity(1024 * 1024, file);

    WindowWriter::write_cal_comment(&mut writer, cal, cal.idx0_offset, slope, SAMPLE_RATE_HZ)?;
    let rph_comment = if rows_per_hour == 0 {
        "raw (1 sample/row)".to_string()
    } else {
        rows_per_hour.to_string()
    };
    writeln!(
        writer,
        "# rows_per_hour={rph_comment} samples_per_row={window_size} (status_or = bitwise OR of all status bytes in the row)"
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
    fn test_add_lost_frames_advances_time() {
        let mut output = Vec::new();
        let mut wr = WindowWriter::new(&mut output, 1, 0.0, 1.0);
        let s = [Sample {
            adc: 100,
            status: 0,
        }];
        wr.add_samples(&s).unwrap(); // sample at t=0
        wr.add_lost_frames(2).unwrap(); // 2 frames lost = 512 samples
        wr.add_samples(&s).unwrap(); // sample at t = (1 + 512) / 256000
        wr.flush().unwrap();
        let text = String::from_utf8(output).unwrap();
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines.len(), 2);
        assert!(lines[0].contains("\"0.0000000\""));
        assert!(lines[1].contains("\"0.0020039\""));
    }

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
