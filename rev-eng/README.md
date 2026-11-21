# XDS110 Firmware `3.0.0.45` Reverse Engineering & Open-Source FW Roadmap

Complete static reverse-engineering analysis of `proprietary_fw/firmware_3.0.0.45.bin` (Texas Instruments XDS110 / **LP-XDS110ET**, MCU: **MSP432E401YTPDT**, 120 MHz ARM Cortex-M4F), cross-referenced against the board schematic (`docs/LP-XDS110ET_SCH.pdf`) and TI MSP432E4 DriverLib SDK headers (`firmware_src/sdk/`).

---

## Key Findings at a Glance

- **Bare-Metal GCC + TivaWare/MSP432E4 SDK**: No RTOS. Single-threaded Cortex-M4F super-loop driven by Cortex-M bit-banded event flags (`g_ui32Flags` at `0x20004B90`) and hardware ISRs.
- **Critical Hardware Fact — External ULPI High-Speed USB PHY Required**: On LP-XDS110ET (`MCU101 Rev B`), the USB-C D+/D- lines connect **exclusively** to `U15` (`USB3300-EZK` ULPI HS-USB PHY), **not** to the MCU's internal PHY (`PL6/PL7`). Firmware auto-detects the ULPI PHY at boot by checking `PP2` (`USBNXT`) and runs USB 2.0 High-Speed (512-byte bulk packets).
- **Safe Bootloader Re-Entry (No Bricking)**: `CMD 0x26` (`XDS_EnterDFU`) cleanly tears down USB, resets `USB0`, switches back to the internal 60 MHz USB clock, delays ~1 s, and branches to `*(uint32_t *)0x0000002C` (the preserved 16 KiB DFU bootloader's SVCall vector).
- **Vector Table IRQ Alignment Fix (`firmware_src/src/startup_gcc.c`)**: The original skeleton omitted `CAN1` (IRQ 39), shifting `USB0` (IRQ 42) and all subsequent peripheral IRQs by -1 slot; this is fixed in `firmware_src/` (along with Phases 0–4 & 6 of `ROADMAP.md`, all hardware-verified).
- **Complete 99-Slot XDS110 Command Table Decoded**: `XDS_DispatchCommand` (`0x0000685C`) dispatches opcodes `0x00..0x62` via a single `TBH` table at `0x0000688A` (94 active opcodes, 5 unsupported `0x4B..0x4F`).
- **EnergyTrace HDR 256 kS/s Zero-CPU-Overhead Pipeline Decoded**: Uses `ADS127L01` (24-bit ADC clocked at 8.192 MHz, OSR=32 → 256 kS/s) where `PB4` (`nDRDY`) triggers **uDMA Channel 5** to push 4 dummy bytes per sample into `SSI1_DR` (15 MHz), while **uDMA Channel 24** ping-pongs 1024-byte frames (256 × 4-byte samples) directly into pre-framed 1030-byte `0x68` USB packets backed by a 96 KiB Bulk IN ring buffer.
- **Boot-Time Cached Calibration**: On LP-XDS110ET (`hw=0x23`), calibration points `idx 0, 1, 2, 4` are measured once at boot while holding target `nSRST` low and isolating `LP_VDD` via `PM1` (`ET_CALIBRATE`). Runtime `ET_Calibrate` (`0x1E`) returns cached values immediately without power-cycling the target.

---

## Directory Layout

| Path | Description |
|---|---|
| `README.md` | This overview & quickstart |
| `ARCHITECTURE.md` | Memory map, boot sequence, vector table, super-loop, USB composite stack, DFU jump |
| `HARDWARE_MAP.md` | Complete MSP432E401Y pinout, peripherals, uDMA channels, board IDs, schematic cross-ref |
| `PROTOCOL_SPEC.md` | Complete XDS110 USB command table (`0x00..0x62`), framing, error codes, CDC/CMSIS-DAP/MSC |
| `ENERGYTRACE_DEEP_DIVE.md` | Full hardware/firmware analysis of LP-XDS110ET EnergyTrace HDR (`ADS127L01`, calibration, DMA, packets) |
| `ROADMAP.md` | Phased open-source firmware implementation roadmap for `firmware_src/` |
| `BMP_ROADMAP.md` | Phase 7 architecture & implementation roadmap for Black Magic Debug Probe (BMP) support |
| `scripts/` | Reproducible Python 3 + Capstone reverse-engineering scripts |
| `output/` | Machine-readable JSON analysis dumps & extracted schematic text |

---

## Reproducing the Analysis

Prerequisites: Python 3 with `capstone` (and `pdftotext` for schematic extraction).

Run the complete pipeline from the repository root:

```bash
python3 rev-eng/scripts/run_all.py
```

Individual scripts:

| Script | Purpose | Output |
|---|---|---|
| `scripts/fwlib.py` | Shared Flash+`.data` loader, SDK header parser (`rom.h`, `msp432e401y.h`, `sysctl.h`, `pin_map.h`), annotated Thumb-2 disassembler | (library) |
| `scripts/01_inspect_binary.py` | Vector table, ResetISR, ASCII/UTF-16LE strings, binary metadata | `output/01_inspect.json` |
| `scripts/02_parse_usb_descriptors.py` | TivaWare `usblib` composite structs, callbacks, buffers, descriptors | `output/02_usb_descriptors.json` |
| `scripts/03_map_architecture_and_peripherals.py` | Whole-binary MMIO, `SYSCTL_PERIPH_*`, pin-mux, ROM API, hot functions | `output/03_architecture.json` |
| `scripts/04_analyze_xds_protocol.py` | Full `TBH` opcode table (`0x00..0x62`), arg/resp extraction, `IntRegister` scan | `output/04_xds_protocol.json` |
| `scripts/05_analyze_energytrace.py` | Deep disassembly of EnergyTrace Mode 0/1/2, HDR `ADS127L01`, Dig, DCDC I2C | `output/05_energytrace.json` |
| `scripts/06_analyze_boot_and_subsystems.py` | Board ID detection (`PK4..PK7`), ULPI PHY, DFU jump, SRST/TRST, SWO/SSI3 | `output/06_boot_subsystems.json` |
| `scripts/07_parse_schematic_pins.py` | Schematic PDF text extraction & verified pinout cross-reference | `output/07_schematic_pins.json` |
