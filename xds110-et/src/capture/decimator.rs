//! Decimates the raw sample stream into low-rate points for live plotting.

use crate::config::SAMPLE_RATE_HZ;
use crate::data_series::PlotPoint;

/// Number of hardware samples per live GUI plot point (20 ms @ 256 kS/s = 50 Hz refresh)
const SAMPLES_PER_GUI_POINT: u64 = 5120;

pub struct GuiDecimator {
    start_sample: u64,
    n: u64,
    sum: i64,
    min: i32,
    max: i32,
}

impl GuiDecimator {
    /// `start_sample` is the global index of the first sample that will be pushed.
    pub fn new(start_sample: u64) -> Self {
        Self {
            start_sample,
            n: 0,
            sum: 0,
            min: i32::MAX,
            max: i32::MIN,
        }
    }

    /// Accumulate one ADC sample; returns a finished point once enough samples are collected.
    pub fn push(&mut self, adc: i32, offset: f64, slope: f64) -> Option<PlotPoint> {
        self.n += 1;
        self.sum += adc as i64;
        self.min = self.min.min(adc);
        self.max = self.max.max(adc);

        if self.n < SAMPLES_PER_GUI_POINT {
            return None;
        }

        let cur = |adc: f64| (adc - offset) * slope;
        let point = PlotPoint {
            time_s: self.start_sample as f64 / SAMPLE_RATE_HZ as f64,
            avg_na: cur(self.sum as f64 / self.n as f64),
            min_na: cur(self.min as f64),
            max_na: cur(self.max as f64),
        };

        *self = Self::new(self.start_sample + self.n);
        Some(point)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_emits_one_point_per_window_and_advances_time() {
        let mut d = GuiDecimator::new(SAMPLE_RATE_HZ as u64);
        let mut points = Vec::new();
        for i in 0..(2 * SAMPLES_PER_GUI_POINT) {
            let adc = if i % SAMPLES_PER_GUI_POINT == 0 {
                110
            } else {
                100
            };
            points.extend(d.push(adc, 100.0, 10.0));
        }
        assert_eq!(points.len(), 2);
        assert_eq!(points[0].time_s, 1.0);
        assert_eq!(points[0].min_na, 0.0);
        assert_eq!(points[0].max_na, 100.0);
        assert_eq!(
            points[1].time_s,
            1.0 + SAMPLES_PER_GUI_POINT as f64 / SAMPLE_RATE_HZ as f64
        );
    }
}
