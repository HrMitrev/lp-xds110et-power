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

- **`xds110-et/`**: Modern, all-in-one Rust CLI and GUI application for high-speed (256 kS/s) EnergyTrace streaming, decoupled CSV logging, real-time visualization, and native USB DFU firmware flashing (`xds110-et dfu-flash`). Zero external C/DLL dependencies on Windows.
- **`firmware_src/`**: Clean-room, open-source bare-metal firmware for the **TI LP-XDS110ET** (`MSP432E401Y`, `hw_version = 0x23`), featuring USB 2.1 High-Speed ULPI (`USB3300`), 256 kS/s EnergyTrace HDR (`ADS127L01` + ping-pong uDMA), Dual Virtual COM Ports (up to 2 Mbps), Target Aux GPIOs, **CMSIS-DAP v2.0.0 WinUSB Bulk** (512-byte HS packets + BOS / MS OS 2.0 auto-binding + Atomic Commands `0x7F` for OpenOCD, `probe-rs`, and `pyOCD`), and **OpenOCD native `interface/xds110.cfg` SWD** (`OCD_DAP_REQUEST 0x3A`) on Interface 2.
- **`rev-eng/`**: Hardware map, architecture analysis, protocol specification, EnergyTrace deep-dive, and phased firmware roadmap.
- **`PROTOCOL.md`**: Protocol documentation of the XDS110 EnergyTrace WinUSB command and data streaming interfaces, including hardware reset.
- **Python Scripts (`py_ref/`)**: Legacy reference Python scripts for EnergyTrace capture and plotting.

## Quick Start (Rust CLI & GUI)

See [`xds110-et/README.md`](xds110-et/README.md) for full documentation.

```bash
cd xds110-et
cargo run -- info
```
