# Black Magic Debug Probe (BMP) Integration Roadmap

Engineering plan for integrating the upstream **Black Magic Debug** firmware (`https://github.com/blackmagic-debug/blackmagic`, **`main` branch** — *not* the default `archive` branch) into `firmware_src/` on **TI LP-XDS110ET** (`MSP432E401YTPDT`, 120 MHz Cortex-M4F, `hw_version = 0x23`), while preserving **EnergyTrace HDR (256 kS/s)**, **Target Application UART (`CDC0` / `ttyACM0`)**, **CMSIS-DAP v2**, and **Native XDS110 (`OpenOCD` / `DSLite`)**.

---

## 1. Hardware Constraints & Port Architecture Decision

### MSP432E401Y `USB0` (`MUSB`) Silicon Audit

The MSP432E401Y USB0 controller provides **7 hardware endpoints** (`EP1..EP7` + `EP0`) and **4,096 bytes of FIFO RAM**. Current utilization in `firmware_src/src/usb/usb_device.c`:

| HW Endpoint | IN (`0x8N`) | OUT (`0x0N`) |
|---|---|---|
| **EP 0** | Control (`64 B`) | Control (`64 B`) |
| **EP 1** | `0x81` CDC0 INT (`16 B`) | `0x01` CDC0 Bulk OUT (`512 B`) |
| **EP 2** | `0x82` CDC0 Bulk IN (`128 B`) | `0x02` XDS110 CMD OUT (`512 B`) |
| **EP 3** | `0x83` XDS110 CMD IN (`512 B`) | *(unused)* |
| **EP 4** | `0x84` CDC1 INT (`16 B`) | `0x04` CDC1 Bulk OUT (`256 B`) |
| **EP 5** | `0x85` CDC1 Bulk IN (`32 B`) | `0x05` CMSIS-DAP v2 OUT (`512 B`) |
| **EP 6** | `0x86` CMSIS-DAP v2 IN (`512 B`) | `0x06` EnergyTrace Data OUT (`512 B`) |
| **EP 7** | `0x87` EnergyTrace Data IN (`512 B`) | *(unused)* |

- **IN endpoints**: **7 / 7 used (`0` free)** — a new CDC ACM port requires **2 IN endpoints** (Interrupt IN + Bulk IN) + 1 OUT endpoint.
- **USB0 FIFO RAM**: **4,096 / 4,096 B used (`0 B` free)**.
- **Flash & SRAM headroom** (plenty of space for BMP!):
  - **Application Flash**: ~25 KiB / 992 KiB used (**~967 KiB free**; full BMP core + ARM Cortex-M targets fits in ~120–180 KiB).
  - **Internal SRAM**: ~132 KiB / 256 KiB used (**~124 KiB free**; plenty for BMP GDB packet buffers, heap allocations, and 16 KiB stack).

### Decision: Reuse `CDC1` (`if03` / `ttyACM1`) with Runtime + Compile-Time Mode Switching

Because 0 IN endpoints and 0 FIFO bytes remain, adding a 3rd CDC port is physically impossible without dropping CMSIS-DAP or EnergyTrace. Reusing **`CDC1` (`if03` / `ttyACM1`)** is strictly superior and matches a native Black Magic Probe's 2-port ergonomics:

| USB Interface | Linux Symlink | Normal Mode (`XDS_MODE_NORMAL`) | BMP Mode (`XDS_MODE_BMP`) |
|---|---|---|---|
| **IF 0 / 1 (`CDC0`)** | `...-if00 -> ttyACM0` | **Target App UART** (`UART0` `PA0/PA1`, up to 2 Mbps) | **Target App UART** (unchanged — acts as `ttyBmpTarg`!) |
| **IF 2 (`CMD`)** | XDS110 Bulk `0x02/0x83` | Control, EnergyTrace, Aux GPIO, OpenOCD/DSLite SWD | Control, **EnergyTrace HDR**, Aux GPIO, Mode Switch (SWD ops return busy) |
| **IF 3 / 4 (`CDC1`)** | `...-if03 -> ttyACM1` | Aux / SWO RX UART (`UART7` `PC4`, RX-only) | **Black Magic Debug GDB RSP Server** (`target extended-remote /dev/ttyACM1`) |
| **IF 5 (`DAP v2`)** | WinUSB Bulk `0x05/0x86` | CMSIS-DAP v2.0.0 SWD | Idle (returns `DAP_ERROR` on SWD connect while BMP owns pins) |
| **IF 6 (`DATA`)** | EnergyTrace Bulk `0x06/0x87` | **EnergyTrace HDR 256 kS/s** | **EnergyTrace HDR 256 kS/s** (runs concurrently with BMP GDB!) |

