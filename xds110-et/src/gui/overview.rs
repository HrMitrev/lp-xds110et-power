//! Timeline overview plot with a draggable window selector.

use eframe::egui::{self, Color32, CursorIcon, RichText, Stroke};
use egui_plot::{Line, Plot, PlotPoints, PlotUi, Span, VLine};

use super::view::{self, clamp_lenient};
use super::{EnergyTraceApp, FrameView, OverviewDragMode, PLOT_BACKGROUND};
use crate::data_series::Unit;

const MIN_PX_PER_SEC: f64 = 2.0;
const MAX_PX_PER_SEC: f64 = 200.0;
const SCALE_STEP: f64 = 1.3;
/// Smallest window (seconds) the overview handles can shrink the detail view to.
const MIN_WINDOW_S: f64 = 0.1;
/// Pixel distance within which a window edge can be grabbed.
const HANDLE_HIT_PX: f64 = 14.0;
/// Maximum number of points drawn in the overview line.
const OVERVIEW_MAX_POINTS: usize = 3000;
const HANDLE_COLOR: Color32 = Color32::from_rgb(255, 230, 80);
const WINDOW_COLOR: Color32 = Color32::from_rgb(255, 215, 0);

impl EnergyTraceApp {
    pub(super) fn show_overview(&mut self, ui: &mut egui::Ui, frame: &FrameView) {
        self.show_overview_controls(ui, frame);

        let available_w = ui.available_width();
        let total_duration = (frame.full_hi - frame.full_lo).max(1.0);
        let content_w = (total_duration * self.overview_px_per_sec).max(available_w as f64) as f32;

        egui::ScrollArea::horizontal()
            .id_salt("timeline_scroll_area")
            .stick_to_right(self.was_at_end)
            .auto_shrink([false, false])
            .show(ui, |ui| {
                ui.set_width(content_w);

                let plot = Plot::new("bottom_overview_plot")
                    .width(content_w)
                    .height(85.0)
                    .show_axes([true, false])
                    .show_grid([true, false])
                    .allow_zoom(false)
                    .allow_scroll(false)
                    .allow_drag(false)
                    .auto_bounds([false, true]);

                ui.scope(|ui| {
                    ui.visuals_mut().extreme_bg_color = PLOT_BACKGROUND;
                    plot.show(ui, |plot_ui| self.draw_overview(plot_ui, frame));
                });
            });
    }

