# XDS110 EnergyTrace protocol notes

Source: static analysis of `jscxds110.dll` (`ET_*` exports), verified on hardware (FW 3.0.0.65, HW 0x23).
Decompiled output: `out/` (generate with `decomp.ps1`).

## Transport
- Debug interface 2 (WinUSB), bulk OUT `0x02`, bulk IN `0x83`.
- Request: `'*'(0x2a) | u16le len | cmd | args`   (len = 1 + len(args))
- Response: `'*' | u16le len | i32le error | data`
- jscxds110 retries each command twice, 4000 ms read timeout. Max frame 0x1100 bytes.
- Sample stream is read via `XDS_ReadDataPort` (function-pointer into jioxds110) - presumably the
  "Data Port" interface (MI_06). **Not decoded yet.**

## Commands (cmd byte -> jscxds110 export)
| cmd  | export                | args (after cmd)                                 | resp data after error |
|------|-----------------------|--------------------------------------------------|-----------------------|
| 0x02 | (session disconnect)  | -                                                | -                     |
| 0x03 | (version query)       | -                                                | 6 bytes: `41 00 00 03 23 00` |
| 0x0e | XDS_SetSrst           | u8 (0x00 = assert nSRST, 0x01 = deassert nSRST)  | -                     |
| 0x1d | ET_Setup              | u8 p2, u8 p4, u32 p3, u8 p5 (wire order)         | -                     |
| 0x1e | ET_Calibrate          | u16                                              | u32, u32              |
| 0x1f | ET_Start              | -                                                | -                     |
| 0x20 | ET_Stop               | -                                                | -                     |
| 0x21 | ET_Cleanup            | -                                                | -                     |
| 0x22 | ET_DCDC_GetMCUVersion | -                                                | u32                   |
| 0x23 | ET_DCDC_PowerDownMCU  | -                                                | -                     |
| 0x24 | ET_DCDC_SetVcc        | u16 (mV)                                         | -                     |
| 0x25 | ET_DCDC_RestartMCU    | -                                                | -                     |
| 0x26 | XDS_EnterDFUMode      | -                                                | -                     |
| 0x28 | XDS_ConnectET         | -                                                | -                     |
| 0x30 | ET_Setup_Range        | u8                                               | -                     |
| 0x31 | ET_Setup_Dig          | u8                                               | -                     |
| 0x44 | ET_Setup_Dig_Params   | u8 n, n bytes                                    | -                     |
| 0x46 | ET_HardwareInfo       | -                                                | u32, u8               |

Hardware check: `raw 0x46` -> `error=0 data=0200000007`; `raw 0x28` -> `error=0`.

## Target MCU Hardware Reset (xds110reset.exe)
Source: Reverse engineering of `xds110reset.exe` from TI CCS/UniFlash (`C:\tools\ti\uniflash\deskdb\content\TICloudAgent\win\ccs_base\common\uscif\xds110\xds110reset.exe`).
- Transport: WinUSB interface 2 (CMD), Bulk OUT 0x02, Bulk IN 0x83.
- Command: `0x0E` (`XDS_SetSrst`).
- Packet structure:
  - Assert reset (pull nSRST low): `2A 02 00 0E 00`
  - Deassert reset (release nSRST high): `2A 02 00 0E 01`
- Toggle sequence (default `xds110reset` behavior):
  1. Send `cmd(0x0e, &[0x00])` -> asserts reset.
  2. Sleep for delay (default 50 ms).
  3. Send `cmd(0x0e, &[0x01])` -> deasserts reset.
  Reply packet is 7 bytes: `2A 04 00 00 00 00 00` (error code 0).


## Observed session (USBPcap capture of `stune energytrace`, `captures/h2.pcap`)
Per connect (repeated 3x): `28`, `03`(version), `05 <u32 2>`, `09 <49 04 00 00, 01 00 00 00>`, `03`, `02`.
Then for the trace:
```
46                      -> data 02000000 07      HardwareInfo
30 00                   Range=0
31 01                   Dig=1
1d 01 00 80bb0000 00    Setup: u8=1, u8=0, u32=48000 (LPRF sample freq), u8=0
1e 0000 / 1e 0400 / 1e 0100   Calibrate idx 0,4,1 -> u32 sum, u32 count(=5)
1f                      Start   -> stream begins on bulk IN 0x87
... 20 (Stop) ... 21 (Cleanup)
```

