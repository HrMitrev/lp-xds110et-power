# LP-XDS110ET Open-Source Firmware (`MSP432E401Y`)

Clean-room, bare-metal ARM Cortex-M4F open-source firmware for the **Texas Instruments LP-XDS110ET** (`MCU101 Rev B`, MCU: `MSP432E401YTPDT` @ 120 MHz, `hw_version = 0x23`).

---

## Implemented Features (Hardware Verified)

- **DFU-Safe Boot & Failsafe**:
  - Application resides at `0x00004000` (preserving the 16 KiB TI USB DFU bootloader at `0x00000000` and the 16 KiB factory serial/config block at `0x000FC000`).
  - Double-reset within 500 ms (via `.noinit` SRAM cookie) and XDS110 opcode `0x26` (`XDS_ENTER_DFU`) both cleanly tear down USB and re-enter the TI Flash DFU bootloader (`1CBE:00FF`).
- **USB 2.1 High-Speed Composite Stack (`USB3300-EZK` ULPI PHY, `0451:BEF3`)**:
  - **Interfaces 0 / 1 (CDC0)**: Application UART (`UART0` on `PA0`/`PA1`, full-duplex, verified bit-exact up to **2,000,000 bps** concurrently with EnergyTrace).
  - **Interface 2 (Vendor Bulk `0x02`/`0x83`)**: XDS110 `0x2A` command channel (session control, `nSRST`, DFU jump, EnergyTrace control, Target Aux GPIOs, and **native OpenOCD `interface/xds110.cfg` SWD**).
  - **Interfaces 3 / 4 (CDC1)**: Auxiliary / SWO UART (`UART7` RX-only on `PC4`; `PC5` preserved for `U10` `XDS_TMS_DIR`).
  - **Interface 5 (Vendor Bulk `0x05`/`0x86`)**: **CMSIS-DAP v2.0.0 WinUSB Bulk** (`512 B` HS / `64 B` FS, 4-deep queues, USB 2.1 BOS + Microsoft OS 2.0 Descriptors for zero-driver WinUSB binding, Atomic Commands `0x7E`/`0x7F`, SWD).
  - **Interface 6 (Vendor Bulk `0x06`/`0x87`)**: EnergyTrace HDR 256 kS/s data stream (64 KiB ring buffer) + byte-reversal loopback self-test when idle.
- **EnergyTrace HDR 256 kS/s Engine**:
  - Hardware-timed `ADS127L01` (`PB4` `nDRDY` → uDMA Ch 5 → `SSI1` Mode 1 @ 15 MHz → uDMA Ch 24 ping-pong) streaming `0x68` framed packets with boot-cached calibration (`idx 0, 1, 2, 4`) and zero dropped frames concurrently with UART0 or SWD debugging.
- **Target Aux GPIOs (`0x29` / `0x2A`)**:
  - Glitch-free control of `PF0` (`GPIO3`), `PF1` (`GPIO1`, `U49` dir `PJ0`), and `PF2` (`GPIO2`, `U49` dir `PJ1`), plus probe-identify LED blink (`0x80000010`).
- **Dual SWD Debug Transports (DWT Cycle-Accurate SWCLK)**:
  - **CMSIS-DAP v2 WinUSB (Interface 5)**: Out-of-the-box support for **OpenOCD** (`interface/cmsis-dap.cfg`), **probe-rs**, and **pyOCD** — runs concurrently with EnergyTrace HDR (`Interface 2`/`6`) with zero USB contention.
  - **Native XDS110 SWD (Interface 2)**: Drop-in compatibility with OpenOCD's **`interface/xds110.cfg`** (`0x04`, `0x05`, `0x07`, `0x0E`, `0x0F..0x12`, `0x15..0x1A`, `0x2B..0x2C`, `0x3A OCD_DAP_REQUEST`), including `connect_assert_srst` and ADIv5.2 Dormant-to-SWD wake. *(Note: `interface/xds110.cfg` claims Interface 2, so use `interface/cmsis-dap.cfg` when running EnergyTrace simultaneously.)*

---

## Memory Layout

