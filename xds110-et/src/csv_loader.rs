//! Load plot points from an existing EnergyTrace CSV log file.

use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::Path;

use crate::config::SAMPLE_RATE_HZ;
use crate::data_series::PlotPoint;

/// Column indices of the fields needed for plotting.
struct Columns {
    time: usize,
    samples: Option<usize>,
    min: usize,
    max: usize,
    avg: usize,
}

impl Columns {
    fn from_header(header: &str) -> Option<Self> {
        let (mut time, mut samples, mut min, mut max, mut avg) = (None, None, None, None, None);
        for (idx, col) in header.split(',').map(str::trim).enumerate() {
            match col {
                "time_s" => time = Some(idx),
                "samples" => samples = Some(idx),
                "current_min_nA" => min = Some(idx),
                "current_max_nA" => max = Some(idx),
                "current_avg_nA" => avg = Some(idx),
                _ => {}
            }
        }
        Some(Self {
            time: time?,
            samples,
            min: min?,
            max: max?,
            avg: avg?,
        })
    }

    /// Returns the point and the number of samples the row covers (0 if unknown).
    fn parse_row(&self, line: &str) -> Option<(PlotPoint, u64)> {
        let parts: Vec<&str> = line
            .split(',')
            .map(|s| s.trim().trim_matches('"'))
            .collect();
        let field = |idx: usize| parts.get(idx)?.parse::<f64>().ok();
        let samples = self
            .samples
            .and_then(|idx| parts.get(idx)?.parse::<u64>().ok())
            .unwrap_or(0);
        let point = PlotPoint {
            time_s: field(self.time)?,
            avg_na: field(self.avg)?,
            min_na: field(self.min)?,
            max_na: field(self.max)?,
        };
        Some((point, samples))
    }
}

/// Load PlotPoints from an existing EnergyTrace CSV log file.
/// Comment lines (`#`) and malformed rows are skipped.
pub fn load_csv_file(path: &Path) -> Result<Vec<PlotPoint>, Box<dyn std::error::Error>> {
    let reader = BufReader::new(File::open(path)?);
    let mut points: Vec<PlotPoint> = Vec::new();
    let mut columns: Option<Columns> = None;
    // End time of the previous row, used to spread rows that share a rounded timestamp
    let mut prev_end = f64::NEG_INFINITY;

    for line in reader.lines() {
        let line = line?;
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }

        match &columns {
            None => columns = Columns::from_header(trimmed),
            Some(cols) => {
                if let Some((mut point, samples)) = cols.parse_row(trimmed) {
                    // Older logs store time_s rounded to 1 ms; with sub-ms rows many points then
                    // share one timestamp and the plot degenerates into a vertical block.
                    point.time_s = point.time_s.max(prev_end);
                    prev_end = point.time_s + samples as f64 / SAMPLE_RATE_HZ as f64;
                    points.push(point);
                }
            }
        }
    }

    Ok(points)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn test_load_csv_skips_comments_and_bad_rows() {
        let path = std::env::temp_dir().join(format!("xds110_et_test_{}.csv", std::process::id()));
        let mut f = File::create(&path).unwrap();
        writeln!(f, "# comment").unwrap();
        writeln!(
            f,
            "index,time_s,utc,samples,frames_lost,adc_min,adc_max,adc_avg,current_min_nA,current_max_nA,current_avg_nA,status_or"
        )
        .unwrap();
        writeln!(f, "0,\"0.000\",t,1,0,1,2,1.5,10.0,30.0,20.0,00").unwrap();
        writeln!(f, "garbage").unwrap();
        writeln!(f, "1,\"1.000\",t,1,0,1,2,1.5,11.0,31.0,21.0,00").unwrap();
        drop(f);

        let pts = load_csv_file(&path).unwrap();
        std::fs::remove_file(&path).unwrap();

        assert_eq!(pts.len(), 2);
        assert_eq!(pts[1].time_s, 1.0);
        assert_eq!(pts[1].avg_na, 21.0);
        assert_eq!(pts[1].min_na, 11.0);
        assert_eq!(pts[1].max_na, 31.0);
    }

    #[test]
    fn test_load_csv_spreads_rows_sharing_a_rounded_timestamp() {
        let path = std::env::temp_dir().join(format!("xds110_et_quant_{}.csv", std::process::id()));
        let mut f = File::create(&path).unwrap();
        writeln!(
            f,
            "index,time_s,samples,current_min_nA,current_max_nA,current_avg_nA"
        )
        .unwrap();
        for i in 0..3 {
            writeln!(f, "{i},\"1.000\",256,1.0,3.0,2.0").unwrap();
        }
        drop(f);

        let pts = load_csv_file(&path).unwrap();
        std::fs::remove_file(&path).unwrap();

        assert_eq!(pts.len(), 3);
        assert_eq!(pts[0].time_s, 1.0);
        assert!((pts[1].time_s - 1.001).abs() < 1e-9);
        assert!((pts[2].time_s - 1.002).abs() < 1e-9);
    }
}
