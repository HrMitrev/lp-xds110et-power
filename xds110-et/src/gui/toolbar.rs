//! Top control bar and window statistics line.

use eframe::egui::{self, Color32, RichText};

use super::{EnergyTraceApp, GuiSource, TOAST_DURATION};
use crate::data_series::{Unit, WindowStats};

const GREEN: Color32 = Color32::from_rgb(0, 200, 50);
const ORANGE: Color32 = Color32::from_rgb(255, 170, 70);

/// A button whose label shows ON/OFF state; returns true when clicked.
fn toggle_button(ui: &mut egui::Ui, label: &str, on: bool, on_color: Color32) -> bool {
    let text = if on {
        RichText::new(format!("{label}: ON"))
            .color(on_color)
            .strong()
    } else {
        RichText::new(format!("{label}: OFF")).color(Color32::GRAY)
    };
    ui.button(text).clicked()
}

impl EnergyTraceApp {
    pub(super) fn show_toolbar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal_wrapped(|ui| {
            ui.heading(
                RichText::new("⚡ XDS110 EnergyTrace")
                    .strong()
                    .color(Color32::from_rgb(235, 235, 235)),
            );
            ui.separator();

            if toggle_button(ui, "Live", self.live_mode, GREEN) {
                self.live_mode = !self.live_mode;
                if self.live_mode {
                    self.was_at_end = true;
                }
            }
            ui.separator();

            if toggle_button(
                ui,
                "Min/Max",
                self.show_min_max,
                Color32::from_rgb(220, 80, 80),
            ) {
                self.show_min_max = !self.show_min_max;
            }
            ui.separator();

            if self.manual_y_zoom {
                let text = RichText::new("Y: Manual (click to auto)")
                    .color(Color32::from_rgb(255, 180, 0))
                    .strong();
                if ui.button(text).clicked() {
                    self.manual_y_zoom = false;
                }
                ui.separator();
            }

            if ui.button("Reset View (r)").clicked() {
                self.reset_view_requested = true;
            }
            ui.separator();

            if ui
                .button(RichText::new("🧹 Clear Plot (c)").color(ORANGE))
                .on_hover_text(
                    "Clear current graph data and restart visual timeline from scratch.\n\
                     Background CSV recording will continue uninterrupted.",
                )
                .clicked()
            {
                self.clear_plot();
            }
            ui.separator();

            if ui
                .button(
                    RichText::new("🔄 Reset MCU")
                        .color(Color32::from_rgb(100, 200, 255))
                        .strong(),
                )
                .on_hover_text("Toggle target MCU nSRST pin (xds110reset.exe logic: CMD 0x0E)")
                .clicked()
            {
                self.trigger_mcu_reset();
            }
            ui.separator();

            self.show_status(ui);
        });
    }

    fn show_status(&self, ui: &mut egui::Ui) {
        if let Some((msg, time)) = &self.status_toast
            && time.elapsed() < TOAST_DURATION
        {
            ui.label(
                RichText::new(format!("✔ {msg}"))
                    .color(Color32::from_rgb(255, 215, 0))
                    .strong(),
            );
            ui.separator();
        }

        if let Some(err) = &self.capture_error {
            ui.label(
                RichText::new(format!("⚠ {err}"))
                    .color(Color32::from_rgb(255, 100, 100))
                    .strong(),
            );
            ui.separator();
        }

        let total_points = self.points.len();
        let duration_s = self.points.last().map_or(0.0, |p| p.time_s);
        match &self.source {
            GuiSource::Live { .. } => {
                ui.label(format!(
                    "Live capture: {duration_s:.1} s ({total_points} points)"
                ));
            }
            GuiSource::File { filename } => {
                ui.label(format!(
                    "File: {filename} ({duration_s:.1} s, {total_points} points)"
                ));
            }
        }
    }

    /// Stats for the current visible window, shown above the detail plot.
    pub(super) fn show_window_stats(
        &self,
        ui: &mut egui::Ui,
        lo: f64,
        hi: f64,
        stats: &WindowStats,
        unit: Unit,
    ) {
        let k = unit.factor();
        let name = unit.name();
        ui.horizontal(|ui| {
            ui.label(format!("Window: {lo:.1} .. {hi:.1} s |"));
            ui.label(
                RichText::new(format!("avg: {:.4} {name}", stats.avg_na * k))
                    .color(Color32::from_rgb(80, 220, 110))
                    .strong(),
            );
            if self.show_min_max {
                ui.label("|");
                ui.label(
                    RichText::new(format!("min: {:.4} {name}", stats.min_na * k))
                        .color(Color32::from_rgb(90, 170, 240)),
                );
                ui.label(
                    RichText::new(format!("max: {:.4} {name}", stats.max_na * k))
                        .color(Color32::from_rgb(255, 100, 100)),
                );
            }
        });
    }
}
