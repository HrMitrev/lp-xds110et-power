# Open-Source LP-XDS110ET Firmware Implementation Roadmap

A phased, clean-room engineering plan for the open-source bare-metal firmware in `firmware_src/` targeting **TI LP-XDS110ET** (`MSP432E401Y`, `hw_version = 0x23`), compatible with `xds110-et/` and standard host debuggers (**OpenOCD**, **probe-rs**, **pyOCD**).

---

## Design Principles & Safety Rules

1. **Never Brick the Probe (DFU-First Strategy)**:
   - Keep application flash origin at `0x00004000` (never touch `0x00000000 – 0x00003FFF` DFU bootloader or `0x000FC000` config/serial block).
   - Boot-time **double-reset DFU failsafe** (`.noinit` SRAM cookie for 500 ms) **and** XDS110 opcode `0x26` (`XDS_ENTER_DFU` → `0x0000002C`).
2. **Target-First Scope**:
   - Focus cleanly on **LP-XDS110ET (`hw=0x23`)** using MSP432E4 DriverLib / CMSIS calls (`MAP_*`, `NVIC_*`, CMSIS register structs).
3. **Zero Proprietary Code Copying**:
   - Clean-room C99 bare-metal implementation written against TI's public MSP432E4 DriverLib, ARM CMSIS-DAP v2 / ADIv5 specs, and OpenOCD's GPLv2 `xds110.c` driver protocol.

---

## Phase 0 — Skeleton Fixes & Board Support Package (Completed & Verified)

- [x] **Fixed Vector Table IRQ Alignment (`firmware_src/src/startup_gcc.c`)**: Inserted missing IRQ 39 (`CAN1`) so `USB0` lands at IRQ 42 (vector 58), `SSI1` at IRQ 34, `uDMA` at IRQ 46, etc.
- [x] **Created `board_lp_xds110et.h` & `board.c`**: Full LP-XDS110ET BSP (120 MHz PLL, 1 kHz SysTick, `PD2`/`PD3` LEDs, `PK4..PK7` HW ID `0x23`, `PA6` nSRST, `PC5` `XDS_TMS_DIR`, ULPI & EnergyTrace HDR pins).
- [x] **Double-Reset DFU Failsafe (`dfu_jump.h / .c`)**: Verified seamless DFU bootloader entry (`1CBE:00FF`).

---

## Phase 1 — USB High-Speed (`USB3300` ULPI) & Core XDS110 Control Channel (Completed & Verified)

- [x] **ULPI High-Speed PHY & Bare-Metal MUSB Device Stack (`usb_descriptors.{h,c}`, `usb_device.{h,c}`)**: Auto-detects `USB3300-EZK` via `PP2`, enumerates `0451:BEF3` at 480 Mbps HS with factory flash serial from `0x000FC000`.
- [x] **XDS110 `0x2A` Framing & Core Opcodes (`xds_cmd.{h,c}`)**: `0x00` `ECHO`, `0x01` `CONNECT`, `0x02` `DISCONNECT`, `0x03` `VERSION`, `0x08` `GET_POWER`, `0x0E` `SET_SRST`, `0x26` `ENTER_DFU`, `0x28` `CONNECT_ET`, `0x4A` `PING`, plus Bulk 1 byte-reversal loopback.

---

## Phase 2 — EnergyTrace HDR 256 kS/s Power Measurement Engine (Completed & Verified)

- [x] **`ADS127L01` + Analog Switch Driver & Ping-Pong uDMA (`et_hdr.{h,c}`)**: `PB4` `nDRDY` → uDMA Ch 5 → `SSI1` Mode 1 @ 15 MHz → uDMA Ch 24 ping-pong into 1030-byte `0x68` packets backed by a 64 KiB Bulk IN ring buffer.
- [x] **Boot-Time Cached Calibration**: Measures calibration points `idx 0, 1, 2, 4` once at boot; serves `0x1D..0x21`, `0x30`, `0x31`, `0x44`, `0x46`.
- [x] **Verified**: Sustained **256 kS/s**, **0 frames lost**, **0 USB reconnects** in standalone captures and concurrently with UART0 (1 Mbps) and SWD (2 MHz).

---

## Phase 3 — Dual Virtual COM Ports (CDC ACM) & Target Aux GPIOs (Completed & Verified)

- [x] **CDC 0 — Application UART (`UART0` `PA0`/`PA1`, `cdc_uart.{h,c}`)**: Priority-1 ISR + nestable Cortex-M4 `BASEPRI` critical sections, verified bit-exact from 9,600 bps to **2,000,000 bps** (and up to 1,000,000 bps concurrently with EnergyTrace HDR).
- [x] **CDC 1 — Auxiliary / SWO UART (`UART7` RX on `PC4`)**: Non-blocking TX drain + RX bridge preserving `PC5` as `XDS_TMS_DIR`.
- [x] **Target Aux GPIOs (`0x29`/`0x2A`, `aux_gpio.{h,c}`)**: Glitch-free `PF0..PF2` + `U49` (`PJ0`/`PJ1`) direction switching + probe-identify LED blink (`0x80000010`), hardware loopback verified.

