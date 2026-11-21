#!/usr/bin/env python3
"""
04_analyze_xds_protocol.py - Complete extraction of the XDS110 USB Bulk command
protocol (opcodes 0x00..0x62) from the TBH dispatch table at 0x0000688A, plus
all dynamic ISR registrations via IntRegister.
"""

import argparse
import json
import re
from pathlib import Path
from fwlib import FirmwareImage

TBH_ADDR = 0x0000688A
TABLE_BASE = 0x0000688E
MAX_OPCODE = 0x62
DEFAULT_CASE = 0x000074A8

# Known opcode names from PROTOCOL.md & public XDS110 / OpenOCD documentation
KNOWN_OPCODES = {
    0x00: "XDS_ECHO / LOOPBACK",
    0x01: "XDS_CONNECT",
    0x02: "XDS_DISCONNECT",
    0x03: "XDS_VERSION",
    0x04: "XDS_SET_TCK_DELAY",
    0x05: "XDS_SET_TRST",
    0x06: "XDS_CYCLE_TCK",
    0x07: "XDS_SET_JTAG_MODE / CONFIG_JTAG",
    0x08: "XDS_GET_POWER / TARGET_VOLTAGE",
    0x09: "XDS_SET_SRST / CONFIG_TARGET",
    0x0A: "XDS_JTAG_SCAN / IR_DR",
    0x0B: "XDS_JTAG_DR_SHIFT",
    0x0C: "XDS_JTAG_IR_SHIFT",
    0x0D: "XDS_JTAG_STATE_MOVE",
    0x0E: "XDS_SET_SRST (Assert/Deassert nSRST)",
    0x0F: "CJTAG_CONNECT",
    0x10: "CJTAG_DISCONNECT",
    0x11: "CJTAG_SCAN",
    0x12: "CJTAG_ESCAPE",
    0x13: "SWD_CONNECT",
    0x14: "SWD_DISCONNECT",
    0x15: "SWD_CLEAR_ERRORS",
    0x16: "SWD_READ_DAP",
    0x17: "SWD_WRITE_DAP",
    0x18: "SWD_DAP_BATCH / TRANSFER",
    0x1D: "ET_SETUP",
    0x1E: "ET_CALIBRATE",
    0x1F: "ET_START",
    0x20: "ET_STOP",
    0x21: "ET_CLEANUP",
    0x22: "ET_DCDC_GET_MCU_VERSION",
    0x23: "ET_DCDC_POWER_DOWN_MCU",
    0x24: "ET_DCDC_SET_VCC",
    0x25: "ET_DCDC_RESTART_MCU",
    0x26: "XDS_ENTER_DFU_MODE",
    0x27: "XDS_GET_SERIAL / READ_CONFIG",
    0x28: "XDS_CONNECT_ET",
    0x29: "XDS_DISCONNECT_ET",
    0x2A: "GPIO_SET_OUT",
    0x2B: "GPIO_GET_IN",
    0x2C: "GPIO_CONFIG",
    0x2D: "GPIO_SET_DIR",
    0x30: "ET_SETUP_RANGE",
    0x31: "ET_SETUP_DIG",
    0x32: "CM_READ_MEM",
    0x33: "CM_WRITE_MEM",
    0x34: "DAP_ACCESS_REG",
    0x35: "DAP_ACCESS_MEM",
    0x44: "ET_SETUP_DIG_PARAMS",
    0x46: "ET_HARDWARE_INFO",
    0x4A: "XDS_PING / KEEP_ALIVE",
}


def extract_tbh_table(fw: FirmwareImage):
    entries = []
    for op in range(MAX_OPCODE + 1):
        hw = fw.u16(TABLE_BASE + op * 2)
        target = TABLE_BASE + hw * 2
        entries.append((op, target))
    return entries


def analyze_case(fw: FirmwareImage, start_addr: int, next_case_addr: int) -> dict:
    """Disassemble a case block up to next_case_addr (capped at 256 bytes)."""
    size = min(max(next_case_addr - start_addr, 4), 256)
    lines = fw.disasm_function(start_addr, size)
    callees = []
    req_reads = []
    resp_writes = []
    epilogue_branches = []

    for line in lines:
        # Extract BL callees
        m = re.search(r"\bbl\s+#0x([0-9a-f]+)", line)
        if m:
            callees.append(f"0x{int(m.group(1), 16):08X}")
        # Extract reads from [r7, #off] (request buffer)
        m = re.search(r"(ldr\w*)\s+\w+,\s*\[r7(?:,\s*#(0x[0-9a-f]+|\d+))?\]", line)
        if m:
            off = int(m.group(2), 0) if m.group(2) else 0
            req_reads.append(f"{m.group(1)}@+{off}")
        # Extract writes to [r6, #off] (response buffer)
        m = re.search(r"(str\w*)\s+\w+,\s*\[r6(?:,\s*#(0x[0-9a-f]+|\d+))?\]", line)
        if m:
            off = int(m.group(2), 0) if m.group(2) else 0
            resp_writes.append(f"{m.group(1)}@+{off}")
        # Epilogue branch
        m = re.search(r"\b(b\.w|b)\s+#0x([0-9a-f]+)", line)
        if m:
            tgt = int(m.group(2), 16)
            if tgt >= 0x7200:
                epilogue_branches.append(f"0x{tgt:08X}")

    return {
        "addr": f"0x{start_addr:08X}",
        "size": size,
        "callees": callees,
        "req_reads": req_reads,
        "resp_writes": resp_writes,
        "epilogue": epilogue_branches,
        "disasm": lines[:28],
    }


