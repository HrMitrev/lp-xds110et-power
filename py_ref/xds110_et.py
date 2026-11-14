#!/usr/bin/env python3

"""XDS110 EnergyTrace CLI: dump raw ADC readings (and optionally current).

Usage:
  xds110_et.py info
  xds110_et.py capture -t 2 -o raw.csv [--current] [--raw-hex]
  xds110_et.py cmd 46            # send arbitrary command bytes (hex)

Requires pyusb + libusb. See PROTOCOL.md.
"""

import usb.core
import usb.util
import threading
import argparse
import struct
import queue
import time
import sys

VID, PID = 0x0451, 0xBEF3
CMD_IF, CMD_OUT, CMD_IN = 2, 0x02, 0x83
DATA_IF, DATA_IN = 6, 0x87
TIMEOUT = 4000

CONNECT, DISCONNECT = 0x28, 0x02
SETUP, CALIBRATE, START, STOP, CLEANUP = 0x1D, 0x1E, 0x1F, 0x20, 0x21
HWINFO, SET_RANGE, SET_DIG = 0x46, 0x30, 0x31

FRAME_LEN = 1030
SAMPLES_PER_FRAME = 256
SAMPLE_RATE = 256000  # observed; the 48000 in Setup is the LPRF frequency


class XDS110:
    def __init__(self, reset=False):
        self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            sys.exit("XDS110 (0451:bef3) not found")
        if reset:
            self.dev.reset()
            time.sleep(1.0)
            self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        try:
            self.dev.set_configuration()
        except NotImplementedError:
            sys.exit(
                "Cannot open XDS110 (libusb: operation not supported). Replug the "
                "probe and close other tools using it (e.g. Code Composer Studio)."
            )
        except usb.core.USBError:
            pass  # already configured
        for i in (CMD_IF, DATA_IF):
            # Not implemented on Windows (WinUSB) and some other backends
            try:
                if self.dev.is_kernel_driver_active(i):
                    self.dev.detach_kernel_driver(i)
            except (NotImplementedError, usb.core.USBError):
                pass
        usb.util.claim_interface(self.dev, CMD_IF)
        usb.util.claim_interface(self.dev, DATA_IF)

    def close(self):
        usb.util.dispose_resources(self.dev)

    def cmd(self, cmd, args=b"", retries=2):
        payload = bytes([cmd]) + args
        frame = b"*" + struct.pack("<H", len(payload)) + payload
        for attempt in range(retries):
            try:
                self.dev.write(CMD_OUT, frame, TIMEOUT)
                rsp = bytes(self.dev.read(CMD_IN, 0x1100, TIMEOUT))
                break
            except usb.core.USBError:
                if attempt == retries - 1:
                    raise
        if rsp[:1] != b"*":
            raise RuntimeError(f"bad response {rsp.hex()}")
        n = struct.unpack_from("<H", rsp, 1)[0]
        err = struct.unpack_from("<i", rsp, 3)[0]
        if err:
            raise RuntimeError(f"cmd 0x{cmd:02x} failed, error {err}")
        return rsp[7 : 3 + n]

    def drain_data(self):
        while True:
            try:
                self.dev.read(DATA_IN, 0x2000, 100)
            except usb.core.USBError:
                return

    def hw_info(self):
        d = self.cmd(HWINFO)
        return struct.unpack("<IB", d[:5])

    def setup(self, freq=48000):
        self.cmd(CONNECT)
        self.cmd(SET_RANGE, b"\x00")
        self.cmd(SET_DIG, b"\x01")
        self.cmd(SETUP, struct.pack("<BBIB", 1, 0, freq, 0))

    def calibrate(self):
        """Return {idx: ADC average} for the calibration points seen in stune."""
        out = {}
        for idx in (0, 4, 1):
            d = self.cmd(CALIBRATE, struct.pack("<H", idx))
            total, count = struct.unpack("<iI", d[:8])
            out[idx] = total / count if count else float("nan")
        return out


def parse_frames(buf):
    """Yield (seq, [(adc, status)...]) from a bytearray, consuming whole frames.

    Handles the 3-byte '35 80 bb' preamble sent at stream start.
    """
    pos = 0
    while True:
        if buf[pos : pos + 1] == b"\x35" and len(buf) - pos >= 3:
            pos += 3
            continue
        if len(buf) - pos < FRAME_LEN:
            break
        if buf[pos] != 0x68:
            pos += 1  # resync
            continue
        seq = buf[pos + 1]
        words = struct.unpack_from(">256I", buf, pos + 6)
        pos += FRAME_LEN
        yield seq, [(w >> 8, w & 0xFF) for w in words]
    del buf[:pos]


