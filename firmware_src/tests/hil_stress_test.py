#!/usr/bin/env python3
"""
Hardware-in-the-Loop (HIL) stress test suite for LP-XDS110ET open-source firmware.

Verifies:
  1. Probe enumeration & version / EnergyTrace HDR hardware info (xds110-et info)
  2. Target Aux GPIOs (opcodes 0x29 / 0x2A) across PF0..PF2 & U49 PJ0/PJ1 level
     shifters (requires a jumper bridging P10 pin 16 GPIO2 <-> pin 18 GPIO1)
  3. Standalone UART0 (/dev/ttyACM0) loopback across baud rates & parity modes
     (requires a jumper bridging P10 pin 7 XDS_TX <-> pin 9 XDS_RX)
  4. Aux / SWO UART7 (/dev/ttyACM1) open, line coding, and non-blocking TX discard
  5. Concurrent EnergyTrace HDR 256 kS/s capture + high-speed UART0 loopback
     (verifies 0 lost EnergyTrace frames and 100% bit-exact UART transfers)

Requires: python-pyusb (usb.core / usb.util) on Linux / WSL2.
"""

import argparse
import os
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

import usb.core
import usb.util


XDS_VID = 0x0451
XDS_PID = 0xBEF3
XDS_CMD_IFACE = 2
XDS_CMD_EP_OUT = 0x02
XDS_CMD_EP_IN = 0x83

XDS_CMD_GPIO_GET = 0x29
XDS_CMD_GPIO_SET = 0x2A
GPIO_SUBCMD_VALUE = 0x80000000
GPIO_SUBCMD_DIR = 0x80000001
GPIO_SUBCMD_BLINK_LED = 0x80000010

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


class XdsCmdUsb:
    """PyUSB bulk command transport for XDS110 Interface 2 (0x02 / 0x83)."""

    def __init__(self, vid: int = XDS_VID, pid: int = XDS_PID):
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is None:
            raise RuntimeError(f"XDS110 USB device {vid:04x}:{pid:04x} not found")
        self.dev = dev
        if dev.is_kernel_driver_active(XDS_CMD_IFACE):
            dev.detach_kernel_driver(XDS_CMD_IFACE)
        usb.util.claim_interface(dev, XDS_CMD_IFACE)

    def close(self) -> None:
        if self.dev is not None:
            try:
                usb.util.release_interface(self.dev, XDS_CMD_IFACE)
            except usb.core.USBError:
                pass
            usb.util.dispose_resources(self.dev)
            self.dev = None

    def __enter__(self) -> "XdsCmdUsb":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def cmd(self, opcode: int, args: bytes = b"", timeout_ms: int = 2000) -> tuple[int, bytes]:
        payload = bytes([opcode]) + args
        frame = struct.pack("<BH", 0x2A, len(payload)) + payload
        self.dev.write(XDS_CMD_EP_OUT, frame, timeout=timeout_ms)
        resp = bytes(self.dev.read(XDS_CMD_EP_IN, 512, timeout=timeout_ms))
        if len(resp) < 7 or resp[0] != 0x2A:
            raise RuntimeError(f"Malformed XDS response to 0x{opcode:02X}: {resp.hex()}")
        resp_len = struct.unpack_from("<H", resp, 1)[0]
        err = struct.unpack_from("<i", resp, 3)[0]
        return err, resp[7 : 3 + resp_len]


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


