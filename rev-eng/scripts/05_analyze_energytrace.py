#!/usr/bin/env python3
"""
05_analyze_energytrace.py - Deep-dive analysis of the TI XDS110 EnergyTrace
subsystem (Mode 0 Standard DCDC, Mode 1 LP-XDS110ET HDR ADS127L01, Mode 2 Alt,
EnergyTrace++ Digital State Capture, and DCDC I2C1 MCU protocol).
"""

import argparse
import json
from pathlib import Path
from fwlib import FirmwareImage

# Key DriverLib & usblib symbols identified in earlier passes (for clean annotations)
KNOWN_SYMS = {
    0x00004EE0: "DelayCycles",
    0x000249FE: "USBBufferInit",
    0x00024A14: "USBBufferFlush",
    0x00024A28: "USBBufferWrite",
    0x00024A8A: "USBBufferDataRemoved",
    0x00024A90: "USBBufferRead",
    0x00024AE4: "USBBufferDataAvailable",
    0x00024AE8: "USBBufferSpaceAvailable",
    0x0002917A: "GPIOIntTypeSet",
    0x000291DC: "GPIOPadConfigSet",
    0x0002932A: "GPIOIntEnable",
    0x00029336: "GPIOIntDisable",
    0x00029358: "GPIOIntRegister",
    0x00029372: "GPIOIntUnregister",
    0x00029388: "GPIOPinRead",
    0x0002938E: "GPIOPinWrite",
    0x000293B0: "GPIOPinTypeGPIOInput",
    0x000293CC: "GPIOPinTypeGPIOOutput",
    0x000294C8: "GPIOPinConfigure",
    0x000295A2: "IntMasterDisable",
    0x000295B2: "IntMasterEnable",
    0x000295E6: "IntRegister",
    0x00029660: "IntEnable",
    0x00029914: "SysCtlPeripheralEnable",
    0x0002995C: "SysCtlClockFreqSet",
}

ET_FUNCTIONS = [
    # High-level dispatch & info
    ("ET_HardwareInfo (0x46)",     0x0001C904, 136),
    ("ET_Cleanup (0x21)",          0x0001C898, 108),
    ("ET_Calibrate_Top (0x1E)",    0x0001C98C, 44),
    # Mode 1: LP-XDS110ET HDR (ADS127L01 + SSI1 + uDMA)
    ("ET_HDR_DMA_or_SSI_ISR_Area", 0x0001FA00, 780),
    ("ET_HDR_Start (vtable+0x04)", 0x0001FD0C, 432),
    ("ET_HDR_Stop (vtable+0x08)",  0x0001FEBC, 244),
    ("ET_HDR_Cleanup (vtable+0x0C)", 0x0001FFB0, 248),
    ("ET_HDR_Setup (vtable+0x00)", 0x000200A8, 204),
    ("ET_HDR_Calibrate (vtable+0x18)", 0x00020174, 480),
    # Digital State Capture (EnergyTrace++)
    ("ET_Dig1_Init",               0x00020DB8, 36),
    ("ET_Dig1_Cleanup",            0x00020DDC, 20),
    ("ET_Dig1_Poll",               0x00020DF0, 20),
    ("ET_Dig1_HDR_Tick",           0x00020E04, 44),
    ("ET_Dig3_Init",               0x00020E30, 132),
    ("ET_Dig3_HDR_Tick",           0x00020F24, 160),
    # Mode 0: Classic DCDC Pulse EnergyTrace
    ("ET_Std_Setup",               0x0001D278, 212),
    ("ET_Std_Start",               0x0001D48C, 156),
    ("ET_Std_Calibrate",           0x0001D580, 120),
    # Mode 2: Alternate / External
    ("ET_Mode2_Setup",             0x00021540, 64),
    ("ET_Mode2_Start",             0x00021580, 260),
    ("ET_Mode2_Calibrate",         0x000216C4, 100),
    # DCDC MCU I2C1 commands (0x22..0x25)
    ("ET_DCDC_GetMCUVersion (0x22)", 0x0001CB58, 172),
    ("ET_DCDC_RestartMCU (0x25)",  0x0001CC04, 180),
    ("ET_DCDC_PowerDown (0x23)",   0x0001CD6C, 140),
    ("ET_DCDC_SetVcc (0x24)",      0x0001CF14, 160),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", nargs="?", default="proprietary_fw/firmware_3.0.0.45.bin")
    parser.add_argument("--out", default="rev-eng/output/05_energytrace.json")
    parser.add_argument("--section", default="hdr", help="Console print filter: hdr|all|dig|dcdc|std|mode2")
    args = parser.parse_args()

    fw = FirmwareImage(args.firmware)
    report = {}

    for name, addr, sz in ET_FUNCTIONS:
        lines = fw.disasm_function(addr, sz, sym_map=KNOWN_SYMS)
        report[name] = {"addr": f"0x{addr:08X}", "size": sz, "disasm": lines}

    # Print requested subset cleanly
    for name, info in report.items():
        low = name.lower()
        show = (
            args.section == "all"
            or (args.section == "hdr" and ("hdr" in low or "hardwareinfo" in low or "dig1" in low))
            or (args.section == "dig" and "dig" in low)
            or (args.section == "dcdc" and "dcdc" in low)
            or (args.section == "std" and "std" in low)
            or (args.section == "mode2" and "mode2" in low)
        )
        if show:
            print(f"\n=== {name} @ {info['addr']} ===")
            for l in info["disasm"]:
                print(" ", l)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2))
    print(f"\nSaved full EnergyTrace disassembly ({len(report)} blocks) to {out_path}")


if __name__ == "__main__":
    main()
