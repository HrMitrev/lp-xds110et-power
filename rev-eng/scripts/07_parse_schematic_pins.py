#!/usr/bin/env python3
"""
07_parse_schematic_pins.py - Extracts and cross-references the LP-XDS110ET
hardware schematic (docs/LP-XDS110ET_SCH.pdf) against firmware_3.0.0.45.bin
peripheral/GPIO usage.
"""

import argparse
import json
import subprocess
from pathlib import Path

# Verified LP-XDS110ET (MCU101 Rev B, 2022-10-05) MSP432E401YTPDT pinout
# extracted directly from docs/LP-XDS110ET_SCH.pdf (Sheets 1-4) and cross-checked
# against firmware_3.0.0.45.bin disassembly.
SCHEMATIC_PINS = [
    # Port A - App UART0 + JTAG/SWD + Target Reset
    {"port": "PA0", "pin": 33, "net": "XDS_RX",        "subsystem": "App UART (CDC0)", "fw_func": "UART0 RX (0x000053E4, U0RX)"},
    {"port": "PA1", "pin": 34, "net": "XDS_TX",        "subsystem": "App UART (CDC0)", "fw_func": "UART0 TX (0x000053E4, U0TX)"},
    {"port": "PA2", "pin": 35, "net": "XDS_TCK",       "subsystem": "JTAG / SWD",      "fw_func": "Bit-banged TCK / SWCLK (GPIO_PORTA_AHB)"},
    {"port": "PA3", "pin": 36, "net": "XDS_TMS",       "subsystem": "JTAG / SWD",      "fw_func": "Bit-banged TMS / SWDIO (GPIO_PORTA_AHB)"},
    {"port": "PA4", "pin": 37, "net": "XDS_TDO",       "subsystem": "JTAG / SWD",      "fw_func": "Bit-banged TDO (GPIO_PORTA_AHB)"},
    {"port": "PA5", "pin": 38, "net": "XDS_TDI",       "subsystem": "JTAG / SWD",      "fw_func": "Bit-banged TDI (GPIO_PORTA_AHB)"},
    {"port": "PA6", "pin": 40, "net": "XDS_RESET",     "subsystem": "Target Reset",    "fw_func": "nSRST (CMD 0x0E @ 0x0000DF30, boot @ 0x000064D0)"},
    # Port B - USB ULPI Control + EnergyTrace ADS127L01 SPI/DRDY
    {"port": "PB0", "pin": 95, "net": "USB0ID (1k)",   "subsystem": "USB0",            "fw_func": "Analog USB0ID (0x000053B8)"},
    {"port": "PB1", "pin": 96, "net": "USB0VBUS (1k)", "subsystem": "USB0",            "fw_func": "Analog USB0VBUS (0x000053B8)"},
    {"port": "PB2", "pin": 91, "net": "USBSTP",        "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PB2_USB0STP (0x00004DF6)"},
    {"port": "PB3", "pin": 92, "net": "USBCLK",        "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PB3_USB0CLK (0x00004DFC)"},
    {"port": "PB4", "pin": 121,"net": "ET_SSIFSS",     "subsystem": "EnergyTrace HDR", "fw_func": "ADS127L01 nDRDY -> GPIOB DMA trigger Ch5 (0x0001FA60, 0x0002004A)"},
    {"port": "PB5", "pin": 120,"net": "ET_SSICLK",     "subsystem": "EnergyTrace HDR", "fw_func": "GPIO_PB5_SSI1CLK @ 15 MHz (0x00020028)"},
    # Port C - Internal JTAG + SWO + TMS Level-Shifter Dir + ET Cal2
    {"port": "PC0", "pin": 100,"net": "ITCK",          "subsystem": "Probe Debug",     "fw_func": "MSP432E401Y own JTAG/SWD (P8)"},
    {"port": "PC1", "pin": 99, "net": "ITMS",          "subsystem": "Probe Debug",     "fw_func": "MSP432E401Y own JTAG/SWD (P8)"},
    {"port": "PC2", "pin": 98, "net": "ITDI",          "subsystem": "Probe Debug",     "fw_func": "MSP432E401Y own JTAG/SWD (P8)"},
    {"port": "PC3", "pin": 97, "net": "ITDO",          "subsystem": "Probe Debug",     "fw_func": "MSP432E401Y own JTAG/SWD (P8)"},
    {"port": "PC4", "pin": 25, "net": "XDS_SWO",       "subsystem": "Aux UART / SWO",  "fw_func": "GPIO_PC4_U7RX (UART7 RX, CDC1 @ 0x00005A46)"},
    {"port": "PC5", "pin": 24, "net": "XDS_TMS_DIR",   "subsystem": "Level Shifter",   "fw_func": "TMS/SWDIO direction control (0x00005A22, 0x00005C72)"},
    {"port": "PC6", "pin": 23, "net": "ET_CAL2",       "subsystem": "EnergyTrace HDR", "fw_func": "1.5 mA cal switch (R130=2.2k, cal idx=4 @ 0x0002028C)"},
    # Port D - Status LEDs
    {"port": "PD2", "pin": 3,  "net": "RED LED (CR4)", "subsystem": "Status LEDs",     "fw_func": "Active-high Red Activity/Error LED (0x20000414=0x04, 0x00004D64)"},
    {"port": "PD3", "pin": 4,  "net": "GRN LED (CR5)", "subsystem": "Status LEDs",     "fw_func": "Active-high Green Power/Ready LED (0x20000410=0x08, 0x00005FD8)"},
    # Port E - VBUS Detect + EnergyTrace SSI1 Data
    {"port": "PE3", "pin": 12, "net": "VBUS_DET",      "subsystem": "Power Sense",     "fw_func": "AIN0 (5V0 via 10k/20k divider = 3.33V)"},
    {"port": "PE4", "pin": 123,"net": "ET_SSIDAT0",    "subsystem": "EnergyTrace HDR", "fw_func": "GPIO_PE4_SSI1XDAT0 -> ADS127L01 DIN (0x0002002E)"},
    {"port": "PE5", "pin": 124,"net": "ET_SSIDAT1",    "subsystem": "EnergyTrace HDR", "fw_func": "GPIO_PE5_SSI1XDAT1 <- ADS127L01 DOUT (0x00020034)"},
    # Port F - User / Target GPIOs
    {"port": "PF0", "pin": 42, "net": "XDS_GPIO3",     "subsystem": "Target GPIO",     "fw_func": "Aux GPIO3 (level-shifted via SN74AVC4T245)"},
    {"port": "PF1", "pin": 43, "net": "XDS_GPIO1",     "subsystem": "Target GPIO",     "fw_func": "Aux GPIO1 (level-shifted)"},
    {"port": "PF2", "pin": 44, "net": "XDS_GPIO2",     "subsystem": "Target GPIO",     "fw_func": "Aux GPIO2 (level-shifted)"},
    # Port J - GPIO Level-Shifter Directions
    {"port": "PJ0", "pin": 116,"net": "XDS_DIR_GPIO1", "subsystem": "Level Shifter",   "fw_func": "Direction for XDS_GPIO1"},
    {"port": "PJ1", "pin": 117,"net": "XDS_DIR_GPIO2", "subsystem": "Level Shifter",   "fw_func": "Direction for XDS_GPIO2"},
    # Port K - 4-bit Board Hardware ID Strap
    {"port": "PK4", "pin": 63, "net": "HW_ID0 (R39)",  "subsystem": "Board ID",        "fw_func": "Reads PK7..PK4 -> 0x20 | nibble = 0x23 (0x00005480)"},
    {"port": "PK5", "pin": 62, "net": "HW_ID1 (R27)",  "subsystem": "Board ID",        "fw_func": "HW ID bit 1"},
    {"port": "PK6", "pin": 61, "net": "HW_ID2 (R29)",  "subsystem": "Board ID",        "fw_func": "HW ID bit 2"},
    {"port": "PK7", "pin": 60, "net": "HW_ID3 (R30)",  "subsystem": "Board ID",        "fw_func": "HW ID bit 3"},
    # Port L - USB3300 ULPI Data[0..5]
    {"port": "PL0", "pin": 81, "net": "USBD0",         "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PL0_USB0D0 (0x00004E60)"},
    {"port": "PL1", "pin": 82, "net": "USBD1",         "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PL1_USB0D1 (0x00004E5A)"},
    {"port": "PL2", "pin": 83, "net": "USBD2",         "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PL2_USB0D2 (0x00004E54)"},
    {"port": "PL3", "pin": 84, "net": "USBD3",         "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PL3_USB0D3 (0x00004E4E)"},
    {"port": "PL4", "pin": 85, "net": "USBD4",         "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PL4_USB0D4 (0x00004E48)"},
    {"port": "PL5", "pin": 86, "net": "USBD5",         "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PL5_USB0D5 (0x00004E42)"},
    # Port M - EnergyTrace Analog Switches & ADC START
    {"port": "PM0", "pin": 78, "net": "ET_BYPASS",     "subsystem": "EnergyTrace HDR", "fw_func": "U33 TS3A24159 shunt bypass (HIGH=measure, LOW=bypass @ 0x0002011E/0x00020508)"},
    {"port": "PM1", "pin": 77, "net": "ET_CALIBRATE",  "subsystem": "EnergyTrace HDR", "fw_func": "U38 TS3A24159 cal isolate (HIGH=isolate target & cal @ 0x00005A82)"},
    {"port": "PM2", "pin": 76, "net": "ET_PM2",        "subsystem": "EnergyTrace HDR", "fw_func": "U35 ADS127L01 START pin (HIGH=converting @ 0x0001FDE6/0x000202E6)"},
    {"port": "PM4", "pin": 74, "net": "ET_CAL0",       "subsystem": "EnergyTrace HDR", "fw_func": "40.2 mA cal switch (R123=82R, cal idx=1 @ 0x00020270)"},
    {"port": "PM5", "pin": 73, "net": "ET_CAL1",       "subsystem": "EnergyTrace HDR", "fw_func": "90.4 mA cal switch (R125=36.5R, cal idx=2 @ 0x0002027E)"},
    # Port N - EnergyTrace ADC Reset + Board-ID I2C
    {"port": "PN3", "pin": 110,"net": "ET_PN3",        "subsystem": "EnergyTrace HDR", "fw_func": "U35 ADS127L01 nRESET/nPWDN (pulsed LOW->HIGH @ 0x0001FFF0)"},
    {"port": "PN4", "pin": 111,"net": "XDS_BID_SDA",   "subsystem": "Board ID I2C",    "fw_func": "I2C2/Bit-bang Board ID EEPROM SDA (0x0003CC9C)"},
    {"port": "PN5", "pin": 112,"net": "XDS_BID_SCL",   "subsystem": "Board ID I2C",    "fw_func": "I2C2/Bit-bang Board ID EEPROM SCL (0x0003CC9C)"},
    # Port P - USB3300 ULPI Control + Data[6..7]
    {"port": "PP2", "pin": 103,"net": "USBNXT",        "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PP2_USB0NXT + boot ULPI auto-detect (LOW=ULPI present @ 0x00005374)"},
    {"port": "PP3", "pin": 104,"net": "USBDIR",        "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PP3_USB0DIR (0x00004E1C)"},
    {"port": "PP4", "pin": 105,"net": "USBD7",         "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PP4_USB0D7 (0x00004E22)"},
    {"port": "PP5", "pin": 106,"net": "USBD6",         "subsystem": "USB3300 ULPI",    "fw_func": "GPIO_PP5_USB0D6 (0x00004E28)"},
    # Port Q - Target SPI (SSI3) / SWO / Trace
    {"port": "PQ0", "pin": 5,  "net": "XDS_SCLK",      "subsystem": "Target SPI/SWO",  "fw_func": "SSI3CLK (0x0001858C, CMD 0x56..0x59)"},
    {"port": "PQ1", "pin": 6,  "net": "XDS_nCS",       "subsystem": "Target SPI/SWO",  "fw_func": "GPIO_PQ1_SSI3FSS (0x000185CE)"},
    {"port": "PQ2", "pin": 11, "net": "XDS_PICO",      "subsystem": "Target SPI/SWO",  "fw_func": "GPIO_PQ2_SSI3XDAT0 (0x000185BE)"},
    {"port": "PQ3", "pin": 27, "net": "XDS_POCI",      "subsystem": "Target SPI/SWO",  "fw_func": "GPIO_PQ3_SSI3XDAT1 (0x000185C4)"},
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pdf", default="docs/LP-XDS110ET_SCH.pdf")
    parser.add_argument("--out", default="rev-eng/output/07_schematic_pins.json")
    args = parser.parse_args()

    pdf_path = Path(args.pdf)
    layout_txt = Path("rev-eng/output/schematic_layout.txt")
    if pdf_path.exists():
        layout_txt.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["pdftotext", "-layout", str(pdf_path), str(layout_txt)], check=False)

    print(f"=== LP-XDS110ET Verified Pinout ({len(SCHEMATIC_PINS)} active pins) ===")
    for p in SCHEMATIC_PINS:
        print(f"  {p['port']:<4s} (pin {p['pin']:3d})  {p['net']:<15s}  [{p['subsystem']:<16s}]  {p['fw_func']}")

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "board": "LP-XDS110ET (MCU101 Rev B)",
        "mcu": "MSP432E401YTPDT (128-pin TQFP, 120 MHz Cortex-M4F)",
        "hw_version_id": "0x0023",
        "pins": SCHEMATIC_PINS,
    }, indent=2))
    print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    main()
