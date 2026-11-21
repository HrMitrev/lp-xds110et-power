#!/usr/bin/env python3
"""
fwlib.py - Shared reverse-engineering library for TI XDS110 MSP432E401Y firmware.

Provides:
  - FirmwareImage: unified Flash + initialized .data (SRAM) memory reader
  - Automatic parsing of TI SDK headers (rom.h, msp432e401y.h, sysctl.h)
  - Annotated Thumb-2 / Cortex-M4 disassembler using Capstone with:
      * PC-relative literal pool resolution
      * MOVW + MOVT 32-bit immediate reconstruction
      * ROM_APITABLE (0x01000010) call identification
      * Peripheral MMIO base & register naming
      * ASCII / UTF-16LE string dereferencing
"""

import re
import struct
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

BASE_ADDR = 0x00004000
DATA_FLASH_OFF = 0x00050B8A - BASE_ADDR  # Auto-detected from ResetISR if different
SRAM_BASE = 0x20000000

SDK_ROOT = Path("firmware_src/sdk/ti/devices/msp432e4")


def parse_irq_table(sdk_root: Path = SDK_ROOT) -> dict[int, str]:
    """Parse IRQn enum from msp432e401y.h -> {vector_index: name}."""
    hdr = sdk_root / "inc/msp432e401y.h"
    irqs = {
        0: "Initial_SP",
        1: "Reset_Handler",
    }
    if not hdr.exists():
        return irqs
    text = hdr.read_text(errors="ignore")
    for m in re.finditer(r"^\s*(\w+_IRQn)\s*=\s*(-?\d+)", text, re.MULTILINE):
        name, num = m.group(1), int(m.group(2))
        irqs[num + 16] = name.replace("_IRQn", "")
    return irqs


def parse_rom_table(sdk_root: Path = SDK_ROOT) -> tuple[dict[int, str], dict[tuple[int, int], str]]:
    """
    Parse driverlib/rom.h to map:
      table_idx -> table_name (e.g., 13 -> ROM_SYSCTLTABLE)
      (table_idx, func_idx) -> func_name (e.g., (13, 3) -> ROM_SysCtlPeripheralEnable)
    """
    rom_h = sdk_root / "driverlib/rom.h"
    table_names: dict[int, str] = {}
    name_to_tidx: dict[str, int] = {}
    funcs: dict[tuple[int, int], str] = {}
    if not rom_h.exists():
        return table_names, funcs
    text = rom_h.read_text(errors="ignore")
    # Join backslash-continued lines
    joined = re.sub(r"\\\r?\n", " ", text)
    for m in re.finditer(r"#define\s+(ROM_\w+TABLE)\s+\(\(uint32_t\s*\*\)\(ROM_APITABLE\[(\d+)\]\)\)", joined):
        tname, tidx = m.group(1), int(m.group(2))
        table_names[tidx] = tname
        name_to_tidx[tname] = tidx
    for m in re.finditer(r"#define\s+(ROM_\w+)\s+.*?\b(ROM_\w+TABLE)\[(\d+)\]", joined):
        fname, tname, fidx = m.group(1), m.group(2), int(m.group(3))
        if tname in name_to_tidx:
            funcs[(name_to_tidx[tname], fidx)] = fname
    return table_names, funcs


