#!/usr/bin/env python3
"""
Hardware-in-the-Loop (HIL) stress test suite for LP-XDS110ET open-source firmware.

Verifies:
  1. Probe enumeration & version / EnergyTrace HDR hardware info (xds110-et info)
  2. Target Aux GPIOs (opcodes 0x29 / 0x2A) across PF0..PF2 & U49 PJ0/PJ1 level
     shifters (requires a jumper bridging P10 pin 16 GPIO2 <-> pin 18 GPIO1)
  3. CMSIS-DAP v2 WinUSB Bulk transport (IF 5, 0x05/0x86, 512 B HS), BOS /
     MS OS 2.0 WinUSB descriptors, DAP_ExecuteCommands (0x7F), and host tool
     discovery (probe-rs, pyocd, openocd when installed in PATH)
  4. Standalone UART0 (/dev/ttyACM0) loopback across baud rates & parity modes
     (requires a jumper bridging P10 pin 7 XDS_TX <-> pin 9 XDS_RX)
  5. Aux / SWO UART7 (/dev/ttyACM1) open, line coding, and non-blocking TX discard
  6. Concurrent EnergyTrace HDR 256 kS/s capture + high-speed UART0 loopback
     (verifies 0 lost EnergyTrace frames and 100% bit-exact UART transfers)

For live SWD target tests (STM32G0 etc.), use tests/swd_target_test.py or
`make -C firmware_src test-swd`.

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

# CMSIS-DAP v2 WinUSB Bulk constants (Interface 5, OUT 0x05, IN 0x86)
DAP_IFACE = 5
DAP_EP_OUT = 0x05
DAP_EP_IN = 0x86
DAP_MS_OS_20_VENDOR_CODE = 0x20
DAP_MS_OS_20_DESC_INDEX = 0x0007
DAP_WINUSB_GUID = "{CDB3B5AD-293B-4663-AA36-1AAE46463776}"

DAP_CMD_INFO = 0x00
DAP_CMD_HOST_STATUS = 0x01
DAP_CMD_CONNECT = 0x02
DAP_CMD_DISCONNECT = 0x03
DAP_CMD_TRANSFER_CONFIGURE = 0x04
DAP_CMD_DELAY = 0x09
DAP_CMD_RESET_TARGET = 0x0A
DAP_CMD_SWJ_PINS = 0x10
DAP_CMD_SWJ_CLOCK = 0x11
DAP_CMD_SWD_CONFIGURE = 0x13
DAP_CMD_EXECUTE_COMMANDS = 0x7F
DAP_BULK_PKT_SIZE_HS = 512

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

    def cmd(
        self, opcode: int, args: bytes = b"", timeout_ms: int = 2000
    ) -> tuple[int, bytes]:
        payload = bytes([opcode]) + args
        frame = struct.pack("<BH", 0x2A, len(payload)) + payload
        self.dev.write(XDS_CMD_EP_OUT, frame, timeout=timeout_ms)
        resp = bytes(self.dev.read(XDS_CMD_EP_IN, 512, timeout=timeout_ms))
        if len(resp) < 7 or resp[0] != 0x2A:
            raise RuntimeError(
                f"Malformed XDS response to 0x{opcode:02X}: {resp.hex()}"
            )
        resp_len = struct.unpack_from("<H", resp, 1)[0]
        err = struct.unpack_from("<i", resp, 3)[0]
        return err, resp[7 : 3 + resp_len]


class CmsisDapBulk:
    """PyUSB Bulk transport for CMSIS-DAP v2 WinUSB Interface 5 (0x05 / 0x86)."""

    def __init__(self, vid: int = XDS_VID, pid: int = XDS_PID):
        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is None:
            raise RuntimeError(f"XDS110 USB device {vid:04x}:{pid:04x} not found")
        self.dev = dev
        if dev.is_kernel_driver_active(DAP_IFACE):
            dev.detach_kernel_driver(DAP_IFACE)
        usb.util.claim_interface(dev, DAP_IFACE)
        cfg = dev.get_active_configuration()
        intf = cfg[(DAP_IFACE, 0)]
        self.iface_str = usb.util.get_string(dev, intf.iInterface) or ""
        ep_in = usb.util.find_descriptor(intf, bEndpointAddress=DAP_EP_IN)
        self.mps = ep_in.wMaxPacketSize if ep_in is not None else DAP_BULK_PKT_SIZE_HS

    def close(self) -> None:
        if self.dev is not None:
            try:
                usb.util.release_interface(self.dev, DAP_IFACE)
            except usb.core.USBError:
                pass
            usb.util.dispose_resources(self.dev)
            self.dev = None

    def __enter__(self) -> "CmsisDapBulk":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def verify_ms_os_20(self) -> tuple[int, int]:
        # GET_DESCRIPTOR (BOS = 0x0F)
        bos = bytes(self.dev.ctrl_transfer(0x80, 0x06, 0x0F00, 0, 256, timeout=1000))
        if len(bos) < 5 or bos[1] != 0x0F:
            raise RuntimeError(f"Invalid USB BOS descriptor: {bos.hex()}")
        bos_total = struct.unpack_from("<H", bos, 2)[0]
        if len(bos) != bos_total or DAP_MS_OS_20_VENDOR_CODE not in bos:
            raise RuntimeError(
                f"BOS descriptor length/vendor-code mismatch: len={len(bos)}, total={bos_total}"
            )

        # Vendor IN: GET_MS_OS_20_DESCRIPTOR_SET (bRequest=0x20, wIndex=0x0007)
        msos = bytes(
            self.dev.ctrl_transfer(
                0xC0,
                DAP_MS_OS_20_VENDOR_CODE,
                0x0000,
                DAP_MS_OS_20_DESC_INDEX,
                512,
                timeout=1000,
            )
        )
        if len(msos) < 10:
            raise RuntimeError(f"MS OS 2.0 descriptor set too short: {msos.hex()}")
        msos_total = struct.unpack_from("<H", msos, 8)[0]
        if len(msos) != msos_total:
            raise RuntimeError(
                f"MS OS 2.0 length mismatch: got {len(msos)}, header says {msos_total}"
            )
        if b"WINUSB\x00\x00" not in msos:
            raise RuntimeError("MS OS 2.0 descriptor missing WINUSB Compatible ID")
        guid_utf16 = (DAP_WINUSB_GUID + "\x00\x00").encode("utf-16le")
        if guid_utf16 not in msos:
            raise RuntimeError(
                f"MS OS 2.0 descriptor missing CMSIS-DAP v2 GUID {DAP_WINUSB_GUID}"
            )
        return bos_total, msos_total

    def xfer(self, payload: bytes, timeout_ms: int = 1000) -> bytes:
        if len(payload) > self.mps:
            raise ValueError(
                f"CMSIS-DAP v2 packet ({len(payload)} B) exceeds MPS ({self.mps} B)"
            )
        self.dev.write(DAP_EP_OUT, payload, timeout=timeout_ms)
        resp = bytes(self.dev.read(DAP_EP_IN, self.mps, timeout=timeout_ms))
        if len(resp) == 0:
            raise RuntimeError(f"Empty CMSIS-DAP v2 response to cmd 0x{payload[0]:02X}")
        return resp


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


def uart_loopback(
    fd: int, payload: bytes, timeout_s: float = 3.0
) -> tuple[bytes, int, float]:
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
    proc = subprocess.run(
        [*xds_cmd, "info"], capture_output=True, text=True, check=False
    )
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
        raise RuntimeError(
            f"Expected firmware version '{expect_version}' in probe info"
        )


def test_aux_gpio(blink_identify: bool) -> None:
    print(
        "\n=== 2. Target Aux GPIOs (Opcodes 0x29 / 0x2A, P10:16 GPIO2 <-> P10:18 GPIO1) ==="
    )

    def gpio_get(usb_dev: XdsCmdUsb, subcmd: int) -> int:
        err, data = usb_dev.cmd(XDS_CMD_GPIO_GET, struct.pack("<I", subcmd))
        if err != 0 or len(data) != 2:
            raise RuntimeError(
                f"XDS_GPIO_GET(0x{subcmd:08X}) failed: err={err}, len={len(data)}"
            )
        return struct.unpack("<H", data)[0]

    def gpio_set(
        usb_dev: XdsCmdUsb, subcmd: int, mask: int, val: int, timeout_ms: int = 2000
    ) -> None:
        mask_val = ((mask & 0xFF) << 8) | (val & 0xFF)
        err, _ = usb_dev.cmd(
            XDS_CMD_GPIO_SET,
            struct.pack("<IH", subcmd, mask_val),
            timeout_ms=timeout_ms,
        )
        if err != 0:
            raise RuntimeError(
                f"XDS_GPIO_SET(0x{subcmd:08X}, 0x{mask_val:04X}) failed: err={err}"
            )

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
            print(
                f"  [GPIO1 -> GPIO2] low=0x{v_lo:02X} high=0x{v_hi:02X} low=0x{v_lo2:02X}"
            )
            if (v_lo, v_hi, v_lo2) != (0x00, 0x06, 0x00):
                raise RuntimeError(
                    f"GPIO1->GPIO2 loopback failed (got {v_lo:#x}/{v_hi:#x}/{v_lo2:#x}). "
                    "Double-check that a jumper bridges P10 pin 16 (GPIO2) <-> pin 18 (GPIO1)."
                )

            # Verify writing to an input pin (GPIO2) is ignored
            gpio_set(usb_dev, GPIO_SUBCMD_VALUE, 0x04, 0x04)
            v_prot = gpio_get(usb_dev, GPIO_SUBCMD_VALUE) & 0x06
            if v_prot != 0x00:
                raise RuntimeError(
                    f"Write to input pin GPIO2 unexpectedly changed state: 0x{v_prot:02X}"
                )

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
            print(
                f"  [GPIO2 -> GPIO1] low=0x{v_lo:02X} high=0x{v_hi:02X} low=0x{v_lo2:02X}"
            )
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
                raise RuntimeError(
                    f"GPIO3 (PF0) output readback failed: high={g3_hi}, low={g3_lo}"
                )

            if blink_identify:
                print(
                    "  [BLINK_LED     ] triggering probe-identify Red/Green LED blink..."
                )
                gpio_set(usb_dev, GPIO_SUBCMD_BLINK_LED, 0x00, 0x01, timeout_ms=5000)
        finally:
            # Always restore PF0..PF2 to safe high-Z inputs (PJ0/PJ1 LOW)
            gpio_set(usb_dev, GPIO_SUBCMD_DIR, 0x07, 0x00)


def test_cmsis_dap_v2() -> None:
    print("\n=== 3. CMSIS-DAP v2 WinUSB Bulk Transport & Host Tool Discovery ===")

    def dap_info_str(dap: CmsisDapBulk, info_id: int) -> str:
        r = dap.xfer(bytes([DAP_CMD_INFO, info_id]))
        if r[0] != DAP_CMD_INFO:
            raise RuntimeError(f"Unexpected DAP_Info response cmd: 0x{r[0]:02X}")
        n = r[1]
        return r[2 : 2 + n].rstrip(b"\x00").decode("ascii", errors="replace")

    with CmsisDapBulk() as dap:
        bos_len, msos_len = dap.verify_ms_os_20()
        print(
            f"  [WinUSB BOS    ] OK (IF {DAP_IFACE}='{dap.iface_str}', BOS={bos_len}B, MS_OS_2.0={msos_len}B, MPS={dap.mps}B)"
        )
        if "CMSIS-DAP" not in dap.iface_str:
            raise RuntimeError(
                f"Interface {DAP_IFACE} string missing 'CMSIS-DAP': {dap.iface_str!r}"
            )

        vendor = dap_info_str(dap, 0x01)
        product = dap_info_str(dap, 0x02)
        serial = dap_info_str(dap, 0x03)
        dap_ver = dap_info_str(dap, 0x04)
        prod_fw = dap_info_str(dap, 0x09)
        caps = dap.xfer(bytes([DAP_CMD_INFO, 0xF0]))[2]
        pkt_cnt = dap.xfer(bytes([DAP_CMD_INFO, 0xFE]))[2]
        pkt_sz = struct.unpack_from("<H", dap.xfer(bytes([DAP_CMD_INFO, 0xFF])), 2)[0]
        print(
            f"  [Bulk 0x05/0x86] {vendor} / {product} (SN={serial}, DAP={dap_ver}, FW={prod_fw}, "
            f"caps=0x{caps:02X}, pkts={pkt_cnt}x{pkt_sz}B)"
        )
        if (
            not dap_ver.startswith("2.")
            or (caps & 0x11) != 0x11
            or pkt_cnt < 1
            or pkt_sz != dap.mps
        ):
            raise RuntimeError(
                f"Invalid CMSIS-DAP v2 info: ver={dap_ver}, caps={caps:#x}, {pkt_cnt}x{pkt_sz} (MPS={dap.mps})"
            )

        try:
            # Verify DAP_ExecuteCommands (0x7F) returns exact unpadded Bulk length
            batch_req = bytes(
                [
                    DAP_CMD_EXECUTE_COMMANDS,
                    3,
                    DAP_CMD_INFO,
                    0xF0,
                    DAP_CMD_INFO,
                    0xFE,
                    DAP_CMD_INFO,
                    0xFF,
                ]
            )
            r_batch = dap.xfer(batch_req)
            expected_batch = bytes(
                [
                    DAP_CMD_EXECUTE_COMMANDS,
                    3,
                    DAP_CMD_INFO,
                    1,
                    caps,
                    DAP_CMD_INFO,
                    1,
                    pkt_cnt,
                ]
            ) + struct.pack("<BBH", DAP_CMD_INFO, 2, pkt_sz)
            if r_batch != expected_batch:
                raise RuntimeError(
                    f"DAP_ExecuteCommands(0x7F) mismatch: got {r_batch.hex()}, want {expected_batch.hex()}"
                )

            # Connect SWD, configure clock/transfer/SWD, toggle SWJ pins, and disconnect
            r_conn = dap.xfer(bytes([DAP_CMD_CONNECT, 0x01]))
            if r_conn != bytes([DAP_CMD_CONNECT, 0x01]):
                raise RuntimeError(f"DAP_Connect(SWD) failed: {r_conn.hex()}")

            # Verify clock=0 is rejected and clock=1MHz succeeds
            r_clk0 = dap.xfer(struct.pack("<BI", DAP_CMD_SWJ_CLOCK, 0))
            r_clk1 = dap.xfer(struct.pack("<BI", DAP_CMD_SWJ_CLOCK, 1_000_000))
            if r_clk0[1] != 0xFF or r_clk1[1] != 0x00:
                raise RuntimeError(
                    f"DAP_SWJ_Clock check failed: clk0={r_clk0[1]:#x}, clk1={r_clk1[1]:#x}"
                )

            if (
                dap.xfer(struct.pack("<BBHH", DAP_CMD_TRANSFER_CONFIGURE, 0, 100, 0))[1]
                != 0x00
            ):
                raise RuntimeError("DAP_TransferConfigure failed")
            if dap.xfer(bytes([DAP_CMD_SWD_CONFIGURE, 0x00]))[1] != 0x00:
                raise RuntimeError("DAP_SWD_Configure failed")
            if dap.xfer(struct.pack("<BH", DAP_CMD_DELAY, 500))[1] != 0x00:
                raise RuntimeError("DAP_Delay failed")
            if dap.xfer(bytes([DAP_CMD_RESET_TARGET]))[1] != 0x00:
                raise RuntimeError("DAP_ResetTarget failed")
            if dap.xfer(bytes([DAP_CMD_HOST_STATUS, 0x00, 0x01]))[1] != 0x00:
                raise RuntimeError("DAP_HostStatus failed")

            # Drive SWCLK/SWDIO/nRESET low then high via DAP_SWJ_Pins and verify readback
            r_lo = dap.xfer(struct.pack("<BBBI", DAP_CMD_SWJ_PINS, 0x00, 0x83, 0))
            r_hi = dap.xfer(struct.pack("<BBBI", DAP_CMD_SWJ_PINS, 0x83, 0x83, 0))
            if (r_lo[1] & 0x83) != 0x00 or (r_hi[1] & 0x83) != 0x83:
                raise RuntimeError(
                    f"DAP_SWJ_Pins toggle mismatch: lo=0x{r_lo[1]:02X}, hi=0x{r_hi[1]:02X}"
                )

            # 100-packet rapid USB HS Bulk round-trip latency check
            t0 = time.monotonic()
            for _ in range(100):
                rr = dap.xfer(bytes([DAP_CMD_INFO, 0xF0]))
                if rr != bytes([DAP_CMD_INFO, 1, caps]):
                    raise RuntimeError(f"Bulk round-trip burst mismatch: {rr.hex()}")
            dt_ms = (time.monotonic() - t0) * 1000.0
            print(
                f"  [Bulk Commands ] 0x7F batch/connect/clock/pins/100x burst OK ({dt_ms:.1f}ms, {dt_ms / 100:.2f}ms/pkt)"
            )
        finally:
            dap.xfer(bytes([DAP_CMD_HOST_STATUS, 0x00, 0x00]))
            dap.xfer(bytes([DAP_CMD_DISCONNECT]))

    # Host debugger discovery checks (run for each tool installed in PATH)
    if shutil.which("probe-rs"):
        p = subprocess.run(
            ["probe-rs", "list"], capture_output=True, text=True, check=False
        )
        out = (p.stdout + p.stderr).strip()
        if (
            p.returncode != 0
            or "0451:bef3" not in out.lower()
            or "cmsis-dap" not in out.lower()
        ):
            raise RuntimeError(
                f"probe-rs list did not discover CMSIS-DAP probe:\n{out}"
            )
        print(f"  [probe-rs list ] OK ({out.splitlines()[-1].strip()})")

    if shutil.which("pyocd"):
        p = subprocess.run(
            ["pyocd", "list"], capture_output=True, text=True, check=False
        )
        out = (p.stdout + p.stderr).strip()
        if p.returncode != 0 or "openfw cmsis-dap" not in out.lower():
            raise RuntimeError(f"pyocd list did not discover CMSIS-DAP probe:\n{out}")
        print(f"  [pyocd list    ] OK ({out.splitlines()[-1].strip()})")

    if shutil.which("openocd"):
        p = subprocess.run(
            [
                "openocd",
                "-c",
                "adapter driver cmsis-dap; cmsis-dap backend usb_bulk; transport select swd; adapter speed 1000; init; shutdown",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        out = (p.stdout + p.stderr).strip()
        if p.returncode != 0 or "CMSIS-DAP: Interface ready" not in out:
            raise RuntimeError(f"openocd CMSIS-DAP v2 usb_bulk init failed:\n{out}")
        print("  [openocd       ] OK (CMSIS-DAP v2 usb_bulk ready, SWD @ 1000 kHz)")


def test_uart_standalone(app_uart: str) -> None:
    print(f"\n=== 4. Standalone UART0 Loopback ({app_uart}) ===")

    # Quick sanity echo first to detect missing RX<->TX loopback jumper
    fd = open_raw_serial(app_uart, 115200, "N")
    time.sleep(0.02)
    probe_msg = b"LP-XDS110ET UART0 HIL LOOPBACK\r\n"
    rx, sent, dt = uart_loopback(fd, probe_msg, timeout_s=1.0)
    os.close(fd)
    print(
        f"  [115200 8N1 ASCII     32B] sent={sent:5d} rx={len(rx):5d} match={rx == probe_msg} dt={dt * 1000:6.1f}ms"
    )
    if len(rx) == 0:
        raise RuntimeError(
            f"No echo received on {app_uart}. Double-check that a jumper bridges XDS_TX <-> XDS_RX."
        )
    if rx != probe_msg:
        raise RuntimeError(
            f"ASCII loopback mismatch: expected {probe_msg!r}, got {rx!r}"
        )

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
        print(
            f"  [{label:17s} {len(payload):5d}B] sent={sent:5d} rx={len(rx):5d} match={ok} dt={dt * 1000:6.1f}ms ({bps:8.0f} bps)"
        )
        if not ok:
            first_diff = next(
                (i for i in range(min(len(rx), len(payload))) if rx[i] != payload[i]),
                None,
            )
            raise RuntimeError(
                f"UART0 loopback failed at {label}: rx={len(rx)}/{len(payload)}, first_diff={first_diff}"
            )


def test_aux_uart(aux_uart: str) -> None:
    print(f"\n=== 5. Aux / SWO UART7 Non-Blocking Drain ({aux_uart}) ===")
    fd = open_raw_serial(aux_uart, 115200, "N")
    payload = b"AUX_TX_DISCARD_TEST" * 100
    written = os.write(fd, payload)
    time.sleep(0.05)
    os.close(fd)
    print(f"  [{aux_uart}] open/SET_LINE_CODING/write-discard OK ({written} bytes)")


def test_concurrent_et_and_uart(
    xds_cmd: list[str], app_uart: str, duration_s: int, run_uart: bool
) -> None:
    label = (
        "Concurrent EnergyTrace HDR (256 kS/s) + UART0"
        if run_uart
        else "EnergyTrace HDR (256 kS/s)"
    )
    print(f"\n=== 6. {label} ({duration_s}s) ===")

    with tempfile.NamedTemporaryFile(
        prefix="et_hil_", suffix=".csv", delete=False
    ) as tmp:
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
                    f"sent={sent:5d} rx={len(rx):5d} match={ok} dt={dt * 1000:6.1f}ms ({bps:8.0f} bps)"
                )
                if not ok:
                    et_proc.terminate()
                    et_proc.wait(timeout=5)
                    first_diff = next(
                        (
                            i
                            for i in range(min(len(rx), len(payload)))
                            if rx[i] != payload[i]
                        ),
                        None,
                    )
                    raise RuntimeError(
                        f"UART0 {label_baud} failed during EnergyTrace: rx={len(rx)}/{size}, first_diff={first_diff}"
                    )

        et_out, _ = et_proc.communicate(timeout=duration_s + 15)
        for line in et_out.strip().splitlines():
            if any(
                k in line
                for k in ("Calibration:", "Capture finished:", "Current:", "Error")
            ):
                print(f"  [ET] {line}")

        if et_proc.returncode != 0:
            raise RuntimeError(
                f"EnergyTrace capture exited with code {et_proc.returncode}"
            )
        if "0 frames lost" not in et_out:
            raise RuntimeError("EnergyTrace capture reported lost frames")
        if "0 reconnects" not in et_out:
            raise RuntimeError("EnergyTrace capture reported USB reconnects")
    finally:
        if os.path.exists(csv_path):
            os.remove(csv_path)


def main() -> int:
    parser = argparse.ArgumentParser(description="LP-XDS110ET HIL stress test suite")
    parser.add_argument(
        "--app-uart",
        default="/dev/ttyACM0",
        help="Application UART CDC device (default: /dev/ttyACM0)",
    )
    parser.add_argument(
        "--aux-uart",
        default="/dev/ttyACM1",
        help="Aux/SWO UART CDC device (default: /dev/ttyACM1)",
    )
    parser.add_argument(
        "--xds110-et", default=None, help="Path to xds110-et CLI binary"
    )
    parser.add_argument(
        "--et-duration",
        type=int,
        default=4,
        help="EnergyTrace capture duration in seconds (default: 4)",
    )
    parser.add_argument(
        "--expect-version",
        default=None,
        help="Expected firmware version string (e.g. 3.0.0.45)",
    )
    parser.add_argument(
        "--blink-identify",
        action="store_true",
        help="Also exercise 0x80000010 probe-identify LED blink",
    )
    parser.add_argument(
        "--skip-gpio",
        action="store_true",
        help="Skip Target Aux GPIO (0x29/0x2A) loopback tests",
    )
    parser.add_argument(
        "--skip-dap",
        action="store_true",
        help="Skip CMSIS-DAP v2 WinUSB & host tool discovery tests",
    )
    parser.add_argument(
        "--skip-uart", action="store_true", help="Skip UART loopback tests"
    )
    parser.add_argument(
        "--skip-et", action="store_true", help="Skip EnergyTrace capture tests"
    )
    args = parser.parse_args()

    xds_cmd = resolve_xds110_et_cmd(args.xds110_et)

    try:
        test_probe_info(xds_cmd, args.expect_version)
        if not args.skip_gpio:
            test_aux_gpio(args.blink_identify)
        if not args.skip_dap:
            test_cmsis_dap_v2()
        if not args.skip_uart:
            test_uart_standalone(args.app_uart)
            test_aux_uart(args.aux_uart)
        if not args.skip_et:
            test_concurrent_et_and_uart(
                xds_cmd, args.app_uart, args.et_duration, run_uart=not args.skip_uart
            )
    except Exception as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        return 1

    print("\nALL HIL STRESS TESTS PASSED!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
