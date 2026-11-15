#!/usr/bin/env python3
"""
Compare two XDS110 firmware binaries (e.g. firmware_3.0.0.41.bin vs firmware_3.0.0.45.bin).

Compares:
- File sizes and flash footprint
- Vector table entries (Reset Handler, exception vectors)
- Version markers
- Added and removed strings (features, commands, test automation hashes)
- Byte similarity / block diff summary
"""

import argparse
import difflib
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


def extract_strings(data: bytes, min_len: int = 5) -> set[str]:
    return set(
        m.group(0).decode("latin1")
        for m in re.finditer(rb"[\x20-\x7e]{" + str(min_len).encode() + rb",}", data)
    )


def extract_version_constant(data: bytes) -> tuple[int, int, int, int] | None:
    # Look for literal pool constant 0x030000xx (major=3, minor=0, patch=0)
    for m in re.finditer(rb"([\x00-\xff])\x00\x00\x03", data):
        build = m.group(1)[0]
        # In XDS110 binaries, the version constant is around offset 0x2AB0 - 0x2AC0
        if 0x2000 <= m.start() <= 0x3000:
            return (3, 0, 0, build)
    return None


def compare_vectors(
    data1: bytes, data2: bytes, flash_base: int
) -> list[tuple[str, int, int]]:
    n = min(len(VECTOR_NAMES), len(data1) // 4, len(data2) // 4)
    v1 = struct.unpack(f"<{n}I", data1[: n * 4])
    v2 = struct.unpack(f"<{n}I", data2[: n * 4])

    diffs = []
    for name, a1, a2 in zip(VECTOR_NAMES, v1, v2):
        if name != "Reserved" and a1 != a2:
            diffs.append((name, a1, a2))
    return diffs


def compare_files(path1: Path, path2: Path, flash_base: int):
    if not path1.exists() or not path2.exists():
        print(f"Error: Could not find both files: {path1} and {path2}")
        return

    d1 = path1.read_bytes()
    d2 = path2.read_bytes()

    print("=" * 76)
    print(f"Comparing Firmware Binaries:")
    print(f"  [A] {path1.name} ({len(d1):,} bytes, {len(d1) / 1024:.2f} KiB)")
    print(f"  [B] {path2.name} ({len(d2):,} bytes, {len(d2) / 1024:.2f} KiB)")
    delta_size = len(d2) - len(d1)
    sign = "+" if delta_size >= 0 else ""
    print(f"  Delta: {sign}{delta_size:,} bytes")
    print("=" * 76)

    # Version comparison
    v1 = extract_version_constant(d1)
    v2 = extract_version_constant(d2)
    print("\n--- Firmware Versions ---")
    s1 = f"{v1[0]}.{v1[1]}.{v1[2]}.{v1[3]:02x}" if v1 else "Unknown"
    s2 = f"{v2[0]}.{v2[1]}.{v2[2]}.{v2[3]:02x}" if v2 else "Unknown"
    print(f"  [A] {s1}")
    print(f"  [B] {s2}")

    # Vector table comparison
    print("\n--- Vector Table Comparison ---")
    vdiffs = compare_vectors(d1, d2, flash_base)
    if not vdiffs:
        print("  Vector tables are identical.")
    else:
        for name, a1, a2 in vdiffs:
            print(f"  {name:<22}: [A] 0x{a1:08X}  ->  [B] 0x{a2:08X}")

    # Strings comparison
    strings1 = extract_strings(d1, min_len=6)
    strings2 = extract_strings(d2, min_len=6)

    added = sorted(strings2 - strings1)
    removed = sorted(strings1 - strings2)

    # Filter out obvious binary noise / random ASCII artifacts
    def is_meaningful_string(s: str) -> bool:
        return any(
            c.isalpha() for c in s
        ) and not re.search(r"^[^\w\s]+$", s) and len(s) >= 8

    added_meaningful = [s for s in added if is_meaningful_string(s)]
    removed_meaningful = [s for s in removed if is_meaningful_string(s)]

    print(f"\n--- Strings Added in [B] ({len(added_meaningful)} notable) ---")
    for s in added_meaningful[:25]:
        print(f"  + {s}")

    print(f"\n--- Strings Removed in [B] ({len(removed_meaningful)} notable) ---")
    for s in removed_meaningful[:25]:
        print(f"  - {s}")

    # Byte diff summary
    common_len = min(len(d1), len(d2))
    diff_count = sum(1 for i in range(common_len) if d1[i] != d2[i])
    pct_diff = (diff_count / common_len) * 100.0
    print(f"\n--- Byte Differences Summary ---")
    print(
        f"  Over common {common_len:,} bytes: {diff_count:,} bytes differ ({pct_diff:.2f}% modified)"
    )


def main():
    parser = argparse.ArgumentParser(
        description="Compare two XDS110 firmware binaries."
    )
    parser.add_argument(
        "file1",
        nargs="?",
        type=Path,
        help="Path to first firmware file (default: firmware/firmware_3.0.0.41.bin)",
    )
    parser.add_argument(
        "file2",
        nargs="?",
        type=Path,
        help="Path to second firmware file (default: firmware/firmware_3.0.0.45.bin)",
    )
    parser.add_argument(
        "--flash-base",
        type=lambda x: int(x, 0),
        default=DEFAULT_FLASH_BASE,
        help="Flash base address (default: 0x4000)",
    )

    args = parser.parse_args()
    base_dir = Path(__file__).resolve().parents[2] / "firmware"

    f1 = args.file1 or (base_dir / "firmware_3.0.0.41.bin")
    f2 = args.file2 or (base_dir / "firmware_3.0.0.45.bin")

    compare_files(f1, f2, args.flash_base)


if __name__ == "__main__":
    main()
