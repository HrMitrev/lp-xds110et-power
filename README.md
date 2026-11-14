# XDS110 EnergyTrace Tools

<p align="center">
  <picture>
    <source srcset="assets/gui.jxl" type="image/jxl" />
    <img src="assets/lp-xds110et.png" alt="Texas Instruments LP-XDS110ET" width="600" />
  </picture>
</p>

Utilities for capturing, logging, and visualizing power measurements from Texas Instruments **LP-XDS110ET** EnergyTrace hardware (`0451:BEF3`).

> ⚡ **Note**: This application is **vibe coded** with LLMs specifically for the **TI LP-XDS110ET** (LaunchPad with EnergyTrace debug probe).

<p align="center">
  <picture>
    <source srcset="assets/lp-xds110et.jxl" type="image/jxl" />
    <img src="assets/lp-xds110et.png" alt="Texas Instruments LP-XDS110ET" width="600" />
  </picture>
</p>

## Project Components

- **`xds110-et/`**: Modern, all-in-one Rust CLI and GUI application for high-speed (256 kS/s) streaming, decoupled CSV logging, and real-time visualization with sliding timeline window. Zero external C/DLL dependencies on Windows.
- **`PROTOCOL.md`**: Reverse-engineered protocol documentation of the XDS110 EnergyTrace WinUSB command and data streaming interfaces, including hardware reset from `xds110reset.exe`.
- **Python Scripts (Reference implementations in `py_ref/`)**:
  - `xds110_et_long.py`: CLI tool for long-duration captures with decimated CSV logging.
  - `xds110_et_live.py`: Real-time streaming and monitoring tool.
  - `plot_long_log.py`: Matplotlib/Tkinter GUI viewer for decimated capture logs.

## Quick Start (Rust CLI & GUI)

See [`xds110-et/README.md`](xds110-et/README.md) for full documentation.

```bash
cd xds110-et
cargo run -- info
```
