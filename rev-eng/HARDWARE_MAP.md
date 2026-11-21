# LP-XDS110ET Hardware & Peripheral Map

Cross-referenced from `docs/LP-XDS110ET_SCH.pdf` (**LP-XDS110ET, PN: MCU101 Rev B, 2022-10-05**) and `proprietary_fw/firmware_3.0.0.45.bin`.

---

## 1. Board Identification & Clocks

| Parameter | Value | Source |
|---|---|---|
| **MCU** | `U5`: **MSP432E401YTPDT** (128-TQFP, 1 MiB Flash, 256 KiB SRAM) | Schematic Sheet 2 |
| **Main Crystal** | `Y3`: **16.000 MHz** on `OSC0`/`OSC1` (pins 88/89) → PLL VCO 480 MHz → **SYSCLK = 120 MHz** | Sheet 2 & `0x00005D88` |
| **USB PHY Crystal** | `Y4`: **24.000 MHz** on `U15` (`USB3300-EZK`) | Sheet 2 |
| **ADC Master Clock** | `Y5`: **16.384 MHz** oscillator ÷ 2 (`U34 SN74LVC2G74DCU`) → **8.192 MHz** into `ADS127L01` `CLK` | Sheet 3 |
| **Hardware ID (`g_hw_version`)** | **`0x0023`** — read as `0x20 \| ((PORTK & 0xF0) >> 4)` from `PK4..PK7` (`R39, R27, R29, R30`) | `0x00005480` |
| **MCU ADC Reference** | `U6`: `LM4040B25IDCK` (**2.500 V** on `VREFA+` pin 9) | Sheet 2 |
| **EnergyTrace ADC Reference** | `U40`: `REF6025IDGK` (**2.500 V** precision buffered reference `ET_VREF`) | Sheet 3 |
| **Power Rails** | `XDS_VDD = 3.3 V` (`U7 TPS79601DR`), `ET_3V3 = 3.3 V` (`U42`), `ET_VDD = 3.3 V` (`U46`), `ET_1V8 = 1.8 V` (`U43 LP5907`), `SW_VDD = 3.6 V` (`U44 TPS73101DBV`, analog switch supply), `5V0` via `U47 TPS259530DSG` eFuse | Sheets 2 & 4 |

### Supported XDS110 Hardware Variants in Firmware `3.0.0.45`

| `hw_version` | Board / Variant | Notable Differences in Firmware |
|---|---|---|
| `0x0021` | Standalone XDS110 Debug Probe (+ optional EnergyTrace HDR pod) | Active-low LEDs (`PD2`=green, `PD3`=red), `UART4` (`PK0/PK1`), external I2C1 DCDC/ET pod (`PF3` trigger, `PN2` range) |
| **`0x0023`** | **LP-XDS110ET (This Board)** | Active-high LEDs (`PD3`=green, `PD2`=red), `UART0` + `UART7`, **USB3300 ULPI HS-USB**, onboard `ADS127L01` EnergyTrace HDR (`SSI1` + `PM0/1/2/4/5`, `PN3`, `PC6`), boot-cached cal, fixed `3300 mV` |
| `0x0022`, `0x0028..0x002D`, `0x0030` | Other LaunchPad / EVM XDS110 variants (TM4C129 / MSP432E4) | Different Aux UART (`UART3` vs `UART7`), optional I2C board-ID EEPROM (`0x2C`/`0x2E`), USB PID `0x029E`/`0x029F` on `0x30` |

---

## 2. Critical Hardware Discovery: USB3300-EZK ULPI PHY