**Key benefits of reusing `CDC1` (`ttyACM1`)**:
- **Zero USB re-enumeration** when switching modes — active `ttyACM0` UART terminals and `xds110-et` EnergyTrace streams are never disrupted.
- **Full concurrency**: GDB debugging on `ttyACM1` + Target serial console on `ttyACM0` + 256 kS/s EnergyTrace HDR power capture simultaneously!
- **Same stable `/dev/serial/by-id/` symlinks** on Linux (and COM ports on Windows).

---

## 2. Mode Switching & Compile-Time Configuration

### Compile-Time Macros (`firmware_src/Makefile` & `src/bmp/bmp_mode.h`)
- `BMP_ENABLE ?= 1`: Enables compiling & linking the Black Magic Debug submodule (allows building a minimal non-BMP firmware with `BMP_ENABLE=0` and keeps GPLv3 licensing opt-in/explicit).
- `BMP_DEFAULT_MODE ?= 0`: Sets `-DFW_DEFAULT_BMP_MODE=$(BMP_DEFAULT_MODE)`:
  - `0` (default): Boots in **Normal Mode** (`CDC1` = Aux/SWO UART; SWD owned by CMSIS-DAP v2 / XDS110).
  - `1`: Boots directly in **BMP Mode** (`CDC1` = Black Magic GDB server; SWD owned by BMP).

### Runtime Mode Switch Mechanism
1. **XDS110 Vendor Opcode `0x63` (`XDS_CMD_PROBE_MODE`) on Interface 2**:
   - Chosen immediately past TI's `0x00..0x62` dispatch table (guaranteed zero conflict with TI `DSLite` / `uscif` / `OpenOCD`).
   - Request:
     - `args_len == 0` (or `args[0] == 0xFF`): Query current mode → returns `u8 mode` (`0` = Normal, `1` = BMP) + `u8 compiled_caps` (`bit 0` = BMP compiled in).
     - `args[0] == 0x00`: Switch to **Normal Mode**.
     - `args[0] == 0x01`: Switch to **BMP Mode** (returns `-122` if `BMP_ENABLE=0`).
   - Works immediately with existing Rust CLI (`xds110-et cmd 63 01` / `xds110-et cmd 63 00`), plus add a friendly subcommand `xds110-et mode [normal|bmp|status]`.
2. **GDB Monitor Command (convenience escape from BMP mode)**:
   - Register a platform monitor command (`monitor xds_normal` / `monitor dfu`) inside BMP so a GDB user can drop back to Normal Mode (or enter DFU) directly from GDB without needing `xds110-et`.
3. **Clean State Handoff**:
   - **Normal → BMP**: Disconnect active CMSIS-DAP / XDS110 SWD (`swd_disconnect()`), pause `UART7` → `CDC1` forwarding, flush `CDC1` RX/TX rings, initialize BMP GDB/SWD state.
   - **BMP → Normal**: Signal `gdb_if_getchar*()` to return EOF (`'\x04'`) so `gdb_main_loop()` exits cleanly, free BMP target list (`target_list_free()`), tri-state SWD pins (`swd_disconnect()`), flush `CDC1` RX/TX rings, resume `UART7` → `CDC1` forwarding.

---

## 3. Cooperative Super-Loop Integration (Zero Blocking!)

Black Magic Debug's `gdb_main_loop()` expects a blocking `gdb_if_getchar()` / `gdb_if_getchar_to(int timeout)` API, whereas our LP-XDS110ET firmware runs a bare-metal super-loop (`xds_cmd_poll()`, `cdc_uart_poll()`, LED status).

