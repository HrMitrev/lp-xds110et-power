#!/usr/bin/env python3
"""Plot min/max/avg current from an xds110_et_long.py CSV log.

Usage: python plot_long_log.py [log.csv]   (without argument: GUI file picker for logs/long_*.csv)

Controls: mouse wheel = zoom X around cursor, left-drag = move window,
Shift+wheel / bottom slider = scroll X, toolbar zoom/pan also work.
'r' resets the view. The box shows avg/min/max for the visible window.
Left axis = avg, right axis = min/max; the "Min/Max" button hides/shows min/max.
"Live" button reloads the CSV every second (for a capture still running);
it keeps following the end of the data if the window is at the end.
"""

import csv
import sys
from bisect import bisect_left, bisect_right
from datetime import datetime
from pathlib import Path

import tkinter as tk

import matplotlib.pyplot as plt
from matplotlib.widgets import Button, Slider

LOG_DIR = Path(__file__).parent / "logs"


def load(path):
    t, mn, mx, avg = [], [], [], []
    with open(path, newline="") as f:
        rows = csv.DictReader(line for line in f if not line.startswith("#"))
        for r in rows:
            t.append(float(r["time_s"]))
            mn.append(float(r["current_min_nA"]))
            mx.append(float(r["current_max_nA"]))
            avg.append(float(r["current_avg_nA"]))
    return t, mn, mx, avg


def load_safe(path):
    """Like load(), but tolerates a half-written last row while capturing live."""
    try:
        return load(path)
    except (ValueError, KeyError, TypeError):
        t, mn, mx, avg = [], [], [], []
        with open(path, newline="") as f:
            rows = csv.DictReader(line for line in f if not line.startswith("#"))
            for r in rows:
                try:
                    row = (
                        float(r["time_s"]),
                        float(r["current_min_nA"]),
                        float(r["current_max_nA"]),
                        float(r["current_avg_nA"]),
                    )
                except (ValueError, KeyError, TypeError):
                    continue
                for lst, v in zip((t, mn, mx, avg), row):
                    lst.append(v)
        return t, mn, mx, avg


def describe(path):
    """Human-readable date/duration for long_YYYYMMDD_HHMMSS.csv."""
    try:
        stamp = datetime.strptime(path.stem[len("long_") :], "%Y%m%d_%H%M%S")
        date = stamp.strftime("%Y-%m-%d %H:%M:%S")
    except ValueError:
        date = "unknown date"
    with open(path) as f:
        rows = sum(1 for line in f if line[:1].isdigit())
    return f"{date}  ({rows} s of data)"


def choose_file():
    files = sorted(LOG_DIR.glob("long_*.csv"), key=lambda p: p.name)
    if not files:
        sys.exit(f"no long_*.csv files in {LOG_DIR}")
    root = tk.Tk()
    root.title("Select long_*.csv log")
    root.geometry("620x320")
    result = []

    box = tk.Listbox(root, font=("Consolas", 10), activestyle="none")
    for p in files:
        box.insert(tk.END, f"{p.name}   {describe(p)}")
    box.selection_set(len(files) - 1)
    box.see(len(files) - 1)
    box.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

    def accept(_=None):
        sel = box.curselection()
        if sel:
            result.append(files[sel[0]])
            root.destroy()

    box.bind("<Double-Button-1>", accept)
    root.bind("<Return>", accept)
    root.bind("<Escape>", lambda _: root.destroy())
    tk.Button(root, text="Open", width=12, command=accept).pack(pady=(0, 6))
    box.focus_set()
    root.mainloop()
    if not result:
        sys.exit("no file selected")
    return result[0]