def parse_peripherals(sdk_root: Path = SDK_ROOT) -> dict[int, str]:
    """Parse peripheral base addresses from msp432e401y.h and Cortex-M4 SCS."""
    periphs: dict[int, str] = {
        0xE000E010: "SysTick",
        0xE000E100: "NVIC",
        0xE000ED00: "SCB",
        0xE000ED88: "SCB_CPACR",
        0xE000EF00: "FPU",
        0x01000010: "ROM_APITABLE",
    }
    hdr = sdk_root / "inc/msp432e401y.h"
    if hdr.exists():
        text = hdr.read_text(errors="ignore")
        for m in re.finditer(r"#define\s+(\w+_BASE)\s+\(?0x([0-9A-Fa-f]+)[ULul]*\)?", text):
            name, addr = m.group(1), int(m.group(2), 16)
            if 0x40000000 <= addr <= 0x45000000:
                # Prefer clean peripheral names
                if addr not in periphs or len(name) < len(periphs[addr]):
                    periphs[addr] = name
    # Also add GPIO AHB base names clearly from driverlib/inc/hw_memmap.h if present or known MSP432E4 bases
    gpio_bases = {
        0x40058000: "GPIO_PORTA_AHB",
        0x40059000: "GPIO_PORTB_AHB",
        0x4005A000: "GPIO_PORTC_AHB",
        0x4005B000: "GPIO_PORTD_AHB",
        0x4005C000: "GPIO_PORTE_AHB",
        0x4005D000: "GPIO_PORTF_AHB",
        0x4005E000: "GPIO_PORTG_AHB",
        0x4005F000: "GPIO_PORTH_AHB",
        0x40060000: "GPIO_PORTJ_AHB",
        0x40061000: "GPIO_PORTK_AHB",
        0x40062000: "GPIO_PORTL_AHB",
        0x40063000: "GPIO_PORTM_AHB",
        0x40064000: "GPIO_PORTN_AHB",
        0x40065000: "GPIO_PORTP_AHB",
        0x40066000: "GPIO_PORTQ_AHB",
        0x40004000: "GPIO_PORTA_APB",
        0x40005000: "GPIO_PORTB_APB",
        0x40006000: "GPIO_PORTC_APB",
        0x40007000: "GPIO_PORTD_APB",
        0x40024000: "GPIO_PORTE_APB",
        0x40025000: "GPIO_PORTF_APB",
        0x40026000: "GPIO_PORTG_APB",
        0x40027000: "GPIO_PORTH_APB",
        0x4003D000: "GPIO_PORTJ_APB",
        0x400FE000: "SYSCTL",
        0x400FD000: "FLASH_CTRL",
        0x400AF000: "EEPROM",
        0x400FF000: "UDMA",
        0x40050000: "USB0",
        0x4000C000: "UART0",
        0x4000D000: "UART1",
        0x4000E000: "UART2",
        0x4000F000: "UART3",
        0x40010000: "UART4",
        0x40011000: "UART5",
        0x40012000: "UART6",
        0x40013000: "UART7",
        0x40008000: "SSI0",
        0x40009000: "SSI1",
        0x4000A000: "SSI2",
        0x4000B000: "SSI3",
        0x40020000: "I2C0",
        0x40021000: "I2C1",
        0x40022000: "I2C2",
        0x40023000: "I2C3",
        0x400C0000: "I2C4",
        0x400C1000: "I2C5",
        0x400C2000: "I2C6",
        0x400C3000: "I2C7",
        0x400B8000: "I2C8",
        0x400B9000: "I2C9",
        0x40030000: "TIMER0",
        0x40031000: "TIMER1",
        0x40032000: "TIMER2",
        0x40033000: "TIMER3",
        0x40034000: "TIMER4",
        0x40035000: "TIMER5",
        0x400E0000: "TIMER6",
        0x400E1000: "TIMER7",
        0x40038000: "ADC0",
        0x40039000: "ADC1",
        0x40028000: "PWM0",
        0x40000000: "WATCHDOG0",
        0x40001000: "WATCHDOG1",
        0x4003C000: "COMP",
        0x400D0000: "EPI0",
        0x400FC000: "HIB",
        0x400EC000: "EMAC0",
    }
    periphs.update(gpio_bases)
    return periphs


def parse_sysctl_periphs(sdk_root: Path = SDK_ROOT) -> dict[int, str]:
    """Parse SYSCTL_PERIPH_* constants from driverlib/sysctl.h."""
    hdr = sdk_root / "driverlib/sysctl.h"
    res: dict[int, str] = {}
    if not hdr.exists():
        return res
    text = hdr.read_text(errors="ignore")
    for m in re.finditer(r"#define\s+(SYSCTL_PERIPH_\w+)\s+0x([0-9A-Fa-f]+)", text):
        res[int(m.group(2), 16)] = m.group(1)
    return res