### Session end (USBPcap of `stune energytrace --duration=100`, Windows) - VERIFIED
```
20          Stop      -> ok
(~0.5 s)    host-side WinUSB AbortPipe on EP 0x87 (nothing sent to the probe)
21          Cleanup   -> ok
20          Stop      -> error -644 (0xfffffd7c)  \  second Stop/Cleanup fail harmlessly
21          Cleanup   -> error -644               /
02          Disconnect -> ok (reply takes ~100 ms)
```
`02` is the same command stune sends after each connect-time probe at session start. A client that stops
after `20`/`21` without `02` leaves the session open, so `xds110_et.py` and `xds110_et_live.py` now send
`02` after Cleanup. The `20`/`21` repeat is not needed.
Calibrate: ADC = sum/count. idx0 = zero offset (e.g. -44495/5 = -8899), idx4 ~ 1.5 mA point,
idx1 ~ 40.2 mA point; these are the rows of TI's "Point ADC Value,currentNA" table.

## Sample stream (bulk IN 0x87, interface 6 "Data Port") - VERIFIED
- After `1f`, a 3-byte message `35 80 bb` (0x35, u16le 48000), then fixed 1030-byte transfers:
  `68 | seq u8 | u16le 0x0400 | u16 0 | 256 x u32be`.
- Each u32be sample: bits 31..8 = raw ADC (20 bit), bits 7..0 = status byte (0xd0, 0x30, 0x70, 0x40, 0xf0 seen; meaning unknown).
- Rate: 256 kS/s (3.9 us/sample), 256 samples per transfer, ~1 transfer/ms.
- `decode_stream.py` reproduces TI's `et_raw.csv` ADC and status columns: 569344/569344 samples match.

## ADC -> current (from et_raw.csv)
adj = adc - offset (offset = calibration idx0, e.g. -8899); current_nA = adj * slope,
piecewise-linear with slopes from the calibration points (e.g. 14.6626 below ~263905, 15.0262 above;
"Line offset,Slope" table in the log). Not fully re-derived yet.

## Using the existing TI driver on Windows
Interfaces 2 and 6 are already bound to TI's own WinUSB driver (`oem464.inf`, class `TI_Debug_Probe`),
so no Zadig/driver swap is needed; pyusb + libusb opens the probe directly. Notes:
- `is_kernel_driver_active` / `detach_kernel_driver` raise `NotImplementedError` on Windows; ignore them.
- Call `set_configuration()` before claiming interfaces.
- `libusb_open` can fail with "Operation not supported" when the probe is in a stale state; replug it
  (and close CCS/other tools using it).
- Read EP 0x87 from a dedicated thread; doing console output in the read loop makes the probe's buffer
  overflow (tens of thousands of frames lost). With a reader thread, 0 frames lost in 30 s captures.

## Capture how-to
USBPcap needs `-d \\.\USBPcapN` (N = root hub; XDS110 was on USBPcap2, device address 2) and
`-A --inject-descriptors`; start it before running `capture_et_raw.ps1`, then
`tshark -r h2.pcap -Y "usb.device_address==2 && usb.capdata" -T fields -e usb.endpoint_address -e usb.capdata`.
If tshark isn't installed, the pcap (link type 249) can be parsed directly: USBPcap header is
`<HQIHBHHBBI` (len, irpId, status, function, info bit0=completion, bus, device, endpoint, transfer type, dataLen).

## Open items
- Meaning of the status byte, and the 3rd `05`/`09` connect-time commands.
- Exact piecewise ADC->nA slope derivation from the calibration points.
- Whether the repeated Stop/Cleanup (-644) in stune has any purpose.

## info
ID : 0451:bef3

Command sequence stune sends

•  46  hardware info,  30 00  range,  31 01  digital.
•  1d 01 00 80bb0000 00  is Setup with a 48000 sample frequency.
•  1e 0000 ,  1e 0400 ,  1e 0100  are three calibration reads. Each returns a sum and a count of 5.
•  1f  starts the stream.  20  stops it and  21  cleans up.

Sample stream

• Samples arrive on bulk IN  0x87  (the Data Port interface).
• After a 3-byte  35 80 bb  message, each transfer is 1030 bytes.
It has a 6-byte header ( 0x68 , a sequence byte, length  0x0400 , then two zero bytes)
followed by 256 big-endian 32-bit samples.
• Each sample holds the raw ADC value in the top 24 bits (20 bits used) and a status byte in the low 8.
• That works out to 256 kS/s.

