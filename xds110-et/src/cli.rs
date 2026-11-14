//! Command-line interface definition.

use std::path::PathBuf;

use clap::{Parser, Subcommand};

use crate::config;

#[derive(Parser, Debug)]
#[command(
    name = "xds110-et",
    author,
    version,
    about = "XDS110 EnergyTrace high-speed power capture & visualization tool"
)]
pub struct Cli {
    /// Optional CSV log file to view in the GUI (or specify `view <file>`)
    #[arg(value_name = "LOG_FILE")]
    pub file: Option<PathBuf>,

    #[command(subcommand)]
    pub command: Option<Commands>,
}

#[derive(Subcommand, Debug)]
pub enum Commands {
    /// Query probe hardware info
    Info,

    /// Send arbitrary raw command bytes to the probe (e.g. `cmd 46` or `cmd 1d 01 00 ...`)
    Cmd {
        /// Command byte followed by optional argument bytes in hex
        #[arg(required = true, num_args = 1..)]
        bytes: Vec<String>,
    },

    /// Headless CLI capture without GUI (logs to CSV until Ctrl-C or time limit)
    Capture {
        /// Duration in seconds (0 = run indefinitely until Ctrl-C)
        #[arg(short = 't', long = "time", default_value_t = 0.0)]
        time: f64,

        /// CSV rows written per hour of capture (default: 3600 = 1 row/s)
        #[arg(short = 'r', long = "rows-per-hour", default_value_t = config::DEFAULT_ROWS_PER_HOUR)]
        rows_per_hour: u32,

        /// Output CSV file path (default: logs/long_YYYYMMDD_HHMMSS.csv)
        #[arg(short = 'o', long = "output")]
        output: Option<PathBuf>,

        /// Calibration slope in nA per ADC LSB
        #[arg(long = "slope", default_value_t = config::DEFAULT_SLOPE_NA_PER_LSB)]
        slope: f64,

        /// Seconds to wait before each reconnect attempt
        #[arg(long = "retry-delay", default_value_t = 5.0)]
        retry_delay: f64,

        /// Maximum consecutive reconnect retries before giving up (0 = retry forever)
        #[arg(long = "max-retries", default_value_t = 0)]
        max_retries: u32,
    },

    /// Open an existing CSV log file in the interactive GUI viewer
    View {
        /// CSV file path
        path: PathBuf,
    },

    /// Query DFU mode status, flash geometry, serial number, and configuration
    DfuInfo,

    /// Switch probe from runtime mode into DFU bootloader mode
    DfuSwitch,

    /// Reset probe from DFU mode back into runtime mode
    DfuReset,

    /// Read or set probe serial number (switches to DFU mode if needed)
    DfuSerial {
        /// New serial number (8 characters for full replacement, or 4 characters with -n)
        serial: Option<String>,

        /// Update only the 4-character suffix, preserving the existing board prefix
        #[arg(short = 'n', long = "suffix")]
        suffix_only: bool,

        /// Reset the probe back to runtime mode after setting serial
        #[arg(short = 'r', long = "reset", default_value_t = true)]
        reset: bool,
    },

    /// Set probe configuration (1 = Standard, 2 = Drag-n-Drop, 4 = CMSIS-DAP 2.0, 5 = EVM)
    DfuConfig {
        /// Configuration number (1, 2, 4, 5)
        config: u32,

        /// Reset the probe back to runtime mode after setting configuration
        #[arg(short = 'r', long = "reset", default_value_t = true)]
        reset: bool,
    },

    /// Flash firmware .bin file to the probe using USB DFU
    DfuFlash {
        /// Path to firmware binary file (e.g. firmware_3.0.0.41.bin)
        file: PathBuf,

        /// Reset the probe back to runtime mode after flashing
        #[arg(short = 'r', long = "reset", default_value_t = true)]
        reset: bool,
    },
}
