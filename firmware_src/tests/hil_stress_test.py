#!/usr/bin/env python3
"""
Hardware-in-the-Loop (HIL) stress test suite for LP-XDS110ET open-source firmware.

Verifies:
  1. Probe enumeration & version / EnergyTrace HDR hardware info (xds110-et info)
  2. Standalone UART0 (/dev/ttyACM0) loopback across baud rates & parity modes
     (requires a jumper bridging XDS_TX <-> XDS_RX on the LaunchPad header)
  3. Aux / SWO UART7 (/dev/ttyACM1) open, line coding, and non-blocking TX discard
  4. Concurrent EnergyTrace HDR 256 kS/s capture + high-speed UART0 loopback
     (verifies 0 lost EnergyTrace frames and 100% bit-exact UART transfers)

Uses Python standard library only (Linux / WSL2 termios).
"""

import argparse
import os
import select
import shutil
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path


BAUD_MAP = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
    230400: termios.B230400,
    460800: termios.B460800,
    921600: termios.B921600,
    1000000: termios.B1000000,
    2000000: termios.B2000000,
}


def resolve_xds110_et_cmd(override: str | None) -> list[str]:
    if override:
        return [override]
    repo_root = Path(__file__).resolve().parents[2]
    release_bin = repo_root / "xds110-et" / "target" / "release" / "xds110-et"
    if release_bin.is_file() and os.access(release_bin, os.X_OK):
        return [str(release_bin)]
    which_bin = shutil.which("xds110-et")
    if which_bin:
        return [which_bin]
    manifest = repo_root / "xds110-et" / "Cargo.toml"
    return ["cargo", "run", "--quiet", "--release", "--manifest-path", str(manifest), "--"]


def open_raw_serial(path: str, baud: int, parity: str = "N") -> int:
    if baud not in BAUD_MAP:
        raise ValueError(f"Unsupported baud rate: {baud}")
    baud_const = BAUD_MAP[baud]

    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag: raw input
    attrs[1] = 0  # oflag: raw output
    cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
    if parity == "E":
        cflag |= termios.PARENB
    elif parity == "O":
        cflag |= termios.PARENB | termios.PARODD
    elif parity != "N":
        os.close(fd)
        raise ValueError(f"Unsupported parity: {parity}")
    attrs[2] = cflag
    attrs[3] = 0  # lflag: non-canonical, no echo
    attrs[4] = baud_const
    attrs[5] = baud_const
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def uart_loopback(fd: int, payload: bytes, timeout_s: float = 3.0) -> tuple[bytes, int, float]:
    sent = 0
    rx = bytearray()
    t0 = time.monotonic()
    total = len(payload)

    while len(rx) < total and (time.monotonic() - t0) < timeout_s:
        wlist = [fd] if sent < total else []
        rlist, wready, _ = select.select([fd], wlist, [], 0.05)
        if wready and sent < total:
            try:
                sent += os.write(fd, payload[sent : sent + 512])
            except BlockingIOError:
                pass
        if rlist:
            try:
                chunk = os.read(fd, 4096)
                if chunk:
                    rx.extend(chunk)
            except BlockingIOError:
                pass

    dt = time.monotonic() - t0
    return bytes(rx), sent, dt


def make_pattern(size: int, step: int = 73, seed: int = 5) -> bytes:
    return bytes(((i * step + seed) & 0xFF) for i in range(size))


def test_probe_info(xds_cmd: list[str], expect_version: str | None) -> None:
    print("=== 1. Probe Info & Capabilities ===")
    proc = subprocess.run([*xds_cmd, "info"], capture_output=True, text=True, check=False)
    output = (proc.stdout + proc.stderr).strip()
    for line in output.splitlines():
        print(f"  {line}")
    if proc.returncode != 0:
        raise RuntimeError(f"xds110-et info failed with exit code {proc.returncode}")
    if "HW: 0x23" not in output:
        raise RuntimeError("Expected LP-XDS110ET hardware ID (HW: 0x23)")
    if "0x00000002" not in output:
        raise RuntimeError("Expected EnergyTrace HDR hardware type (0x00000002)")
    if expect_version and expect_version not in output:
        raise RuntimeError(f"Expected firmware version '{expect_version}' in probe info")


def test_uart_standalone(app_uart: str) -> None:
    print(f"\n=== 2. Standalone UART0 Loopback ({app_uart}) ===")

    # Quick sanity echo first to detect missing RX<->TX loopback jumper
    fd = open_raw_serial(app_uart, 115200, "N")
    time.sleep(0.02)
    probe_msg = b"LP-XDS110ET UART0 HIL LOOPBACK\r\n"
    rx, sent, dt = uart_loopback(fd, probe_msg, timeout_s=1.0)
    os.close(fd)
    print(f"  [115200 8N1 ASCII     32B] sent={sent:5d} rx={len(rx):5d} match={rx == probe_msg} dt={dt*1000:6.1f}ms")
    if len(rx) == 0:
        raise RuntimeError(
            f"No echo received on {app_uart}. Double-check that a jumper bridges XDS_TX <-> XDS_RX."
        )
    if rx != probe_msg:
        raise RuntimeError(f"ASCII loopback mismatch: expected {probe_msg!r}, got {rx!r}")

    cases: list[tuple[str, int, str, bytes]] = [
        ("115200 8N1 00..FF", 115200, "N", bytes(range(256))),
        ("9600 8N1", 9600, "N", make_pattern(256, 31, 7)),
        ("115200 8N1", 115200, "N", make_pattern(4096, 37, 13)),
        ("115200 8E1", 115200, "E", make_pattern(512, 41, 3)),
        ("115200 8O1", 115200, "O", make_pattern(512, 43, 9)),
        ("921600 8N1", 921600, "N", make_pattern(16384, 73, 5)),
        ("1000000 8N1", 1000000, "N", make_pattern(16384, 89, 17)),
        ("2000000 8N1", 2000000, "N", make_pattern(32768, 97, 23)),
    ]

    for label, baud, parity, payload in cases:
        fd = open_raw_serial(app_uart, baud, parity)
        time.sleep(0.02)
        rx, sent, dt = uart_loopback(fd, payload, timeout_s=3.5)
        os.close(fd)
        ok = rx == payload
        bps = (len(rx) * 10.0 / dt) if dt > 0 else 0.0
        print(f"  [{label:17s} {len(payload):5d}B] sent={sent:5d} rx={len(rx):5d} match={ok} dt={dt*1000:6.1f}ms ({bps:8.0f} bps)")
        if not ok:
            first_diff = next((i for i in range(min(len(rx), len(payload))) if rx[i] != payload[i]), None)
            raise RuntimeError(f"UART0 loopback failed at {label}: rx={len(rx)}/{len(payload)}, first_diff={first_diff}")


