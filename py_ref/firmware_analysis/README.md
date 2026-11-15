# Firmware Analysis Scripts

Python utilities for analyzing and comparing Texas Instruments XDS110 debug probe firmware binaries (`firmware_3.0.0.xx.bin`) targeted for the **MSP432E401YTPDT** microcontroller (ARM Cortex-M4F).

## Scripts

### 1. `inspect_firmware.py`
Inspects one or more firmware `.bin` files:
- Dumps the ARM Cortex-M interrupt vector table (Initial SP at top of 256 KiB SRAM, Reset Handler, Thumb entry offset).
- Discovers firmware version constants formatted per the probe's runtime `CMD_VERSION` protocol (`3.0.0.xx`).
- Extracts categorized text strings:
  - Test Automation build commit hash
  - USB descriptor and product identification strings
  - EVM test automation commands (`auto dut`, reset controls, `hold_por`, `release_por`)
  - EnergyTrace & INA current-sense monitor commands (`ina <rail> ...`)

**Usage:**
```bash
# Analyze default firmware binaries (firmware_3.0.0.45.bin and firmware_3.0.0.41.bin)
python inspect_firmware.py

# Inspect a specific firmware file with custom flash base (default is 0x4000)
python inspect_firmware.py ../../firmware/firmware_3.0.0.45.bin --flash-base 0x4000
```

### 2. `compare_firmware.py`
Compares two firmware binary revisions:
- Reports flash footprint delta.
- Identifies version differences.
- Verifies vector table alignment and handler addresses.
- Compares text strings and build commit hashes between revisions.
- Summarizes byte modifications across the common payload.

**Usage:**
```bash
# Compare firmware_3.0.0.41.bin against firmware_3.0.0.45.bin
python compare_firmware.py

# Compare arbitrary binaries
python compare_firmware.py ../../firmware/firmware_3.0.0.41.bin ../../firmware/firmware_3.0.0.43.bin
```