Calibration

• A calibration read returns a sum and a count; ADC = sum/count. Index 0 is the zero offset, e.g. −44495/5 = −8899.
• Current is  (adc − offset) × slope , piecewise-linear between the calibration points. I haven't fully derived the slopes yet.

## Device Firmware Upgrade (DFU) & Serial Configuration
Reverse engineered from TI's `xdsdfu.exe` and verified against hardware.

### 1. DFU Mode Switch
- Runtime device: `VID: 0x0451, PID: 0xBEF3`.
- Send 4-byte command `2A 01 00 26` (`'*'`, len=1, cmd=0x26) on Interface 2, Bulk OUT endpoint `0x02`.
- Probe reboots and enumerates as a USB DFU device: `VID: 0x1CBE, PID: 0x00FF` (or `0x0451:0xBEF4`).
- DFU Interface: Interface 0 (`bInterfaceClass = 0xFE`, `bInterfaceSubClass = 0x01`, `bInterfaceProtocol = 0x02`).
- Transfer block size (`wTransferSize`): 1024 bytes (0x0400).

### 2. Standard USB DFU Requests (Control Endpoint 0)
| Request | Code | Type | Value / Index | Description |
|---------|------|------|---------------|-------------|
| `DFU_DETACH` | `0x00` | Host->Dev `0x21` | timeout / iface | Detach and re-enumerate |
| `DFU_DNLOAD` | `0x01` | Host->Dev `0x21` | block_num / iface | Send command (wValue=0) or data chunk (wValue>=2) |
| `DFU_UPLOAD` | `0x02` | Dev->Host `0xA1` | block_num / iface | Read memory chunk (wValue>=2) |
| `DFU_GETSTATUS` | `0x03` | Dev->Host `0xA1` | 0 / iface | Read 6-byte status (`status`, `pollTimeout[3]`, `state`, `iString`) |
| `DFU_CLRSTATUS` | `0x04` | Host->Dev `0x21` | 0 / iface | Clear error state |
| `DFU_ABORT` | `0x06` | Host->Dev `0x21` | 0 / iface | Abort current transfer to idle |

### 3. Vendor Command Packets (sent via `DFU_DNLOAD` with `wValue = 0`, length 8)
- `DFU_CMD_PROG (0x01)`: `01 00 <start_block: u16le> <length: u32le>` (start_block = start_address / 1024)
- `DFU_CMD_READ (0x02)`: `02 00 <start_block: u16le> <length: u32le>`
- `DFU_CMD_CHECK (0x03)`: `03 00 <start_block: u16le> <length: u32le>`
- `DFU_CMD_ERASE (0x04)`: `04 00 <start_block: u16le> <num_blocks: u16le> 00 00`
- `DFU_CMD_INFO (0x05)`: `05 00 00 00 00 00 00 00` (returns 20 bytes via `DFU_UPLOAD`)
- `DFU_CMD_BIN (0x06)`: `06 00 00 00 00 00 00 00`
- `DFU_CMD_RESET (0x07)`: `07 00 00 00 00 00 00 00` (triggers probe reboot to runtime mode)

### 4. Memory Layout
- Flash size: 1048576 bytes (1 MB, 64 flash erase blocks of 16 KB / 0x4000).
- `0x00000000 - 0x00003FFF` (16 KB): Bootloader area (`boot_loader.bin`).
- `0x00004000 - ...` (Application): Firmware binary (`firmware_3.0.0.xx.bin`), start block = 16 (`0x4000 / 1024`).
- `0x000FC000` (Last 16 KB block, block 63): Configuration and serial information block:
  - `offset 0..7`: 8 ASCII characters for the Serial Number (e.g. `LS4205CD`).
  - `offset 16..19` (`0x10`): Configuration number encoded as `0xAA550000 | (config & 0xFFFF)`.
    - Config 1: `0xAA550001` (Standard: Debug, App COM, Aux COM, CMSIS-DAP 1.0, EnergyTrace)
    - Config 2: `0xAA550002` (Drag-n-Drop)
    - Config 4: `0xAA550004` (CMSIS-DAP 2.0)
    - Config 5: `0xAA550005` (EVM Test Automation)