def test_aux_uart(aux_uart: str) -> None:
    print(f"\n=== 3. Aux / SWO UART7 Non-Blocking Drain ({aux_uart}) ===")
    fd = open_raw_serial(aux_uart, 115200, "N")
    payload = b"AUX_TX_DISCARD_TEST" * 100
    written = os.write(fd, payload)
    time.sleep(0.05)
    os.close(fd)
    print(f"  [{aux_uart}] open/SET_LINE_CODING/write-discard OK ({written} bytes)")


def test_concurrent_et_and_uart(xds_cmd: list[str], app_uart: str, duration_s: int, run_uart: bool) -> None:
    label = "Concurrent EnergyTrace HDR (256 kS/s) + UART0" if run_uart else "EnergyTrace HDR (256 kS/s)"
    print(f"\n=== 4. {label} ({duration_s}s) ===")

    with tempfile.NamedTemporaryFile(prefix="et_hil_", suffix=".csv", delete=False) as tmp:
        csv_path = tmp.name

    try:
        et_proc = subprocess.Popen(
            [*xds_cmd, "capture", "-t", str(duration_s), "-o", csv_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        if run_uart:
            time.sleep(0.7)
            for label_baud, baud, size in [
                ("115200 8N1", 115200, 8192),
                ("921600 8N1", 921600, 65536),
                ("1000000 8N1", 1000000, 65536),
            ]:
                fd = open_raw_serial(app_uart, baud, "N")
                payload = make_pattern(size, 53, 11)
                rx, sent, dt = uart_loopback(fd, payload, timeout_s=4.0)
                os.close(fd)
                ok = rx == payload
                bps = (len(rx) * 10.0 / dt) if dt > 0 else 0.0
                print(
                    f"  [UART0 {label_baud:11s} {size:5d}B during ET] "
                    f"sent={sent:5d} rx={len(rx):5d} match={ok} dt={dt*1000:6.1f}ms ({bps:8.0f} bps)"
                )
                if not ok:
                    et_proc.terminate()
                    et_proc.wait(timeout=5)
                    first_diff = next((i for i in range(min(len(rx), len(payload))) if rx[i] != payload[i]), None)
                    raise RuntimeError(
                        f"UART0 {label_baud} failed during EnergyTrace: rx={len(rx)}/{size}, first_diff={first_diff}"
                    )

        et_out, _ = et_proc.communicate(timeout=duration_s + 15)
        for line in et_out.strip().splitlines():
            if any(k in line for k in ("Calibration:", "Capture finished:", "Current:", "Error")):
                print(f"  [ET] {line}")

        if et_proc.returncode != 0:
            raise RuntimeError(f"EnergyTrace capture exited with code {et_proc.returncode}")
        if "0 frames lost" not in et_out:
            raise RuntimeError("EnergyTrace capture reported lost frames")
        if "0 reconnects" not in et_out:
            raise RuntimeError("EnergyTrace capture reported USB reconnects")
    finally:
        if os.path.exists(csv_path):
            os.remove(csv_path)


def main() -> int:
    parser = argparse.ArgumentParser(description="LP-XDS110ET HIL stress test suite")
    parser.add_argument("--app-uart", default="/dev/ttyACM0", help="Application UART CDC device (default: /dev/ttyACM0)")
    parser.add_argument("--aux-uart", default="/dev/ttyACM1", help="Aux/SWO UART CDC device (default: /dev/ttyACM1)")
    parser.add_argument("--xds110-et", default=None, help="Path to xds110-et CLI binary")
    parser.add_argument("--et-duration", type=int, default=4, help="EnergyTrace capture duration in seconds (default: 4)")
    parser.add_argument("--expect-version", default=None, help="Expected firmware version string (e.g. 3.0.0.02)")
    parser.add_argument("--skip-uart", action="store_true", help="Skip UART loopback tests")
    parser.add_argument("--skip-et", action="store_true", help="Skip EnergyTrace capture tests")
    args = parser.parse_args()

    xds_cmd = resolve_xds110_et_cmd(args.xds110_et)

    try:
        test_probe_info(xds_cmd, args.expect_version)
        if not args.skip_uart:
            test_uart_standalone(args.app_uart)
            test_aux_uart(args.aux_uart)
        if not args.skip_et:
            test_concurrent_et_and_uart(xds_cmd, args.app_uart, args.et_duration, run_uart=not args.skip_uart)
    except Exception as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        return 1

    print("\nALL HIL STRESS TESTS PASSED!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
