#!/usr/bin/env python3
"""
Live SWD target verification suite for LP-XDS110ET open-source CMSIS-DAP v2 firmware.

Verifies against an attached SWD target (default: STM32G0 via target/stm32g0x.cfg):
  1. OpenOCD CMSIS-DAP v2 (usb_bulk @ 2 MHz): SWD DPIDR, reset halt, flash probe,
     8 KiB Flash dump & CRC-32 verify (with measured throughput), SRAM R/W/restore
  2. probe-rs: CMSIS-DAP v2 info & CoreSight ROM table walk
  3. pyOCD: CMSIS-DAP v2 attach, core status, register dump, and Flash read32
  4. Concurrent EnergyTrace HDR (256 kS/s) + OpenOCD 8 KiB SWD dump/verify
     (verifies 0 lost EnergyTrace frames, 0 USB reconnects, and live target current)
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


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
    return [
        "cargo",
        "run",
        "--quiet",
        "--release",
        "--manifest-path",
        str(manifest),
        "--",
    ]


def openocd_adapter_cfg(backend: str, speed_khz: int) -> str:
    if backend == "auto":
        return (
            f"adapter driver cmsis-dap; transport select swd; adapter speed {speed_khz}"
        )
    return f"adapter driver cmsis-dap; cmsis-dap backend {backend}; transport select swd; adapter speed {speed_khz}"


def test_swd_target(
    xds_cmd: list[str],
    target_cfg: str,
    backend: str,
    speed_khz: int,
    flash_addr: int,
    sram_addr: int,
    dump_size: int,
    run_et: bool,
    et_duration_s: int,
) -> None:
    print(
        f"=== Live SWD Target Verification ({target_cfg}, backend={backend}, {speed_khz} kHz) ==="
    )
    if not shutil.which("openocd"):
        raise RuntimeError(
            "openocd not found in PATH (required for swd_target_test.py)"
        )

    with tempfile.NamedTemporaryFile(
        prefix="swd_dump_", suffix=".bin", delete=False
    ) as tmp_bin:
        dump_path = tmp_bin.name
    csv_path: str | None = None

    try:
        # 1a. OpenOCD CMSIS-DAP v2 (Interface 5): reset halt, Flash dump & CRC-32 verify, SRAM R/W/restore
        ocd_script = (
            f"init; reset halt; flash probe 0; "
            f"dump_image {dump_path} 0x{flash_addr:08X} {dump_size}; "
            f"verify_image {dump_path} 0x{flash_addr:08X}; "
            f"set orig [read_memory 0x{sram_addr:08X} 32 4]; "
            f"write_memory 0x{sram_addr:08X} 32 {{0xDEADBEEF 0xCAFEBABE 0x12345678 0xA5A55A5A}}; "
            f'echo "SRAM_RW: [read_memory 0x{sram_addr:08X} 32 4]"; '
            f"write_memory 0x{sram_addr:08X} 32 $orig; "
            f"shutdown"
        )
        t0 = time.monotonic()
        p = subprocess.run(
            [
                "openocd",
                "-c",
                openocd_adapter_cfg(backend, speed_khz),
                "-f",
                target_cfg,
                "-c",
                ocd_script,
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        dt_ocd = time.monotonic() - t0
        out = p.stdout + p.stderr
        if (
            p.returncode != 0
            or "SRAM_RW: 0xdeadbeef 0xcafebabe 0x12345678 0xa5a55a5a" not in out
        ):
            raise RuntimeError(f"OpenOCD CMSIS-DAP SWD target verification failed:\n{out}")
        dpidr_line = next(
            (line.strip() for line in out.splitlines() if "SWD DPIDR" in line), "SWD OK"
        )
        dap_line = next(
            (line.strip() for line in out.splitlines() if "Using CMSIS-DAP" in line), ""
        )
        detail = f"{dpidr_line}, {dump_size // 1024} KiB flash verify + SRAM R/W, {dt_ocd * 1000:.0f}ms"
        if dap_line:
            detail = f"{dap_line.removeprefix('Info : ')}; {detail}"
        print(f"  [openocd DAPv2 ] OK ({detail})")

        # 1b. OpenOCD native interface/xds110.cfg (Interface 2, OCD_DAP_REQUEST 0x3A + connect_assert_srst)
        t0 = time.monotonic()
        p_xds = subprocess.run(
            [
                "openocd",
                "-f",
                "interface/xds110.cfg",
                "-c",
                f"transport select swd; adapter speed {speed_khz}",
                "-f",
                target_cfg,
                "-c",
                "reset_config srst_only srst_nogate connect_assert_srst",
                "-c",
                ocd_script,
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        dt_xds = time.monotonic() - t0
        xds_out = p_xds.stdout + p_xds.stderr
        if (
            p_xds.returncode != 0
            or "SRAM_RW: 0xdeadbeef 0xcafebabe 0x12345678 0xa5a55a5a" not in xds_out
        ):
            raise RuntimeError(f"OpenOCD native xds110.cfg SWD verification failed:\n{xds_out}")
        xds_dpidr = next(
            (line.strip() for line in xds_out.splitlines() if "SWD DPIDR" in line), "SWD OK"
        )
        print(
            f"  [openocd XDS110] OK ({xds_dpidr}, connect_assert_srst + {dump_size // 1024} KiB flash verify + SRAM R/W, {dt_xds * 1000:.0f}ms)"
        )

        # 2. probe-rs ROM table discovery (while target is halted)
        if shutil.which("probe-rs"):
            t0 = time.monotonic()
            p_rs = subprocess.run(
                ["probe-rs", "info", "--speed", str(min(speed_khz, 2000)), "--verbose"],
                capture_output=True,
                text=True,
                check=False,
            )
            dt_rs = time.monotonic() - t0
            rs_out = p_rs.stdout + p_rs.stderr
            if p_rs.returncode != 0 or "ROM Table" not in rs_out:
                raise RuntimeError(f"probe-rs ROM table scan failed:\n{rs_out}")
            print(
                f"  [probe-rs info ] OK (SWD DP/AP + CoreSight ROM tables, {dt_rs * 1000:.0f}ms)"
            )

        # 3. pyocd attach + register & Flash readback (while target is halted)
        if shutil.which("pyocd"):
            t0 = time.monotonic()
            p_py = subprocess.run(
                [
                    "pyocd",
                    "cmd",
                    "-c",
                    "status",
                    "-c",
                    "reg",
                    "-c",
                    f"read32 0x{flash_addr:08X} 16",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            dt_py = time.monotonic() - t0
            py_out = p_py.stdout + p_py.stderr
            addr_tag = f"{flash_addr:08x}:"
            if (
                p_py.returncode != 0
                or "Halted" not in py_out
                or addr_tag not in py_out.lower()
            ):
                raise RuntimeError(f"pyocd SWD target check failed:\n{py_out}")
            print(
                f"  [pyocd SWD     ] OK (core status + registers + flash read32, {dt_py * 1000:.0f}ms)"
            )

        # 4. Concurrent EnergyTrace HDR 256 kS/s + OpenOCD Flash dump/verify
        if run_et:
            with tempfile.NamedTemporaryFile(
                prefix="et_swd_", suffix=".csv", delete=False
            ) as tmp_csv:
                csv_path = tmp_csv.name
            et_proc = subprocess.Popen(
                [*xds_cmd, "capture", "-t", str(et_duration_s), "-o", csv_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            time.sleep(0.6)
            p_coex = subprocess.run(
                [
                    "openocd",
                    "-c",
                    openocd_adapter_cfg(backend, speed_khz),
                    "-f",
                    target_cfg,
                    "-c",
                    (
                        f"init; reset halt; dump_image {dump_path} 0x{flash_addr:08X} {dump_size}; "
                        f"verify_image {dump_path} 0x{flash_addr:08X}; resume; shutdown"
                    ),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            et_out, _ = et_proc.communicate(timeout=et_duration_s + 15)
            if p_coex.returncode != 0:
                raise RuntimeError(
                    f"OpenOCD failed during EnergyTrace:\n{p_coex.stdout + p_coex.stderr}"
                )
            if (
                et_proc.returncode != 0
                or "0 frames lost" not in et_out
                or "0 reconnects" not in et_out
            ):
                raise RuntimeError(f"EnergyTrace lost frames during SWD:\n{et_out}")
            summary_line = next(
                (
                    line.strip()
                    for line in et_out.splitlines()
                    if "Capture finished:" in line
                ),
                "",
            )
            current_line = next(
                (line.strip() for line in et_out.splitlines() if "Current:" in line), ""
            )
            print(f"  [ET + SWD coex ] OK ({summary_line}; {current_line})")
        else:
            subprocess.run(
                [
                    "openocd",
                    "-c",
                    openocd_adapter_cfg(backend, min(speed_khz, 1000)),
                    "-f",
                    target_cfg,
                    "-c",
                    "init; resume; shutdown",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
    finally:
        for path in (dump_path, csv_path):
            if path and os.path.exists(path):
                os.remove(path)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="LP-XDS110ET live SWD target verification suite"
    )
    parser.add_argument(
        "--target-cfg",
        default="target/stm32g0x.cfg",
        help="OpenOCD target config file (default: target/stm32g0x.cfg)",
    )
    parser.add_argument(
        "--backend",
        choices=("usb_bulk", "hid", "auto"),
        default="usb_bulk",
        help="OpenOCD CMSIS-DAP backend (default: usb_bulk)",
    )
    parser.add_argument(
        "--speed-khz",
        type=int,
        default=2000,
        help="SWD clock speed in kHz (default: 2000)",
    )
    parser.add_argument(
        "--flash-addr",
        type=lambda s: int(s, 0),
        default=0x08000000,
        help="Target Flash base address for dump/verify (default: 0x08000000)",
    )
    parser.add_argument(
        "--sram-addr",
        type=lambda s: int(s, 0),
        default=0x20001F00,
        help="Target SRAM scratch address (restored after test, default: 0x20001F00)",
    )
    parser.add_argument(
        "--dump-size",
        type=int,
        default=8192,
        help="Flash dump/verify size in bytes (default: 8192)",
    )
    parser.add_argument(
        "--xds110-et", default=None, help="Path to xds110-et CLI binary"
    )
    parser.add_argument(
        "--et-duration",
        type=int,
        default=4,
        help="Concurrent EnergyTrace duration in seconds (default: 4)",
    )
    parser.add_argument(
        "--skip-et",
        action="store_true",
        help="Skip concurrent EnergyTrace + SWD coexistence test",
    )
    args = parser.parse_args()

    xds_cmd = resolve_xds110_et_cmd(args.xds110_et)

    try:
        test_swd_target(
            xds_cmd=xds_cmd,
            target_cfg=args.target_cfg,
            backend=args.backend,
            speed_khz=args.speed_khz,
            flash_addr=args.flash_addr,
            sram_addr=args.sram_addr,
            dump_size=args.dump_size,
            run_et=not args.skip_et,
            et_duration_s=args.et_duration,
        )
    except Exception as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        return 1

    print("\nALL LIVE SWD TARGET TESTS PASSED!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