On LP-XDS110ET (Schematic Sheet 2):
- USB-C connector `J1` (`USBDP`, `USBDM`) routes **exclusively** to `U15` (**`USB3300-EZK`**) pins 7 & 8.
- The MSP432E401Y internal USB PHY pins `PL6/USB0DP` (pin 94) and `PL7/USB0DM` (pin 93) are **unconnected**.
- Firmware auto-detects the ULPI PHY at boot (`0x00005374`): configures `PP2` (`USBNXT`) as input with weak pull-up (`GPIO_PIN_TYPE_STD_WPU`); `USB3300` pulls `USBNXT` **LOW**, selecting ULPI High-Speed mode (`512`-byte bulk endpoints).
- `USB_ULPI_PinInit` (`0x00004DF4`) configures all 12 ULPI pins with high drive (`8 mA`, `GPIO_PIN_TYPE_STD`) and calls `USBStackModeSet` / `USBDCDFeatureSet(0, USBLIB_FEATURE_USBULPI, ...)`:
  - `PB2` → `GPIO_PB2_USB0STP` (`0x0001080E`)
  - `PB3` → `GPIO_PB3_USB0CLK` (`0x00010C0E`)
  - `PP2` → `GPIO_PP2_USB0NXT` (`0x000D080E`)
  - `PP3` → `GPIO_PP3_USB0DIR` (`0x000D0C0E`)
  - `PL0..PL5` → `GPIO_PL0_USB0D0..GPIO_PL5_USB0D5` (`0x000A000E..0x000A140E`)
  - `PP5` → `GPIO_PP5_USB0D6` (`0x000D140E`), `PP4` → `GPIO_PP4_USB0D7` (`0x000D100E`)

---

## 3. Complete MSP432E401Y Pinout (`LP-XDS110ET`)

