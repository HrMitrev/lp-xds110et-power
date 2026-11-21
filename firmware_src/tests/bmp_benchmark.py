#!/usr/bin/env python3
"""
Cross-platform (Linux & Windows) Black Magic Probe flash & RTT benchmark
for TI LP-XDS110ET (OpenFW).

Automatically discovers the LP-XDS110ET OpenFW BMP GDB port (CDC1 / interface 03)
and Target UART/RTT port (CDC0 / interface 00) on both Linux and Windows:
  - Linux:   /dev/ttyBmpGdb, /dev/serial/by-id/*OpenFW*-if03, or pyserial VID:PID 0451:BEF3 (if03)
             /dev/ttyBmpTarg, /dev/serial/by-id/*OpenFW*-if00, or pyserial VID:PID 0451:BEF3 (if00)
  - Windows: COM port matching USB\\VID_0451&PID_BEF3&MI_03 (GDB) and MI_00 (RTT/Target),
             automatically formatted as \\\\.\\COMxx for arm-none-eabi-gdb.

Examples:
  python3 firmware_src/tests/bmp_benchmark.py path/to/firmware.elf
  python3 firmware_src/tests/bmp_benchmark.py path/to/image.hex --freqs 4000000 0 --no-rtt
  python  firmware_src\\tests\\bmp_benchmark.py build\\app.elf --password "Open Sesame!"
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None  # type: ignore[assignment]
    list_ports = None  # type: ignore[assignment]

TI_VID = 0x0451
XDS110_PID = 0xBEF3
DEFAULT_BENCH_HEX = Path(__file__).resolve().parent / "cc23xx_bench_64k.hex"


@dataclass
class BenchResult:
    label: str
    freq_hz: int | None
    load_bytes: int | None
    transfer_kbs: float | None
    verify_ok: bool | None
    wall_s: float
    returncode: int
    target_line: str
    raw_output: str


def _format_gdb_port(port: str) -> str:
    """Format a serial port name for arm-none-eabi-gdb (adds \\\\.\\ prefix on Windows COM ports)."""
    if sys.platform == "win32":
        upper = port.upper()
        if upper.startswith("COM") and not port.startswith("\\\\.\\"):
            return f"\\\\.\\{port}"
    return port


def _extract_iface_num(info) -> int | None:
    """Extract USB interface number (0 for CDC0/Targ, 3 for CDC1/GDB) from pyserial ListPortInfo."""
    hwid = (info.hwid or "").upper()
    # Windows HWID: USB VID:PID=0451:BEF3 ... MI_03 or LOCATION=1-1:1.3
    m_mi = re.search(r"MI_([0-9A-F]{2})", hwid)
    if m_mi:
        return int(m_mi.group(1), 16)

    loc = (info.location or "").strip()
    # Linux pyserial location: e.g. "1-1:1.3" or "1-1.2:1.0"
    m_loc = re.search(r":\d+\.(\d+)$", loc)
    if m_loc:
        return int(m_loc.group(1))

    iface = getattr(info, "interface", None) or ""
    if "gdb" in iface.lower():
        return 3
    if "uart" in iface.lower() or "targ" in iface.lower():
        return 0
    return None


def _is_xds110_openfw(info) -> bool:
    if info.vid == TI_VID and info.pid == XDS110_PID:
        return True
    hwid = (info.hwid or "").upper()
    return "0451:BEF3" in hwid or ("VID_0451" in hwid and "PID_BEF3" in hwid)


def _discover_windows_ports_stdlib(
    serial_filter: str | None = None,
) -> tuple[str | None, str | None]:
    """Windows stdlib fallback when pyserial is unavailable."""
    gdb_port: str | None = None
    rtt_port: str | None = None

    # 1. Query present PnP Ports via PowerShell (accurate for currently connected devices)
    ps_script = (
        "Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue | "
        "Where-Object { $_.InstanceId -match 'VID_0451&PID_BEF3' } | "
        "ForEach-Object { "
        "$parent = (Get-PnpDeviceProperty -InstanceId $_.InstanceId "
        "-KeyName DEVPKEY_Device_Parent -ErrorAction SilentlyContinue).Data; "
        '"$($_.InstanceId)|$($_.FriendlyName)|$parent" '
        "}"
    )
    entries: list[tuple[int | None, str, str]] = []
    try:
        proc = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps_script],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if proc.returncode == 0:
            for line in proc.stdout.splitlines():
                parts = line.strip().split("|")
                if len(parts) < 2:
                    continue
                inst_id, friendly = parts[0], parts[1]
                parent = parts[2] if len(parts) >= 3 else ""
                if (
                    serial_filter
                    and serial_filter.lower() not in (inst_id + " " + parent).lower()
                ):
                    continue
                m_com = re.search(r"\b(COM\d+)\b", friendly, re.IGNORECASE)
                if not m_com:
                    continue
                m_mi = re.search(r"MI_([0-9A-F]{2})", inst_id, re.IGNORECASE)
                ifnum = int(m_mi.group(1), 16) if m_mi else None
                entries.append((ifnum, m_com.group(1).upper(), friendly))
    except (OSError, subprocess.SubprocessError):
        pass

    # 2. Registry fallback if PowerShell/CIM is unavailable
    if not entries:
        try:
            import winreg  # type: ignore[import-not-found]

            active_coms: set[str] = set()
            with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM"
            ) as key:
                idx = 0
                while True:
                    try:
                        _, val, _ = winreg.EnumValue(key, idx)
                        if isinstance(val, str) and val.upper().startswith("COM"):
                            active_coms.add(val.upper())
                        idx += 1
                    except OSError:
                        break

            usb_root = r"SYSTEM\CurrentControlSet\Enum\USB"
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, usb_root) as usb_key:
                idx = 0
                while True:
                    try:
                        subname = winreg.EnumKey(usb_key, idx)
                        idx += 1
                    except OSError:
                        break
                    upper_sub = subname.upper()
                    if "VID_0451" not in upper_sub or "PID_BEF3" not in upper_sub:
                        continue
                    m_mi = re.search(r"MI_([0-9A-F]{2})", upper_sub)
                    ifnum = int(m_mi.group(1), 16) if m_mi else None
                    with winreg.OpenKey(usb_key, subname) as dev_key:
                        didx = 0
                        while True:
                            try:
                                inst = winreg.EnumKey(dev_key, didx)
                                didx += 1
                            except OSError:
                                break
                            if (
                                serial_filter
                                and serial_filter.lower() not in inst.lower()
                            ):
                                continue
                            try:
                                with winreg.OpenKey(
                                    dev_key, f"{inst}\\Device Parameters"
                                ) as dp_key:
                                    port_name, _ = winreg.QueryValueEx(
                                        dp_key, "PortName"
                                    )
                                if (
                                    isinstance(port_name, str)
                                    and port_name.upper() in active_coms
                                ):
                                    friendly = ""
                                    try:
                                        with winreg.OpenKey(dev_key, inst) as ikey:
                                            friendly, _ = winreg.QueryValueEx(
                                                ikey, "FriendlyName"
                                            )
                                    except OSError:
                                        pass
                                    entries.append(
                                        (ifnum, port_name.upper(), str(friendly))
                                    )
                            except OSError:
                                continue
        except OSError:
            pass

    openfw_entries = [e for e in entries if "openfw" in e[2].lower()]
    candidates = openfw_entries or entries

    for ifnum, com, _ in candidates:
        if ifnum == 3 and not gdb_port:
            gdb_port = com
        elif ifnum == 0 and not rtt_port:
            rtt_port = com

    if (not gdb_port or not rtt_port) and len(candidates) >= 2:
        ordered = sorted(candidates, key=lambda e: int(e[1][3:]))
        if not rtt_port:
            rtt_port = ordered[0][1]
        if not gdb_port:
            gdb_port = ordered[1][1]

    return gdb_port, rtt_port


def discover_bmp_ports(
    serial_filter: str | None = None,
) -> tuple[str | None, str | None]:
    """Return (gdb_port, rtt_port) for LP-XDS110ET OpenFW BMP on Linux or Windows."""
    gdb_port: str | None = None
    rtt_port: str | None = None

    # 1. Linux udev symlinks (preferred when no specific serial filter is requested)
    if sys.platform != "win32":
        if serial_filter:
            cand_gdb = f"/dev/ttyBmpGdb{serial_filter}"
            cand_rtt = f"/dev/ttyBmpTarg{serial_filter}"
        else:
            cand_gdb = "/dev/ttyBmpGdb"
            cand_rtt = "/dev/ttyBmpTarg"

        if os.path.exists(cand_gdb):
            gdb_port = cand_gdb
        if os.path.exists(cand_rtt):
            rtt_port = cand_rtt

        if not gdb_port:
            pattern = f"/dev/serial/by-id/*XDS110*{serial_filter or ''}*-if03"
            matches = sorted(glob.glob(pattern))
            if matches:
                gdb_port = matches[0]
        if not rtt_port:
            pattern = f"/dev/serial/by-id/*XDS110*{serial_filter or ''}*-if00"
            matches = sorted(glob.glob(pattern))
            if matches:
                rtt_port = matches[0]

        if gdb_port and rtt_port:
            return gdb_port, rtt_port

    # 2. Cross-platform pyserial enumeration (Windows MI_03/MI_00 + Linux fallback)
    if list_ports is not None:
        xds_ports = []
        for info in list_ports.comports():
            if not _is_xds110_openfw(info):
                continue
            if (
                serial_filter
                and serial_filter.lower() not in (info.serial_number or "").lower()
            ):
                continue
            xds_ports.append(info)

        # Prefer OpenFW product strings when multiple XDS110 probes are attached
        openfw_ports = [
            p
            for p in xds_ports
            if "openfw" in ((p.product or "") + " " + (p.description or "")).lower()
        ]
        candidates = openfw_ports or xds_ports

        for info in candidates:
            ifnum = _extract_iface_num(info)
            if ifnum == 3 and not gdb_port:
                gdb_port = info.device
            elif ifnum == 0 and not rtt_port:
                rtt_port = info.device

        # Fallback if OS hides MI_xx / location: sort by device name (lower=CDC0/Targ, higher=CDC1/GDB)
        if (not gdb_port or not rtt_port) and len(candidates) >= 2:
            ordered = sorted(candidates, key=lambda p: p.device)
            if not rtt_port:
                rtt_port = ordered[0].device
            if not gdb_port:
                gdb_port = ordered[1].device

    # 3. Windows stdlib fallback (PowerShell Get-PnpDevice / winreg) when pyserial is unavailable
    if sys.platform == "win32" and (not gdb_port or not rtt_port):
        fb_gdb, fb_rtt = _discover_windows_ports_stdlib(serial_filter)
        if not gdb_port:
            gdb_port = fb_gdb
        if not rtt_port:
            rtt_port = fb_rtt

    return gdb_port, rtt_port


def find_gdb(explicit: str | None) -> str:
    if explicit:
        return explicit

    repo_root = Path(__file__).resolve().parents[1]
    local_candidates = [
        repo_root / "toolchain" / "bin" / "arm-none-eabi-gdb.exe",
        repo_root / "toolchain" / "bin" / "arm-none-eabi-gdb",
    ]
    for cand in local_candidates:
        if cand.is_file():
            return str(cand)

    for name in ("arm-none-eabi-gdb", "arm-none-eabi-gdb.exe", "gdb-multiarch"):
        found = shutil.which(name)
        if found:
            return found

    raise FileNotFoundError(
        "Could not locate arm-none-eabi-gdb on PATH or in firmware_src/toolchain/bin/. "
        "Pass --gdb /path/to/arm-none-eabi-gdb."
    )


def run_flash_once(
    gdb_bin: str,
    gdb_port: str,
    firmware: Path,
    freq_hz: int | None,
    password: str | None,
    verify: bool,
    timeout_s: float,
) -> BenchResult:
    label = (
        "default (4 MHz)"
        if freq_hz is None
        else ("max (freq 0)" if freq_hz == 0 else f"{freq_hz / 1e6:.2f} MHz")
    )
    cmd = [
        gdb_bin,
        "-nx",
        "--batch",
        str(firmware),
        "-ex",
        f"target extended-remote {_format_gdb_port(gdb_port)}",
    ]
    if freq_hz is not None:
        cmd.extend(["-ex", f"monitor frequency {freq_hz}"])
    cmd.extend(
        ["-ex", "monitor frequency", "-ex", "monitor swd_scan", "-ex", "attach 1"]
    )
    if password:
        cmd.extend(["-ex", f"monitor pass {password}"])
    cmd.extend(["-ex", "load"])
    if verify:
        cmd.extend(["-ex", "compare-sections"])
    cmd.extend(["-ex", "detach"])

    t0 = time.monotonic()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    wall_s = time.monotonic() - t0
    combined = (proc.stdout or "") + "\n" + (proc.stderr or "")

    actual_freq: int | None = None
    load_bytes: int | None = None
    transfer_kbs: float | None = None
    target_line = ""
    matched_count = 0
    mismatched_count = 0

    for line in combined.splitlines():
        m_freq = re.search(r"Debug iface frequency set to (\d+)Hz", line)
        if m_freq:
            actual_freq = int(m_freq.group(1))
        m_load = re.search(r"load size (\d+)", line)
        if m_load:
            load_bytes = int(m_load.group(1))
        m_rate = re.search(r"Transfer rate:\s*([\d.]+)\s*KB/sec", line)
        if m_rate:
            transfer_kbs = float(m_rate.group(1))
        if re.match(r"^\s*\*?\s*1\s+", line):
            target_line = line.strip()
        if "MIS-MATCHED!" in line:
            mismatched_count += 1
        elif line.rstrip().endswith(": matched."):
            matched_count += 1

    verify_ok: bool | None = None
    if verify:
        verify_ok = (mismatched_count == 0) and (matched_count > 0)

    return BenchResult(
        label=label,
        freq_hz=actual_freq,
        load_bytes=load_bytes,
        transfer_kbs=transfer_kbs,
        verify_ok=verify_ok,
        wall_s=wall_s,
        returncode=proc.returncode,
        target_line=target_line,
        raw_output=combined,
    )


def run_rtt_capture(
    gdb_bin: str,
    gdb_port: str,
    rtt_port: str,
    firmware: Path,
    duration_s: float,
    password: str | None,
) -> tuple[str, bytes]:
    if serial is None:
        raise RuntimeError(
            "pyserial is required for RTT capture (pip install pyserial)."
        )

    rtt_bytes = bytearray()
    stop_evt = threading.Event()
    reader_err: list[Exception] = []

    def reader() -> None:
        try:
            with serial.Serial(rtt_port, 115200, timeout=0.1) as ser:
                ser.dtr = True
                ser.reset_input_buffer()
                while not stop_evt.is_set():
                    chunk = ser.read(1024)
                    if chunk:
                        rtt_bytes.extend(chunk)
        except Exception as exc:  # noqa: BLE001
            reader_err.append(exc)

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    time.sleep(0.1)

    gdb_cmd = [
        gdb_bin,
        "-nx",
        str(firmware),
        "-ex",
        f"target extended-remote {_format_gdb_port(gdb_port)}",
        "-ex",
        "monitor swd_scan",
        "-ex",
        "attach 1",
    ]
    if password:
        gdb_cmd.extend(["-ex", f"monitor pass {password}"])
    gdb_cmd.extend(
        [
            "-ex",
            "monitor rtt enable",
            "-ex",
            "monitor rtt status",
            "-ex",
            "continue",
        ]
    )

    popen_kwargs: dict = {
        "stdin": subprocess.PIPE,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.STDOUT,
        "text": True,
    }
    if sys.platform == "win32":
        popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP

    proc = subprocess.Popen(gdb_cmd, **popen_kwargs)
    time.sleep(duration_s)

    if sys.platform == "win32":
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.send_signal(signal.SIGINT)
    time.sleep(0.4)

    try:
        out, _ = proc.communicate(
            "monitor rtt status\nmonitor rtt disable\ndetach\nquit\n", timeout=8
        )
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()

    stop_evt.set()
    th.join(timeout=2)

    if reader_err:
        raise RuntimeError(
            f"RTT port {rtt_port} error: {reader_err[0]}"
        ) from reader_err[0]

    return out, bytes(rtt_bytes)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Benchmark Black Magic Probe flash speed and verify RTT logs on LP-XDS110ET (Linux & Windows)."
    )
    parser.add_argument(
        "firmware",
        nargs="?",
        type=Path,
        default=None,
        help=f"Path to target firmware image (.elf or .hex; default: {DEFAULT_BENCH_HEX.name})",
    )
    parser.add_argument(
        "--gdb-port",
        default=None,
        help="Override BMP GDB serial port (e.g. /dev/ttyBmpGdb or COM12)",
    )
    parser.add_argument(
        "--rtt-port",
        default=None,
        help="Override BMP Target/RTT serial port (e.g. /dev/ttyBmpTarg or COM11)",
    )
    parser.add_argument(
        "--serial",
        default=None,
        help="Optional XDS110 serial number filter when multiple probes are connected",
    )
    parser.add_argument(
        "--gdb",
        default=None,
        help="Path to arm-none-eabi-gdb executable (auto-detected if omitted)",
    )
    parser.add_argument(
        "--freqs",
        type=int,
        nargs="+",
        default=[4000000, 0],
        help="SWD frequencies in Hz to benchmark (0 = max speed; default: 4000000 0)",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=1,
        help="Number of flash iterations per frequency (default: 1)",
    )
    parser.add_argument(
        "--password",
        default=None,
        help="Optional TI CC23xx debug auth password passed via 'monitor pass <password>'",
    )
    parser.add_argument(
        "--skip-verify",
        action="store_true",
        help="Skip GDB 'compare-sections' verification after 'load'",
    )
    parser.add_argument(
        "--rtt-seconds",
        type=float,
        default=3.0,
        help="Seconds to run target and capture RTT logs when RTT is enabled (default: 3.0)",
    )
    parser.add_argument(
        "--rtt",
        action="store_true",
        help="Force RTT capture even when using the default random benchmark HEX",
    )
    parser.add_argument(
        "--no-rtt",
        action="store_true",
        help="Skip RTT log capture test",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help="Per-run GDB timeout in seconds (default: 60)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Print full GDB output for each run",
    )
    args = parser.parse_args()

    using_default_hex = args.firmware is None
    firmware = (
        (DEFAULT_BENCH_HEX if using_default_hex else args.firmware)
        .expanduser()
        .resolve()
    )
    if not firmware.is_file():
        print(f"ERROR: Firmware file not found: {firmware}", file=sys.stderr)
        return 2

    gdb_bin = find_gdb(args.gdb)
    auto_gdb, auto_rtt = discover_bmp_ports(args.serial)
    gdb_port = args.gdb_port or auto_gdb
    rtt_port = args.rtt_port or auto_rtt

    if not gdb_port:
        print(
            "ERROR: Could not find LP-XDS110ET BMP GDB port (VID 0451, PID BEF3, interface 03). "
            "Pass --gdb-port explicitly.",
            file=sys.stderr,
        )
        return 2

    print(f"Firmware : {firmware} ({firmware.stat().st_size:,} B on disk)")
    print(f"GDB      : {gdb_bin}")
    print(f"GDB Port : {gdb_port} (GDB target: {_format_gdb_port(gdb_port)})")
    print(f"RTT Port : {rtt_port or '(not detected)'}")

    results: list[BenchResult] = []
    any_fail = False

    for freq in args.freqs:
        for run_idx in range(1, args.runs + 1):
            res = run_flash_once(
                gdb_bin=gdb_bin,
                gdb_port=gdb_port,
                firmware=firmware,
                freq_hz=freq,
                password=args.password,
                verify=not args.skip_verify,
                timeout_s=args.timeout,
            )
            results.append(res)
            freq_str = f"{res.freq_hz / 1e6:.2f} MHz" if res.freq_hz else res.label
            rate_str = (
                f"{res.transfer_kbs:.1f} KB/s"
                if res.transfer_kbs is not None
                else "n/a"
            )
            size_str = f"{res.load_bytes:,} B" if res.load_bytes is not None else "n/a"
            ver_str = (
                "SKIP"
                if res.verify_ok is None
                else ("PASS" if res.verify_ok else "FAIL")
            )
            run_tag = f" [run {run_idx}/{args.runs}]" if args.runs > 1 else ""
            print(
                f"\n[{freq_str}{run_tag}] target={res.target_line or '?'} | "
                f"loaded={size_str} | rate={rate_str} | verify={ver_str} | wall={res.wall_s:.2f} s"
            )
            if res.returncode != 0 or res.verify_ok is False or res.load_bytes is None:
                any_fail = True
                print(res.raw_output, file=sys.stderr)
            elif args.verbose:
                print(res.raw_output)

    run_rtt = (
        (not args.no_rtt)
        and (args.rtt_seconds > 0)
        and (args.rtt or not using_default_hex)
    )
    if run_rtt:
        if not rtt_port:
            print(
                "\n[RTT] Skipped: could not detect Target/RTT port (pass --rtt-port).",
                file=sys.stderr,
            )
        else:
            print(f"\n=== RTT Capture ({args.rtt_seconds:.1f} s on {rtt_port}) ===")
            gdb_out, rtt_data = run_rtt_capture(
                gdb_bin=gdb_bin,
                gdb_port=gdb_port,
                rtt_port=rtt_port,
                firmware=firmware,
                duration_s=args.rtt_seconds,
                password=args.password,
            )
            for line in gdb_out.splitlines():
                if any(k in line for k in ("RTT", "Using", "Polling")):
                    print(" ", line)
            print(f"Captured {len(rtt_data)} bytes:")
            if rtt_data:
                print(rtt_data.decode("utf-8", errors="replace")[:2000])
            else:
                print("  (no RTT bytes captured)")

    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
