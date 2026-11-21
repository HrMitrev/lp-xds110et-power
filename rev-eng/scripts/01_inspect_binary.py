#!/usr/bin/env python3
"""
01_inspect_binary.py - Initial structural inspection of TI XDS110 firmware binary.

Parses:
  - Cortex-M4 vector table (base 0x00004000) mapped to MSP432E401Y IRQ names
  - Reset handler disassembly (identifies compiler/CRT, .data/.bss/cinit)
  - ASCII and UTF-16LE strings (USB strings, debug/version strings, CMSIS-DAP)
  - Binary entropy / region layout estimation
"""

import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

BASE_ADDR = 0x00004000

VECTOR_NAMES = [
    "Initial_SP", "Reset_Handler", "NMI_Handler", "HardFault_Handler",
    "MemManage_Handler", "BusFault_Handler", "UsageFault_Handler",
    "Reserved_7", "Reserved_8", "Reserved_9", "Reserved_10",
    "SVC_Handler", "DebugMon_Handler", "Reserved_13", "PendSV_Handler", "SysTick_Handler",
    # External IRQs (MSP432E401Y / TM4C1294)
    "GPIO_Port_A", "GPIO_Port_B", "GPIO_Port_C", "GPIO_Port_D", "GPIO_Port_E",
    "UART0", "UART1", "SSI0", "I2C0", "PWM0_Fault",
    "PWM0_Gen0", "PWM0_Gen1", "PWM0_Gen2", "QEI0",
    "ADC0_Seq0", "ADC0_Seq1", "ADC0_Seq2", "ADC0_Seq3",
    "Watchdog", "Timer0A", "Timer0B", "Timer1A", "Timer1B", "Timer2A", "Timer2B",
    "Comp0", "Comp1", "Comp2", "SysCtl", "Flash",
    "GPIO_Port_F", "GPIO_Port_G", "GPIO_Port_H", "UART2", "SSI1",
    "Timer3A", "Timer3B", "I2C1", "CAN0", "CAN1", "Ethernet", "Hibernate", "USB0",
    "PWM0_Gen3", "uDMA_SW", "uDMA_Err",
    "ADC1_Seq0", "ADC1_Seq1", "ADC1_Seq2", "ADC1_Seq3", "EPI0",
    "GPIO_Port_J", "GPIO_Port_K", "GPIO_Port_L", "SSI2", "SSI3",
    "UART3", "UART4", "UART5", "UART6", "UART7", "I2C2", "I2C3",
    "Timer4A", "Timer4B", "Timer5A", "Timer5B", "FPU",
    "Reserved_85", "Reserved_86", "I2C4", "I2C5", "GPIO_Port_M", "GPIO_Port_N",
    "Reserved_91", "Tamper",
    "GPIO_Port_P0", "GPIO_Port_P1", "GPIO_Port_P2", "GPIO_Port_P3",
    "GPIO_Port_P4", "GPIO_Port_P5", "GPIO_Port_P6", "GPIO_Port_P7",
    "GPIO_Port_Q0", "GPIO_Port_Q1", "GPIO_Port_Q2", "GPIO_Port_Q3",
    "GPIO_Port_Q4", "GPIO_Port_Q5", "GPIO_Port_Q6", "GPIO_Port_Q7",
    "GPIO_Port_R", "GPIO_Port_S", "SHAMD5_0", "AES_0", "DES_0", "Reserved_114",
    "Timer6A", "Timer6B", "Timer7A", "Timer7B", "I2C6", "I2C7", "Reserved_121",
    "OneWire0", "Reserved_123", "Reserved_124", "Reserved_125",
    "I2C8", "I2C9", "GPIO_Port_T",
]


def parse_vector_table(data: bytes):
    vectors = []
    counts = {}
    for i, name in enumerate(VECTOR_NAMES):
        val = struct.unpack_from("<I", data, i * 4)[0]
        vectors.append({"index": i, "irq": i - 16 if i >= 16 else None, "name": name, "addr": val})
        if i >= 2 and val != 0:
            counts[val] = counts.get(val, 0) + 1
    default_handler = max(counts, key=counts.get) if counts else 0
    return vectors, default_handler


def disasm_range(data: bytes, start_addr: int, size: int):
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    off = start_addr - BASE_ADDR
    code = data[off:off + size]
    lines = []
    for insn in md.disasm(code, start_addr):
        lines.append(f"0x{insn.address:08x}: {insn.bytes.hex():<10s} {insn.mnemonic:<10s} {insn.op_str}")
    return lines