def parse_gpio_pin_mux(sdk_root: Path = SDK_ROOT) -> dict[int, str]:
    """Parse GPIO_Pxx_yyyy pin-mux constants from driverlib/pin_map.h."""
    hdr = sdk_root / "driverlib/pin_map.h"
    res: dict[int, list[str]] = {}
    if not hdr.exists():
        return {}
    text = hdr.read_text(errors="ignore")
    for m in re.finditer(r"#define\s+(GPIO_P[A-T][0-7]_\w+)\s+0x([0-9A-Fa-f]+)", text):
        val = int(m.group(2), 16)
        res.setdefault(val, []).append(m.group(1))
    return {k: "/".join(v) for k, v in res.items()}


class FirmwareImage:
    def __init__(self, path: str | Path = "proprietary_fw/firmware_3.0.0.45.bin"):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.base = BASE_ADDR
        self.end = BASE_ADDR + len(self.data)
        self.irqs = parse_irq_table()
        self.rom_tables, self.rom_funcs = parse_rom_table()
        self.periphs = parse_peripherals()
        self.sysctl_periphs = parse_sysctl_periphs()
        self.gpio_mux = parse_gpio_pin_mux()
        self.md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        self.md.detail = True

        # Auto-detect ResetISR and .data/.bss layout
        self.initial_sp = self.u32(self.base)
        self.reset_addr = self.u32(self.base + 4) & ~1
        self._detect_sections()

    def _detect_sections(self):
        """Parse literal pool of ResetISR to extract exact .data/.bss/main addresses."""
        # ResetISR in XDS110 GCC startup has literal pool right after b.w main
        # Pattern: ldr r2, =_data_load; ldr r3, =_data_start; ...; ldr r1, =_data_end;
        #          ldr r0, =_bss_start; ldr r1, =_bss_end; ...; b.w main
        self.data_load = 0x00050B8A
        self.data_start = 0x20000000
        self.data_end = 0x20002570
        self.bss_start = 0x20002800
        self.bss_end = 0x20033588
        self.main_addr = 0x00005D7C

        off = self.reset_addr - self.base
        code = self.data[off:off + 64]
        lit_vals = []
        for insn in self.md.disasm(code, self.reset_addr):
            if insn.mnemonic == "ldr" and "[pc," in insn.op_str:
                m = re.search(r"\[pc,\s*#0x([0-9a-f]+)\]|\[pc,\s*#(\d+)\]", insn.op_str)
                if m:
                    imm = int(m.group(1), 16) if m.group(1) else int(m.group(2))
                    pool_addr = ((insn.address + 4) & ~3) + imm
                    lit_vals.append(self.u32(pool_addr))
            elif insn.mnemonic in ("b.w", "b") and insn.address > self.reset_addr + 0x24:
                m = re.search(r"#0x([0-9a-f]+)", insn.op_str)
                if m:
                    self.main_addr = int(m.group(1), 16)
                break
        if len(lit_vals) >= 6:
            self.data_load = lit_vals[0]
            self.data_start = lit_vals[1]
            self.data_end = lit_vals[2]
            self.bss_start = lit_vals[3]
            self.bss_end = lit_vals[4]

    def in_flash(self, addr: int) -> bool:
        return self.base <= addr < self.end

    def in_data_sram(self, addr: int) -> bool:
        return self.data_start <= addr < self.data_end

    def read(self, addr: int, size: int) -> bytes | None:
        if self.base <= addr and addr + size <= self.end:
            off = addr - self.base
            return self.data[off:off + size]
        if self.data_start <= addr and addr + size <= self.data_end:
            off = (self.data_load - self.base) + (addr - self.data_start)
            if 0 <= off and off + size <= len(self.data):
                return self.data[off:off + size]
        return None

    def u32(self, addr: int) -> int:
        b = self.read(addr, 4)
        if b is None:
            raise ValueError(f"Address 0x{addr:08X} out of range")
        return struct.unpack("<I", b)[0]

    def u16(self, addr: int) -> int:
        b = self.read(addr, 2)
        if b is None:
            raise ValueError(f"Address 0x{addr:08X} out of range")
        return struct.unpack("<H", b)[0]

    def u8(self, addr: int) -> int:
        b = self.read(addr, 1)
        if b is None:
            raise ValueError(f"Address 0x{addr:08X} out of range")
        return b[0]

    def read_cstr(self, addr: int, max_len: int = 80) -> str | None:
        b = self.read(addr, max_len)
        if not b:
            return None
        nul = b.find(0)
        if nul <= 0:
            return None
        s = b[:nul]
        if all(32 <= c < 127 or c in (9, 10, 13) for c in s) and len(s) >= 3:
            return s.decode("ascii")
        return None

    def describe_const(self, val: int) -> str | None:
        """Return a human-readable label for a 32-bit constant if recognized."""
        if val in self.periphs:
            return self.periphs[val]
        # Peripheral + offset (4 KB page)
        page = val & ~0xFFF
        if page in self.periphs and 0x40000000 <= val <= 0x45000000:
            off = val - page
            return f"{self.periphs[page]}+0x{off:X}"
        if val in self.sysctl_periphs:
            return self.sysctl_periphs[val]
        if 0x01000010 <= val <= 0x01000100 and (val - 0x01000010) % 4 == 0:
            tidx = (val - 0x01000010) // 4
            if tidx in self.rom_tables:
                return f"&{self.rom_tables[tidx]}"
        if val == 0x000FC000:
            return "CONFIG_FLASH_BLOCK"
        if self.in_flash(val) or self.in_data_sram(val):
            s = self.read_cstr(val)
            if s:
                return f'"{s[:48]}"'
        return None

    def disasm_function(self, start_addr: int, max_bytes: int = 512, sym_map: dict[int, str] | None = None) -> list[str]:
        """Disassemble a function with constant tracking, ROM API & peripheral annotations."""
        start_addr &= ~1
        sym_map = sym_map or {}
        off = start_addr - self.base
        code = self.data[off:off + max_bytes]
        regs: dict[str, int] = {}
        rom_table_reg: dict[str, int] = {}
        rom_func_reg: dict[str, str] = {}
        lines = []
        insns = list(self.md.disasm(code, start_addr))
        for idx, insn in enumerate(insns):
            notes = []
            mn = insn.mnemonic
            op = insn.op_str

            # Track MOV / MOVS / MOV.W / MOVW / MOVT / LDR literal
            parts = [p.strip() for p in op.split(",")]
            if mn in ("mov", "movs", "mov.w", "movw") and len(parts) == 2 and parts[1].startswith("#"):
                try:
                    val = int(parts[1][1:], 0)
                    regs[parts[0]] = val
                    rom_table_reg.pop(parts[0], None)
                    rom_func_reg.pop(parts[0], None)
                except ValueError:
                    pass
            elif mn == "movt" and len(parts) == 2 and parts[1].startswith("#"):
                try:
                    hi = int(parts[1][1:], 0)
                    lo = regs.get(parts[0], 0) & 0xFFFF
                    val = (hi << 16) | lo
                    regs[parts[0]] = val
                    lbl = self.describe_const(val)
                    if lbl:
                        notes.append(f"{parts[0]}=0x{val:08X} ({lbl})")
                    else:
                        notes.append(f"{parts[0]}=0x{val:08X}")
                except ValueError:
                    pass
            elif mn in ("ldr", "ldr.w") and "[pc," in op:
                m = re.search(r"\[pc,\s*#(-?0x[0-9a-f]+|-?\d+)\]", op)
                if m:
                    imm = int(m.group(1), 0)
                    pool_addr = ((insn.address + 4) & ~3) + imm
                    if self.in_flash(pool_addr):
                        val = self.u32(pool_addr)
                        dst = parts[0]
                        regs[dst] = val
                        rom_table_reg.pop(dst, None)
                        rom_func_reg.pop(dst, None)
                        if val == 0x01000010:
                            pass
                        elif 0x01000010 <= val <= 0x01000100 and (val - 0x01000010) % 4 == 0:
                            tidx = (val - 0x01000010) // 4
                            rom_table_reg[dst] = tidx  # pointer to ROM_APITABLE[tidx]
                        lbl = self.describe_const(val)
                        if lbl:
                            notes.append(f"=0x{val:08X} ({lbl})")
                        else:
                            notes.append(f"=0x{val:08X}")
            elif mn in ("ldr", "ldr.w") and "[" in op:
                # Check ROM_APITABLE indirection:
                # Step 1: ldr rX, [rY, #imm] where rY == 0x01000010 -> table tidx = imm // 4
                # OR ldr rX, [rY] where rY holds &ROM_APITABLE[tidx]
                m = re.match(r"(\w+),\s*\[(\w+)(?:,\s*#(0x[0-9a-f]+|\d+))?\]", op)
                if m:
                    dst, base_r, imm_s = m.group(1), m.group(2), m.group(3)
                    imm = int(imm_s, 0) if imm_s else 0
                    if regs.get(base_r) == 0x01000010:
                        tidx = imm // 4
                        rom_table_reg[dst] = tidx
                        tname = self.rom_tables.get(tidx, f"ROM_TABLE[{tidx}]")
                        notes.append(f"{dst}={tname}")
                    elif base_r in rom_table_reg:
                        tidx = rom_table_reg[base_r]
                        # Check if base_r held &ROM_APITABLE[tidx] (needs one deref first)
                        if regs.get(base_r, 0) == 0x01000010 + tidx * 4 and imm == 0:
                            # Dereferencing &ROM_APITABLE[tidx] -> now dst holds ROM_xxxTABLE
                            rom_table_reg[dst] = tidx
                            regs.pop(dst, None)
                            tname = self.rom_tables.get(tidx, f"ROM_TABLE[{tidx}]")
                            notes.append(f"{dst}={tname}")
                        else:
                            fidx = imm // 4
                            fname = self.rom_funcs.get((tidx, fidx))
                            if fname:
                                rom_func_reg[dst] = fname
                                notes.append(f"{dst}={fname}")
                            rom_table_reg.pop(dst, None)
                    else:
                        regs.pop(dst, None)
                        rom_table_reg.pop(dst, None)
                        rom_func_reg.pop(dst, None)
            elif mn in ("blx", "bx"):
                tgt = parts[0]
                if tgt in rom_func_reg:
                    arg_info = []
                    for ar in ("r0", "r1", "r2", "r3"):
                        if ar in regs:
                            lbl = self.describe_const(regs[ar])
                            arg_info.append(f"{ar}={lbl or hex(regs[ar])}")
                    notes.append(f"CALL {rom_func_reg[tgt]}({', '.join(arg_info)})")
                    # Clobber caller-saved regs
                    for cr in ("r0", "r1", "r2", "r3", "r12"):
                        regs.pop(cr, None)
                        rom_table_reg.pop(cr, None)
                        rom_func_reg.pop(cr, None)
            elif mn in ("bl", "b.w", "b"):
                m = re.search(r"#0x([0-9a-f]+)", op)
                if m:
                    tgt = int(m.group(1), 16)
                    if tgt in sym_map:
                        notes.append(f"-> {sym_map[tgt]}")
                    elif (tgt | 1) in sym_map:
                        notes.append(f"-> {sym_map[tgt | 1]}")
                if mn == "bl":
                    for cr in ("r0", "r1", "r2", "r3", "r12"):
                        regs.pop(cr, None)
                        rom_table_reg.pop(cr, None)
                        rom_func_reg.pop(cr, None)

            comment = f"  ; {' | '.join(notes)}" if notes else ""
            lines.append(f"0x{insn.address:08x}: {insn.bytes.hex():<10s} {mn:<9s} {op:<28s}{comment}")

            # Stop heuristic: pop {..., pc} or bx lr after at least some instructions
            if idx > 2 and ((mn in ("pop", "pop.w") and "pc" in op) or (mn == "bx" and op == "lr")):
                # Only stop if no branch jumps past this point within the function so far
                pass
        return lines
