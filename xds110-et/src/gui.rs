//! Interactive GUI for XDS110 EnergyTrace power visualization.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender};
use eframe::egui::{self, Color32, RichText};
use egui_plot::{FilledArea, Line, Plot, PlotPoints, Span, VLine};

use crate::capture::ProbeControlCommand;
use crate::cleanup::{restart_mcu_direct, set_vcc_direct};
use crate::data_series::{PlotPoint, Unit, compute_window_stats};

pub enum GuiSource {
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
pub enum OverviewDragMode {
    #[default]
    None,
    ResizeLeft,
    ResizeRight,
    SlideBody,
}

pub struct EnergyTraceApp {
    source: GuiSource,
    points: Vec<PlotPoint>,

    // Display controls
    pub live_mode: bool,
    pub show_min_max: bool,

    // Target voltage and MCU controls
    pub target_vcc_mv: u16,
    pub status_toast: Option<(String, Instant)>,

    // Current view boundaries on the detailed top plot [lo, hi]
    pub top_x_range: Option<[f64; 2]>,
    pub top_y_range: Option<[f64; 2]>,
    pub was_at_end: bool,
    pub reset_view_requested: bool,
    pub manual_y_zoom: bool,

    // Overview fixed timeline scale (pixels per second)
    pub overview_px_per_sec: f64,

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
    pub fn new_live(
        stop_signal: Arc<AtomicBool>,
        rx: Receiver<Vec<PlotPoint>>,
        thread_handle: JoinHandle<Result<(), String>>,
        ctrl_tx: Sender<ProbeControlCommand>,
    ) -> Self {
        Self {
            source: GuiSource::Live {
                stop_signal,
                rx,
                thread_handle: Some(thread_handle),
                ctrl_tx,
            },
            points: Vec::with_capacity(300_000),
            live_mode: true,     // Live ON by default
            show_min_max: false, // Min/Max OFF by default
            target_vcc_mv: 3300, // Default 3.3V
            status_toast: None,
            top_x_range: None,
            top_y_range: None,
            was_at_end: true,
            reset_view_requested: true,
            manual_y_zoom: false,
            overview_px_per_sec: 25.0,
            overview_drag_mode: OverviewDragMode::None,
            last_overview_drag_t: None,
        }
    }

    pub fn new_file(filename: String, points: Vec<PlotPoint>) -> Self {
        Self {
            source: GuiSource::File { filename },
            points,
            live_mode: false,
            show_min_max: false, // Min/Max OFF by default
            target_vcc_mv: 3300,
            status_toast: None,
            top_x_range: None,
            top_y_range: None,
            was_at_end: false,
            reset_view_requested: true,
            manual_y_zoom: false,
            overview_px_per_sec: 25.0,
            overview_drag_mode: OverviewDragMode::None,
            last_overview_drag_t: None,
        }
    }

    /// Trigger target MCU reset via CMD 0x0E (XDS_SetSrst toggle from xds110reset.exe)
    pub fn trigger_mcu_reset(&mut self) {
        match &self.source {
            GuiSource::Live { ctrl_tx, .. } => {
                let _ = ctrl_tx.send(ProbeControlCommand::RestartMcu);
            }
            GuiSource::File { .. } => {
                restart_mcu_direct();
            }
        }
        self.status_toast = Some(("MCU Reset toggled (Cmd 0x0E: nSRST)".into(), Instant::now()));
    }

    /// Set target DCDC supply voltage via CMD 0x24 (ET_DCDC_SetVcc)
    pub fn trigger_set_vcc(&mut self, mv: u16) {
        self.target_vcc_mv = mv;
        match &self.source {
            GuiSource::Live { ctrl_tx, .. } => {
                let _ = ctrl_tx.send(ProbeControlCommand::SetVcc(mv));
            }
            GuiSource::File { .. } => {
                set_vcc_direct(mv);
            }
        }
        self.status_toast = Some((
            format!("Target VCC set to {mv} mV (Cmd 0x24)"),
            Instant::now(),
        ));
    }

