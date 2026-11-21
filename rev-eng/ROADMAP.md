# Open-Source LP-XDS110ET Firmware Implementation Roadmap

A phased, clean-room engineering plan to evolve the bare-metal skeleton in `firmware_src/` into a full open-source firmware for **TI LP-XDS110ET** (`MSP432E401Y`, `hw_version = 0x23`), 100% compatible with the existing Rust host tools (`xds110-et/`) and standard debuggers (OpenOCD / pyOCD / CCS).

---

## Design Principles & Safety Rules

1. **Never Brick the Probe (DFU-First Strategy)**:
   - Keep application flash origin at `0x00004000` (never touch `0x00000000 – 0x00003FFF` DFU bootloader or `0x000FC000` config/serial block).
   - Implement a **hardware failsafe DFU trigger** at boot (e.g., hold a GPIO jumper or detect double-reset via SRAM magic word) **and** implement `CMD 0x26` (`XDS_EnterDFU` → `0x0000002C`) in Phase 1 before adding any complex drivers!
2. **Target-First Scope**:
   - Focus cleanly on **LP-XDS110ET (`hw=0x23`)** first (no dead code for 12 legacy TI boards, no FAT16 Drag-n-Drop blobs, no EVM INA CLI unless needed later).
3. **Zero Proprietary Code Copying**:
   - Use **TinyUSB** (MIT license) or clean MSP432E4 `driverlib` + bare-metal `USB0` ULPI driver — no need to import proprietary TI `usblib` binaries. *(Note: TinyUSB has a mature `musb` / Mentor Graphics MUSB device controller driver `dcd_musb` that matches MSP432E401Y's `USB0` Mentor core!)*

---

## Phase 0 — Fix Existing Skeleton & Board Support Package (BSP)

**Goal**: Fix critical bugs found during reverse engineering and establish a clean LP-XDS110ET BSP in `firmware_src/`.

- [ ] **Fix Vector Table IRQ Alignment Bug in `firmware_src/src/startup_gcc.c`**:
  - Insert the missing **IRQ 39 (`CAN1`)** entry between `CAN0` (IRQ 38) and `Ethernet` (`EMAC0`, IRQ 40) so **`USB0` lands at vector index 58 (IRQ 42)**, `uDMA` at 60, `SSI1` at 50, `Timer3A` at 51, etc.
  - Verify all vector offsets against `firmware_src/sdk/ti/devices/msp432e4/inc/msp432e401y.h`.
- [ ] **Create `src/board_lp_xds110et.h` & `src/board.c`**:
  - Define every LP-XDS110ET pin from `rev-eng/HARDWARE_MAP.md` (`PD2` Red LED, `PD3` Green LED, `PK4..PK7` HW ID, `PA6` nSRST, `PC5` TMS_DIR, `PB2/PB3/PL0..5/PP2..5` USB3300 ULPI, `PB4/PB5/PE4/PE5` + `PM0/1/2/4/5` + `PN3` + `PC6` EnergyTrace HDR).
  - Update LED blink demo in `main.c` from `PN0/PN1` (EK-TM4C1294) to **`PD3` (Green) & `PD2` (Red)** (actual LP-XDS110ET LEDs!).
  - Add boot-time **double-reset DFU failsafe** (write magic cookie in `.noinit` SRAM for 500 ms at boot; if reset occurs within 500 ms, immediately jump to `0x0000002C` DFU bootloader!).
- [ ] **Verification**:
  - `make` compiles cleanly with 0 warnings.
  - Flash via `xds110-et dfu-flash`; confirm Green/Red LEDs blink and double-reset re-enters DFU mode (`1CBE:00FF`).

---

## Phase 1 — USB 2.0 High-Speed (ULPI `USB3300`) & Core XDS110 Control Channel

**Goal**: Enumerate as `0451:BEF3` at **USB 2.0 High-Speed (480 Mbps)** with Bulk 0 (CMD) + Bulk 1 (Data) and pass `xds110-et info` and `xds110-et dfu-flash`.

- [ ] **ULPI High-Speed PHY Initialization (`src/usb_ulpi.c`)**:
  - Auto-detect `USB3300` via `PP2` (`USBNXT` LOW with weak pull-up).
  - Mux all 12 ULPI pins (`PB2/3`, `PL0..5`, `PP2..5`) with 8 mA drive (`0x08`).
  - Enable `USB0` peripheral clock, configure `USB0` MUSB controller for **external ULPI High-Speed PHY** (`USB0->PC = USB_PC_ULPIEN`, `USB0->POWER |= USB_POWER_HSENAB`).
- [ ] **USB Device Stack (`src/usb_descriptors.c`, `src/usb_device.c`)**:
  - Integrate **TinyUSB (`dcd_musb`)** or a minimal clean MUSB HS device driver.
  - Implement Standard Config (`0xAA550001`, `VID=0x0451, PID=0xBEF3`):
    - Read 8-byte ASCII serial from `0x000FC000` (fallback `"NOserial"`).
    - Dynamic UTF-16LE product string `"XDS110 (03.00.00.99) OpenFW"`.
    - **Interface 2**: Vendor Bulk (OUT `0x02`, IN `0x83`, 512 B HS MPS) — XDS110 Command Port.
    - **Interface 6**: Vendor Bulk (OUT `0x06`, IN `0x87`, 512 B HS MPS) — EnergyTrace Data Port.
    - *(Placeholder interfaces 0/1, 3/4, 5 for CDC0, CDC1, HID so Windows `oem464.inf` WinUSB binding matches `MI_02` and `MI_06` out-of-the-box without Zadig!)*
- [ ] **XDS110 `0x2A` Command Framing & Core Opcodes (`src/xds_cmd.c`)**:
  - `0x00` `XDS_ECHO`
  - `0x01` `XDS_CONNECT`, `0x02` `XDS_DISCONNECT`, `0x28` `XDS_CONNECT_ET`
  - `0x03` `XDS_VERSION` → returns firmware version + `hw_version = 0x0023`
  - `0x08` `XDS_GET_POWER` → returns `3300` mV
  - `0x0E` `XDS_SET_SRST` → drives `PA6` (`XDS_RESET`)
  - `0x26` `XDS_ENTER_DFU` → executes exact DFU teardown & jumps to `*(uint32_t *)0x0000002C`
  - `0x4A` `XDS_PING`
- [ ] **Verification**:
  - `xds110-et info` prints firmware version, HW `0x23`, serial number.
  - `xds110-et reset` toggles `PA6` verified with scope/target LED.
  - `xds110-et dfu-flash` seamlessly transitions back to DFU mode via `CMD 0x26`.

---

## Phase 2 — EnergyTrace HDR 256 kS/s Power Measurement Engine (MVP Milestone!)

**Goal**: Achieve **100% drop-in compatibility** with `xds110-et` CLI/GUI live streaming and logging at **256 kS/s, 0 dropped frames**, with accurate calibration!

- [ ] **`ADS127L01` & Analog Switch Driver (`src/et_hdr.c`)**:
  - `et_adc_reset()`: pulse `PN3` (`ET_PN3`) LOW → HIGH.
  - `et_ssi1_init()`: configure `SSI1` (`PB5/PE4/PE5`) Motorola Mode 2 (`CPOL=1, CPHA=0`), 8-bit, 15 MHz.
  - `et_switches`: clean API for `PM0` (`ET_BYPASS`), `PM1` (`ET_CALIBRATE`), `PM2` (`ADS127L01 START`), `PM4` (`ET_CAL0` 40.2 mA), `PM5` (`ET_CAL1` 90.4 mA), `PC6` (`ET_CAL2` 1.5 mA).
- [ ] **Hardware Ping-Pong uDMA Pipeline (`src/et_dma.c`)**:
  - 1024-byte aligned uDMA control table in SRAM.
  - **uDMA Ch 5 (`UDMA_CH5_GPIOB`)**: `PB4` (`nDRDY`) falling-edge trigger, `ARB_4`, 1024 B ping-pong dummy TX → `SSI1_DR`.
  - **uDMA Ch 24 (`UDMA_CH24_SSI1RX`)**: `ARB_4`, 1024 B ping-pong `SSI1_DR` → pre-framed 1030-byte `0x68` packet buffers.
  - **96 KiB Lock-Free Ring Buffer** feeding Bulk 1 IN (`0x87`) with 512-byte HS USB transfers (or USB DMA).
- [ ] **Boot-Time Calibration Cache**:
  - Replicate exact boot sequence: assert `PA6` (`nSRST`), set `PM1=1`, `PM0=1`, wait 300 ms, measure `idx 0, 1, 2, 4` (5 × 256-sample averages, discarding first 256 settling samples per burst), cache sums, restore `PM0=0`, `PM1=0`, wait 300 ms, release `PA6`.
  - Optional improvement over TI FW: expose a custom vendor command (e.g., `0x70`) to trigger on-demand recalibration or report live temperature/VBUS!
- [ ] **EnergyTrace Protocol Opcodes**:
  - `0x46` `ET_HARDWARE_INFO` → `(u32 2, u8 0x07)`
  - `0x30` `ET_SETUP_RANGE`, `0x31` `ET_SETUP_DIG`, `0x44` `ET_SETUP_DIG_PARAMS`
  - `0x1D` `ET_SETUP`, `0x1E` `ET_CALIBRATE`, `0x1F` `ET_START`, `0x20` `ET_STOP`, `0x21` `ET_CLEANUP`
  - Emit `35 80 BB` sync header on `ET_START`, followed by 1030-byte `68 | seq | 00 04 | 00 00 | 256×u32be` packets.
- [ ] **Verification**:
  - Run `xds110-et live` and 60-second `xds110-et log` capture: verify **0 lost sequence numbers**, **256,000 samples/sec**, and current accuracy within **<1%** against a reference multimeter across 10 µA – 100 mA loads.
  - Compare calibration ADC values directly against `firmware_3.0.0.45.bin` on the same board!

---

## Phase 3 — Dual Virtual COM Ports (CDC ACM) & Aux GPIO

**Goal**: Full UART debugging and SWO serial output on both Virtual COM Ports.

- [ ] **CDC 0 — Application UART (`UART0` on `PA0/PA1`, Interface 0/1)**:
  - Dynamic baud rate / parity / stop-bit configuration via `SET_LINE_CODING`.
  - uDMA Ch 8 RX + TX FIFO interrupt/ring buffer (1 KiB+ each, or upgrade to 4 KiB for high-baud 921600/2M logs!).
- [ ] **CDC 1 — Auxiliary UART / SWO (`UART7` RX on `PC4`, Interface 3/4)**:
  - uDMA Ch 20 RX for high-speed SWO / auxiliary target serial.
- [ ] **Target Aux GPIOs (`0x29`, `0x2A`)**:
  - Support `PF0/PF1/PF2` (`XDS_GPIO1..3`) with level-shifter direction control (`PJ0/PJ1`).
- [ ] **Verification**:
  - Loopback test at 115,200 bps and 921,600 bps on `UART0` with zero byte drops.

---

## Phase 4 — SWD, JTAG, cJTAG & CMSIS-DAP Debugging

**Goal**: Make the open-source firmware a complete drop-in debug probe for **OpenOCD**, **probe-rs**, **pyOCD**, and **TI CCS**.

- [ ] **CMSIS-DAP 2.0 (WinUSB Bulk) & CMSIS-DAP 1.0 (HID, Interface 5)**:
  - Implement standard ARM CMSIS-DAP commands (`DAP_Info`, `DAP_Connect`, `DAP_SWJ_Clock`, `DAP_SWJ_Sequence`, `DAP_SWD_Configure`, `DAP_Transfer`, `DAP_TransferBlock`) using fast masked GPIO writes on `GPIO_PORTA_AHB` (`PA2=SWCLK`, `PA3=SWDIO`, `PC5=XDS_TMS_DIR`, `PA6=nSRST`).
  - *Advantage*: Immediately unlocks **probe-rs**, **pyOCD**, and **OpenOCD (`cmsis-dap`)** with clean, well-documented ARM standard specs!
- [ ] **Native XDS110 SWD & JTAG Opcodes**:
  - Implement `0x04..0x0D` (4-wire JTAG TAP engine using `GPIO_PORTA_AHB+0xB0`), `0x13..0x1C` (SWD DAP), and `0x32..0x38` (Cortex-M memory access) for compatibility with OpenOCD `xds110` driver and TI tools.
- [ ] **Verification**:
  - Attach, halt, flash, and step a target Cortex-M LaunchPad via `probe-rs` and `openocd`.

---

## Phase 5 — Open-Source Enhancements Beyond Proprietary Firmware

Once parity with `3.0.0.45` is verified, an open-source firmware can unlock features TI's closed firmware doesn't provide:

1. **Zero-Interference Continuous Calibration / Diagnostics**: Optional live readback of `VBUS_DET` (`PE3/AIN0`) and `ADS127L01` saturation/CRC flags (`STATUS` byte) + on-demand recalibration opcode.
2. **Higher-Speed SWO / RTT / Multi-Megabaud UART**: Leverage the 480 Mbps USB3300 HS-USB link for 2–4 Mbps UART logging without drops.
3. **Configurable EnergyTrace Decimation / Triggering**: Optional hardware-side trigger on GPIO (`PF0..PF2`) or current-threshold wake detection in firmware.
4. **USB WebUSB / MS OS 2.0 Descriptors**: Add `MSFT100` / BOS MS OS 2.0 descriptors so Windows binds WinUSB automatically on *any* machine without needing TI's `oem464.inf` installer!

---

## Recommended Source Tree for `firmware_src/src/`

```
firmware_src/src/
├── startup_gcc.c          # Fixed vector table (with CAN1 @ IRQ 39, USB0 @ IRQ 42)
├── main.c                 # Super-loop, DFU failsafe, subsystem dispatch
├── board_lp_xds110et.h    # Verified pinout & hardware constants (hw=0x23)
├── dfu_jump.c             # Safe USB teardown + 0x0000002C bootloader jump
├── usb/
│   ├── usb_ulpi.c         # USB3300-EZK ULPI PHY detection & MUSB HS init
│   ├── usb_descriptors.c  # 0451:BEF3 composite descriptors + flash serial reader
│   └── usb_endpoints.c    # Bulk0 (CMD), Bulk1 (ET 96 KiB ring), CDC0/1, HID
├── protocol/
│   ├── xds_cmd.c          # 0x2A packet framing & opcode dispatcher
│   └── cmsis_dap.c        # CMSIS-DAP 1.0/2.0 (Phase 4)
├── energytrace/
│   ├── et_hdr.c           # ADS127L01, switches, boot calibration, state machine
│   └── et_dma.c           # PB4 nDRDY -> uDMA Ch5 -> SSI1 -> uDMA Ch24 ping-pong
├── uart/
│   └── cdc_uart.c         # UART0 (PA0/PA1) & UART7 (PC4) uDMA bridge
└── debug/
    ├── swd.c              # Fast GPIO_PORTA_AHB SWD driver (Phase 4)
    └── jtag.c             # Fast GPIO_PORTA_AHB+0xB0 JTAG TAP driver (Phase 4)
```
