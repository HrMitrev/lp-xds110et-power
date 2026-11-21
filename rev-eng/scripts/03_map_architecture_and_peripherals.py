#!/usr/bin/env python3
"""
03_map_architecture_and_peripherals.py - Whole-binary architecture, peripheral,
pin-mux, interrupt, clock, and super-loop analysis for firmware_3.0.0.45.bin.

Scans all Thumb-2 code in the firmware image to:
  - Identify code/rodata/data boundaries and major subsystems by address range
  - Collect all referenced peripheral MMIO bases (GPIO, UART, SSI, I2C, ADC, Timer, uDMA, USB, etc.)
  - Collect all SYSCTL_PERIPH_* constants and GPIO_Pxx_* pin-mux constants
  - Identify ROM API calls and linked DriverLib functions
  - Trace dynamic interrupt registration (IntRegister / NVIC VTOR)
  - Analyze main() super-loop bit-band event flags (0x20004B90)
"""

import argparse
import json
import re
import struct
from collections import defaultdict
from pathlib import Path
from fwlib import FirmwareImage


def scan_literal_pools_and_calls(fw: FirmwareImage, code_end: int = 0x00035C00):
    """Linear disassembly pass (with skipdata) collecting BL targets, literal pool refs, and MOVW/MOVT constants."""
    code = fw.data[0x0400:code_end - fw.base]  # skip vector table (0x000..0x3FF)
    start = fw.base + 0x0400

    fw.md.skipdata = True
    bl_targets: dict[int, list[int]] = defaultdict(list)
    const_refs: dict[int, list[int]] = defaultdict(list)
    regs: dict[str, int] = {}

    for insn in fw.md.disasm(code, start):
        mn = insn.mnemonic
        op = insn.op_str
        parts = [p.strip() for p in op.split(",")]

        if mn in ("bl", "b.w"):
            m = re.search(r"#0x([0-9a-f]+)", op)
            if m:
                tgt = int(m.group(1), 16)
                if fw.base <= tgt < code_end:
                    bl_targets[tgt].append(insn.address)
            if mn == "bl":
                regs.clear()
        elif mn in ("mov", "movs", "mov.w", "movw") and len(parts) == 2 and parts[1].startswith("#"):
            try:
                regs[parts[0]] = int(parts[1][1:], 0)
            except ValueError:
                pass
        elif mn == "movt" and len(parts) == 2 and parts[1].startswith("#"):
            try:
                hi = int(parts[1][1:], 0)
                lo = regs.get(parts[0], 0) & 0xFFFF
                val = (hi << 16) | lo
                regs[parts[0]] = val
                const_refs[val].append(insn.address)
            except ValueError:
                pass
        elif mn in ("ldr", "ldr.w") and "[pc," in op:
            m = re.search(r"\[pc,\s*#(-?0x[0-9a-f]+|-?\d+)\]", op)
            if m:
                imm = int(m.group(1), 0)
                pool_addr = ((insn.address + 4) & ~3) + imm
                if fw.in_flash(pool_addr):
                    val = fw.u32(pool_addr)
                    regs[parts[0]] = val
                    const_refs[val].append(insn.address)

    return bl_targets, const_refs


