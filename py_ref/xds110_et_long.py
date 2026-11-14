#!/usr/bin/env python3

"""XDS110 EnergyTrace CLI for long (hours/days) captures with a decimated CSV.

Usage:
  xds110_et_long.py info
  xds110_et_long.py capture [-t SECONDS] [--rows-per-hour N] [-o out.csv]
  xds110_et_long.py cmd 46            # send arbitrary command bytes (hex)

Instead of one CSV row per ADC sample (256 kS/s), the samples are grouped into
windows of 3600/N seconds and each window is written as ONE row with the
min/max/avg of the ADC value and the current, plus the bitwise OR of all
status bytes in the window (a non-zero low nibble means some sample had it set).

Default: 3600 rows/hour (1 row/s, ~86 k rows/day). -t 0 (default) runs until Ctrl-C.
Rows are flushed as they are written; Ctrl-C writes the partial last window.

If the probe stops streaming or disappears (USB error, unplug, probe reset) the
partial window is written, a '# reconnect' comment line is added to the CSV and
the capture is restarted (including re-calibration) after --retry-delay seconds.
The time lost is added to time_s, so the time axis stays continuous.

Requires pyusb + libusb. See PROTOCOL.md.
"""

import usb.core
import usb.util
import threading
import argparse
import datetime
import struct
import queue
import time
import sys
import os

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

# Errors that trigger a reconnect attempt during a long capture
RECOVERABLE = (usb.core.USBError, RuntimeError, OSError)


class ProbeError(RuntimeError):
    pass