def extract_ascii_strings(data: bytes, min_len: int = 5):
    pat = re.compile(rb"[\x20-\x7e]{%d,}" % min_len)
    res = []
    for m in pat.finditer(data):
        res.append({"addr": BASE_ADDR + m.start(), "text": m.group().decode("ascii")})
    return res


def extract_utf16le_strings(data: bytes, min_len: int = 4):
    pat = re.compile(rb"(?:[\x20-\x7e]\x00){%d,}" % min_len)
    res = []
    for m in pat.finditer(data):
        text = m.group().decode("utf-16-le")
        res.append({"addr": BASE_ADDR + m.start(), "text": text})
    return res


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", nargs="?", default="proprietary_fw/firmware_3.0.0.45.bin")
    parser.add_argument("--out", default="rev-eng/output/01_inspect.json")
    args = parser.parse_args()

    fw_path = Path(args.firmware)
    data = fw_path.read_bytes()
    sha256 = hashlib.sha256(data).hexdigest()

    vectors, default_handler = parse_vector_table(data)
    active_vectors = [
        v for v in vectors
        if v["index"] < 2 or (v["addr"] != 0 and v["addr"] != default_handler)
    ]

    reset_addr = vectors[1]["addr"] & ~1
    reset_disasm = disasm_range(data, reset_addr, 128)

    ascii_strs = extract_ascii_strings(data, 6)
    utf16_strs = extract_utf16le_strings(data, 4)

    # Check tail of binary (e.g., CRC/signature/padding)
    tail_hex = data[-64:].hex()

    print(f"=== Binary Summary ===")
    print(f"File:        {fw_path}")
    print(f"Size:        {len(data)} bytes (0x{len(data):X})")
    print(f"End Address: 0x{BASE_ADDR + len(data):08X}")
    print(f"SHA256:      {sha256}")
    print(f"Initial SP:  0x{vectors[0]['addr']:08X}")
    print(f"Reset ISR:   0x{vectors[1]['addr']:08X}")
    print(f"Default ISR: 0x{default_handler:08X}")
    print()

    print(f"=== Active Vector Table Entries ({len(active_vectors)}) ===")
    for v in active_vectors:
        irq_str = f"IRQ {v['irq']:3d}" if v["irq"] is not None else "Core   "
        print(f"  [{v['index']:3d}] {irq_str}  {v['name']:<20s} -> 0x{v['addr']:08X}")
    print()

    print(f"=== Reset Handler Disassembly (0x{reset_addr:08X}) ===")
    for line in reset_disasm[:35]:
        print(" ", line)
    print()

    print(f"=== UTF-16LE Strings ({len(utf16_strs)}) ===")
    for s in utf16_strs:
        print(f"  0x{s['addr']:08X}: {s['text']!r}")
    print()

    print(f"=== Interesting ASCII Strings (total {len(ascii_strs)}, showing filtered) ===")
    interesting_kw = [
        "xds", "ti", "texas", "cmsis", "dap", "energy", "trace", "dfu", "usb",
        "uart", "jtag", "swd", "arm", "msp", "tm4c", "version", "copyright",
        "error", "fail", "gcc", "ti-rtos", "bios", "freertos", "boot", "flash",
        "serial", "probe", "com", "debug", "cdc", "winusb", "msft", "vcp",
        "3.0.0", "110", "et", "dcdc", "adc", "cal", "hdr", "lprf", "spi", "i2c",
    ]
    shown = 0
    for s in ascii_strs:
        low = s["text"].lower()
        if any(k in low for k in interesting_kw) or len(s["text"]) >= 16:
            print(f"  0x{s['addr']:08X}: {s['text']!r}")
            shown += 1
            if shown >= 120:
                print("  ... (truncated; see JSON output for full list)")
                break

    print(f"\n=== Tail 64 bytes (0x{BASE_ADDR + len(data) - 64:08X}) ===")
    for off in range(len(data) - 64, len(data), 16):
        chunk = data[off:off + 16]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        asc_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  0x{BASE_ADDR + off:08X}: {hex_part:<47s}  {asc_part}")

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "file": str(fw_path),
        "size": len(data),
        "base_addr": BASE_ADDR,
        "end_addr": BASE_ADDR + len(data),
        "sha256": sha256,
        "default_handler": default_handler,
        "active_vectors": active_vectors,
        "utf16_strings": utf16_strs,
        "ascii_strings": ascii_strs,
        "tail_hex": tail_hex,
    }, indent=2))
    print(f"\nSaved full report to {out_path}")


if __name__ == "__main__":
    main()