def pick_unit(peak_nA):
    p = abs(peak_nA)
    if p > 1e6:
        return "mA", 1e-6
    if p > 1e3:
        return "uA", 1e-3
    return "nA", 1.0


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else choose_file()
    raw_t, raw_mn, raw_mx, raw_avg = load(path)
    if not raw_t:
        sys.exit("no data rows in " + str(path))

    t, mn, mx, avg = [], [], [], []
    state = {"unit": "nA", "fill": None}

    def rescale(data):
        """Refill the scaled lists in place (nA -> nA/uA/mA) and return the full X range."""
        d_t, d_mn, d_mx, d_avg = data
        unit, k = pick_unit(max(abs(v) for v in d_mx + d_mn))
        state["unit"] = unit
        state["k"] = k
        t[:] = d_t
        mn[:] = [v * k for v in d_mn]
        mx[:] = [v * k for v in d_mx]
        avg[:] = [v * k for v in d_avg]
        return (t[0], t[-1] if t[-1] > t[0] else t[0] + 1)

    full = list(rescale((raw_t, raw_mn, raw_mx, raw_avg)))

    fig, ax = plt.subplots(figsize=(12, 6.5))
    fig.subplots_adjust(bottom=0.17, top=0.94, right=0.9)
    ax2 = ax.twinx()
    # keep the avg axis (left) drawn and interactive above the min/max axis (right)
    ax.set_zorder(ax2.get_zorder() + 1)
    ax.patch.set_visible(False)
    state["fill"] = ax2.fill_between(
        t, mn, mx, color="tab:blue", alpha=0.2, label="min..max"
    )
    (l_mx,) = ax2.plot(t, mx, color="tab:red", lw=0.8, label="max")
    (l_mn,) = ax2.plot(t, mn, color="tab:blue", lw=0.8, label="min")
    (l_avg,) = ax.plot(t, avg, color="tab:green", lw=1.5, label="avg")
    ax.set_xlabel("time [s]")
    ax.set_ylabel(f"avg [{state['unit']}]", color="tab:green")
    ax2.set_ylabel(f"min/max [{state['unit']}]", color="tab:red")
    ax.set_title(path.name)
    ax.grid(True, alpha=0.4)

    def update_legend():
        handles = [l_avg]
        if ax2.get_visible():
            handles += [state["fill"], l_mx, l_mn]
        ax.legend(handles=handles, loc="upper right")

    update_legend()

    ax.set_xlim(*full)

    sax = fig.add_axes([0.26, 0.03, 0.62, 0.03])
    slider = Slider(sax, "scroll", full[0], full[1], valinit=full[0])
    updating = [False]

    def set_xlim(lo, hi):
        width = hi - lo
        width = min(width, full[1] - full[0])
        lo = max(full[0], min(lo, full[1] - width))
        ax.set_xlim(lo, lo + width)
        updating[0] = True
        slider.set_val(lo)
        updating[0] = False
        fig.canvas.draw_idle()

    def on_slider(val):
        if updating[0]:
            return
        lo, hi = ax.get_xlim()
        set_xlim(val, val + (hi - lo))

    def on_scroll(ev):
        if ev.inaxes not in (ax, ax2):
            return
        lo, hi = ax.get_xlim()
        if ev.key == "shift":
            step = (hi - lo) * 0.1 * (-1 if ev.button == "down" else 1)
            set_xlim(lo + step, hi + step)
            return
        f = 0.8 if ev.button == "up" else 1.25
        x = ev.xdata
        set_xlim(x - (x - lo) * f, x + (hi - x) * f)

    def on_key(ev):
        if ev.key == "r":
            set_xlim(*full)

    # toolbar zoom/pan changes xlim directly; keep slider in sync
    info = ax.text(
        0.01,
        0.98,
        "",
        transform=ax.transAxes,
        va="top",
        bbox=dict(boxstyle="round", fc="white", alpha=0.85),
    )

    def update_info(lo, hi):
        i, j = bisect_left(t, lo), bisect_right(t, hi)
        if j <= i:
            info.set_text("no data in window")
            return
        unit = state["unit"]
        avg_nA = sum(avg[i:j]) / (j - i) / state["k"]
        a = abs(avg_nA)
        a_unit, a_k = (
            ("mA", 1e-6) if a >= 1e6 else ("uA", 1e-3) if a >= 1e3 else ("nA", 1.0)
        )
        lines = [
            f"window {lo:.1f}..{hi:.1f} s",
            f"avg: {avg_nA * a_k:.4g} {a_unit}",
        ]
        if ax2.get_visible():
            lines += [
                f"min: {min(mn[i:j]):.4g} {unit}",
                f"max: {max(mx[i:j]):.4g} {unit}",
            ]
        info.set_text("\n".join(lines))

    def on_xlim(a):
        lo, hi = a.get_xlim()
        updating[0] = True
        slider.set_val(max(full[0], min(lo, full[1])))
        updating[0] = False
        update_info(lo, hi)

    drag = {}

    def panning():
        tb = fig.canvas.toolbar
        return tb is not None and tb.mode != ""

    def on_press(ev):
        if ev.inaxes in (ax, ax2) and ev.button == 1 and not panning():
            drag["x"], drag["xlim"] = ev.x, ax.get_xlim()

    def on_motion(ev):
        if "x" not in drag:
            return
        lo, hi = drag["xlim"]
        dx = (ev.x - drag["x"]) / ax.bbox.width * (hi - lo)
        set_xlim(lo - dx, hi - dx)

    def on_release(ev):
        drag.clear()

    fig.canvas.mpl_connect("button_press_event", on_press)
    fig.canvas.mpl_connect("motion_notify_event", on_motion)
    fig.canvas.mpl_connect("button_release_event", on_release)
    update_info(*full)

    def refresh():
        data = load_safe(path)
        if not data[0] or (len(data[0]) == len(t) and data[0][-1] == t[-1]):
            return
        lo, hi = ax.get_xlim()
        was_full = lo <= full[0] and hi >= full[1]
        at_end = hi >= full[1] - 1e-9
        old_unit = state["unit"]
        full[:] = rescale(data)
        for line, ys in ((l_mx, mx), (l_avg, avg), (l_mn, mn)):
            line.set_data(t, ys)
        state["fill"].remove()
        state["fill"] = ax2.fill_between(
            t, mn, mx, color="tab:blue", alpha=0.2, label="min..max"
        )
        update_legend()
        if state["unit"] != old_unit:
            ax.set_ylabel(f"avg [{state['unit']}]", color="tab:green")
            ax2.set_ylabel(f"min/max [{state['unit']}]", color="tab:red")
        for a in (ax, ax2):
            a.relim()
            a.autoscale(axis="y")
        slider.valmin, slider.valmax = full
        sax.set_xlim(*full)
        if was_full:
            set_xlim(*full)
        elif at_end:
            set_xlim(full[1] - (hi - lo), full[1])
        else:
            set_xlim(lo, hi)
        update_info(*ax.get_xlim())

    timer = fig.canvas.new_timer(interval=1000)
    timer.add_callback(refresh)

    bax = fig.add_axes([0.01, 0.015, 0.07, 0.035])
    live_btn = Button(bax, "Live: OFF")

    def toggle_live(_):
        if live_btn.label.get_text().endswith("OFF"):
            live_btn.label.set_text("Live: ON")
            refresh()
            timer.start()
        else:
            live_btn.label.set_text("Live: OFF")
            timer.stop()
        fig.canvas.draw_idle()

    live_btn.on_clicked(toggle_live)

    mm_btn = Button(fig.add_axes([0.09, 0.015, 0.09, 0.035]), "Min/Max: ON")

    def toggle_minmax(_):
        show = not ax2.get_visible()
        ax2.set_visible(show)
        mm_btn.label.set_text("Min/Max: ON" if show else "Min/Max: OFF")
        update_legend()
        update_info(*ax.get_xlim())
        fig.canvas.draw_idle()

    mm_btn.on_clicked(toggle_minmax)

    slider.on_changed(on_slider)
    ax.callbacks.connect("xlim_changed", on_xlim)
    fig.canvas.mpl_connect("scroll_event", on_scroll)
    fig.canvas.mpl_connect("key_press_event", on_key)
    plt.show()


if __name__ == "__main__":
    main()
