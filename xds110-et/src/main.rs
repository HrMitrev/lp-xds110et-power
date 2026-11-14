//! XDS110 EnergyTrace CLI & GUI tool in Rust.

pub mod capture;
pub mod cleanup;
pub mod config;
pub mod csv_writer;
pub mod data_series;
pub mod frame;
pub mod gui;
pub mod protocol;
pub mod usb;

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;

use clap::{Parser, Subcommand};
use crossbeam_channel::bounded;
use protocol::CMD_CONNECT;
use usb::Xds110;

#[derive(Parser, Debug)]
#[command(
    name = "xds110-et",
    author,
    version,
    about = "XDS110 EnergyTrace high-speed power capture & visualization tool"
)]
struct Cli {
    /// Optional CSV log file to view in the GUI (or specify `view <file>`)
    #[arg(value_name = "LOG_FILE")]
    file: Option<PathBuf>,

    #[command(subcommand)]
    command: Option<Commands>,
}

#[derive(Subcommand, Debug)]
enum Commands {
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
}

fn cmd_info() -> Result<(), Box<dyn std::error::Error>> {
    let probe = Xds110::open(false)?;
    probe.cmd(CMD_CONNECT, &[], 2)?;
    let (hw, extra) = probe.hw_info()?;
    println!("HardwareInfo: 0x{hw:08x} 0x{extra:02x}");
    probe.shutdown();
    Ok(())
}

fn cmd_raw(bytes: &[String]) -> Result<(), Box<dyn std::error::Error>> {
    let mut parsed = Vec::new();
    for b in bytes {
        let clean = b.trim_start_matches("0x").trim_start_matches("0X");
        let byte = u8::from_str_radix(clean, 16)?;
        parsed.push(byte);
    }

    let cmd_byte = parsed[0];
    let args = &parsed[1..];

    let probe = Xds110::open(false)?;
    let rsp = probe.cmd(cmd_byte, args, 2)?;
    for b in &rsp {
        print!("{b:02x}");
    }
    println!();
    probe.shutdown();
    Ok(())
}

fn cmd_capture_headless(
    time: f64,
    rows_per_hour: u32,
    output: Option<PathBuf>,
    slope: f64,
    retry_delay: f64,
    max_retries: u32,
) -> Result<(), Box<dyn std::error::Error>> {
    if rows_per_hour == 0 {
        return Err("--rows-per-hour must be >= 1".into());
    }

    let output_path = output.unwrap_or_else(|| {
        let timestamp = chrono::Local::now().format("%Y%m%d_%H%M%S");
        PathBuf::from(format!("logs/long_{timestamp}.csv"))
    });

    let config = capture::CaptureConfig {
        time_limit_sec: time,
        rows_per_hour,
        output_path,
        slope,
        retry_delay_sec: retry_delay,
        max_retries,
    };

    let stop_signal = Arc::new(AtomicBool::new(false));

    // Register exit hooks (Ctrl-C and panic hook)
    cleanup::register_exit_hooks(Arc::clone(&stop_signal));
    let _exit_guard = cleanup::ProbeExitGuard::new(Arc::clone(&stop_signal));

    capture::run_capture(config, stop_signal, None, None)
}

fn run_file_viewer(path: PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    let filename = path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "capture.csv".into());

    let points = data_series::load_csv_file(&path)?;
    if points.is_empty() {
        return Err(format!("No data points found in {}", path.display()).into());
    }

    let app = gui::EnergyTraceApp::new_file(filename, points);
    gui::launch_gui(app)
}

fn run_live_gui() -> Result<(), Box<dyn std::error::Error>> {
    let stop_signal = Arc::new(AtomicBool::new(false));
    let stop_clone = Arc::clone(&stop_signal);

    // Register exit hooks to guarantee CMD 0x02 is sent
    cleanup::register_exit_hooks(Arc::clone(&stop_signal));
    let _exit_guard = cleanup::ProbeExitGuard::new(Arc::clone(&stop_signal));

    // Bounded channels:
    // tx, rx for passing decimated display points from USB to GUI
    let (tx, rx) = bounded(256);
    // ctrl_tx, ctrl_rx for passing control commands (Reset MCU, Set VCC) from GUI to USB probe
    let (ctrl_tx, ctrl_rx) = bounded(16);

    let config = capture::CaptureConfig::default();

    let capture_handle = thread::spawn(move || {
        match capture::run_capture(config, stop_clone, Some(tx), Some(ctrl_rx)) {
            Ok(()) => Ok(()),
            Err(e) => Err(e.to_string()),
        }
    });

    let stop_gui = Arc::clone(&stop_signal);
    let app = gui::EnergyTraceApp::new_live(stop_gui, rx, capture_handle, ctrl_tx);
    let gui_res = gui::launch_gui(app);

    // On exit from GUI, guarantee stop signal is signaled
    stop_signal.store(true, Ordering::SeqCst);

    gui_res
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();

    if let Some(file_path) = cli.file {
        return run_file_viewer(file_path);
    }

    match cli.command {
        Some(Commands::Info) => cmd_info(),
        Some(Commands::Cmd { bytes }) => cmd_raw(&bytes),
        Some(Commands::Capture {
            time,
            rows_per_hour,
            output,
            slope,
            retry_delay,
            max_retries,
        }) => cmd_capture_headless(time, rows_per_hour, output, slope, retry_delay, max_retries),
        Some(Commands::View { path }) => run_file_viewer(path),
        None => run_live_gui(),
    }
}
