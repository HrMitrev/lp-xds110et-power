# Firmware Architecture — `firmware_3.0.0.45.bin`

## 1. Binary Identification & Memory Layout

- **File**: `proprietary_fw/firmware_3.0.0.45.bin`
- **Size**: `323,834` bytes (`0x4F0FA`)
- **SHA-256**: `1fe5c87dcdf9b3683f3fed005e737cd071b45bd767648ebac2e1ae1ac6102b97`
- **Target MCU**: TI MSP432E401YTPDT (128-pin TQFP, ARM Cortex-M4F @ 120 MHz, 1 MiB Flash, 256 KiB SRAM; also supports 512 KiB TM4C129 variants detected via `FLASH_PP` at `0x400FDFC0`).
- **Compiler / Runtime**: GNU Arm Embedded GCC (bare-metal, newlib-nano, linked against TivaWare/MSP432E4 `driverlib` + `usblib` + selective ROM API calls). **No RTOS.**

### Flash & SRAM Map (Auto-Extracted from `Reset_Handler` @ `0x00006564`)

| Region | Address Range | Size | Contents |
|---|---|---|---|
| **DFU Bootloader** (not in `.bin`) | `0x00000000 – 0x00003FFF` | 16 KiB | Preserved TI USB DFU bootloader (`0x2C` = re-entry vector) |
| **Vector Table** | `0x00004000 – 0x00004207` | 520 B | 130 Cortex-M4 vectors (relocated to SRAM at runtime by `IntRegister`) |
| **Application `.text`** | `0x00004208 – 0x00035CEF` | ~203 KiB | Executable Thumb-2 code |
| **Application `.rodata`** | `0x00035CF0 – 0x00050B89` | ~110 KiB | USB descriptors, JTAG TAP tables, FAT16 virtual disk & target flash algos, EVM CLI strings |
| **`.data` Load Image (Flash)** | `0x00050B8A – 0x000530FA` | 9,584 B | Copied to `0x20000000 – 0x20002570` by `Reset_Handler` |
| **Config / Serial Flash** | `0x000FC000 – 0x000FFFFF` | 16 KiB | 8-byte ASCII serial (`+0x00`), config word `0xAA55000x` (`+0x10`); `0x0007C000` on 512 KiB MCUs |
| **`.data` (SRAM)** | `0x20000000 – 0x20002570` | 9,584 B | Initialized globals, TivaWare USB structs, mutable USB string descriptors |
| **Alignment / Reserved** | `0x20002570 – 0x200027FF` | 656 B | Padding before `.bss` |
| **`.bss` (SRAM)** | `0x20002800 – 0x20033588` | 200,072 B | USB ring buffers (incl. 96 KiB ET TX), uDMA table (`0x20005000`), CMSIS-DAP queues, ET ping-pong buffers |
| **Heap & Stack (SRAM)** | `0x20033588 – 0x20040000` | 51,832 B | Stack grows down from `Initial_SP = 0x20040000` |

### Code Subsystem Map (GCC Link Order)

| Flash Range | Subsystem |
|---|---|
| `0x00004000 – 0x0000685B` | Startup (`Reset_Handler`), `main()`, USB composite & buffer callbacks, ULPI init, board ID detection, LEDs, UART/CDC glue |
| `0x0000685C – 0x000074FF` | XDS110 Bulk Command Dispatcher (`TBH` table for opcodes `0x00..0x62`) |
| `0x00007500 – 0x00014F8B` | JTAG 4-wire, cJTAG 2-wire (OSCAN0/OSCAN1), TAP state machine, `nSRST`/`nTRST` |
| `0x00014F8C – 0x00016F5B` | SWD / ADIv5 / ADIv6 (64-bit) DAP engine (`0x10..0x1C`, `0x3D..0x3E`, `0x5A..0x62`) |
| `0x00016F5C – 0x000181F3` | Cortex-M / ARM / DSP memory & register access (`0x33..0x38`, `0x3A..0x3C`, `0x47..0x48`, `0x52..0x55`) |
| `0x000181F4 – 0x0001858B` | Target GPIO & auxiliary power control (`0x29..0x2A`) |
| `0x0001858C – 0x0001912F` | Target SPI / SWO streaming via `SSI3` (`0x50..0x51`, `0x56..0x59`) |
| `0x00019130 – 0x0001AEEF` | Ping/keep-alive (`0x49..0x4A`), target reset/mode helpers |
| `0x0001AEF0 – 0x0001C4C3` | CMSIS-DAP 1.0 (HID) & CMSIS-DAP 2.0 (Bulk) protocol stack |
| `0x0001C4C4 – 0x00020F4F` | **EnergyTrace & EnergyTrace++ Subsystem** (Mode 0 DCDC pulse, Mode 1 HDR `ADS127L01`, digital trace, DCDC I2C1) |
| `0x00020F50 – 0x000249FD` | Board-ID I2C driver, EnergyTrace Mode 2 (`0x21540`), Drag-n-Drop SWD target flasher (`DHCSR`/`DCRSR`), libc |
| `0x000249FE – 0x0002887B` | TI `usblib` (`USBBuffer`, `USBDComposite`, `USBDCDC`, `USBDBulk`, `USBDHID`, `USBDMSC`, `USB0DeviceIntHandler`) |
| `0x0002887C – 0x0002B137` | TI `driverlib` (`GPIO`, `SysCtl`, `Int`, `UART`, `SSI`, `I2C`, `Timer`, `ADC`, `Flash`, `uDMA`) |
| `0x0002B138 – 0x00035CEF` | Drag-n-Drop FAT16 virtual filesystem + EVM Test Automation CLI (`ina`, `sysboot`, `i2c`, `temperature`, `rtc_wakeup`) + math/stdio |

