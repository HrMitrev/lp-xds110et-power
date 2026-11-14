//! Pure helpers for manipulating plot view ranges (no UI dependencies).

/// Smallest time span (seconds) the detail view may be zoomed or resized to.
pub const MIN_SPAN_S: f64 = 0.05;
/// A view whose right edge is within this many seconds of the data end counts as "following live".
pub const END_TOLERANCE_S: f64 = 0.2;

/// Clamp that, unlike `f64::clamp`, tolerates `lo > hi` (the upper bound wins).
pub fn clamp_lenient(v: f64, lo: f64, hi: f64) -> f64 {
    v.max(lo).min(hi)
}

/// Scale `[lo, hi]` around `center` by `factor` (>1 zooms in).
/// Returns the input unchanged if the result would be degenerate or non-finite.
pub fn zoom_range([lo, hi]: [f64; 2], center: f64, factor: f64, min_span: f64) -> [f64; 2] {
    if !(factor.is_finite() && factor > 0.0) {
        return [lo, hi];
    }
    let new_lo = center - (center - lo) / factor;
    let new_hi = center + (hi - center) / factor;
    let span = new_hi - new_lo;
    if span.is_finite() && span >= min_span {
        [new_lo, new_hi]
    } else {
        [lo, hi]
    }
}

/// Move `[lo, hi]` by `delta`, keeping its span and staying inside `[full_lo, full_hi]`.
pub fn slide_range([lo, hi]: [f64; 2], delta: f64, full_lo: f64, full_hi: f64) -> [f64; 2] {
    let span = hi - lo;
    let mut new_lo = lo + delta;
    let mut new_hi = hi + delta;
    if new_lo < full_lo {
        new_lo = full_lo;
        new_hi = (full_lo + span).min(full_hi);
    }
    if new_hi > full_hi {
        new_hi = full_hi;
        new_lo = (full_hi - span).max(full_lo);
    }
    [new_lo, new_hi]
}

/// Interpret mouse wheel / pinch input as `(x_zoom, y_zoom)` factors.
/// With Ctrl held the Y axis is zoomed, otherwise the X axis.
pub fn wheel_zoom(ctrl: bool, zoom_delta: f32, scroll_y: f32) -> (f64, f64) {
    const WHEEL_STEP: f64 = 1.15;
    let pinch = ((zoom_delta - 1.0).abs() > 0.001).then_some(zoom_delta as f64);
    let wheel = (scroll_y.abs() > 0.1).then(|| {
        if scroll_y > 0.0 {
            WHEEL_STEP
        } else {
            1.0 / WHEEL_STEP
        }
    });

    if ctrl {
        (1.0, pinch.or(wheel).unwrap_or(1.0))
    } else {
        (wheel.or(pinch).unwrap_or(1.0), 1.0)
    }
}

/// Whether a view with right edge `hi` is following the end of the data.
pub fn is_at_end(hi: f64, full_hi: f64) -> bool {
    hi >= full_hi - END_TOLERANCE_S
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_zoom_keeps_center_fixed() {
        let [lo, hi] = zoom_range([0.0, 10.0], 5.0, 2.0, MIN_SPAN_S);
        assert_eq!([lo, hi], [2.5, 7.5]);
    }

    #[test]
    fn test_zoom_refuses_degenerate_ranges() {
        assert_eq!(zoom_range([0.0, 0.1], 0.05, 100.0, MIN_SPAN_S), [0.0, 0.1]);
        assert_eq!(
            zoom_range([0.0, 1.0], 0.5, f64::NAN, MIN_SPAN_S),
            [0.0, 1.0]
        );
        assert_eq!(zoom_range([0.0, 1.0], 0.5, 0.0, MIN_SPAN_S), [0.0, 1.0]);
    }

    #[test]
    fn test_slide_clamps_and_preserves_span() {
        assert_eq!(slide_range([2.0, 4.0], -5.0, 0.0, 10.0), [0.0, 2.0]);
        assert_eq!(slide_range([2.0, 4.0], 20.0, 0.0, 10.0), [8.0, 10.0]);
        assert_eq!(slide_range([2.0, 4.0], 1.0, 0.0, 10.0), [3.0, 5.0]);
    }

    #[test]
    fn test_clamp_lenient_does_not_panic_on_inverted_bounds() {
        assert_eq!(clamp_lenient(5.0, 3.0, 1.0), 1.0);
        assert_eq!(clamp_lenient(5.0, 0.0, 10.0), 5.0);
    }

    #[test]
    fn test_wheel_zoom_axis_selection() {
        assert_eq!(wheel_zoom(false, 1.0, 5.0), (1.15, 1.0));
        assert_eq!(wheel_zoom(true, 1.0, -5.0), (1.0, 1.0 / 1.15));
        assert_eq!(wheel_zoom(false, 1.0, 0.0), (1.0, 1.0));
    }
}
