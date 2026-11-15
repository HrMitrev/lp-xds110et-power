#!/usr/bin/env python3
"""
Inspect XDS110 firmware binary files for MSP432E401Y / TM4C129x MCU.

Extracts:
- ARM Cortex-M vector table (Initial SP, Reset Handler, Exception Vectors)
- Firmware version markers matching the CMD_VERSION protocol format
- Hardware & USB descriptor strings
- Test Automation & EVM interface commands
- EnergyTrace & INA current-sense monitor commands
"""

import argparse
import re
import struct
from pathlib import Path

DEFAULT_FLASH_BASE = 0x00004000
VECTOR_NAMES = [
    "Initial SP",
    "Reset Handler",
    "NMI Handler",
    "HardFault Handler",
    "MemManage Handler",
    "BusFault Handler",
    "UsageFault Handler",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "SVCall Handler",
    "Debug Monitor Handler",
    "Reserved",
    "PendSV Handler",
    "SysTick Handler",
]


def parse_vector_table(data: bytes, flash_base: int):
    print(f"\n--- ARM Cortex-M Vector Table (Flash base: 0x{flash_base:08X}) ---")
    num_vectors = min(len(VECTOR_NAMES), len(data) // 4)
    vectors = struct.unpack(f"<{num_vectors}I", data[: num_vectors * 4])

    for i, (name, addr) in enumerate(zip(VECTOR_NAMES, vectors)):
        if name == "Reserved":
            continue
        if i == 0:
            print(f"  [{i:02d}] {name:<22}: 0x{addr:08X} (SRAM Top)")
        elif i == 1:
            file_offset = (addr & ~1) - flash_base
            print(
                f"  [{i:02d}] {name:<22}: 0x{addr:08X} (Entry offset: 0x{file_offset:06X})"
            )
        else:
            print(f"  [{i:02d}] {name:<22}: 0x{addr:08X}")


def find_firmware_version(data: bytes, flash_base: int):
    print("\n--- Firmware Version Marker (CMD_VERSION format) ---")
    # Matches pattern: [build (0x00-0xFF), patch (0x00), minor (0x00), major (0x03)]
    matches = []
    for m in re.finditer(rb"([\x00-\xff])\x00\x00\x03", data):
        offset = m.start()
        build = data[offset]
        patch = data[offset + 1]
        minor = data[offset + 2]
        major = data[offset + 3]
        flash_addr = flash_base + offset
        matches.append((offset, flash_addr, major, minor, patch, build))

    if matches:
        for offset, flash_addr, major, minor, patch, build in matches:
            print(
                f"  Found version at file offset 0x{offset:06X} (flash 0x{flash_addr:08X}): "
                f"{major}.{minor}.{patch}.{build:02x} (build hex: 0x{build:02X}, dec: {build})"
            )
    else:
        print("  No standard version pattern found.")


def extract_categorized_strings(data: bytes):
    raw_strings = [
        m.group(0).decode("latin1") for m in re.finditer(rb"[\x20-\x7e]{5,}", data)
    ]

    # Test automation build commit hash
    hashes = [s for s in raw_strings if "Test Automation Interface -" in s]
    if hashes:
        print("\n--- Test Automation Interface Build Hash ---")
        for h in hashes:
            print(f"  {h}")

    # USB Descriptors / Identifiers
    usb_keywords = ["xds110", "texas instruments", "cmsis-dap", "mass storage"]
    usb_strings = [
        s for s in raw_strings if any(k in s.lower() for k in usb_keywords)
    ]
    if usb_strings:
        print("\n--- USB & Hardware Identifiers ---")
        seen = set()
        for s in usb_strings:
            if s not in seen:
                seen.add(s)
                print(f"  {s}")

    # INA / Power Measurement Rails
    ina_keywords = ["ina", "shunt", "rail", "power", "temperature", "sampling"]
    ina_strings = [
        s
        for s in raw_strings
        if any(k in s.lower() for k in ina_keywords)
        and (":" in s or "%" in s or s.startswith("ina "))
    ]
    if ina_strings:
        print("\n--- INA / Power Rail Commands & Formats (Sample) ---")
        seen = set()
        for s in ina_strings[:15]:
            if s not in seen:
                seen.add(s)
                print(f"  {s}")

    # EVM Automation Commands
    auto_strings = [
        s for s in raw_strings if s.startswith("auto ") or "DUT" in s or "por" in s
    ]
    if auto_strings:
        print("\n--- EVM Automation Commands (Sample) ---")
        seen = set()
        for s in auto_strings[:15]:
            if s not in seen:
                seen.add(s)
                print(f"  {s}")


def inspect_file(fpath: Path, flash_base: int):
    if not fpath.exists():
        print(f"Error: File not found: {fpath}")
        return

    data = fpath.read_bytes()
    print("=" * 72)
    print(f"File: {fpath.name}")
    print(f"Path: {fpath.resolve()}")
    print(f"Size: {len(data):,} bytes ({len(data) / 1024:.2f} KiB)")
    print("=" * 72)

    parse_vector_table(data, flash_base)
    find_firmware_version(data, flash_base)
    extract_categorized_strings(data)


def main():
    parser = argparse.ArgumentParser(
        description="Inspect XDS110 firmware binaries for MSP432E401Y."
    )
    parser.add_argument(
        "files",
        nargs="*",
        type=Path,
        help="Path to .bin file(s) (default: firmware_3.0.0.45.bin and firmware_3.0.0.41.bin)",
    )
    parser.add_argument(
        "--flash-base",
        type=lambda x: int(x, 0),
        default=DEFAULT_FLASH_BASE,
        help="Flash base address for application firmware (default: 0x4000)",
    )

    args = parser.parse_args()

    # Default to analyzing firmware_3.0.0.45.bin and firmware_3.0.0.41.bin if no files passed
    files = args.files
    if not files:
        base_dir = Path(__file__).resolve().parents[2] / "firmware"
        default_files = [
            base_dir / "firmware_3.0.0.45.bin",
            base_dir / "firmware_3.0.0.41.bin",
        ]
        files = [f for f in default_files if f.exists()]

    if not files:
        print("No firmware files found to inspect.")
        return

    for f in files:
        inspect_file(f, args.flash_base)


if __name__ == "__main__":
    main()