| MCU Pin | TQFP# | Schematic Net | Subsystem | Firmware Configuration & Notes |
|---|---|---|---|---|
| **PA0** | 33 | `XDS_RX` | App UART (CDC0) | `UART0 RX` (`GPIO_PA0_U0RX`), 115200 8N1 default, level-shifted |
| **PA1** | 34 | `XDS_TX` | App UART (CDC0) | `UART0 TX` (`GPIO_PA1_U0TX`), level-shifted |
| **PA2** | 35 | `XDS_TCK` | JTAG / SWD | Bit-banged `TCK` / `SWCLK` (`GPIO_PORTA_AHB` masked write `+0xB0`) |
| **PA3** | 36 | `XDS_TMS` | JTAG / SWD | Bit-banged `TMS` / `SWDIO` (bi-dir with `PC5` `XDS_TMS_DIR`) |
| **PA4** | 37 | `XDS_TDO` | JTAG / SWD | Bit-banged `TDO` input |
| **PA5** | 38 | `XDS_TDI` | JTAG / SWD | Bit-banged `TDI` output |
| **PA6** | 40 | `XDS_RESET` | Target Reset | `nSRST` (`CMD 0x0E` @ `0x0000DF30`), open-drain/push-pull via level shifter |
| **PB0** | 95 | `USB0ID` (1k) | USB0 | Analog `USB0ID` (`0x000053B8`) |
| **PB1** | 96 | `USB0VBUS` (1k) | USB0 | Analog `USB0VBUS` (`0x000053B8`) |
| **PB2** | 91 | `USBSTP` | USB3300 ULPI | `GPIO_PB2_USB0STP` (`0x00004DF6`) |
| **PB3** | 92 | `USBCLK` | USB3300 ULPI | `GPIO_PB3_USB0CLK` (`0x00004DFC`) |
| **PB4** | 121 | `ET_SSIFSS` | **EnergyTrace HDR** | `ADS127L01 nDRDY` → GPIO input falling-edge **uDMA Ch5 trigger** (`0x0001FA60`) |
| **PB5** | 120 | `ET_SSICLK` | **EnergyTrace HDR** | `GPIO_PB5_SSI1CLK` → `ADS127L01 SCLK` @ **15 MHz** (`0x00020028`) |
| **PC0..PC3** | 100..97 | `ITCK/ITMS/ITDI/ITDO` | Probe Debug | MSP432E401Y own JTAG/SWD header `P8` (DNM) |
| **PC4** | 25 | `XDS_SWO` | Aux UART / SWO | `GPIO_PC4_U7RX` (`UART7 RX` → CDC1 @ `0x00005A46`) |
| **PC5** | 24 | `XDS_TMS_DIR` | Level Shifter | `SN74AVC4T245` direction for `XDS_TMS` (HIGH=out, LOW=in) |
| **PC6** | 23 | `ET_CAL2` | **EnergyTrace HDR** | `U39` switch IN3 → `R130` = 2.2 kΩ (**1.5 mA** cal point, `idx=4`) |
| **PD2** | 3 | `CR4` (RED LED) | Status LED | Active-HIGH via 180 Ω (`R23`). Blinks on USB commands; solid on fatal error |
| **PD3** | 4 | `CR5` (GRN LED) | Status LED | Active-HIGH via 220 Ω (`R25`). Solid ON when probe is ready |
| **PE3** | 12 | `VBUS_DET` | Power Sense | `AIN0` (5V0 ÷ 3 via `R92=10k`, `R93=20k`) |
| **PE4** | 123 | `ET_SSIDAT0` | **EnergyTrace HDR** | `GPIO_PE4_SSI1XDAT0` → `ADS127L01 DIN` (`0x0002002E`) |
| **PE5** | 124 | `ET_SSIDAT1` | **EnergyTrace HDR** | `GPIO_PE5_SSI1XDAT1` ← `ADS127L01 DOUT` (`0x00020034`) |
| **PF0** | 42 | `XDS_GPIO3` | Target GPIO | Level-shifted aux GPIO3 |
| **PF1** | 43 | `XDS_GPIO1` | Target GPIO | Level-shifted aux GPIO1 (dir = `PJ0`) |
| **PF2** | 44 | `XDS_GPIO2` | Target GPIO | Level-shifted aux GPIO2 (dir = `PJ1`) |
| **PJ0, PJ1** | 116, 117 | `XDS_DIR_GPIO1/2` | Level Shifter | Direction control for `PF1`, `PF2` |
| **PK4..PK7** | 63..60 | `HW_ID0..3` | Board ID | 1 kΩ strap resistors → reads `0x3` → `hw_version = 0x23` |
| **PL0..PL5** | 81..86 | `USBD0..USBD5` | USB3300 ULPI | `GPIO_PL0_USB0D0 .. GPIO_PL5_USB0D5` |
| **PM0** | 78 | `ET_BYPASS` | **EnergyTrace HDR** | `U33 TS3A24159`: **LOW = shunt bypassed** (normal), **HIGH = current routed through 1 Ω shunt** |
| **PM1** | 77 | `ET_CALIBRATE` | **EnergyTrace HDR** | `U38 TS3A24159`: **LOW = target `LP_VDD` connected**, **HIGH = target isolated, cal bank connected** |
| **PM2** | 76 | `ET_PM2` | **EnergyTrace HDR** | `U35 ADS127L01 START` pin (**HIGH = continuous 256 kS/s conversion**) |
| **PM4** | 74 | `ET_CAL0` | **EnergyTrace HDR** | `U39` switch IN1 → `R123` = 82 Ω (**40.2 mA** cal point, `idx=1`) |
| **PM5** | 73 | `ET_CAL1` | **EnergyTrace HDR** | `U39` switch IN2 → `R125` = 36.5 Ω (**90.4 mA** cal point, `idx=2`) |
| **PN3** | 110 | `ET_PN3` | **EnergyTrace HDR** | `U35 ADS127L01 nRESET/nPWDN` (pulsed LOW→HIGH in `0x0001FFF0`) |
| **PN4, PN5** | 111, 112 | `XDS_BID_SDA/SCL` | Board ID I2C | External board-ID EEPROM bus (not used on `0x23`) |
| **PP2** | 103 | `USBNXT` | USB3300 ULPI | `GPIO_PP2_USB0NXT` (also probed at boot for ULPI auto-detect) |
| **PP3** | 104 | `USBDIR` | USB3300 ULPI | `GPIO_PP3_USB0DIR` |
| **PP4, PP5** | 105, 106 | `USBD7`, `USBD6` | USB3300 ULPI | `GPIO_PP4_USB0D7`, `GPIO_PP5_USB0D6` |
| **PQ0..PQ3** | 5, 6, 11, 27 | `XDS_SCLK/nCS/PICO/POCI` | Target SPI / SWO | `SSI3` (`CMD 0x56..0x59` @ `0x0001858C`) |

