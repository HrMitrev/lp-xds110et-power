//! Load plot points from an existing EnergyTrace CSV log file.

use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::Path;

use crate::data_series::PlotPoint;

/// Column indices of the fields needed for plotting.
struct Columns {
    time: usize,
    min: usize,
    max: usize,
    avg: usize,
}

impl Columns {
    fn from_header(header: &str) -> Option<Self> {
        let (mut time, mut min, mut max, mut avg) = (None, None, None, None);
        for (idx, col) in header.split(',').map(str::trim).enumerate() {
            match col {
                "time_s" => time = Some(idx),
                "current_min_nA" => min = Some(idx),
                "current_max_nA" => max = Some(idx),
                "current_avg_nA" => avg = Some(idx),
                _ => {}
            }
        }
        Some(Self {
            time: time?,
            min: min?,
            max: max?,
            avg: avg?,
        })
    }

    fn parse_row(&self, line: &str) -> Option<PlotPoint> {
        let parts: Vec<&str> = line
            .split(',')
            .map(|s| s.trim().trim_matches('"'))
            .collect();
        let field = |idx: usize| parts.get(idx)?.parse::<f64>().ok();
        Some(PlotPoint {
            time_s: field(self.time)?,
            avg_na: field(self.avg)?,
            min_na: field(self.min)?,
            max_na: field(self.max)?,
        })
    }
}

/// Load PlotPoints from an existing EnergyTrace CSV log file.
/// Comment lines (`#`) and malformed rows are skipped.
pub fn load_csv_file(path: &Path) -> Result<Vec<PlotPoint>, Box<dyn std::error::Error>> {
    let reader = BufReader::new(File::open(path)?);
    let mut points = Vec::new();
    let mut columns: Option<Columns> = None;

    for line in reader.lines() {
        let line = line?;
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }

        match &columns {
            None => columns = Columns::from_header(trimmed),
            Some(cols) => points.extend(cols.parse_row(trimmed)),
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
}
