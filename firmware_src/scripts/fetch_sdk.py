#!/usr/bin/env python3
"""
Fetch TI SimpleLink MSP432E4 DriverLib and CMSIS header files.
Extracts the official driverlib and device headers into sdk/.
"""

import io
import sys
import tarfile
import urllib.request
from pathlib import Path

ARCHIVE_URL = (
    "https://github.com/energia/msp432e-core/archive/refs/heads/master.tar.gz"
)
PREFIX = "msp432e-core-master/system/source/"


def fetch_sdk(dest_dir: Path):
    dest_dir.mkdir(parents=True, exist_ok=True)
    ti_dir = dest_dir / "ti" / "devices" / "msp432e4"

    if (
        (ti_dir / "driverlib" / "driverlib.h").exists()
        and (ti_dir / "inc" / "msp432e401y.h").exists()
        and (dest_dir / "third_party" / "CMSIS" / "Include" / "core_cm4.h").exists()
    ):
        print(f"SDK already exists in {dest_dir}. Skipping download.")
        return

    print(f"Downloading TI MSP432E4 SDK components from {ARCHIVE_URL}...")
    req = urllib.request.Request(
        ARCHIVE_URL, headers={"User-Agent": "Mozilla/5.0"}
    )
    with urllib.request.urlopen(req) as resp:
        tar_bytes = resp.read()

    print(
        f"Downloaded {len(tar_bytes):,} bytes. Extracting driverlib and inc..."
    )
    with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:gz") as tf:
        for member in tf.getmembers():
            if not member.name.startswith(PREFIX):
                continue

            rel_path = member.name[len(PREFIX) :]
            if not (
                rel_path.startswith("ti/devices/msp432e4/driverlib/")
                or rel_path.startswith("ti/devices/msp432e4/inc/")
                or rel_path.startswith("third_party/CMSIS/Include/")
            ):
                continue

            target_path = dest_dir / rel_path
            if member.isdir():
                target_path.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target_path.parent.mkdir(parents=True, exist_ok=True)
                with target_path.open("wb") as out_f:
                    f = tf.extractfile(member)
                    if f:
                        out_f.write(f.read())

    print(f"SDK successfully extracted to {dest_dir}.")


if __name__ == "__main__":
    dest = Path(__file__).resolve().parents[1] / "sdk"
    fetch_sdk(dest)