---

## 4. Peripherals & uDMA Channel Allocation

| Peripheral | Base Address | Purpose on LP-XDS110ET |
|---|---|---|
| **USB0** | `0x40050000` | USB 2.0 High-Speed Device via external `USB3300` ULPI PHY |
| **SSI1** | `0x40009000` | **EnergyTrace HDR `ADS127L01` SPI Master** (Mode 2 / `SSI_FRF_MOTO_MODE_2`, 8-bit, 15 MHz) |
| **SSI3** | `0x4000B000` | Target SPI / SWO streaming (`CMD 0x56..0x59`) |
| **UART0** | `0x4000C000` | CDC 0 Application Virtual COM Port (`PA0/PA1`) |
| **UART7** | `0x40013000` | CDC 1 Auxiliary COM / SWO RX (`PC4`) |
| **TIMER0** | `0x40030000` | General microsecond / timeout timer (`0x00004C7C`) |
| **TIMER1** | `0x40031000` | Profile / EnergyTrace helper timer (`0x0001C526`) |
| **TIMER3** | `0x40033000` | EnergyTrace++ digital state sample timer (`120 MHz / freq`, e.g., 48 kHz) |
| **TIMER4** | `0x40034000` | Classic EnergyTrace Mode 0 `T4CCP0` pulse counter (not used on `0x23`) |
| **I2C1** | `0x40021000` | External DCDC MSP430 / EVM Automation INA/temp sensors (not populated on `0x23`) |
| **uDMA** | `0x400FF000` | Control table at `0x20005000` (1024-byte aligned): |

### uDMA Channels Discovered

| Channel | Assignment (`UDMA_CHn_*`) | Mode | Usage |
|---|---|---|---|
| **Ch 5** | `UDMA_CH5_GPIOB` (`0x00030005`) | Ping-Pong, 8-bit, `ARB_4`, `SRC_INC_NONE`, `DST_INC_NONE` | Triggered on `PB4` (`ADS127L01 nDRDY`) falling edge; bursts **4 dummy bytes** into `SSI1_DR` (`0x40009008`) per ADC sample (1024 B/buffer) |
| **Ch 8** | `UDMA_CH8_UART0RX` (`0x00000008`) | Basic / Ping-Pong | `UART0` (CDC 0) receive |
| **Ch 20** | `UDMA_CH20_UART7RX` (`0x00020014`) | Basic / Ping-Pong | `UART7` (CDC 1 / SWO) receive |
| **Ch 24** | `UDMA_CH24_SSI1RX` (`0x00000018`) | Ping-Pong, 8-bit, `ARB_4`, `SRC_INC_NONE`, `DST_INC_8` | Drains `SSI1_DR` (`0x40009008`) into 2×1024-byte EnergyTrace packet payloads (`0x2002C89F`, `0x2002CCA5`) |

---

## 5. Performance Optimization Used for JTAG/SWD

The MSP432E401Y AHB GPIO peripheral supports **hardware bit-masked data registers** (`base + (mask << 2)`). `firmware_3.0.0.45.bin` heavily exploits `GPIO_PORTA_AHB` (`0x40058000`) masked offsets (`+0x10, +0x20, +0x30, +0x40, +0x80, +0xB0`), e.g.:
- `GPIO_PORTA_AHB + 0xB0` (`mask = 0x2C = PA2|PA3|PA5 = TCK|TMS|TDI`): single-instruction atomic update of `TCK`, `TMS`, and `TDI` without touching `PA0/PA1` (UART0) or `PA6` (nSRST).
