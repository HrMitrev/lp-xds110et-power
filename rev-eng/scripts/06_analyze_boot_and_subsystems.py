#!/usr/bin/env python3
"""
06_analyze_boot_and_subsystems.py - Analyzes hardware board ID detection (PK4..PK7),
ULPI HS-USB PHY setup, JTAG/SWD/SRST control, SWO/SSI3 streaming (0x56..0x59),
DFU entry (0x26), and Flash config/serial block handling (0x27).
"""

import argparse
import json
from pathlib import Path
from fwlib import FirmwareImage

KNOWN_SYMS = {
    0x00004DF4: "USB_ULPI_PinInit",
    0x00004EE0: "DelayCycles",
    0x000249FE: "USBBufferInit",
    0x00024A28: "USBBufferWrite",
    0x00025F58: "USBDCompositeTerm_Or_DFU",
    0x000291DC: "GPIOPadConfigSet",
    0x00029388: "GPIOPinRead",
    0x0002938E: "GPIOPinWrite",
    0x000293B0: "GPIOPinTypeGPIOInput",
    0x000293CC: "GPIOPinTypeGPIOOutput",
    0x000294C8: "GPIOPinConfigure",
    0x00029914: "SysCtlPeripheralEnable",
    0x0002995C: "SysCtlClockFreqSet",
}

BLOCKS = [
    ("BoardDetect_And_PerBoardInit (0x5480..0x5960)", 0x00005480, 1248),
    ("USB_ULPI_PinInit (0x4DF4..0x4EE0)",            0x00004DF4, 236),
    ("XDS_SetSrst_0x0E (0xDF30..0xDFB0)",            0x0000DF30, 128),
    ("XDS_SetTrst_0x05 (0xDE18..0xDE8C)",            0x0000DE18, 116),
    ("XDS_GetPower_0x08 (0xDED8..0xDEE4)",           0x0000DED8, 24),
    ("XDS_EnterDFU_0x26 (0x6EB4..0x6ED0)",           0x00006EB4, 32),
    ("USBDCompositeTerm_DFUJump (0x25F58..0x25FE0)", 0x00025F58, 136),
    ("XDS_Cmd27_ConfigRead (0x1EDB0..0x1EE50)",      0x0001EDB0, 160),
    ("XDS_ConnectET_0x28 (0xEEF4..0xEFE0)",          0x0000EEF4, 236),
    ("SWO_SSI3_Setup_0x56 (0x1858C..0x186DC)",       0x0001858C, 336),
    ("SWO_SSI3_Stop_0x57 (0x186DC..0x18730)",        0x000186DC, 84),
    ("SWO_SSI3_Pump (0x187A4..0x18880)",             0x000187A4, 220),
    ("GPIO_Config_0x29_0x2A (0x181F4..0x182C0)",     0x000181F4, 204),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", nargs="?", default="proprietary_fw/firmware_3.0.0.45.bin")
    parser.add_argument("--out", default="rev-eng/output/06_boot_subsystems.json")
    parser.add_argument("--show", default="summary", help="summary|board|dfu|swo|jtag|all")
    args = parser.parse_args()

    fw = FirmwareImage(args.firmware)
    report = {}
    for name, addr, sz in BLOCKS:
        lines = fw.disasm_function(addr, sz, sym_map=KNOWN_SYMS)
        report[name] = {"addr": f"0x{addr:08X}", "size": sz, "disasm": lines}

    for name, info in report.items():
        low = name.lower()
        show = (
            args.show == "all"
            or (args.show == "board" and ("board" in low or "ulpi" in low))
            or (args.show == "dfu" and ("dfu" in low or "cmd27" in low))
            or (args.show == "swo" and "swo" in low)
            or (args.show == "jtag" and ("srst" in low or "trst" in low or "connectet" in low or "gpio_config" in low))
        )
        if show:
            print(f"\n=== {name} ===")
            for l in info["disasm"]:
                print(" ", l)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2))
    print(f"\nSaved {len(report)} subsystem disassemblies to {out_path}")


if __name__ == "__main__":
    main()