    /// Clear displayed points in the GUI graph and restart visual timeline from scratch.
    /// The background CSV file logging continues recording uninterrupted.
    pub fn clear_plot(&mut self) {
        self.points.clear();
        self.top_x_range = None;
        self.top_y_range = None;
        self.was_at_end = true;
        self.reset_view_requested = true;
        self.manual_y_zoom = false;
        self.status_toast = Some((
            "Graph restarted (CSV logging continues)".into(),
            Instant::now(),
        ));
    }

    /// Process incoming data from live channel
    fn drain_live_data(&mut self) {
        if let GuiSource::Live { rx, .. } = &self.source {
            let mut new_points = false;
            while let Ok(batch) = rx.try_recv() {
                self.points.extend(batch);
                new_points = true;
            }

            if new_points && self.live_mode && self.was_at_end {
                if let Some(last) = self.points.last() {
                    let end_t = last.time_s;
                    let full_lo = self.points.first().map(|p| p.time_s).unwrap_or(0.0);

                    if let Some([lo, hi]) = self.top_x_range {
                        let span = (hi - lo).max(0.5);
                        // If user was viewing entire span, autoscale to full range
                        if span >= (hi - full_lo) - 0.5 {
                            self.top_x_range = Some([full_lo, end_t]);
                        } else {
                            // Move sliding window with newly coming data
                            self.top_x_range = Some([end_t - span, end_t]);
                        }
                    } else {
                        self.top_x_range = Some([full_lo, end_t]);
                    }
                }
            }
        }
    }
}

impl eframe::App for EnergyTraceApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        // Detect window close request early and trigger stop signal
        if ui.input(|i| i.viewport().close_requested()) {
            if let GuiSource::Live { stop_signal, .. } = &self.source {
                stop_signal.store(true, Ordering::SeqCst);
            }
        }

        self.drain_live_data();

        // Keyboard shortcuts: 'r' resets view to full range, 'c' clears graph
        if ui.input(|i| i.key_pressed(egui::Key::R)) {
            self.reset_view_requested = true;
        }
        if ui.input(|i| i.key_pressed(egui::Key::C)) {
            self.clear_plot();
        }

        // Top Control Panel
        ui.horizontal_wrapped(|ui| {
            ui.heading(
                RichText::new("⚡ XDS110 EnergyTrace")
                    .strong()
                    .color(Color32::from_rgb(235, 235, 235)),
            );
            ui.separator();

            // Live button toggle (ON by default)
            let live_text = if self.live_mode {
                RichText::new("Live: ON").color(Color32::from_rgb(0, 200, 50)).strong()
            } else {
                RichText::new("Live: OFF").color(Color32::GRAY)
            };
            if ui.button(live_text).clicked() {
                self.live_mode = !self.live_mode;
                if self.live_mode {
                    self.was_at_end = true;
                }
            }

            ui.separator();

            // Min/Max button toggle (OFF by default)
            let mm_text = if self.show_min_max {
                RichText::new("Min/Max: ON").color(Color32::from_rgb(220, 80, 80)).strong()
            } else {
                RichText::new("Min/Max: OFF").color(Color32::GRAY)
            };
            if ui.button(mm_text).clicked() {
                self.show_min_max = !self.show_min_max;
            }

            ui.separator();

            // Manual Y indicator / toggle
            if self.manual_y_zoom {
                if ui
                    .button(
                        RichText::new("Y: Manual (click to auto)")
                            .color(Color32::from_rgb(255, 180, 0))
                            .strong(),
                    )
                    .clicked()
                {
                    self.manual_y_zoom = false;
                }
                ui.separator();
            }

            if ui.button("Reset View (r)").clicked() {
                self.reset_view_requested = true;
            }

            ui.separator();

            if ui
                .button(RichText::new("🧹 Clear Plot (c)").color(Color32::from_rgb(255, 170, 70)))
                .on_hover_text("Clear current graph data and restart visual timeline from scratch.\nBackground CSV recording will continue uninterrupted.")
                .clicked()
            {
                self.clear_plot();
            }

            ui.separator();

            // Reset MCU button (next to Reset View)
            if ui
                .button(RichText::new("🔄 Reset MCU").color(Color32::from_rgb(100, 200, 255)).strong())
                .on_hover_text("Toggle target MCU nSRST pin (xds110reset.exe logic: CMD 0x0E)")
                .clicked()
            {
                self.trigger_mcu_reset();
            }

            ui.separator();

            // Status toast notification
            if let Some((msg, time)) = &self.status_toast {
                if time.elapsed() < Duration::from_secs(4) {
                    ui.label(RichText::new(format!("✔ {msg}")).color(Color32::from_rgb(255, 215, 0)).strong());
                    ui.separator();
                }
            }

            // Status info
            let total_points = self.points.len();
            let duration_s = self.points.last().map(|p| p.time_s).unwrap_or(0.0);
            match &self.source {
                GuiSource::Live { .. } => {
                    ui.label(format!(
                        "Live capture: {:.1} s ({total_points} points)",
                        duration_s
                    ));
                }
                GuiSource::File { filename } => {
                    ui.label(format!(
                        "File: {filename} ({duration_s:.1} s, {total_points} points)"
                    ));
                }
            }
        });

