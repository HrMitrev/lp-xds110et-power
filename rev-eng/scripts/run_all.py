#!/usr/bin/env python3
"""
run_all.py - Master runner for all XDS110 firmware_3.0.0.45.bin reverse-engineering scripts.
Regenerates all JSON analysis reports under rev-eng/output/.
"""

import subprocess
import sys
from pathlib import Path

SCRIPTS = [
    ["01_inspect_binary.py"],
    ["02_parse_usb_descriptors.py"],
    ["03_map_architecture_and_peripherals.py"],
    ["04_analyze_xds_protocol.py"],
    ["05_analyze_energytrace.py", "--section", "hdr"],
    ["06_analyze_boot_and_subsystems.py", "--show", "summary"],
    ["07_parse_schematic_pins.py"],
]


def main():
    root = Path(__file__).resolve().parent.parent.parent
    scripts_dir = Path(__file__).resolve().parent
    for entry in SCRIPTS:
        cmd = [sys.executable, str(scripts_dir / entry[0]), *entry[1:]]
        print(f"\n>>> Running {' '.join(entry)} ...")
        res = subprocess.run(cmd, cwd=root)
        if res.returncode != 0:
            print(f"ERROR: {entry[0]} exited with {res.returncode}", file=sys.stderr)
            sys.exit(res.returncode)
    print("\nAll reverse-engineering scripts completed successfully. Outputs in rev-eng/output/.")


if __name__ == "__main__":
    main()