def find_int_register_calls(fw: FirmwareImage):
    """Find IntRegister function (writes VTOR 0xE000ED08) and all callers with (irq_num, handler_addr)."""
    # From 03_architecture, 0xE000ED08 is referenced at 0x000295F4 and 0x00029612
    # Let's inspect around 0x000295E0 to get exact entry address
    int_reg_addr = 0x000295E4  # will auto-detect prologue
    # Scan backwards from 0x295F4 for push
    for probe in range(0x295F4, 0x295C0, -2):
        b = fw.u16(probe)
        # thumb push: 0xB5xx or 0xE92D
        if (b & 0xFF00) == 0xB500 or b == 0xE92D:
            int_reg_addr = probe
            break

    # Scan all code for BL to int_reg_addr, tracking r0 (vector index) and r1 (ISR func ptr)
    code = fw.data[0x0400:0x00035C00 - fw.base]
    fw.md.skipdata = True
    regs = {}
    calls = []
    for insn in fw.md.disasm(code, fw.base + 0x0400):
        mn, op = insn.mnemonic, insn.op_str
        parts = [p.strip() for p in op.split(",")]
        if mn in ("mov", "movs", "mov.w", "movw") and len(parts) == 2 and parts[1].startswith("#"):
            try:
                regs[parts[0]] = int(parts[1][1:], 0)
            except ValueError:
                pass
        elif mn == "movt" and len(parts) == 2 and parts[1].startswith("#"):
            try:
                regs[parts[0]] = (int(parts[1][1:], 0) << 16) | (regs.get(parts[0], 0) & 0xFFFF)
            except ValueError:
                pass
        elif mn in ("ldr", "ldr.w") and "[pc," in op:
            m = re.search(r"\[pc,\s*#(-?0x[0-9a-f]+|-?\d+)\]", op)
            if m:
                pool = ((insn.address + 4) & ~3) + int(m.group(1), 0)
                if fw.in_flash(pool):
                    regs[parts[0]] = fw.u32(pool)
        elif mn == "bl":
            m = re.search(r"#0x([0-9a-f]+)", op)
            if m and int(m.group(1), 16) == int_reg_addr:
                vec = regs.get("r0")
                isr = regs.get("r1")
                vname = fw.irqs.get(vec, f"VEC_{vec}") if vec is not None else "?"
                calls.append({
                    "call_site": f"0x{insn.address:08X}",
                    "vector_idx": vec,
                    "irq_name": vname,
                    "isr_addr": f"0x{isr:08X}" if isr is not None else "dynamic",
                })
            regs.clear()
    return int_reg_addr, calls


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", nargs="?", default="proprietary_fw/firmware_3.0.0.45.bin")
    parser.add_argument("--out", default="rev-eng/output/04_xds_protocol.json")
    args = parser.parse_args()

    fw = FirmwareImage(args.firmware)
    entries = extract_tbh_table(fw)

    # Sort unique case addresses to find boundaries
    unique_addrs = sorted(set(t for _, t in entries))
    unique_addrs.append(0x000074E0)  # end of dispatcher

    case_map = {}
    for i, addr in enumerate(unique_addrs[:-1]):
        if addr == DEFAULT_CASE:
            continue
        case_map[addr] = analyze_case(fw, addr, unique_addrs[i + 1])

    # Also disassemble epilogue (0x0000722A..0x000074D0)
    epilogue_disasm = fw.disasm_function(0x00007440, 160)

    # Dynamic ISR registrations
    int_reg_addr, isr_calls = find_int_register_calls(fw)

    print(f"=== XDS110 Command Table (0x00..0x{MAX_OPCODE:02X}, TBH @ 0x{TBH_ADDR:08X}) ===")
    results = []
    for op, tgt in entries:
        is_def = (tgt == DEFAULT_CASE)
        hint = KNOWN_OPCODES.get(op, "")
        if is_def:
            print(f"  0x{op:02X} -> DEFAULT (unsupported)")
            results.append({"opcode": f"0x{op:02X}", "supported": False, "target": f"0x{tgt:08X}"})
        else:
            c = case_map[tgt]
            callees_s = ", ".join(c["callees"][:4])
            req_s = ", ".join(c["req_reads"][:4])
            resp_s = ", ".join(c["resp_writes"][:4])
            print(f"  0x{op:02X} -> 0x{tgt:08X}  calls=[{callees_s:<36s}]  req=[{req_s:<24s}]  resp=[{resp_s:<20s}]  {hint}")
            results.append({
                "opcode": f"0x{op:02X}",
                "supported": True,
                "hint": hint,
                **c,
            })

    print(f"\n=== Dynamic ISR Registrations (IntRegister @ 0x{int_reg_addr:08X}) ===")
    for c in isr_calls:
        print(f"  site={c['call_site']}  vec={c['vector_idx']} ({c['irq_name']:<16s})  ISR={c['isr_addr']}")

    print(f"\n=== Dispatcher Epilogue (0x00007440) ===")
    for l in epilogue_disasm[:35]:
        print(" ", l)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "tbh_addr": f"0x{TBH_ADDR:08X}",
        "default_case": f"0x{DEFAULT_CASE:08X}",
        "opcodes": results,
        "int_register_addr": f"0x{int_reg_addr:08X}",
        "dynamic_isrs": isr_calls,
        "epilogue_disasm": epilogue_disasm,
    }, indent=2))
    print(f"\nSaved full command table to {out_path}")


if __name__ == "__main__":
    main()
