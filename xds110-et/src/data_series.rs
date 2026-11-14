//! Data structures and helpers for time-series plotting.

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

    /// Multiplier converting nA into this unit.
    pub fn factor(&self) -> f64 {
        match self {
            Unit::NA => 1.0,
            Unit::UA => 1e-3,
            Unit::MA => 1e-6,
        }
    }

    /// Select unit based on a magnitude in nA:
    /// at least 1 mA (1,000,000 nA) gives mA, at least 1 uA (1,000 nA) gives uA, else nA.
    pub fn from_magnitude(na: f64) -> Self {
        let a = na.abs();
        if a >= 1_000_000.0 {
            Unit::MA
        } else if a >= 1_000.0 {
            Unit::UA
        } else {
            Unit::NA
        }
    }
}

/// Human-readable formatting of current with automatic unit selection.
pub fn fmt_current(na: f64) -> String {
    let unit = Unit::from_magnitude(na);
    let precision = if unit == Unit::NA { 1 } else { 3 };
    format!("{:.precision$} {}", na * unit.factor(), unit.name())
}

/// Statistics for points falling in a time range.
#[derive(Debug, Clone, Copy)]
pub struct WindowStats {
    pub avg_na: f64,
    pub min_na: f64,
    pub max_na: f64,
}

/// Points (sorted by time) whose timestamp lies in `[lo, hi]`.
pub fn visible_slice(points: &[PlotPoint], lo: f64, hi: f64) -> &[PlotPoint] {
    let start = points.partition_point(|p| p.time_s < lo);
    let end = points.partition_point(|p| p.time_s <= hi);
    &points[start..end.max(start)]
}

/// Like [`visible_slice`] but extended by one point on each side so that lines
/// entering and leaving the range are still drawn.
pub fn visible_slice_padded(points: &[PlotPoint], lo: f64, hi: f64) -> &[PlotPoint] {
    let start = points.partition_point(|p| p.time_s < lo).saturating_sub(1);
    let end = (points.partition_point(|p| p.time_s <= hi) + 1).min(points.len());
    &points[start..end.max(start)]
}

/// Minimum and maximum of the per-point averages.
pub fn avg_extent(points: &[PlotPoint]) -> Option<(f64, f64)> {
    points
        .iter()
        .map(|p| p.avg_na)
        .fold(None, |acc, v| match acc {
            None => Some((v, v)),
            Some((lo, hi)) => Some((lo.min(v), hi.max(v))),
        })
}

pub fn compute_window_stats(points: &[PlotPoint], lo: f64, hi: f64) -> Option<WindowStats> {
    let slice = visible_slice(points, lo, hi);
    if slice.is_empty() {
        return None;
    }

    let mut sum = 0.0;
    let mut min = f64::INFINITY;
    let mut max = f64::NEG_INFINITY;
    for p in slice {
        sum += p.avg_na;
        min = min.min(p.min_na);
        max = max.max(p.max_na);
    }

    Some(WindowStats {
        avg_na: sum / slice.len() as f64,
        min_na: min,
        max_na: max,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pt(time_s: f64, avg_na: f64) -> PlotPoint {
        PlotPoint {
            time_s,
            avg_na,
            min_na: avg_na - 20.0,
            max_na: avg_na + 20.0,
        }
    }

    #[test]
    fn test_unit_scaling() {
        assert_eq!(Unit::from_magnitude(500.0), Unit::NA);
        assert_eq!(Unit::from_magnitude(1500.0), Unit::UA);
        assert_eq!(Unit::from_magnitude(1_500_000.0), Unit::MA);
        assert_eq!(Unit::from_magnitude(-2_000_000.0), Unit::MA);
        assert_eq!(Unit::from_magnitude(999_999.0), Unit::UA);
        assert_eq!(Unit::from_magnitude(999.0), Unit::NA);
    }

    #[test]
    fn test_fmt_current() {
        assert_eq!(fmt_current(250.0), "250.0 nA");
        assert_eq!(fmt_current(-1500.0), "-1.500 uA");
        assert_eq!(fmt_current(2_500_000.0), "2.500 mA");
    }

    #[test]
    fn test_window_stats() {
        let points = [
            pt(0.0, 100.0),
            pt(1.0, 200.0),
            pt(2.0, 300.0),
            pt(3.0, 400.0),
        ];

        let stats = compute_window_stats(&points, 0.5, 2.5).unwrap();
        assert_eq!(stats.avg_na, 250.0);
        assert_eq!(stats.min_na, 180.0);
        assert_eq!(stats.max_na, 320.0);

        assert!(compute_window_stats(&points, 10.0, 20.0).is_none());
        assert!(compute_window_stats(&points, 2.5, 0.5).is_none());
    }

    #[test]
    fn test_visible_slice_padded_includes_neighbours() {
        let points = [
            pt(0.0, 1.0),
            pt(1.0, 1.0),
            pt(2.0, 1.0),
            pt(3.0, 1.0),
            pt(4.0, 1.0),
        ];
        let s = visible_slice_padded(&points, 1.5, 2.5);
        assert_eq!(s.first().unwrap().time_s, 1.0);
        assert_eq!(s.last().unwrap().time_s, 3.0);
        assert_eq!(visible_slice_padded(&points, -5.0, 10.0).len(), 5);
        assert_eq!(visible_slice_padded(&[], 0.0, 1.0).len(), 0);
    }

    #[test]
    fn test_avg_extent() {
        assert_eq!(avg_extent(&[]), None);
        assert_eq!(
            avg_extent(&[pt(0.0, 5.0), pt(1.0, -3.0), pt(2.0, 9.0)]),
            Some((-3.0, 9.0))
        );
    }
}