**Architecture to guarantee zero starvation of EnergyTrace, `UART0`, Mode Switch, or DFU**:
- Extract the background tick in `main.c` into a single reusable helper:
  ```c
  void app_background_poll(void); /* polls xds_cmd_poll(), cdc_uart_poll(), LEDs */
  ```
- In `main.c`:
  - In **Normal Mode**: run `app_background_poll()` + `cmsis_dap_poll()` + `__WFI()`.
  - In **BMP Mode**: enter `bmp_service_loop()`, which invokes BMP's `gdb_main_loop()` inside `TRY/CATCH` whenever BMP mode is active.
- Inside our BMP platform shims (`src/bmp/bmp_platform.c` & `src/bmp/bmp_mode.c`):
  - `gdb_if_getchar()` and `gdb_if_getchar_to(timeout)` continuously call `app_background_poll()` while waiting for bytes from `usb_cdc_rx_available(USB_CDC_PORT_AUX)` (calling `__WFI()` when idle).
  - If `bmp_mode_is_active()` becomes `false` (via opcode `0x63 00` on Interface 2!) or CDC1 `DTR` drops, `gdb_if_getchar*()` immediately returns `'\x04'` (EOF) so `gdb_main_loop()` unwinds cleanly back to `main.c`.
  - `platform_delay(ms)` also pumps `app_background_poll()` while waiting on `board_get_uptime_ms()`.
  - `gdb_if_putchar(c, flush)` buffers into a 64 B / 256 B TX chunk and flushes to `usb_cdc_tx_write(USB_CDC_PORT_AUX, ...)` (polling `app_background_poll()` if the 8 KiB CDC1 TX ring is momentarily full, with a disconnect/DTR abort guard so GDB unplug can never deadlock the probe!).
  - Hardware ISRs (`USB0` prio 2, `SSI1` / `GPIOB` EnergyTrace uDMA prio 1–2, `UART0` prio 1, `SysTick`) run completely unimpeded throughout.

---

## 4. Phased Implementation Plan

### Phase 7.0 — Upstream Fetch & Build System Isolation ✅
- [x] Added `firmware_src/scripts/fetch_bmp.py` cloning upstream Black Magic Debug **`main`** branch into `firmware_src/third_party/blackmagic` (git-ignored, matching `fetch_sdk.py`).
- [x] Updated `firmware_src/Makefile` with `BMP_ENABLE ?= 1`, `BMP_DEFAULT_MODE ?= 0`, auto-generated `build/bmp/version.h`, and dedicated `BMP_CFLAGS` (`-std=gnu11`) while keeping `APP_SRCS` strict `-std=c99 -Wall -Wextra`.
- [x] Updated `firmware_src/msp432e401y.ld` with `__heap_start__` / `__heap_end__` (16 KiB reserved stack, ~108 KiB heap) and stack-guarded `_sbrk()` in `src/bmp/bmp_platform.c`.

### Phase 7.1 — Mode Manager & `CDC1` Multiplexing ✅
- [x] Implemented `firmware_src/src/bmp/bmp_mode.{h,c}` with `FW_DEFAULT_BMP_MODE`, `bmp_mode_set()`, `bmp_mode_is_active()`, and cooperative `bmp_mode_poll()`.
- [x] Added opcode `0x63` (`XDS_CMD_PROBE_MODE`) in `xds_cmd.{h,c}`, gated Normal-mode SWD & `UART7` → `CDC1` forwarding, and added `usb_cdc_rx_flush()` / `usb_cdc_tx_flush()`.
- [x] Added `xds110-et mode [normal|bmp|status]` CLI subcommand and protocol/unit tests in `xds110-et/`.