        ui.separator();

        if self.points.is_empty() {
            ui.centered_and_justified(|ui| {
                ui.label(RichText::new("Waiting for data stream...").italics());
            });
            if self.live_mode {
                ui.ctx()
                    .request_repaint_after(std::time::Duration::from_millis(50));
            }
            return;
        }

        let full_lo = self.points.first().map(|p| p.time_s).unwrap_or(0.0);
        let full_hi = self.points.last().map(|p| p.time_s).unwrap_or(1.0);

        if self.reset_view_requested || self.top_x_range.is_none() {
            self.top_x_range = Some([full_lo, full_hi]);
            self.top_y_range = None;
            self.was_at_end = true;
            self.manual_y_zoom = false;
            self.reset_view_requested = false;
        }

        let [cur_lo, cur_hi] = self.top_x_range.unwrap();

        // Calculate stats for current visible window
        let stats = compute_window_stats(&self.points, cur_lo, cur_hi);

        // Determine unit based on window average current:
        // >= 1 mA -> mA
        // < 1 mA and >= 1 uA -> uA
        // < 1 uA -> nA
        let avg_na = stats.map(|s| s.avg_na).unwrap_or(0.0);
        let unit = Unit::from_avg(avg_na);
        let k = unit.factor();

        // Display current window stats in a clean panel above plot
        if let Some(s) = stats {
            ui.horizontal(|ui| {
                ui.label(format!("Window: {cur_lo:.1} .. {cur_hi:.1} s |"));
                ui.label(
                    RichText::new(format!("avg: {:.4} {}", s.avg_na * k, unit.name()))
                        .color(Color32::from_rgb(80, 220, 110))
                        .strong(),
                );
                if self.show_min_max {
                    ui.label("|");
                    ui.label(
                        RichText::new(format!("min: {:.4} {}", s.min_na * k, unit.name()))
                            .color(Color32::from_rgb(90, 170, 240)),
                    );
                    ui.label(
                        RichText::new(format!("max: {:.4} {}", s.max_na * k, unit.name()))
                            .color(Color32::from_rgb(255, 100, 100)),
                    );
                }
            });
        }

        // If not manually zoomed in Y, calculate Y range (in nA) directly from visible window points
        if !self.manual_y_zoom {
            if let Some(s) = stats {
                let (y_min_val_na, y_max_val_na) = if self.show_min_max {
                    (s.min_na, s.max_na)
                } else {
                    let start_idx = self.points.partition_point(|p| p.time_s < cur_lo);
                    let end_idx = self.points.partition_point(|p| p.time_s <= cur_hi);
                    let slice = &self.points[start_idx..end_idx];
                    let mut mn = f64::INFINITY;
                    let mut mx = f64::NEG_INFINITY;
                    for p in slice {
                        let v = p.avg_na;
                        if v < mn {
                            mn = v;
                        }
                        if v > mx {
                            mx = v;
                        }
                    }
                    if mn.is_finite() && mx.is_finite() {
                        (mn, mx)
                    } else {
                        (s.min_na, s.max_na)
                    }
                };

                let span_na = (y_max_val_na - y_min_val_na).abs();
                let margin_na = if span_na > 1.0 { span_na * 0.08 } else { 500.0 };
                self.top_y_range = Some([y_min_val_na - margin_na, y_max_val_na + margin_na]);
            }
        }