    fn show_overview_controls(&mut self, ui: &mut egui::Ui, frame: &FrameView) {
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
                self.overview_px_per_sec =
                    (self.overview_px_per_sec / SCALE_STEP).max(MIN_PX_PER_SEC);
            }
            ui.label(format!("{:.0} px/s", self.overview_px_per_sec));
            if ui
                .button(" + ")
                .on_hover_text("Zoom in timeline scale")
                .clicked()
            {
                self.overview_px_per_sec =
                    (self.overview_px_per_sec * SCALE_STEP).min(MAX_PX_PER_SEC);
            }
            if ui
                .button("Fit All")
                .on_hover_text("Fit entire timeline to view")
                .clicked()
            {
                let total_t = (frame.full_hi - frame.full_lo).max(1.0);
                self.overview_px_per_sec =
                    (ui.available_width() as f64 / total_t).clamp(MIN_PX_PER_SEC, MAX_PX_PER_SEC);
            }
        });
    }

    fn draw_overview(&mut self, plot_ui: &mut PlotUi, frame: &FrameView) {
        let (full_lo, full_hi) = (frame.full_lo, frame.full_hi);
        let [cur_lo, cur_hi] = self.top_x_range.unwrap_or([full_lo, full_hi]);

        // Keep fixed timeline range on overview X-axis
        plot_ui.set_plot_bounds_x(full_lo..=full_hi.max(full_lo + 1.0));

        let peak = self
            .points
            .iter()
            .fold(0.0f64, |acc, p| acc.max(p.max_na.abs()).max(p.min_na.abs()));
        let ok = Unit::from_magnitude(peak).factor();

        // Downsample for a smooth 60 fps
        let step = (self.points.len() / OVERVIEW_MAX_POINTS).max(1);
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

        // Shaded window indicator with draggable handle lines
        let win_lo = clamp_lenient(cur_lo, full_lo, full_hi);
        let win_hi = clamp_lenient(cur_hi, full_lo, full_hi).max(win_lo + 0.05);

        plot_ui.span(
            Span::new("window_selector", win_lo..=win_hi)
                .fill(Color32::from_rgba_unmultiplied(255, 215, 0, 75))
                .border(Stroke::new(2.0, WINDOW_COLOR)),
        );
        for (name, x) in [("handle_left", win_lo), ("handle_right", win_hi)] {
            plot_ui.vline(VLine::new(name, x).stroke(Stroke::new(3.5, HANDLE_COLOR)));
        }

        self.handle_overview_pointer(plot_ui, frame, [win_lo, win_hi]);
    }

    /// Mouse interaction: grab an edge to resize, the body to slide, or click outside to jump.
    fn handle_overview_pointer(&mut self, plot_ui: &mut PlotUi, frame: &FrameView, win: [f64; 2]) {
        let (full_lo, full_hi) = (frame.full_lo, frame.full_hi);
        let [win_lo, win_hi] = win;
        let [cur_lo, cur_hi] = self.top_x_range.unwrap_or(win);

        let resp = plot_ui.response();
        let pointer = plot_ui.pointer_coordinate();
        let hit_tol = (HANDLE_HIT_PX * plot_ui.transform().dvalue_dpos()[0].abs()).max(0.05);

        let (down, pressed, released) = plot_ui.ctx().input(|i| {
            (
                i.pointer.primary_down(),
                i.pointer.primary_pressed(),
                i.pointer.primary_released(),
            )
        });
        let set_cursor = |icon| plot_ui.ctx().set_cursor_icon(icon);
        let near_edge = |x: f64| (x - win_lo).abs() <= hit_tol || (x - win_hi).abs() <= hit_tol;

        if !down {
            self.overview_drag_mode = OverviewDragMode::None;
            self.last_overview_drag_t = None;
            if let (true, Some(c)) = (resp.hovered(), pointer) {
                if near_edge(c.x) {
                    set_cursor(CursorIcon::ResizeHorizontal);
                } else if c.x > win_lo && c.x < win_hi {
                    set_cursor(CursorIcon::Grab);
                }
            }
        } else if pressed {
            let Some(coord) = pointer else { return };
            let t = coord.x;
            if (t - win_lo).abs() <= hit_tol {
                self.overview_drag_mode = OverviewDragMode::ResizeLeft;
                set_cursor(CursorIcon::ResizeHorizontal);
            } else if (t - win_hi).abs() <= hit_tol {
                self.overview_drag_mode = OverviewDragMode::ResizeRight;
                set_cursor(CursorIcon::ResizeHorizontal);
            } else if t >= win_lo && t <= win_hi {
                self.overview_drag_mode = OverviewDragMode::SlideBody;
                set_cursor(CursorIcon::Grabbing);
            } else {
                // Clicked outside: jump-center the window around the click
                let span = (cur_hi - cur_lo).max(0.5);
                let [lo, hi] =
                    view::slide_range([t - span / 2.0, t + span / 2.0], 0.0, full_lo, full_hi);
                self.set_top_x_range(lo, hi);
                self.overview_drag_mode = OverviewDragMode::SlideBody;
                set_cursor(CursorIcon::Grabbing);
            }
            self.last_overview_drag_t = Some(t);
        } else if let (Some(coord), Some(last_t)) = (pointer, self.last_overview_drag_t) {
            let delta = coord.x - last_t;
            match self.overview_drag_mode {
                OverviewDragMode::ResizeLeft => {
                    set_cursor(CursorIcon::ResizeHorizontal);
                    let lo = clamp_lenient(cur_lo + delta, full_lo, cur_hi - MIN_WINDOW_S);
                    self.set_top_x_range(lo, cur_hi);
                }
                OverviewDragMode::ResizeRight => {
                    set_cursor(CursorIcon::ResizeHorizontal);
                    let hi = clamp_lenient(cur_hi + delta, cur_lo + MIN_WINDOW_S, full_hi);
                    self.set_top_x_range(cur_lo, hi);
                }
                OverviewDragMode::SlideBody => {
                    set_cursor(CursorIcon::Grabbing);
                    let [lo, hi] = view::slide_range([cur_lo, cur_hi], delta, full_lo, full_hi);
                    self.set_top_x_range(lo, hi);
                }
                OverviewDragMode::None => {}
            }
            self.last_overview_drag_t = Some(coord.x);
        }

        if released {
            self.overview_drag_mode = OverviewDragMode::None;
            self.last_overview_drag_t = None;
        }
    }

    /// Apply a new detail range chosen via the overview and update live-follow state.
    fn set_top_x_range(&mut self, lo: f64, hi: f64) {
        self.top_x_range = Some([lo, hi]);
        if let Some(last) = self.points.last() {
            self.was_at_end = view::is_at_end(hi, last.time_s);
        }
    }
}