### Phase 7.2 — BMP Platform Layer & SWD/JTAG TAP Bridge (`firmware_src/src/bmp/`) ✅
Consolidated into 5 cohesive files under `firmware_src/src/bmp/`:
- [x] **`bmp_mode.{h,c}`**: Runtime Normal/BMP mode manager and cooperative GDB super-loop bridge (replaces upstream `main.c`).
- [x] **`platform.h`**: BMP platform macros (`PLATFORM_IDENT "(LP-XDS110ET) "`, `NO_LIBOPENCM3`, LED & background-poll hooks).
- [x] **`bmp_platform.c`**: Platform HAL (`platform_nrst_*`, `platform_delay`, `platform_max_frequency_*`, `platform_target_clk_output_enable`, `platform_request_boot`), GDB RSP transport over `CDC1` (`gdb_if_*`), and stack-guarded `_sbrk` / nosys syscall stubs.
- [x] **`bmp_tap.c`**: Bit-masked `HWREG` SWD (`swd_proc`) and 4-wire JTAG (`jtag_proc`) TAP drivers on `PA2..PA5` + `PC5` (`U10` `TMS_DIR`), plus SWCLK-only `bmp_tap_clk_output_enable()`.

### Phase 7.3 — Hardware Verification & Coexistence Testing
- [ ] **Mode switching test**:
  - Boot in Normal Mode (`BMP_DEFAULT_MODE=0`) → verify `hil_stress_test.py` passes (CMSIS-DAP v2, EnergyTrace, UART0).
  - Switch via `xds110-et mode bmp` (`0x63 01`) → verify zero USB disconnect, `ttyACM1` responds to GDB `monitor version`.
  - Switch back via `xds110-et mode normal` (`0x63 00`) or GDB `monitor xds_normal` → verify OpenOCD / `DSLite` / CMSIS-DAP v2 resume cleanly.
  - Test `BMP_DEFAULT_MODE=1` build boots directly into BMP Mode.
- [ ] **Live GDB target debugging (STM32G0 & CC2340R5)**:
  - `arm-none-eabi-gdb -ex "target extended-remote /dev/ttyACM1" -ex "monitor swdp_scan" -ex "attach 1"`:
    - Target discovery (`monitor swdp_scan`), `attach 1`, `info reg`, SRAM read/write, Flash erase/load/compare-sections (STM32G0), hardware breakpoints, single-step, `monitor reset`, `detach`.
    - Test `connect_srst enable` (`monitor connect_srst enable`) and SWD frequency scaling (`monitor freq 4000000`).
- [ ] **Triple-concurrency stress test**:
  - Simultaneously run:
    1. **Black Magic GDB** on `/dev/ttyACM1` (repeated memory reads / flash verify / stepping),
    2. **Target App UART** on `/dev/ttyACM0` (1 Mbps continuous traffic), and
    3. **EnergyTrace HDR 256 kS/s** via `xds110-et capture` (`0 frames lost, 0 reconnects`).
- [ ] **Automated test script**: Add `firmware_src/tests/bmp_target_test.py` + `make -C firmware_src test-bmp` target driving `arm-none-eabi-gdb --batch`.

### Phase 7.4 — Optional Follow-Ups
- [ ] **4-Wire JTAG TAP for BMP**: Wire `PA2..PA5` JTAG engine (`bmp_jtagtap.c`) alongside Phase 5 JTAG support (`monitor jtag_scan`).
- [ ] **Persistent Default Mode in Unused Config Flash** *(optional — only if desired, keeping factory `0x000FC000` serial/config block untouched!)*: Or keep pure compile-time `BMP_DEFAULT_MODE` + runtime `0x63` switch for maximum DFU safety.
- [ ] **TI CC2340R5 Target Driver Upstream / Local**: Check whether BMP `main` includes CC23xx flash support (or generic Cortex-M0+ attach/RAM debug works out of the box, with CC23xx flash loader as a follow-up).

---

## 5. Risks & Licensing Notes

- **GPLv3 License**: Upstream `blackmagic-debug/blackmagic` is licensed under **GPL-3.0-or-later**. Keeping `BMP_ENABLE=0/1` in `Makefile` makes it explicit when GPLv3 BMP code is linked into `firmware.bin`.
- **Cortex-M Stack Depth**: BMP's `gdb_main` + flash stubs + `TRY`/`CATCH` (`setjmp`/`longjmp`) use ~4–6 KiB of stack; reserving 16 KiB stack in `msp432e401y.ld` + guarded `_sbrk()` prevents silent heap/stack collisions.
- **BMP `main` API Churn**: Because `main` is actively developed (post-v2.0), pinning a tested submodule commit hash ensures reproducible builds while allowing clean fast-forward updates later.