        let [cur_y_lo_na, cur_y_hi_na] = self.top_y_range.unwrap_or([-1_000_000.0, 1_000_000.0]);

        // Available height for top plot: total minus bottom overview graph (~130px) and margins
        let available_h = (ui.available_height() - 135.0).max(180.0);

        let ctrl_held = ui.input(|i| i.modifiers.ctrl);

        // --- 1. Top Detailed Graph ---
        let top_plot = Plot::new("top_detailed_plot")
            .height(available_h)
            .x_axis_label("time [s]")
            .y_axis_label(format!("current [{}]", unit.name()))
            .legend(egui_plot::Legend::default().position(egui_plot::Corner::RightTop))
            .show_grid(true)
            .allow_scroll(false) // Handle scroll zoom explicitly to differentiate X vs Y (with Ctrl)
            .allow_zoom(false) // Custom mathematical zoom to prevent 1-frame delayed state reversion
            .allow_drag(false) // Custom drag to synchronize with sliding window
            .auto_bounds([false, false]);

        let mut user_changed_x = None;
        let mut user_changed_y = None;

        // 2. Override extreme_bg_color for this plot's canvas
        ui.scope(|ui| {
            // Dark background for the graph area (adjust RGB as dark as you like, e.g. 15, 15, 15)
            ui.visuals_mut().extreme_bg_color = Color32::from_rgb(20, 20, 20);

            top_plot.show(ui, |plot_ui| {
                // Apply X window range
                plot_ui.set_plot_bounds_x(cur_lo..=cur_hi);
                // Apply Y bounds scaled to current unit
                plot_ui.set_plot_bounds_y((cur_y_lo_na * k)..=(cur_y_hi_na * k));

                let resp = plot_ui.response();
                let pointer_coord = plot_ui.pointer_coordinate();

                // Detect mouse wheel / zoom input
                // Note: egui routes Ctrl+wheel into i.zoom_delta(), and normal wheel into i.smooth_scroll_delta.y
                let (zoom_x, zoom_y) = plot_ui.ctx().input(|i| {
                    let ctrl = i.modifiers.ctrl;
                    let z = i.zoom_delta();
                    let scroll_y = i.smooth_scroll_delta.y;

                    if ctrl {
                        // When Ctrl is held, zoom Y axis
                        if (z - 1.0).abs() > 0.001 {
                            (1.0, z as f64)
                        } else if scroll_y.abs() > 0.1 {
                            let factor = if scroll_y > 0.0 { 1.15 } else { 1.0 / 1.15 };
                            (1.0, factor)
                        } else {
                            (1.0, 1.0)
                        }
                    } else {
                        // Normal wheel (no Ctrl), zoom X axis
                        if scroll_y.abs() > 0.1 {
                            let factor = if scroll_y > 0.0 { 1.15 } else { 1.0 / 1.15 };
                            (factor, 1.0)
                        } else if (z - 1.0).abs() > 0.001 {
                            (z as f64, 1.0)
                        } else {
                            (1.0, 1.0)
                        }
                    }
                });

                if resp.hovered() && (zoom_x - 1.0).abs() > 0.001 {
                    let cx = pointer_coord
                        .map(|c| c.x)
                        .unwrap_or((cur_lo + cur_hi) / 2.0);
                    let new_x_lo = cx - (cx - cur_lo) / zoom_x;
                    let new_x_hi = cx + (cur_hi - cx) / zoom_x;
                    user_changed_x = Some([new_x_lo, new_x_hi]);
                }

                if resp.hovered() && (zoom_y - 1.0).abs() > 0.001 {
                    let cy_unit = pointer_coord
                        .map(|c| c.y)
                        .unwrap_or(((cur_y_lo_na + cur_y_hi_na) / 2.0) * k);
                    let cy_na = cy_unit / k;
                    let new_y_lo_na = cy_na - (cy_na - cur_y_lo_na) / zoom_y;
                    let new_y_hi_na = cy_na + (cur_y_hi_na - cy_na) / zoom_y;
                    user_changed_y = Some([new_y_lo_na, new_y_hi_na]);
                } else if resp.dragged() {
                    let delta = resp.drag_delta();
                    let dpos = plot_ui.transform().dvalue_dpos();
                    let dx = -(delta.x as f64) * dpos[0].abs();
                    let dy_unit = (delta.y as f64) * dpos[1].abs();
                    let dy_na = dy_unit / k;

                    user_changed_x = Some([cur_lo + dx, cur_hi + dx]);
                    if ctrl_held {
                        user_changed_y = Some([cur_y_lo_na + dy_na, cur_y_hi_na + dy_na]);
                    }
                }

                // Plot average line (green)
                let avg_pts: PlotPoints = self
                    .points
                    .iter()
                    .map(|p| [p.time_s, p.avg_na * k])
                    .collect();
                plot_ui.line(
                    Line::new("avg", avg_pts)
                        .color(Color32::from_rgb(80, 220, 110))
                        .width(1.5),
                );

                // Min and Max traces (if enabled)
                if self.show_min_max {
                    let mut xs = Vec::with_capacity(self.points.len());
                    let mut ys_min = Vec::with_capacity(self.points.len());
                    let mut ys_max = Vec::with_capacity(self.points.len());
                    let mut min_pts = Vec::with_capacity(self.points.len());
                    let mut max_pts = Vec::with_capacity(self.points.len());

                    for p in &self.points {
                        let t = p.time_s;
                        let mn = p.min_na * k;
                        let mx = p.max_na * k;
                        xs.push(t);
                        ys_min.push(mn);
                        ys_max.push(mx);
                        min_pts.push([t, mn]);
                        max_pts.push([t, mx]);
                    }

                    // Shaded confidence/envelope band between min and max lines
                    plot_ui.add(
                        FilledArea::new("min..max", &xs, &ys_min, &ys_max)
                            .fill_color(Color32::from_rgba_unmultiplied(90, 170, 240, 35)),
                    );

                    plot_ui.line(
                        Line::new("min", PlotPoints::from(min_pts))
                            .color(Color32::from_rgb(90, 170, 240))
                            .width(1.0),
                    );

                    plot_ui.line(
                        Line::new("max", PlotPoints::from(max_pts))
                            .color(Color32::from_rgb(255, 100, 100))
                            .width(1.0),
                    );
                }
            });

            // Store user-modified Y bounds and mark manual zoom active
            if let Some(new_y_bounds) = user_changed_y {
                self.top_y_range = Some(new_y_bounds);
                self.manual_y_zoom = true;
            }

            // If user changed top plot X via mouse drag/zoom, update range safely
            if let Some([new_lo, new_hi]) = user_changed_x {
                self.top_x_range = Some([new_lo, new_hi]);
                self.was_at_end = new_hi >= full_hi - 0.2;
            }
        });
        ui.add_space(4.0);