---

## 2. Vector Table & Interrupts

### Static Flash Vector Table (`0x00004000`)

Only 7 entries are statically populated in Flash; all other entries point to `IntDefaultHandler` (`0x00006559`):

| Index | IRQ | Name | Handler | Purpose |
|---|---|---|---|---|
| 0 | — | `Initial_SP` | `0x20040000` | Top of 256 KiB SRAM |
| 1 | -15 | `Reset_Handler` | `0x00006565` | Copies `.data`, zeros `.bss`, enables FPU (`CPACR |= 0x00F00000`), jumps to `main()` (`0x00005D7C`) |
| 2 | -14 | `NMI_Handler` | `0x00006541` | Infinite loop |
| 3 | -13 | `HardFault_Handler` | `0x0000654D` | Infinite loop |
| 15 | -1 | `SysTick_Handler` | `0x00004471` | 10 ms / 100 Hz system tick (`SysCtlClock / 100`) |
| 16 | 0 | `GPIOA` | `0x00006455` | Target reset / GPIOA event handler |
| **58** | **42** | **`USB0`** | **`0x00026D49`** | **`USB0DeviceIntHandler` (TivaWare `usblib`)** |

> **Important Bug Found in `firmware_src/src/startup_gcc.c`**:
> The existing skeleton `startup_gcc.c` is missing **IRQ 39 (`CAN1`)** between `CAN0` (IRQ 38) and `EMAC0` (IRQ 40). As a result, `USB0` is placed at vector index 57 instead of **58** (and every IRQ >= 40 is off by -1). This must be fixed before enabling `USB0` or any Timer/SSI/uDMA interrupts!

### Dynamically Registered Interrupts (`IntRegister` @ `0x000295E6`)

At runtime, DriverLib's `IntRegister()` copies the Flash vector table to a 1 KiB-aligned SRAM table and updates `SCB->VTOR` (`0xE000ED08`). Peripheral ISRs registered dynamically include:

- **UART0** (CDC 0 App UART): `0x00004A6F`
- **UART7** (CDC 1 Aux UART / SWO on LP-XDS110ET): `0x00004A69`
- **GPIOB** (EnergyTrace HDR `PB4` `nDRDY` DMA trigger): `0x0001FB21` (calibration) / `0x0001FB61` (streaming)
- **SSI1** (EnergyTrace HDR `ADS127L01` Rx uDMA completion): `0x0001FB31` (calibration) / `0x0001FBC5` (streaming)
- **Timer3** (EnergyTrace++ digital state polling / Mode 0 sampling): `0x00020E05` / `0x00020F25`
- **GPIOF** (`PF3` external EnergyTrace trigger on `hw=0x21`): `0x0001C651`

---

## 3. Boot Sequence (`main()` @ `0x00005D7C`)

1. **Pre-clock init** (`0x0001EB5C`): Early state initialization.
2. **System Clock** (`0x00005D88`): `SysCtlClockFreqSet(SYSCTL_XTAL_16MHZ | SYSCTL_OSC_MAIN | SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480, 120000000)` → **120 MHz** from external 16 MHz crystal `Y3` (480 MHz VCO). Stored in `g_ui32SysClock` (`0x20007610`).
3. **Status LEDs** (`0x00005D9C`): Enables `GPIOD`, configures `PD2` (Red) & `PD3` (Green) as 8 mA push-pull outputs (`0x66, 0x08`), turns both off.
4. **Flash Size Check** (`0x00005DD2`): Reads `FLASH_PP` (`0x400FDFC0`):
   - `0x00FF` (512 KiB) → Config block at `0x0007C000`, config word at `0x0007C010`.
   - `0x01FF` (1 MiB, MSP432E401Y) → Keeps default `0x000FC000` / `0x000FC010`.
   - Anything else → Lights Red LED and halts.