def signed24(v):
    return v - (1 << 24) if v & 0x800000 else v


def cmd_info(args):
    x = XDS110()
    x.cmd(CONNECT)
    hw, extra = x.hw_info()
    print(f"HardwareInfo: 0x{hw:08x} 0x{extra:02x}")
    x.close()


def cmd_raw(args):
    x = XDS110()
    rsp = x.cmd(int(args.bytes[0], 16), bytes.fromhex("".join(args.bytes[1:])))
    print(rsp.hex())
    x.close()


def fmt_current(na):
    ua = na / 1000
    return f"{ua / 1000:.3f} mA" if abs(ua) > 1000 else f"{ua:.3f} uA"


def cmd_capture(args):
    x = XDS110()
    x.drain_data()
    x.setup()
    cal = x.calibrate()
    offset = cal[0]
    print("calibration:", cal, file=sys.stderr)
    out = open(args.output, "w", newline="") if args.output else sys.stdout
    hdr = "index,time_s,adc,status" + (",current_nA" if args.current else "")
    out.write(
        "# calibration (ADC average per point): "
        + ", ".join(f"idx{i}={v:.3f}" for i, v in cal.items())
        + f"\n# offset_adc={offset:.3f} slope_nA_per_lsb={args.slope}"
        + f" sample_rate_hz={SAMPLE_RATE}\n"
    )
    out.write(hdr + "\n")

    buf = bytearray()
    idx = 0
    lost = 0
    last_seq = None
    max_samples = int(args.time * SAMPLE_RATE)
    n_total, i_sum = 0, 0.0
    i_min, i_max = float("inf"), float("-inf")
    # A dedicated thread keeps USB reads going so slow output (e.g. a console)
    # never stalls the device; parsing and writing happen on the main thread.
    chunks = queue.Queue()
    stop = threading.Event()

    def reader():
        idle = 0
        while not stop.is_set():
            try:
                chunks.put(bytes(x.dev.read(DATA_IN, 0x4000, 200)))
                idle = 0
            except usb.core.USBError as e:
                idle += 200
                if idle >= TIMEOUT:
                    chunks.put(e)
                    return

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    t0 = time.perf_counter()
    x.cmd(START)
    try:
        while idx < max_samples:
            chunk = chunks.get()
            if isinstance(chunk, Exception):
                raise chunk
            buf += chunk
            rows = []
            for seq, samples in parse_frames(buf):
                if last_seq is not None and seq != (last_seq + 1) & 0xFF:
                    lost += (seq - last_seq - 1) & 0xFF
                last_seq = seq
                for adc, st in samples:
                    if idx >= max_samples:
                        break
                    a = signed24(adc)
                    # Single-slope approximation (first segment, nA/LSB)
                    na = (a - offset) * args.slope
                    n_total += 1
                    i_sum += na
                    i_min = min(i_min, na)
                    i_max = max(i_max, na)
                    row = f"{idx},{idx / SAMPLE_RATE:.7f},{a},{st:02x}"
                    if args.current:
                        row += f",{na:.1f}"
                    rows.append(row)
                    idx += 1
            if rows:
                out.write("\n".join(rows) + "\n")
    finally:
        elapsed_ms = (time.perf_counter() - t0) * 1000
        stop.set()
        t.join()
        x.cmd(STOP)
        x.cmd(CLEANUP)
        x.cmd(DISCONNECT)  # stune ends its session with this; leaves the probe idle
        if args.output:
            out.close()
        x.close()
    print(f"{idx} samples, {lost} frames lost", file=sys.stderr)
    print(f"capture time: {elapsed_ms:.0f} ms", file=sys.stderr)
    if n_total:
        print(
            f"current: max {fmt_current(i_max)}, min {fmt_current(i_min)}, "
            f"avg {fmt_current(i_sum / n_total)}",
            file=sys.stderr,
        )


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="c", required=True)
    sub.add_parser("info").set_defaults(f=cmd_info)
    r = sub.add_parser("cmd")
    r.add_argument("bytes", nargs="+", help="cmd byte then args as hex")
    r.set_defaults(f=cmd_raw)
    c = sub.add_parser("capture")
    c.add_argument("-t", "--time", type=float, default=1.0, help="seconds")
    c.add_argument("-o", "--output", help="CSV file (default stdout)")
    c.add_argument(
        "--current", action="store_true", help="add approximate current column"
    )
    c.add_argument("--slope", type=float, default=14.6626, help="nA per ADC LSB")
    c.set_defaults(f=cmd_capture)
    a = p.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()

# EOF