        // --- 2. Bottom Overview Graph with Fixed Scale & Horizontal ScrollArea ---
        ui.horizontal(|ui| {
            ui.label(RichText::new("Timeline Overview:").strong());
            ui.label(
                RichText::new("(drag edges to resize, drag middle to slide)")
                    .weak()
                    .small(),
            );
            ui.separator();

            ui.label("Scale:");
            if ui
                .button(" - ")
                .on_hover_text("Zoom out timeline scale")
                .clicked()
            {
                self.overview_px_per_sec = (self.overview_px_per_sec / 1.3).max(2.0);
            }
            ui.label(format!("{:.0} px/s", self.overview_px_per_sec));
            if ui
                .button(" + ")
                .on_hover_text("Zoom in timeline scale")
                .clicked()
            {
                self.overview_px_per_sec = (self.overview_px_per_sec * 1.3).min(200.0);
            }
            if ui
                .button("Fit All")
                .on_hover_text("Fit entire timeline to view")
                .clicked()
            {
                let total_t = (full_hi - full_lo).max(1.0);
                self.overview_px_per_sec =
                    (ui.available_width() as f64 / total_t).clamp(2.0, 200.0);
            }
        });

        let available_w = ui.available_width();
        let total_duration = (full_hi - full_lo).max(1.0);
        let content_w = (total_duration * self.overview_px_per_sec).max(available_w as f64) as f32;

        let [mut cur_lo, mut cur_hi] = self.top_x_range.unwrap();