5. **Board & Peripheral Init** (`0x00005318`):
   - Enables all GPIO ports `A..Q`, `uDMA` (control table at `0x20005000`).
   - **Auto-detects USB3300 ULPI PHY** via `PP2` (`USBNXT`): if LOW → `g_bULPI = 1`, `maxPacketSize = 512`; if HIGH → internal FS PHY (`64`).
   - Initializes default `UART0` (`PA0/PA1`, 115200 8N1) for CDC 0.
   - Reads **Board Hardware ID** from `PK7..PK4`: `g_hw_version = 0x20 | ((GPIOPinRead(PORTK, 0xF0) >> 4) & 0x0F)` (`0x23` on LP-XDS110ET).
   - Reads config word (`0xAA550001..0xAA550004`) from `0x000FC010`.
   - Runs **LP-XDS110ET (`0x23`) init** (`0x00005A1A`):
     - Configures `PC5` (`XDS_TMS_DIR`), `UART7` (`PC4` `XDS_SWO`) for CDC 1.
     - Asserts `nSRST` (`PA6=0`), sets `PM1` (`ET_CALIBRATE=1`), waits 300 ms, executes **`ET_HDR_BootCalibrate` (`0x00020420`)** to cache all 4 calibration points (`0, 1, 2, 4`), clears `PM1`, waits 300 ms, deasserts `nSRST` (`PA6=1`), sets `g_target_vcc_mv = 3300`.
   - Calls `USB_ULPI_PinInit` (`0x00004DF4`) when `g_bULPI == 1`.
6. **Target Reset Pin & JTAG/SWD Init** (`0x00006478`, `0x000154D4`): Configures `PA6` (`XDS_RESET`) and JTAG/SWD defaults.
7. **SysTick** (`0x00005E18`): Configures 100 Hz SysTick (`120 MHz / 100`).
8. **USB String Patching** (`0x00005E20 – 0x00005F68`):
   - Formats firmware version `03.00.00.45` into UTF-16LE product string at `0x200015CC` (`"XDS110 (03.00.00.45) Embed with CMSIS-DAP"`).
   - Copies 8-char ASCII serial from `0x000FC000` (or `"NOserial"` if erased `0xFF`) into UTF-16LE serial string at `0x20001749`.
9. **USB Buffers & Composite Enumeration** (`0x00005F6A – 0x00005FBE`):
   - Initializes all 8 `tUSBBuffer` instances (4 pairs: CDC0, CDC1, Bulk0, Bulk1).
   - Calls `USBCompositeSetup` (`0x00004708`), `CMSIS_DAP_Init` (`0x0001AF34`, `0x0001C3E8`), `Timer0` init (`0x00004C7C`), and UART0/UART7 activation (`0x00004BC4`).
10. **Ready LED** (`0x00005FD6`): Turns ON Green LED (`PD3`).
11. **Main Super-Loop** (`0x0000606E – 0x000062C2`).

---

## 4. Super-Loop Architecture (`0x0000606E – 0x000062C2`)

Event flags live in `g_ui32Flags` (`0x20004B90`), accessed atomically via Cortex-M4 SRAM bit-banding (`0x22000000 + (addr - 0x20000000)*32 + bit*4`):

| Bit / Variable | Bit-Band Offset | Set By | Handler in `main()` | Role |
|---|---|---|---|---|
| `g_ui32Flags` bit 1 | `+0x04` (`r8/sb`) | Bulk 0 RX CB (`0x000044B8`) | `XDS_ProcessCommand` (`0x00004D60` → `0x0000685C`) | XDS110 `0x2A` framed debug/ET commands |
| `g_ui32Flags` bit 2 | `+0x08` | CDC0 / CDC1 RX CB (`0x00004619`) | Inline UART bridge (`0x00006246`) | Pumps bytes between CDC0/CDC1 USB buffers and `UART0`/`UART7` |
| `g_ui32Flags` bit 4 | `+0x10` (`fp`) | HID RX CB (`0x0001C2F1`) | `CMSIS_DAP_HID_Process` (`0x0001C428`) | CMSIS-DAP 1.0 HID packet processing |
| `g_ui32Flags` bit 5 | `+0x14` (`sl/r6`) | Bulk 1 RX CB (`0x00004D08`) | `Bulk1_Loopback` (`0x00004DBC`) | Reverses & echoes Bulk 1 OUT test packets |
| `g_ui32Flags` bit 6 | `+0x18` (`r7`) | Bulk 0 CMSIS-DAP 2.0 CB | `DAP_ExecuteCommand` (`0x0001AEF0`) | 64×64-byte queued CMSIS-DAP 2.0 (Bulk) commands |
| `g_ui32Flags` bit 7 | `+0x1C` | MSC init / writes | `MSC_DragDrop_Poll` (`0x0002B190`) | Drag-n-Drop FAT16 virtual disk & target flasher |
| `g_bEnterDFU` (`0x20005400`) | — | DFU request | `USBDCompositeTerm_DFUJump` (`0x00025F58`) | Cleanly shuts down USB and jumps to `0x0000002C` |
| `g_bETDigPoll` (`0x2002C70E`) | — | `ET_Start` (when `p4 != 0`) | `g_et_ops.dig_poll()` (`0x2002C7DC+0x10`) | Polls EnergyTrace++ digital state capture |
| `g_bSWOActive` (`0x20029128`) | — | `CMD 0x56` (`0x0001858C`) | `SWO_SSI3_Pump` (`0x000187A4`) | Streams `SSI3` SPI/SWO data to USB |