def test_aux_gpio(blink_identify: bool) -> None:
    print("\n=== 2. Target Aux GPIOs (Opcodes 0x29 / 0x2A, P10:16 GPIO2 <-> P10:18 GPIO1) ===")

    def gpio_get(usb_dev: XdsCmdUsb, subcmd: int) -> int:
        err, data = usb_dev.cmd(XDS_CMD_GPIO_GET, struct.pack("<I", subcmd))
        if err != 0 or len(data) != 2:
            raise RuntimeError(f"XDS_GPIO_GET(0x{subcmd:08X}) failed: err={err}, len={len(data)}")
        return struct.unpack("<H", data)[0]

    def gpio_set(usb_dev: XdsCmdUsb, subcmd: int, mask: int, val: int, timeout_ms: int = 2000) -> None:
        mask_val = ((mask & 0xFF) << 8) | (val & 0xFF)
        err, _ = usb_dev.cmd(XDS_CMD_GPIO_SET, struct.pack("<IH", subcmd, mask_val), timeout_ms=timeout_ms)
        if err != 0:
            raise RuntimeError(f"XDS_GPIO_SET(0x{subcmd:08X}, 0x{mask_val:04X}) failed: err={err}")

    with XdsCmdUsb() as usb_dev:
        try:
            # Start clean: all 3 pins as safe inputs
            gpio_set(usb_dev, GPIO_SUBCMD_DIR, 0x07, 0x00)
            dir_mask = gpio_get(usb_dev, GPIO_SUBCMD_DIR)
            if dir_mask != 0x00:
                raise RuntimeError(f"Expected initial DIR=0x00, got 0x{dir_mask:02X}")

            # Direction A: GPIO1 (bit 1, PF1, PJ0=HIGH) output -> GPIO2 (bit 2, PF2, PJ1=LOW) input
            gpio_set(usb_dev, GPIO_SUBCMD_DIR, 0x07, 0x02)
            dir_mask = gpio_get(usb_dev, GPIO_SUBCMD_DIR)
            if dir_mask != 0x02:
                raise RuntimeError(f"Expected DIR=0x02, got 0x{dir_mask:02X}")

            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x02, 0x00)
            v_lo = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x06
            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x02, 0x02)
            v_hi = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x06
            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x02, 0x00)
            v_lo2 = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x06
            print(f"  [GPIO1 -> GPIO2] low=0x{v_lo:02X} high=0x{v_hi:02X} low=0x{v_lo2:02X}")
            if (v_lo, v_hi, v_lo2) != (0x00, 0x06, 0x00):
                raise RuntimeError(
                    f"GPIO1->GPIO2 loopback failed (got {v_lo:#x}/{v_hi:#x}/{v_lo2:#x}). "
                    "Double-check that a jumper bridges P10 pin 16 (GPIO2) <-> pin 18 (GPIO1)."
                )

            # Verify writing to an input pin (GPIO2) is ignored
            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x04, 0x04)
            v_prot = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x06
            if v_prot != 0x00:
                raise RuntimeError(f"Write to input pin GPIO2 unexpectedly changed state: 0x{v_prot:02X}")

            # Direction B: GPIO2 (bit 2, PF2, PJ1=HIGH) output -> GPIO1 (bit 1, PF1, PJ0=LOW) input
            gpio_set(usb_dev, GPIO_SUBCMD_DIR, 0x07, 0x04)
            dir_mask = gpio_get(usb_dev, GPIO_SUBCMD_DIR)
            if dir_mask != 0x04:
                raise RuntimeError(f"Expected DIR=0x04, got 0x{dir_mask:02X}")

            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x04, 0x00)
            v_lo = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x06
            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x04, 0x04)
            v_hi = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x06
            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x04, 0x00)
            v_lo2 = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x06
            print(f"  [GPIO2 -> GPIO1] low=0x{v_lo:02X} high=0x{v_hi:02X} low=0x{v_lo2:02X}")
            if (v_lo, v_hi, v_lo2) != (0x00, 0x06, 0x00):
                raise RuntimeError(
                    f"GPIO2->GPIO1 loopback failed (got {v_lo:#x}/{v_hi:#x}/{v_lo2:#x}). "
                    "Check U49 PJ1/PJ0 direction control and P10 pin 16 <-> pin 18 jumper."
                )

            # Bit 0 (PF0 / XDS_GPIO3, U8 fixed A->B output) direction & readback check
            gpio_set(usb_dev, GPIO_SUBCMD_DIR, 0x01, 0x01)
            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x01, 0x01)
            g3_hi = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x01
            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x01, 0x00)
            g3_lo = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x01
            print(f"  [GPIO3 / PF0   ] high=0x{g3_hi:02X} low=0x{g3_lo:02X}")
            if (g3_hi, g3_lo) != (0x01, 0x00):
                raise RuntimeError(f"GPIO3 (PF0) output readback failed: high={g3_hi}, low={g3_lo}")

            if blink_identify:
                print("  [BLINK_LED     ] triggering probe-identify Red/Green LED blink...")
                gpio_set(usb_dev, GPIO_SUBCMD_BLINK_LED, 0x00, 0x01, timeout_ms=5000)
        finally:
            # Always restore PF0..PF2 to safe high-Z inputs (PJ0/PJ1 LOW)
            gpio_set(usb_dev, GPIO_SUBCMD_DIR, 0x07, 0x00)


def test_uart_standalone(app_uart: str) -> None:
    print(f"\n=== 3. Standalone UART0 Loopback ({app_uart}) ===")

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
    print(f"\n=== 4. Aux / SWO UART7 Non-Blocking Drain ({aux_uart}) ===")
    fd = open_raw_serial(aux_uart, 115200, "N")
    payload = b"AUX_TX_DISCARD_TEST" * 100
    written = os.write(fd, payload)
    time.sleep(0.05)
    os.close(fd)
    print(f"  [{aux_uart}] open/SET_LINE_CODING/write-discard OK ({written} bytes)")


def test_concurrent_et_and_uart(xds_cmd: list[str], app_uart: str, duration_s: int, run_uart: bool) -> None:
    label = "Concurrent EnergyTrace HDR (256 kS/s) + UART0" if run_uart else "EnergyTrace HDR (256 kS/s)"
    print(f"\n=== 5. {label} ({duration_s}s) ===")

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
    parser.add_argument("--blink-identify", action="store_true", help="Also exercise 0x80000010 probe-identify LED blink")
    parser.add_argument("--skip-gpio", action="store_true", help="Skip Target Aux GPIO (0x29/0x2A) loopback tests")
    parser.add_argument("--skip-uart", action="store_true", help="Skip UART loopback tests")
    parser.add_argument("--skip-et", action="store_true", help="Skip EnergyTrace capture tests")
    args = parser.parse_args()

    xds_cmd = resolve_xds110_et_cmd(args.xds110_et)

    try:
        test_probe_info(xds_cmd, args.expect_version)
        if not args.skip_gpio:
            test_aux_gpio(args.blink_identify)
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
