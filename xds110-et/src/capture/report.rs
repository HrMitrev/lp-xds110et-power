//! Terminal progress and summary output for headless capture.

use std::io::Write;
use std::time::Duration;

use super::CaptureStats;
use crate::data_series::fmt_current;

/// Print the single-line live status (overwrites itself).
pub fn print_status(
    elapsed: Duration,
    stats: &CaptureStats,
    rows_written: u64,
    offset: f64,
    slope: f64,
) {
    if stats.total_samples == 0 {
        return;
    }
    let cur = |adc: f64| (adc - offset) * slope;
    eprint!(
        "\r{:9.1} s  {:>13} samples  {:>8} rows  rc={}  min {:>10}  max {:>10}  avg {:>10}   ",
        elapsed.as_secs_f64(),
        stats.total_samples,
        rows_written,
        stats.reconnects,
        fmt_current(cur(stats.adc_min as f64)),
        fmt_current(cur(stats.adc_max as f64)),
        fmt_current(cur(stats.adc_avg())),
    );
    let _ = std::io::stderr().flush();
}

/// Print the end-of-capture summary.
pub fn print_summary(
    elapsed: Duration,
    stats: &CaptureStats,
    rows_written: u64,
    offset: f64,
    slope: f64,
) {
    eprintln!();
    eprintln!(
        "Capture finished: {} samples, {rows_written} rows written, {} frames lost, {} reconnects",
        stats.total_samples, stats.frames_lost, stats.reconnects
    );
    eprintln!("Elapsed time: {:.2} s", elapsed.as_secs_f64());

    if stats.total_samples > 0 {
        let cur = |adc: f64| (adc - offset) * slope;
        eprintln!(
            "Current: min {}, max {}, avg {}",
            fmt_current(cur(stats.adc_min as f64)),
            fmt_current(cur(stats.adc_max as f64)),
            fmt_current(cur(stats.adc_avg())),
        );
    }
}
