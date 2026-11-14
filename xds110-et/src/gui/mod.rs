//! Interactive GUI for XDS110 EnergyTrace power visualization.

mod overview;
mod toolbar;
mod top_plot;
mod view;

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender};
use eframe::egui::{self, Color32, RichText};

use crate::capture::ProbeControlCommand;
use crate::cleanup::restart_mcu_direct;
use crate::data_series::{PlotPoint, Unit, avg_extent, compute_window_stats, visible_slice};

const PLOT_BACKGROUND: Color32 = Color32::from_rgb(20, 20, 20);
const TOAST_DURATION: Duration = Duration::from_secs(4);

enum GuiSource {
    Live {
        stop_signal: Arc<AtomicBool>,
        rx: Receiver<Vec<PlotPoint>>,
        thread_handle: Option<JoinHandle<Result<(), String>>>,
        ctrl_tx: Sender<ProbeControlCommand>,
    },
    File {
        filename: String,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum OverviewDragMode {
    #[default]
    None,
    ResizeLeft,
    ResizeRight,
    SlideBody,
}

/// Values derived once per frame and shared by the plot sections.
struct FrameView {
    full_lo: f64,
    full_hi: f64,
    unit: Unit,
}

pub struct EnergyTraceApp {
    source: GuiSource,
    points: Vec<PlotPoint>,

    // Display controls
    live_mode: bool,
    show_min_max: bool,
    status_toast: Option<(String, Instant)>,
    /// Set when the background capture thread ended with an error
    capture_error: Option<String>,

    // Current view boundaries on the detailed top plot [lo, hi]
    top_x_range: Option<[f64; 2]>,
    top_y_range: Option<[f64; 2]>,
    was_at_end: bool,
    reset_view_requested: bool,
    manual_y_zoom: bool,

    // Overview fixed timeline scale (pixels per second)
    overview_px_per_sec: f64,

    // State tracking for dragging/resizing the overview window
    overview_drag_mode: OverviewDragMode,
    last_overview_drag_t: Option<f64>,
}

impl Drop for EnergyTraceApp {
    fn drop(&mut self) {
        if let GuiSource::Live {
            stop_signal,
            thread_handle,
            ..
        } = &mut self.source
        {
            stop_signal.store(true, Ordering::SeqCst);
            if let Some(handle) = thread_handle.take() {
                let _ = handle.join();
            }
        }
    }
}

impl EnergyTraceApp {
    fn new(source: GuiSource, points: Vec<PlotPoint>, live: bool) -> Self {
        Self {
            source,
            points,
            live_mode: live,
            show_min_max: false,
            status_toast: None,
            capture_error: None,
            top_x_range: None,
            top_y_range: None,
            was_at_end: live,
            reset_view_requested: true,
            manual_y_zoom: false,
            overview_px_per_sec: 25.0,
            overview_drag_mode: OverviewDragMode::None,
            last_overview_drag_t: None,
        }
    }

    pub fn new_live(
        stop_signal: Arc<AtomicBool>,
        rx: Receiver<Vec<PlotPoint>>,
        thread_handle: JoinHandle<Result<(), String>>,
        ctrl_tx: Sender<ProbeControlCommand>,
    ) -> Self {
        let source = GuiSource::Live {
            stop_signal,
            rx,
            thread_handle: Some(thread_handle),
            ctrl_tx,
        };
        Self::new(source, Vec::with_capacity(300_000), true)
    }

    pub fn new_file(filename: String, points: Vec<PlotPoint>) -> Self {
        Self::new(GuiSource::File { filename }, points, false)
    }

    fn toast(&mut self, msg: impl Into<String>) {
        self.status_toast = Some((msg.into(), Instant::now()));
    }

    /// Trigger target MCU reset via CMD 0x0E (XDS_SetSrst toggle from xds110reset.exe)
    fn trigger_mcu_reset(&mut self) {
        match &self.source {
            GuiSource::Live { ctrl_tx, .. } => {
                let _ = ctrl_tx.send(ProbeControlCommand::RestartMcu);
            }
            GuiSource::File { .. } => restart_mcu_direct(),
        }
        self.toast("MCU Reset toggled (Cmd 0x0E: nSRST)");
    }

    /// Clear displayed points in the GUI graph and restart visual timeline from scratch.
    /// The background CSV file logging continues recording uninterrupted.
    fn clear_plot(&mut self) {
        self.points.clear();
        self.top_x_range = None;
        self.top_y_range = None;
        self.was_at_end = true;
        self.reset_view_requested = true;
        self.manual_y_zoom = false;
        self.toast("Graph restarted (CSV logging continues)");
    }

    /// Process incoming data from the live channel and detect a dead capture thread.
    fn drain_live_data(&mut self) {
        let GuiSource::Live {
            rx, thread_handle, ..
        } = &mut self.source
        else {
            return;
        };

        let mut new_points = false;
        while let Ok(batch) = rx.try_recv() {
            self.points.extend(batch);
            new_points = true;
        }

        if thread_handle.as_ref().is_some_and(|h| h.is_finished())
            && let Some(handle) = thread_handle.take()
        {
            self.capture_error = match handle.join() {
                Ok(Ok(())) => Some("Capture stopped".to_string()),
                Ok(Err(e)) => Some(e),
                Err(_) => Some("Capture thread panicked".to_string()),
            };
        }

        if !(new_points && self.live_mode && self.was_at_end) {
            return;
        }
        let (Some(first), Some(last)) = (self.points.first(), self.points.last()) else {
            return;
        };
        let (full_lo, end_t) = (first.time_s, last.time_s);

        self.top_x_range = Some(match self.top_x_range {
            Some([lo, hi]) => {
                let span = (hi - lo).max(view::MIN_SPAN_S);
                // If the user was viewing the entire span, keep autoscaling to the full range
                if span >= (hi - full_lo) - 0.5 {
                    [full_lo, end_t]
                } else {
                    // Slide the window along with newly arriving data
                    [end_t - span, end_t]
                }
            }
            None => [full_lo, end_t],
        });
    }

    /// Compute the Y range (nA) that fits the points in the visible window.
    fn auto_y_range(&self, lo: f64, hi: f64, min_na: f64, max_na: f64) -> [f64; 2] {
        let (y_lo, y_hi) = if self.show_min_max {
            (min_na, max_na)
        } else {
            avg_extent(visible_slice(&self.points, lo, hi)).unwrap_or((min_na, max_na))
        };
        let span = (y_hi - y_lo).abs();
        let margin = if span > 1.0 { span * 0.08 } else { 500.0 };
        [y_lo - margin, y_hi + margin]
    }

    fn show_waiting_screen(&self, ui: &mut egui::Ui) {
        ui.centered_and_justified(|ui| match &self.capture_error {
            Some(err) => ui.label(
                RichText::new(format!("Capture failed: {err}"))
                    .color(Color32::from_rgb(255, 100, 100))
                    .strong(),
            ),
            None => ui.label(RichText::new("Waiting for data stream...").italics()),
        });
    }
}

impl eframe::App for EnergyTraceApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        // Detect window close request early and trigger stop signal
        if ui.input(|i| i.viewport().close_requested())
            && let GuiSource::Live { stop_signal, .. } = &self.source
        {
            stop_signal.store(true, Ordering::SeqCst);
        }

        self.drain_live_data();

        // Keyboard shortcuts: 'r' resets view to full range, 'c' clears graph
        if ui.input(|i| i.key_pressed(egui::Key::R)) {
            self.reset_view_requested = true;
        }
        if ui.input(|i| i.key_pressed(egui::Key::C)) {
            self.clear_plot();
        }

        self.show_toolbar(ui);
        ui.separator();

        let (Some(first), Some(last)) = (self.points.first(), self.points.last()) else {
            self.show_waiting_screen(ui);
            if self.live_mode {
                ui.ctx().request_repaint_after(Duration::from_millis(50));
            }
            return;
        };
        let (full_lo, full_hi) = (first.time_s, last.time_s);

        if self.reset_view_requested || self.top_x_range.is_none() {
            self.top_x_range = Some([full_lo, full_hi]);
            self.top_y_range = None;
            self.was_at_end = true;
            self.manual_y_zoom = false;
            self.reset_view_requested = false;
        }

        let [cur_lo, cur_hi] = self.top_x_range.unwrap_or([full_lo, full_hi]);
        let stats = compute_window_stats(&self.points, cur_lo, cur_hi);

        // Unit follows the window average: >= 1 mA -> mA, >= 1 uA -> uA, else nA
        let unit = Unit::from_magnitude(stats.map_or(0.0, |s| s.avg_na));
        let frame = FrameView {
            full_lo,
            full_hi,
            unit,
        };

        if let Some(s) = stats {
            self.show_window_stats(ui, cur_lo, cur_hi, &s, unit);
            if !self.manual_y_zoom {
                self.top_y_range = Some(self.auto_y_range(cur_lo, cur_hi, s.min_na, s.max_na));
            }
        }

        self.show_top_plot(ui, &frame);
        ui.add_space(4.0);
        self.show_overview(ui, &frame);

        // Request repaint in Live mode to maintain smooth rendering
        if self.live_mode {
            ui.ctx().request_repaint_after(Duration::from_millis(30));
        }
    }
}

/// Launch the eframe application window
pub fn launch_gui(app: EnergyTraceApp) -> Result<(), Box<dyn std::error::Error>> {
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("XDS110 EnergyTrace Power Monitor")
            .with_inner_size([1100.0, 750.0])
            .with_min_inner_size([650.0, 450.0]),
        ..Default::default()
    };

    eframe::run_native(
        "XDS110 EnergyTrace",
        native_options,
        Box::new(|cc| {
            cc.egui_ctx.set_theme(egui::ThemePreference::Dark);
            Ok(Box::new(app))
        }),
    )
    .map_err(|e| format!("GUI launch failed: {e}").into())
}
