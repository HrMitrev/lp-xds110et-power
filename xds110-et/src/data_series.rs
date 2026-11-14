//! Data structures and helpers for time-series plotting and CSV loading.

use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PlotPoint {
    pub time_s: f64,
    pub avg_na: f64,
    pub min_na: f64,
    pub max_na: f64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Unit {
    NA,
    UA,
    MA,
}

impl Unit {
    pub fn name(&self) -> &'static str {
        match self {
            Unit::NA => "nA",
            Unit::UA => "uA",
            Unit::MA => "mA",
        }
    }

    pub fn factor(&self) -> f64 {
        match self {
            Unit::NA => 1.0,
            Unit::UA => 1e-3,
            Unit::MA => 1e-6,
        }
    }

    /// Select unit based on average current (in nA):
    /// >= 1 mA (1,000,000 nA) -> mA
    /// >= 1 uA (1,000 nA) and < 1 mA -> uA
    /// < 1 uA -> nA
    pub fn from_avg(avg_na: f64) -> Self {
        let a = avg_na.abs();
        if a >= 1_000_000.0 {
            Unit::MA
        } else if a >= 1_000.0 {
            Unit::UA
        } else {
            Unit::NA
        }
    }

    pub fn from_peak(peak_na: f64) -> Self {
        Self::from_avg(peak_na)
    }
}

/// Computes statistics for points falling in the range [lo, hi]
#[derive(Debug, Clone, Copy)]
pub struct WindowStats {
    pub count: usize,
    pub avg_na: f64,
    pub min_na: f64,
    pub max_na: f64,
}

pub fn compute_window_stats(points: &[PlotPoint], lo: f64, hi: f64) -> Option<WindowStats> {
    if points.is_empty() {
        return None;
    }
    // Binary search range
    let start_idx = points.partition_point(|p| p.time_s < lo);
    let end_idx = points.partition_point(|p| p.time_s <= hi);

    if start_idx >= end_idx {
        return None;
    }

    let slice = &points[start_idx..end_idx];
    let count = slice.len();
    let mut sum = 0.0;
    let mut min = f64::INFINITY;
    let mut max = f64::NEG_INFINITY;

    for p in slice {
        sum += p.avg_na;
        if p.min_na < min {
            min = p.min_na;
        }
        if p.max_na > max {
            max = p.max_na;
        }
    }

    Some(WindowStats {
        count,
        avg_na: sum / count as f64,
        min_na: min,
        max_na: max,
    })
}

/// Load PlotPoints from an existing EnergyTrace CSV log file
pub fn load_csv_file(path: &Path) -> Result<Vec<PlotPoint>, Box<dyn std::error::Error>> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut points = Vec::new();

    let mut col_time = None;
    let mut col_min = None;
    let mut col_max = None;
    let mut col_avg = None;

    for line_res in reader.lines() {
        let line = line_res?;
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }

        // Header line detection
        if col_time.is_none() && trimmed.contains("time_s") {
            let cols: Vec<&str> = trimmed.split(',').map(|s| s.trim()).collect();
            for (idx, &col) in cols.iter().enumerate() {
                match col {
                    "time_s" => col_time = Some(idx),
                    "current_min_nA" => col_min = Some(idx),
                    "current_max_nA" => col_max = Some(idx),
                    "current_avg_nA" => col_avg = Some(idx),
                    _ => {}
                }
            }
            continue;
        }

        // Parse data row
        let parts: Vec<&str> = trimmed.split(',').map(|s| s.trim_matches('"')).collect();
        if let (Some(t_idx), Some(mn_idx), Some(mx_idx), Some(avg_idx)) =
            (col_time, col_min, col_max, col_avg)
        {
            if parts.len() > t_idx.max(mn_idx).max(mx_idx).max(avg_idx) {
                if let (Ok(t), Ok(mn), Ok(mx), Ok(avg)) = (
                    parts[t_idx].parse::<f64>(),
                    parts[mn_idx].parse::<f64>(),
                    parts[mx_idx].parse::<f64>(),
                    parts[avg_idx].parse::<f64>(),
                ) {
                    points.push(PlotPoint {
                        time_s: t,
                        avg_na: avg,
                        min_na: mn,
                        max_na: mx,
                    });
                }
            }
        }
    }

    Ok(points)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_unit_scaling() {
        assert_eq!(Unit::from_avg(500.0), Unit::NA);
        assert_eq!(Unit::from_avg(1500.0), Unit::UA);
        assert_eq!(Unit::from_avg(1_500_000.0), Unit::MA);
        assert_eq!(Unit::from_avg(-2_000_000.0), Unit::MA);
        assert_eq!(Unit::from_avg(999_999.0), Unit::UA);
        assert_eq!(Unit::from_avg(999.0), Unit::NA);
    }

    #[test]
    fn test_window_stats() {
        let points = vec![
            PlotPoint { time_s: 0.0, avg_na: 100.0, min_na: 80.0, max_na: 120.0 },
            PlotPoint { time_s: 1.0, avg_na: 200.0, min_na: 180.0, max_na: 220.0 },
            PlotPoint { time_s: 2.0, avg_na: 300.0, min_na: 280.0, max_na: 320.0 },
            PlotPoint { time_s: 3.0, avg_na: 400.0, min_na: 380.0, max_na: 420.0 },
        ];

        let stats = compute_window_stats(&points, 0.5, 2.5).unwrap();
        assert_eq!(stats.count, 2);
        assert_eq!(stats.avg_na, 250.0);
        assert_eq!(stats.min_na, 180.0);
        assert_eq!(stats.max_na, 320.0);
    }

    #[test]
    fn test_load_real_log() {
        let log_path = Path::new("../logs/long_20261113_215600.csv");
        if log_path.exists() {
            let pts = load_csv_file(log_path).expect("failed to load real log");
            assert!(!pts.is_empty());
            assert!(pts[0].time_s >= 0.0);
        }
    }
}