| Memory Region | Address Range | Size | Description |
|---|---|---|---|
| **DFU Bootloader** | `0x0000_0000 - 0x0000_3FFF` | 16 KiB | Preserved primary TI USB DFU bootloader |
| **Application Flash** | `0x0000_4000 - 0x000F_BFFF` | 992 KiB | **This firmware binary** (~22 KiB used) |
| **Config / Serial** | `0x000F_C000 - 0x000F_FFFF` | 16 KiB | Factory 8-char serial number & config word |
| **Internal SRAM** | `0x2000_0000 - 0x2003_FFFF` | 256 KiB | Data, BSS, Ring Buffers, uDMA & Stack |

---

## Project Structure

```
firmware_src/
├── Makefile                        # Build, format, clangd-check, DFU flash, HIL & SWD test targets
├── msp432e401y.ld                  # Linker script (flash offset 0x4000, 256 KiB SRAM)
├── scripts/
│   ├── fetch_sdk.py                # Automated TI MSP432E4 DriverLib & CMSIS downloader
│   ├── fetch_toolchain.py          # Optional standalone GCC 16 arm-none-eabi fetcher
│   └── gen_compile_commands.py     # Generates compile_commands.json for clangd
├── src/
│   ├── main.c                      # Super-loop, version (3.0.0.02), subsystem dispatch
│   ├── startup_gcc.c               # Cortex-M4F vector table (CAN1 @ IRQ 39, USB0 @ IRQ 42)
│   ├── board_lp_xds110et.h / .c    # LP-XDS110ET BSP (120 MHz PLL, LEDs, HW ID, nSRST, SysTick)
│   ├── dfu_jump.h / .c             # Double-reset failsafe & safe DFU bootloader jump
│   ├── usb/
│   │   ├── usb_descriptors.h / .c  # USB 2.1 HS composite + BOS / MS OS 2.0 WinUSB descriptors
│   │   └── usb_device.h / .c       # MUSB HS ULPI stack, 4 KiB FIFO layout, Bulk/CDC/DAP queues
│   ├── protocol/
│   │   └── xds_cmd.h / .c          # XDS110 0x2A framing & opcode dispatcher
│   ├── energytrace/
│   │   └── et_hdr.h / .c           # ADS127L01 SPI + uDMA Ch5/Ch24 ping-pong + boot cal cache
│   ├── uart/
│   │   └── cdc_uart.h / .c         # UART0 (CDC0) & UART7 RX (CDC1) prio-1 ISR bridges
│   ├── gpio/
│   │   └── aux_gpio.h / .c         # Target Aux GPIOs (PF0..PF2, U49 PJ0/PJ1) & LED identify
│   └── dap/
│       ├── swd.h / .c              # DWT-paced SWD wire engine (PA2/PA3/PA6 + PC5 TMS_DIR)
│       ├── cmsis_dap.h / .c        # CMSIS-DAP v2.0.0 WinUSB Bulk + Atomic Commands (0x7E/0x7F)
│       └── xds_swd.h / .c          # Native XDS110 Interface 2 SWD (CMAPI + OCD_DAP_REQUEST 0x3A)
└── tests/
    ├── hil_stress_test.py          # Probe HIL suite (info, Aux GPIO, DAPv2/BOS, UART0/7, ET)
    └── swd_target_test.py          # Live SWD target suite (OpenOCD DAPv2 & xds110, probe-rs, pyocd, ET coex)
```

---

## Build, Flash & Test Commands

```bash
# Build firmware (auto-fetches MSP432E4 DriverLib SDK on first run)
make -C firmware_src

# Format & static analysis
make -C firmware_src clang-format
make -C firmware_src clangd-check

# Flash directly to connected LP-XDS110ET via native Rust DFU tool
make -C firmware_src flash

# Run probe HIL stress suite (requires P10 TX<->RX & GPIO1<->GPIO2 jumpers)
make -C firmware_src test-hil

# Run live SWD target verification suite (default: STM32G0 via target/stm32g0x.cfg)
make -C firmware_src test-swd
make -C firmware_src test-swd SWD_TARGET_CFG=target/stm32f4x.cfg
```

### Using with Host Debuggers

```bash
# OpenOCD via CMSIS-DAP v2 WinUSB (allows concurrent EnergyTrace capture)
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32g0x.cfg

# OpenOCD via native XDS110 driver (drop-in for existing XDS110 scripts)
openocd -f interface/xds110.cfg -c "transport select swd" -f target/stm32g0x.cfg

# probe-rs / pyOCD (auto-detects CMSIS-DAP v2 WinUSB)
probe-rs info --speed 2000
pyocd list
```