---

## 5. USB Composite Stack & Configurations (`0x00004708`)

Built on TivaWare `usblib` (`tUSBDCompositeDevice` at `0x2000175C`). Configuration is selected by `0x000FC010` (`0xAA55000x`):

| Config Word | Mode | VID:PID | Composite Functions |
|---|---|---|---|
| `0xAA550001` (default) | **Standard** | `0x0451:0xBEF3` (`0x1CBE:0x029E` on hw `0x30`) | 5 devices: CDC0 (App UART), Bulk0 (Debug/CMD), CDC1 (Aux UART), HID (CMSIS-DAP 1.0), Bulk1 (Data/ET) |
| `0xAA550002` | **Drag-n-Drop** | `0x0451:0xBEF4` | 5 devices: CDC0, Bulk0, CDC1, **MSC (FAT16 Mass Storage)**, Bulk1 |
| `0xAA550003` | **Minimal (hw `0x30`)** | `0x1CBE:0x029F` | 2 devices |
| `0xAA550004` | **CMSIS-DAP 2.0** | `0x1CBE:0x02A5` | 4 or 5 devices: CDC0, Bulk0 (CMSIS-DAP 2.0), CDC1, Bulk1 |

### USB Ring Buffers (`tUSBBuffer`)

| Buffer | Device Handle | Direction | SRAM Address | Size | Callback |
|---|---|---|---|---|---|
| CDC 0 TX | `0x2000164C` | Dev → Host | `0x200242E4` | 1,024 B | `0x00004615` |
| CDC 0 RX | `0x2000164C` | Host → Dev | `0x20023A4C` | 1,024 B | `0x00004619` |
| CDC 1 TX | `0x200016B8` | Dev → Host | `0x200246E4` | 1,024 B | `0x00004615` |
| CDC 1 RX | `0x200016B8` | Host → Dev | `0x20023E4C` | 1,024 B | `0x00004619` |
| **Bulk 0 TX (CMD)** | `0x2000152C` | Dev → Host | `0x20024B50` | **8,704 B** (`0x2200`) | `0x0000449D` |
| **Bulk 0 RX (CMD)** | `0x2000152C` | Host → Dev | `0x2001F64C` | **8,704 B** (`0x2200`) | `0x000044B9` |
| **Bulk 1 TX (ET Data)** | `0x2000157C` | Dev → Host | `0x2000764C` | **98,304 B** (**96 KiB!**) | `0x0000449D` |
| **Bulk 1 RX (Loopback)** | `0x2000157C` | Host → Dev | `0x2002184C` | **8,704 B** (`0x2200`) | `0x00004D09` |

*(The 96 KiB Bulk 1 TX buffer absorbs ~93 ms of host USB scheduling jitter at 256 kS/s × 4 B/sample = 1.03 MB/s.)*

---

## 6. USB DFU Bootloader Re-Entry (`0x00025F58`)

Invoked by `CMD 0x26` (`XDS_EnterDFU`, `0x00006EB4`) or `g_bEnterDFU`:

```c
// Exact clean-room equivalent of 0x00025F58:
GPIOPinWrite(GPIO_PORTD_BASE, GPIO_PIN_2 | GPIO_PIN_3, 0); // LEDs off
USBDCDTerm(0);                                             // Detach USB
ROM_IntMasterDisable();
ROM_SysTickIntDisable();
ROM_SysTickDisable();
for (int i = 0; i < 5; i++) NVIC_DIS[i] = 0xFFFFFFFF;      // Mask all IRQs
ROM_SysCtlPeripheralReset(SYSCTL_PERIPH_USB0);
ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_USB0);
ROM_USBClockEnable(USB0_BASE, 8, USB_CLOCK_INTERNAL);      // 480 MHz / 8 = 60 MHz
ROM_SysCtlDelay(g_ui32SysClock / 3);                       // ~1 second detach wait
ROM_IntMasterEnable();
((void (*)(void))(*(uint32_t *)0x0000002C))();             // Jump to Flash DFU bootloader!
```

Implementing this sequence early in any open-source firmware guarantees that `xds110-et dfu-flash` can always recover and reflash the probe over USB without needing an external JTAG debugger on `P8`.