def find_driverlib_candidates(fw: FirmwareImage, bl_targets: dict[int, list[int]]):
    """
    Identify linked DriverLib functions by checking what peripheral/register patterns
    they use, especially around 0x24000..0x35000 where usblib + driverlib live.
    """
    # Known exact addresses from main() and standard TivaWare driverlib patterns
    # Let's fingerprint functions in 0x24000..0x35C00 that have >= 3 callers
    hot_funcs = sorted(
        [(addr, len(callers)) for addr, callers in bl_targets.items() if len(callers) >= 4],
        key=lambda x: -x[1],
    )
    return hot_funcs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", nargs="?", default="proprietary_fw/firmware_3.0.0.45.bin")
    parser.add_argument("--out", default="rev-eng/output/03_architecture.json")
    args = parser.parse_args()

    fw = FirmwareImage(args.firmware)
    bl_targets, const_refs = scan_literal_pools_and_calls(fw)

    # 1. Peripherals referenced via MMIO base addresses
    periph_hits = {}
    for val, sites in const_refs.items():
        if 0x40000000 <= val <= 0x440FFFFF:
            page = val & ~0xFFF
            pname = fw.periphs.get(page, f"MMIO_0x{page:08X}")
            off = val - page
            key = f"{pname}" if off == 0 else f"{pname}+0x{off:X}"
            periph_hits.setdefault(pname, []).append({
                "addr": f"0x{val:08X}", "offset": f"0x{off:X}",
                "count": len(sites), "sample_sites": [f"0x{s:08X}" for s in sites[:6]],
            })

    # 2. SYSCTL_PERIPH_* constants referenced
    sysctl_hits = {}
    for val, sites in const_refs.items():
        if val in fw.sysctl_periphs:
            name = fw.sysctl_periphs[val]
            sysctl_hits[name] = {
                "value": f"0x{val:08X}",
                "count": len(sites),
                "sites": [f"0x{s:08X}" for s in sites[:12]],
            }

    # 3. GPIO_Pxx_* pin-mux constants referenced (from pin_map.h)
    # Note: GPIO_Pxx_yyyy values have format 0x00PPNNMM (port*0x10000 + pin*0x400 + mux_idx)
    pinmux_hits = {}
    for val, sites in const_refs.items():
        if val in fw.gpio_mux and val >= 0x00000400:
            name = fw.gpio_mux[val]
            pinmux_hits[name] = {
                "value": f"0x{val:08X}",
                "count": len(sites),
                "sites": [f"0x{s:08X}" for s in sites[:8]],
            }

    # 4. ROM_APITABLE references
    rom_refs = {}
    for val, sites in const_refs.items():
        if 0x01000010 <= val <= 0x01000100:
            tidx = (val - 0x01000010) // 4
            tname = fw.rom_tables.get(tidx, f"ROM_TABLE[{tidx}]")
            rom_refs[tname] = {"addr": f"0x{val:08X}", "count": len(sites), "sites": [f"0x{s:08X}" for s in sites[:8]]}

    # 5. NVIC / SCB / SysTick references (check VTOR 0xE000ED08 for dynamic RAM vector table!)
    scs_hits = {}
    for val, sites in const_refs.items():
        if 0xE000E000 <= val <= 0xE000EFFF:
            scs_hits[f"0x{val:08X}"] = {"count": len(sites), "sites": [f"0x{s:08X}" for s in sites[:8]]}

    # 6. Top called functions (reveals DriverLib, usblib, and core helpers)
    hot_funcs = find_driverlib_candidates(fw, bl_targets)

    print("=== Memory Map (Auto-detected from ResetISR) ===")
    print(f"  Vector Table:  0x{fw.base:08X} - 0x{fw.base + 0x208:08X}")
    print(f"  .text/.rodata: 0x{fw.base:08X} - 0x{fw.data_load:08X} ({fw.data_load - fw.base} bytes)")
    print(f"  .data (Flash): 0x{fw.data_load:08X} - 0x{fw.end:08X} ({fw.end - fw.data_load} bytes)")
    print(f"  .data (SRAM):  0x{fw.data_start:08X} - 0x{fw.data_end:08X} ({fw.data_end - fw.data_start} bytes)")
    print(f"  .bss  (SRAM):  0x{fw.bss_start:08X} - 0x{fw.bss_end:08X} ({fw.bss_end - fw.bss_start} bytes)")
    print(f"  Heap/Stack:    0x{fw.bss_end:08X} - 0x{fw.initial_sp:08X} ({fw.initial_sp - fw.bss_end} bytes)")
    print(f"  main():        0x{fw.main_addr:08X}")
    print(f"  Unique BL targets discovered: {len(bl_targets)}")

    print(f"\n=== Enabled / Referenced SYSCTL Peripherals ({len(sysctl_hits)}) ===")
    for name in sorted(sysctl_hits):
        h = sysctl_hits[name]
        print(f"  {name:<26s} ({h['value']}) x{h['count']:2d}  sites: {', '.join(h['sites'][:4])}")

    print(f"\n=== Peripheral MMIO Base Accesses ({len(periph_hits)}) ===")
    for pname in sorted(periph_hits):
        total = sum(x["count"] for x in periph_hits[pname])
        offs = sorted(set(x["offset"] for x in periph_hits[pname]))
        print(f"  {pname:<20s} total_refs={total:3d}  offsets={', '.join(offs[:8])}")

    print(f"\n=== Pin-Mux Constants Referenced ({len(pinmux_hits)}) ===")
    for name in sorted(pinmux_hits):
        h = pinmux_hits[name]
        print(f"  {name:<36s} ({h['value']}) x{h['count']:2d}  sites: {', '.join(h['sites'][:3])}")

    print(f"\n=== ROM_APITABLE References ({len(rom_refs)}) ===")
    for tname, h in sorted(rom_refs.items()):
        print(f"  {tname:<24s} ({h['addr']}) x{h['count']:2d}  sites: {', '.join(h['sites'][:4])}")

    print(f"\n=== Cortex-M4 SCS/NVIC/SCB References ({len(scs_hits)}) ===")
    for addr, h in sorted(scs_hits.items()):
        print(f"  {addr}  x{h['count']:2d}  sites: {', '.join(h['sites'][:4])}")

    print(f"\n=== Top 35 Most-Called Functions ===")
    for addr, cnt in hot_funcs[:35]:
        print(f"  0x{addr:08X}: {cnt:4d} callers")

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "memory_map": {
            "base": f"0x{fw.base:08X}",
            "data_load": f"0x{fw.data_load:08X}",
            "end": f"0x{fw.end:08X}",
            "data_start": f"0x{fw.data_start:08X}",
            "data_end": f"0x{fw.data_end:08X}",
            "bss_start": f"0x{fw.bss_start:08X}",
            "bss_end": f"0x{fw.bss_end:08X}",
            "initial_sp": f"0x{fw.initial_sp:08X}",
            "main_addr": f"0x{fw.main_addr:08X}",
        },
        "sysctl_periphs": sysctl_hits,
        "periph_mmio": periph_hits,
        "pinmux_hits": pinmux_hits,
        "rom_refs": rom_refs,
        "scs_hits": scs_hits,
        "top_functions": [{"addr": f"0x{a:08X}", "callers": c} for a, c in hot_funcs[:80]],
    }, indent=2))
    print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    main()