        egui::ScrollArea::horizontal()
            .id_salt("timeline_scroll_area")
            .stick_to_right(self.was_at_end)
            .auto_shrink([false, false])
            .show(ui, |ui| {
                ui.set_width(content_w);

                let bottom_plot = Plot::new("bottom_overview_plot")
                    .width(content_w)
                    .height(85.0)
                    .show_axes([true, false])
                    .show_grid([true, false])
                    .allow_zoom(false)
                    .allow_scroll(false)
                    .allow_drag(false)
                    .auto_bounds([false, true]);

                ui.scope(|ui| {
                    ui.visuals_mut().extreme_bg_color = Color32::from_rgb(20, 20, 20);
                    bottom_plot.show(ui, |plot_ui| {
                        // Keep fixed timeline range on overview X-axis
                        plot_ui.set_plot_bounds_x(full_lo..=full_hi.max(full_lo + 1.0));

                        let overview_peak = self
                            .points
                            .iter()
                            .fold(0.0f64, |acc, p| acc.max(p.max_na.abs()).max(p.min_na.abs()));
                        let ok = Unit::from_peak(overview_peak).factor();

                        // Overview line across all points (downsampled for smooth 60 fps)
                        let step = (self.points.len() / 3000).max(1);
                        let overview_pts: PlotPoints = self
                            .points
                            .iter()
                            .step_by(step)
                            .map(|p| [p.time_s, p.avg_na * ok])
                            .collect();
                        plot_ui.line(
                            Line::new("overview", overview_pts)
                                .color(Color32::from_rgb(120, 200, 130))
                                .width(1.0),
                        );

                        // Shaded sliding window indicator using egui_plot::Span
                        let win_lo = cur_lo.clamp(full_lo, full_hi);
                        let win_hi = cur_hi.clamp(full_lo, full_hi).max(win_lo + 0.05);

                        plot_ui.span(
                            Span::new("window_selector", win_lo..=win_hi)
                                .fill(Color32::from_rgba_unmultiplied(255, 215, 0, 75))
                                .border(egui::Stroke::new(2.0, Color32::from_rgb(255, 215, 0))),
                        );

                        // Left and right draggable handle lines
                        plot_ui.vline(
                            VLine::new("handle_left", win_lo)
                                .stroke(egui::Stroke::new(3.5, Color32::from_rgb(255, 230, 80))),
                        );
                        plot_ui.vline(
                            VLine::new("handle_right", win_hi)
                                .stroke(egui::Stroke::new(3.5, Color32::from_rgb(255, 230, 80))),
                        );

                        let resp = plot_ui.response();
                        let pointer_coord = plot_ui.pointer_coordinate();
                        let dpos_x = plot_ui.transform().dvalue_dpos()[0].abs();
                        // Hit tolerance: 14 screen pixels converted to time units
                        let hit_tol = (14.0 * dpos_x).max(0.05);

                        let is_mouse_down = plot_ui.ctx().input(|i| i.pointer.primary_down());
                        let is_mouse_pressed = plot_ui.ctx().input(|i| i.pointer.primary_pressed());
                        let is_mouse_released =
                            plot_ui.ctx().input(|i| i.pointer.primary_released());

                        if !is_mouse_down {
                            self.overview_drag_mode = OverviewDragMode::None;
                            self.last_overview_drag_t = None;
                            if resp.hovered() {
                                if let Some(coord) = pointer_coord {
                                    if (coord.x - win_lo).abs() <= hit_tol
                                        || (coord.x - win_hi).abs() <= hit_tol
                                    {
                                        plot_ui
                                            .ctx()
                                            .set_cursor_icon(egui::CursorIcon::ResizeHorizontal);
                                    } else if coord.x > win_lo && coord.x < win_hi {
                                        plot_ui.ctx().set_cursor_icon(egui::CursorIcon::Grab);
                                    }
                                }
                            }
                        } else if is_mouse_pressed {
                            if let Some(coord) = pointer_coord {
                                let click_t = coord.x;
                                if (click_t - win_lo).abs() <= hit_tol {
                                    self.overview_drag_mode = OverviewDragMode::ResizeLeft;
                                    plot_ui
                                        .ctx()
                                        .set_cursor_icon(egui::CursorIcon::ResizeHorizontal);
                                } else if (click_t - win_hi).abs() <= hit_tol {
                                    self.overview_drag_mode = OverviewDragMode::ResizeRight;
                                    plot_ui
                                        .ctx()
                                        .set_cursor_icon(egui::CursorIcon::ResizeHorizontal);
                                } else if click_t >= win_lo && click_t <= win_hi {
                                    self.overview_drag_mode = OverviewDragMode::SlideBody;
                                    plot_ui.ctx().set_cursor_icon(egui::CursorIcon::Grabbing);
                                } else {
                                    // Clicked outside: jump-center the window around click_t
                                    let span = (cur_hi - cur_lo).max(0.5);
                                    let mut new_lo = click_t - span / 2.0;
                                    let mut new_hi = click_t + span / 2.0;

                                    if new_lo < full_lo {
                                        new_lo = full_lo;
                                        new_hi = (full_lo + span).min(full_hi);
                                    }
                                    if new_hi > full_hi {
                                        new_hi = full_hi;
                                        new_lo = (full_hi - span).max(full_lo);
                                    }

                                    cur_lo = new_lo;
                                    cur_hi = new_hi;
                                    self.top_x_range = Some([cur_lo, cur_hi]);
                                    self.was_at_end = cur_hi >= full_hi - 0.2;
                                    self.overview_drag_mode = OverviewDragMode::SlideBody;
                                    plot_ui.ctx().set_cursor_icon(egui::CursorIcon::Grabbing);
                                }
                                self.last_overview_drag_t = Some(click_t);
                            }
                        } else if is_mouse_down {
                            if let (Some(coord), Some(last_t)) =
                                (pointer_coord, self.last_overview_drag_t)
                            {
                                let delta_t = coord.x - last_t;

                                match self.overview_drag_mode {
                                    OverviewDragMode::ResizeLeft => {
                                        plot_ui
                                            .ctx()
                                            .set_cursor_icon(egui::CursorIcon::ResizeHorizontal);
                                        let min_span = 0.1;
                                        let new_lo =
                                            (cur_lo + delta_t).clamp(full_lo, cur_hi - min_span);
                                        cur_lo = new_lo;
                                        self.top_x_range = Some([cur_lo, cur_hi]);
                                        self.last_overview_drag_t = Some(coord.x);
                                    }
                                    OverviewDragMode::ResizeRight => {
                                        plot_ui
                                            .ctx()
                                            .set_cursor_icon(egui::CursorIcon::ResizeHorizontal);
                                        let min_span = 0.1;
                                        let new_hi =
                                            (cur_hi + delta_t).clamp(cur_lo + min_span, full_hi);
                                        cur_hi = new_hi;
                                        self.top_x_range = Some([cur_lo, cur_hi]);
                                        self.was_at_end = cur_hi >= full_hi - 0.2;
                                        self.last_overview_drag_t = Some(coord.x);
                                    }
                                    OverviewDragMode::SlideBody => {
                                        plot_ui.ctx().set_cursor_icon(egui::CursorIcon::Grabbing);
                                        let span = cur_hi - cur_lo;
                                        let mut new_lo = cur_lo + delta_t;
                                        let mut new_hi = cur_hi + delta_t;

                                        if new_lo < full_lo {
                                            new_lo = full_lo;
                                            new_hi = (full_lo + span).min(full_hi);
                                        }
                                        if new_hi > full_hi {
                                            new_hi = full_hi;
                                            new_lo = (full_hi - span).max(full_lo);
                                        }

                                        cur_lo = new_lo;
                                        cur_hi = new_hi;
                                        self.top_x_range = Some([cur_lo, cur_hi]);
                                        self.was_at_end = cur_hi >= full_hi - 0.2;
                                        self.last_overview_drag_t = Some(coord.x);
                                    }
                                    OverviewDragMode::None => {}
                                }
                            }
                        }

                        if is_mouse_released {
                            self.overview_drag_mode = OverviewDragMode::None;
                            self.last_overview_drag_t = None;
                        }
                    });
                });
            });

        // Request repaint in Live mode to maintain smooth rendering
        if self.live_mode {
            ui.ctx()
                .request_repaint_after(std::time::Duration::from_millis(30));
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
