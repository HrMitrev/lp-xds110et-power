//! XDS110 EnergyTrace CLI & GUI tool in Rust.

mod capture;
mod cleanup;
mod cli;
mod commands;
mod config;
mod csv_loader;
mod csv_writer;
mod data_series;
mod frame;
mod gui;
mod protocol;
mod usb;

use clap::Parser;
use cli::{Cli, Commands};

fn main() -> commands::CmdResult {
    let cli = Cli::parse();

    if let Some(path) = cli.file {
        return commands::view_file(&path);
    }

    match cli.command {
        Some(Commands::Info) => commands::info(),
        Some(Commands::Cmd { bytes }) => commands::raw(&bytes),
        Some(Commands::Capture {
            time,
            rows_per_hour,
            output,
            slope,
            retry_delay,
            max_retries,
        }) => {
            commands::capture_headless(time, rows_per_hour, output, slope, retry_delay, max_retries)
        }
        Some(Commands::View { path }) => commands::view_file(&path),
        None => commands::live_gui(),
    }
}
