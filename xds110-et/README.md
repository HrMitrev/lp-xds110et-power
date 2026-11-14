# XDS110 EnergyTrace (Rust CLI & GUI)

A standalone, high-performance CLI and GUI tool for capturing and visualizing power measurements using Texas Instruments **LP-XDS110ET** EnergyTrace hardware (`0451:BEF3`).

> ⚡ **Note**: This is a **vibe coded app** built using AI, tailored specifically for the **TI LP-XDS110ET** (LaunchPad with EnergyTrace debug probe).

<p align="center">
  <picture>
    <source srcset="assets/lp-xds110et.jxl" type="image/jxl" />
    <img src="assets/lp-xds110et.png" alt="Texas Instruments LP-XDS110ET" width="600" />
  </picture>
</p>

## Key Features

- **Zero External Runtime Dependencies**: Uses pure WinUSB / native OS USB interfaces via vendored `rusb`. No external DLLs or manual driver swaps (Zadig) needed on Windows.
- **High-Speed Hardware Streaming**: Captures continuous 256 kS/s sample streams from interface 6 (Bulk IN `0x87`).
- **Decoupled Architecture**: CSV decimated logging is decoupled from the live raw USB stream and the GUI renderer.
- **Real-Time Interactive GUI**: Built with `egui` and `egui_plot` featuring:
  - Top detailed graph showing live avg current (with optional min/max).
  - Bottom overview graph showing the complete capture timeline with an interactive sliding selection window.
  - Auto-follow mode when the selection window is dragged to the latest incoming data.
  - Interactive mouse cursor edge resizing (`↔`) on the timeline selection window.
  - Fixed-scale timeline overview with horizontal scrollbar so the window never shrinks over long captures.
  - Dynamic unit scaling (switching between `mA`, `uA`, and `nA` based on the window's `avg` current).
  - Hardware target MCU reset using TI's `xds110reset.exe` `0x0E` nSRST pin toggle sequence.
- **Configurable Header**: Key parameters like slope (`nA/LSB`), sample frequency, and default logging intervals are centralized in `src/config.rs`.

## Requirements

- **Rust**: 1.85+ (Edition 2024 supported)
- **Hardware**: Texas Instruments **LP-XDS110ET** Debug Probe with EnergyTrace (`0451:BEF3`). On Windows, the default TI WinUSB driver works out of the box.

## Building

```bash
cd xds110-et
cargo build --release
```

The resulting standalone executable is located at `target/release/xds110-et.exe`.

## Usage

### Query Probe Info
Check if the probe is connected and query its hardware information:
```bash
cargo run -- info
```
Output:
```text
HardwareInfo: 0x00000002 0x07
```

### Send Raw Command Bytes
Send arbitrary command bytes in hex directly to the probe:
```bash
cargo run -- cmd 46
```
Output:
```text
0200000007
```

### Stream and Capture to Decoupled CSV
Capture power measurements with custom rows-per-hour and duration:
```bash
# Capture for 10 seconds with default 3600 rows/hour (1 row/sec)
cargo run -- capture -t 10 -o capture.csv

# Run continuously until Ctrl-C with 7200 rows/hour (2 rows/sec)
cargo run -- capture -r 7200
```

### Interactive GUI Mode

Launch the live real-time capture GUI (default when run without arguments):
```bash
cargo run
```

Or open and inspect an existing capture log in the GUI:
```bash
cargo run -- logs/long_20261113_215600.csv
```

#### GUI Navigation & Controls
- **Mouse Wheel / Zoom**: Zooms the **X-axis only** around the mouse pointer while the **Y-axis dynamically autoscales** to fit whatever data is visible in the current window.
- **Ctrl + Mouse Wheel**: Zooms the **Y-axis** for detailed vertical inspection.
- **Bottom Timeline Overview**: Fixed scale overview with horizontal scrollbar. Click anywhere or drag the yellow window to slide across the timeline.
- **Resize Window with Cursor**: Hover over the left or right yellow handle line until the cursor turns into `↔`, then drag to resize the time window.
- **Auto-Follow Mode**: Drag the sliding window to the far right edge to automatically track and autoscale with new incoming live data.
- **Reset View (`r`)**: Resets the window to full capture range and restores automatic Y autoscaling.
- **🧹 Clear Plot (`c`)**: Clears in-memory GUI graph points to restart the visual timeline from scratch. The background `.csv` log continues recording uninterrupted without any gap.
- **🔄 Reset MCU**: Restarts the target MCU by pulsing the hardware nSRST reset line (`CMD 0x0E`, using TI's `xds110reset.exe` toggle logic).
- **Min/Max Button**: Toggles min/max boundary traces (OFF by default).
- **Live Button**: Toggles live following mode (ON by default).

### Running Tests
Execute unit tests for protocol frame encoding, decoding, ADC conversions, CSV window decoupling, and log parsing:
```bash
cargo test
```

## Implementation Stages

- [x] **Stage 1**: Rust project setup, hardcoded configuration header (`src/config.rs`), USB transport & WinUSB interface handling (`src/usb.rs`), protocol encoding/decoding (`src/protocol.rs`), and CLI commands (`info`, `cmd`).
- [x] **Stage 2**: 256 kS/s stream reader thread, lock-free sample buffering, decoupled CSV logging (`capture`), and terminal live reporter.
- [x] **Stage 3**: `eframe`/`egui_plot` interactive GUI with default Live mode (ON), toggleable min/max traces (OFF by default), window statistics panel, and CSV file viewing.
- [x] **Stage 4**: Bottom timeline overview graph with interactive sliding window, two-way synchronized bounds, auto-follow when dragged to the right edge, and graceful probe disconnect on GUI close.
