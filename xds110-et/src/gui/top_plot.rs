//! Detailed (top) plot with wheel zoom and drag panning.

use eframe::egui::{self, Color32};
use egui_plot::{Corner, FilledArea, Legend, Line, Plot, PlotPoints};

use super::view::{self, MIN_SPAN_S};
use super::{EnergyTraceApp, FrameView, PLOT_BACKGROUND};
use crate::data_series::visible_slice_padded;

const AVG_COLOR: Color32 = Color32::from_rgb(80, 220, 110);
const MIN_COLOR: Color32 = Color32::from_rgb(90, 170, 240);
const MAX_COLOR: Color32 = Color32::from_rgb(255, 100, 100);
/// Smallest Y span (nA) the manual zoom may reach.
const MIN_Y_SPAN_NA: f64 = 1.0;
/// Height reserved below the detail plot for the overview section.
const OVERVIEW_RESERVED_H: f32 = 135.0;

/// View changes requested by the user during one frame.
#[derive(Default)]
struct UserEdit {
    x: Option<[f64; 2]>,
    y: Option<[f64; 2]>,
}

impl EnergyTraceApp {
    pub(super) fn show_top_plot(&mut self, ui: &mut egui::Ui, frame: &FrameView) {
        let FrameView { full_hi, unit, .. } = *frame;
        let k = unit.factor();
        let x_range = self.top_x_range.unwrap_or([frame.full_lo, full_hi]);
        let y_range_na = self.top_y_range.unwrap_or([-1_000_000.0, 1_000_000.0]);

        let height = (ui.available_height() - OVERVIEW_RESERVED_H).max(180.0);
        let ctrl_held = ui.input(|i| i.modifiers.ctrl);

        let plot = Plot::new("top_detailed_plot")
            .height(height)
            .x_axis_label("time [s]")
            .y_axis_label(format!("current [{}]", unit.name()))
            .legend(Legend::default().position(Corner::RightTop))
            .show_grid(true)
            .allow_scroll(false) // Scroll zoom is handled explicitly to separate X vs Y (Ctrl)
            .allow_zoom(false) // Custom zoom avoids a one-frame delayed state reversion
            .allow_drag(false) // Custom drag keeps the sliding window in sync
            .auto_bounds([false, false]);

        let mut edit = UserEdit::default();
        let points = &self.points;
        let show_min_max = self.show_min_max;

        ui.scope(|ui| {
            ui.visuals_mut().extreme_bg_color = PLOT_BACKGROUND;

            plot.show(ui, |plot_ui| {
                plot_ui.set_plot_bounds_x(x_range[0]..=x_range[1]);
                plot_ui.set_plot_bounds_y((y_range_na[0] * k)..=(y_range_na[1] * k));

                let resp = plot_ui.response();
                let pointer = plot_ui.pointer_coordinate();

                // egui routes Ctrl+wheel into zoom_delta() and the plain wheel into smooth_scroll_delta
                let (zoom_x, zoom_y) = plot_ui.ctx().input(|i| {
                    view::wheel_zoom(i.modifiers.ctrl, i.zoom_delta(), i.smooth_scroll_delta.y)
                });

                if resp.hovered() && zoom_x != 1.0 {
                    let cx = pointer.map_or((x_range[0] + x_range[1]) / 2.0, |c| c.x);
                    edit.x = Some(view::zoom_range(x_range, cx, zoom_x, MIN_SPAN_S));
                }

                if resp.hovered() && zoom_y != 1.0 {
                    let cy_na = pointer.map_or((y_range_na[0] + y_range_na[1]) / 2.0, |c| c.y / k);
                    edit.y = Some(view::zoom_range(y_range_na, cy_na, zoom_y, MIN_Y_SPAN_NA));
                } else if resp.dragged() {
                    let delta = resp.drag_delta();
                    let dpos = plot_ui.transform().dvalue_dpos();
                    let dx = -(delta.x as f64) * dpos[0].abs();
                    edit.x = Some([x_range[0] + dx, x_range[1] + dx]);
                    if ctrl_held {
                        let dy_na = (delta.y as f64) * dpos[1].abs() / k;
                        edit.y = Some([y_range_na[0] + dy_na, y_range_na[1] + dy_na]);
                    }
                }

                // Only the visible portion (plus neighbours) needs to be handed to the plotter
                let visible = visible_slice_padded(points, x_range[0], x_range[1]);

                let avg_pts: PlotPoints =
                    visible.iter().map(|p| [p.time_s, p.avg_na * k]).collect();
                plot_ui.line(Line::new("avg", avg_pts).color(AVG_COLOR).width(1.5));

                if show_min_max {
                    let xs: Vec<f64> = visible.iter().map(|p| p.time_s).collect();
                    let ys_min: Vec<f64> = visible.iter().map(|p| p.min_na * k).collect();
                    let ys_max: Vec<f64> = visible.iter().map(|p| p.max_na * k).collect();

                    // Shaded envelope between the min and max traces
                    plot_ui.add(
                        FilledArea::new("min..max", &xs, &ys_min, &ys_max)
                            .fill_color(Color32::from_rgba_unmultiplied(90, 170, 240, 35)),
                    );

                    let trace = |name: &str, ys: &[f64], color| {
                        let pts: PlotPoints = xs.iter().zip(ys).map(|(&x, &y)| [x, y]).collect();
                        Line::new(name, pts).color(color).width(1.0)
                    };
                    plot_ui.line(trace("min", &ys_min, MIN_COLOR));
                    plot_ui.line(trace("max", &ys_max, MAX_COLOR));
                }
            });
        });

        if let Some(y) = edit.y {
            self.top_y_range = Some(y);
            self.manual_y_zoom = true;
        }
        if let Some([lo, hi]) = edit.x {
            self.top_x_range = Some([lo, hi]);
            self.was_at_end = view::is_at_end(hi, full_hi);
        }
    }
}
