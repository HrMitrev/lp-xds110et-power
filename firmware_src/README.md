# MSP432E401Y XDS110 Firmware Project

Bare-metal ARM Cortex-M4F firmware build environment for the **Texas Instruments MSP432E401Y** microcontroller on the LP-XDS110ET debug probe.

This project reconstructs the exact build structure, toolchain, and SDK used in official TI XDS110 firmware releases (`firmware_3.0.0.xx.bin`).

---

## Architecture & Memory Layout

The binary is designed to be flashed into the application partition via the XDS110 USB DFU bootloader:

| Memory Region | Address Range | Size | Description |
|---|---|---|---|
| **DFU Bootloader** | `0x0000_0000 - 0x0000_3FFF` | 16 KiB | Preserved primary ROM/Flash DFU bootloader |
| **Application Flash** | `0x0000_4000 - 0x000F_BFFF` | 992 KiB | **This firmware binary** |
| **Config / Serial** | `0x000F_C000 - 0x000F_FFFF` | 16 KiB | Serial number & probe configuration block |
| **Internal SRAM** | `0x2000_0000 - 0x2003_FFFF` | 256 KiB | Data, BSS, Heap & Stack (Initial SP = `0x2004_0000`) |

---

## Prerequisites

1. **GNU Arm Embedded Toolchain (`arm-none-eabi-gcc`)**:
   - You can either use a system-installed toolchain or run `make toolchain` to fetch the standalone **GCC 16 (`arm-none-eabi-gcc 16.2.0`)** toolchain automatically.
2. **GNU Make**:
   - Available via package manager or Scoop (`scoop install make`).
3. **Python 3**:
   - Used by the SDK and toolchain fetcher scripts.

---

## Project Structure

```
firmware_src/
├── Makefile                 # Master Makefile (autodetects local toolchain if present)
├── msp432e401y.ld           # GNU linker script (flash offset 0x4000, 256K SRAM)
├── README.md                # Project documentation
├── scripts/
│   ├── fetch_sdk.py         # Automated TI DriverLib & CMSIS downloader
│   └── fetch_toolchain.py   # Standalone GCC 16 toolchain fetcher from MSYS2
├── src/
│   ├── main.c              # Application entry point using ROM DriverLib
│   └── startup_gcc.c       # Cortex-M4 vector table & runtime init
├── sdk/ (auto-downloaded)
│   ├── third_party/CMSIS/   # ARM CMSIS Core headers
│   └── ti/devices/msp432e4/ # Official DriverLib source & headers
└── toolchain/ (optional)    # Standalone GCC 16 arm-none-eabi toolchain
```

---

## Build Commands

### 1. Fetch GCC 16 Toolchain (Optional)
Downloads and extracts the standalone GCC 16 toolchain (`16.2.0-1`) directly from MSYS2 UCRT64:
```bash
make toolchain
```
*(If present, `make` will automatically prioritize `firmware_src/toolchain/` over system PATH).*

### 2. Build Firmware
```bash
# Concise output (default)
make

# Verbose output (prints full command lines)
make V=1
# or
make VERBOSE=1
```
Automatically downloads the required SDK components (if not already present) and generates:
- `build/firmware.elf`
- `build/firmware.bin` (ready for DFU flashing)
- `build/firmware.hex`
- `build/firmware.map`

### 3. Verify Binary
Inspect the generated binary vector table and version markers using the analysis tool:
```bash
make inspect
```

### 3. Flash to Probe via DFU
Flash the compiled `.bin` file directly to a connected XDS110 probe:
```bash
make flash
```
*(Uses the workspace's native `xds110-et` DFU command).*

### 4. Clean Build
```bash
make clean
```