---

## Phase 4 — CMSIS-DAP v2 WinUSB Bulk & Fast SWD Engine (Completed & Verified)

- [x] **Fast SWD Wire Engine (`dap/swd.{h,c}`)**: `DWT->CYCCNT` cycle-accurate SWCLK pacing, glitch-free `PA3`/`PC5` turnaround, `SWCLK` parked LOW per ARM ADIv5, and 8 trailing idle clocks after final AP/DP writes (verified `connect_assert_srst` + `VC_CORERESET` + `OBL_LAUNCH`).
- [x] **CMSIS-DAP v2.0.0 WinUSB Bulk (`dap/cmsis_dap.{h,c}`, Interface 5 `0x05`/`0x86`)**:
  - 512-byte HS / 64-byte FS variable-length Bulk packets, 4-deep queues, exact 4,096 B USB0 FIFO packing.
  - USB 2.1 BOS Descriptor + Microsoft OS 2.0 Descriptor Set (`WINUSB` + `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`).
  - Full SWD transfer + block transfer + Atomic Commands (`0x7E`/`0x7F`, `caps = 0x11`).
- [x] **Verified**: Live STM32G0 Flash dump/verify/program/mass-erase, SRAM R/W, and ROM-table discovery across **OpenOCD** (`usb_bulk` @ 2 MHz), **probe-rs** (`512 B` v2 packets), **pyOCD**, and concurrent 256 kS/s EnergyTrace HDR (`0 frames lost`).

---

## Phase 5 — Polish, Enhancements & Future Features

- [ ] **4-Wire JTAG & cJTAG Support**: Add JTAG TAP engine (`DAP_JTAG_*` + XDS110 `0x09..0x0D`, `0x3B`, `0x3C`) for non-SWD targets.
- [ ] **CMSIS-DAP v2.1 `DAP_SWD_Sequence (0x1D)`**: Enable SWD multidrop targets (e.g., RP2040).
- [ ] **Diagnostics / Telemetry**: Optional live `VBUS_DET` (`PE3/AIN0`) monitoring, `ADS127L01` saturation flags, and on-demand recalibration command.
- [ ] **Full HIL Re-Verification**: Re-run `make -C firmware_src test-hil` with `TX↔RX` and `GPIO1↔GPIO2` loopback jumpers after the 4 KiB USB FIFO rebalance (`128 B` `CDC0 IN` FIFO).

---

## Phase 6 — Native OpenOCD `interface/xds110.cfg` & TI UniFlash `DSLite` SWD on Interface 2 (Completed & Verified)

- [x] **Native XDS110 SWD Protocol (`dap/xds_swd.{h,c}`)**:
  - Implemented `0x04` (`XDS_SET_TCK`), `0x05`/`0x06` (`XDS_SET/GET_TRST`), `0x07` (`XDS_CYCLE_TCK`), `0x09` (`JTAG_GOTO_STATE`), `0x0F..0x12` (`CMAPI_CONNECT`/`DISCONNECT`/`ACQUIRE`/`RELEASE`), `0x13..0x14` (`CMAPI_READ/WRITE_MEM`), `0x15..0x16` (`CMAPI_REG_READ`/`WRITE`), `0x17..0x1A` (`SWD_CONNECT` with ADIv5.2 Dormant-to-SWD fallback / `SWD_DISCONNECT` / SWO routing no-ops), `0x2B..0x2C` (`CJTAG_CONNECT`/`DISCONNECT`), `0x32` (`SET_SUPPLY`), `0x39` (`JTAG_SET_MODE_EX`), `0x3A` (`OCD_DAP_REQUEST` batched raw SWD queue up to 4 KiB), `0x3D..0x3E` (`CMAPI_READ/WRITE_MEM_64` with CSW `SProt` + cache), `0x45` (`JTAG_SET_FREQ_EX`), `0x49` (`SET_OPTION`), `0x50..0x51` (`SEC_AP` SACI mailbox write/read), and `0x5A` (`CMAPI_CONNECT_EX` with DP power-up + automatic SWD `FAULT` sticky-error recovery).
  - Updated reported BCD firmware version to `3.0.0.45` so TI UniFlash / `xds110reset` accepts the probe without forcing a proprietary firmware overwrite.
  - Verified live against STM32G0 with OpenOCD `interface/xds110.cfg` (`connect_assert_srst`, `reset halt`, 64 KiB Flash dump/verify, SRAM R/W/restore) and against **TI CC2340R5** with **TI UniFlash `DSLite` (`noConnectFlash`)** (full `xds110reset`, `SEC_AP` SACI debug unlock, Chip Erase, 83+ KiB program + CCFG @ `0x4e020000`, Fast Verify, and Run).