class XDS110:
    def __init__(self, reset=False):
        self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            raise ProbeError("XDS110 (0451:bef3) not found")
        if reset:
            try:
                self.dev.reset()
            except usb.core.USBError:
                pass
            time.sleep(1.0)
            self.dev = usb.core.find(idVendor=VID, idProduct=PID)
            if self.dev is None:
                raise ProbeError("XDS110 (0451:bef3) not found after reset")
        try:
            self.dev.set_configuration()
        except NotImplementedError:
            raise ProbeError(
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


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def print_live(elapsed, n, rows, reconnects, i_min, i_max, i_avg):
    sys.stderr.write(
        f"\r{elapsed:9.1f} s  {n:>13} samples  {rows:>8} rows  rc={reconnects}  "
        f"min {fmt_current(i_min):>10}  max {fmt_current(i_max):>10}  "
        f"avg {fmt_current(i_avg):>10}   "
    )
    sys.stderr.flush()


def cal_comment(cal, offset, slope):
    return (
        "# calibration (ADC average per point): "
        + ", ".join(f"idx{i}={v:.3f}" for i, v in cal.items())
        + f"\n# offset_adc={offset:.3f} slope_nA_per_lsb={slope}"
        + f" sample_rate_hz={SAMPLE_RATE}\n"
    )


class WindowWriter:
    """Accumulates samples and writes one CSV row per window of `size` samples."""

    HEADER = (
        "index,time_s,utc,samples,frames_lost,adc_min,adc_max,adc_avg,"
        "current_min_nA,current_max_nA,current_avg_nA,status_or"
    )

    def __init__(self, out, size, offset, slope):
        self.out, self.size, self.offset, self.slope = out, size, offset, slope
        self.rows = 0
        self.start = 0  # sample-equivalent position of the current window's start
        self.lost = 0
        self._reset()

    def _reset(self):
        self.n = 0
        self.total = 0
        self.lo = None
        self.hi = None
        self.status = 0

    def add_lost(self, frames):
        self.lost += frames

    def skip(self, seconds):
        """Advance the time axis over a period without data (reconnect gap)."""
        self.start += round(seconds * SAMPLE_RATE)

    def add(self, adcs, status_or):
        """Add a slice of signed ADC values (never crossing a window boundary)."""
        self.n += len(adcs)
        self.total += sum(adcs)
        lo, hi = min(adcs), max(adcs)
        self.lo = lo if self.lo is None else min(self.lo, lo)
        self.hi = hi if self.hi is None else max(self.hi, hi)
        self.status |= status_or
        if self.n >= self.size:
            self.flush()

    def room(self):
        return self.size - self.n

    def flush(self):
        if not self.n:
            return
        avg = self.total / self.n
        cur = lambda a: (a - self.offset) * self.slope
        self.out.write(
            f"{self.rows},{self.start / SAMPLE_RATE:.3f},{utc_now()},{self.n},"
            f"{self.lost},{self.lo},{self.hi},{avg:.2f},{cur(self.lo):.1f},"
            f"{cur(self.hi):.1f},{cur(avg):.1f},{self.status:02x}\n"
        )
        self.out.flush()
        self.rows += 1
        self.start += self.n
        self.lost = 0
        self._reset()


class Stats:
    """Whole-capture statistics, kept across reconnects."""

    def __init__(self):
        self.n = 0
        self.adc_sum = 0
        self.adc_min = None
        self.adc_max = None
        self.lost = 0
        self.reconnects = 0


def stream(x, wr, st, max_samples, offset, slope, t0):
    """Read from an already set-up probe until max_samples or an error."""
    buf = bytearray()
    last_seq = None
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
    last_report = time.perf_counter()
    try:
        x.cmd(START)
        while max_samples is None or st.n < max_samples:
            chunk = chunks.get()
            if isinstance(chunk, Exception):
                raise chunk
            buf += chunk
            for seq, samples in parse_frames(buf):
                if last_seq is not None and seq != (last_seq + 1) & 0xFF:
                    gap = (seq - last_seq - 1) & 0xFF
                    st.lost += gap
                    wr.add_lost(gap)
                last_seq = seq
                pos = 0
                while pos < len(samples):
                    take = min(wr.room(), len(samples) - pos)
                    if max_samples is not None:
                        take = min(take, max_samples - st.n)
                    if take <= 0:
                        break
                    part = samples[pos : pos + take]
                    adcs = [signed24(a) for a, _ in part]
                    s_or = 0
                    for _, s in part:
                        s_or |= s
                    lo, hi = min(adcs), max(adcs)
                    st.adc_min = lo if st.adc_min is None else min(st.adc_min, lo)
                    st.adc_max = hi if st.adc_max is None else max(st.adc_max, hi)
                    st.adc_sum += sum(adcs)
                    st.n += take
                    wr.add(adcs, s_or)
                    pos += take
            now = time.perf_counter()
            if st.n and now - last_report >= 0.5:
                last_report = now
                cur = lambda a: (a - offset) * slope
                print_live(
                    now - t0, st.n, wr.rows, st.reconnects,
                    cur(st.adc_min), cur(st.adc_max), cur(st.adc_sum / st.n),
                )
    finally:
        stop.set()
        t.join()


def shutdown(x):
    """Best-effort probe shutdown; the probe may already be gone."""
    for c in (STOP, CLEANUP, DISCONNECT):
        try:
            x.cmd(c, retries=1)
        except RECOVERABLE:
            pass
    try:
        x.close()
    except RECOVERABLE:
        pass


def cmd_capture(args):
    window = round(SAMPLE_RATE * 3600 / args.rows_per_hour)
    if window < 1:
        sys.exit("--rows-per-hour too large")
    max_samples = int(args.time * SAMPLE_RATE) if args.time > 0 else None

    out = None
    wr = None
    st = Stats()
    x = None
    t0 = time.perf_counter()
    failures = 0
    try:
        while True:
            try:
                x = XDS110(reset=st.reconnects > 0)
                x.drain_data()
                x.setup()
                cal = x.calibrate()
                offset = cal[0]
                print("\ncalibration:", cal, file=sys.stderr)
                if out is None:
                    out = open(args.output, "w", newline="")
                    out.write(cal_comment(cal, offset, args.slope))
                    out.write(
                        f"# rows_per_hour={args.rows_per_hour} samples_per_row={window}"
                        " (status_or = bitwise OR of all status bytes in the row)\n"
                    )
                    out.write(WindowWriter.HEADER + "\n")
                    print(f"writing windowed samples to {args.output}", file=sys.stderr)
                    wr = WindowWriter(out, window, offset, args.slope)
                else:
                    out.write(f"# reconnect {utc_now()} (#{st.reconnects})\n")
                    out.write(cal_comment(cal, offset, args.slope))
                    out.flush()
                    wr.offset = offset
                failures = 0
                stream(x, wr, st, max_samples, offset, args.slope, t0)
                break  # requested time reached
            except KeyboardInterrupt:
                raise
            except RECOVERABLE as e:
                if wr is None:
                    raise  # initial connect failed; don't retry silently
                failures += 1
                lost_at = time.perf_counter()
                wr.flush()  # partial window before the gap
                print(
                    f"\n{utc_now()} probe error: {e!r}; reconnecting "
                    f"(attempt {failures})",
                    file=sys.stderr,
                )
                if x is not None:
                    shutdown(x)
                    x = None
                if args.max_retries and failures > args.max_retries:
                    print("too many failed reconnects, giving up", file=sys.stderr)
                    break
                time.sleep(args.retry_delay)
                st.reconnects += 1
                wr.skip(time.perf_counter() - lost_at)
    except KeyboardInterrupt:
        pass
    finally:
        elapsed_ms = (time.perf_counter() - t0) * 1000
        if wr is not None:
            wr.flush()  # partial last window
        if out is not None:
            out.close()
        if x is not None:
            shutdown(x)
    sys.stderr.write("\n")
    rows = wr.rows if wr else 0
    print(
        f"{st.n} samples, {rows} rows, {st.lost} frames lost, "
        f"{st.reconnects} reconnects",
        file=sys.stderr,
    )
    print(f"capture time: {elapsed_ms:.0f} ms", file=sys.stderr)
    if st.n:
        cur = lambda a: (a - wr.offset) * args.slope
        print(
            f"current: max {fmt_current(cur(st.adc_max))}, "
            f"min {fmt_current(cur(st.adc_min))}, "
            f"avg {fmt_current(cur(st.adc_sum / st.n))}",
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
    c.add_argument(
        "-t", "--time", type=float, default=0.0, help="seconds (0 = until Ctrl-C)"
    )
    c.add_argument(
        "-r",
        "--rows-per-hour",
        type=int,
        default=3600,
        help="CSV rows written per hour of capture (default 3600 = 1 row/s)",
    )
    c.add_argument(
        "-o",
        "--output",
        default=None,
        help="CSV file (default: logs/long_<timestamp>.csv)",
    )
    c.add_argument("--slope", type=float, default=14.6626, help="nA per ADC LSB")
    c.add_argument(
        "--retry-delay",
        type=float,
        default=5.0,
        help="seconds to wait before each reconnect attempt (default 5)",
    )
    c.add_argument(
        "--max-retries",
        type=int,
        default=0,
        help="give up after N consecutive failed reconnects (0 = never, default)",
    )
    c.set_defaults(f=cmd_capture)
    a = p.parse_args()
    if a.c == "capture":
        if a.rows_per_hour < 1:
            p.error("--rows-per-hour must be >= 1")
        if not a.output:
            log_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
            os.makedirs(log_dir, exist_ok=True)
            a.output = os.path.join(log_dir, time.strftime("long_%Y%m%d_%H%M%S.csv"))
    try:
        a.f(a)
    except ProbeError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()

# EOF
