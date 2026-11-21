# XDS110 USB Protocol Specification (`firmware_3.0.0.45.bin`)

Extracted directly from `XDS_DispatchCommand` (`0x0000685C`, `TBH` jump table at `0x0000688A`), Bulk 0/1 callbacks (`0x000044A0`, `0x000044B8`, `0x00004D08`), and subsystem handlers.

---

## 1. Bulk Command Transport (Interface 2: Debug / CMD Port)

- **Endpoints**: Bulk OUT `0x02` (`tUSBBuffer` RX @ `0x00035E08`, 8,704 B), Bulk IN `0x83` (`tUSBBuffer` TX @ `0x00035F08`, 8,704 B).
- **Packet Framing** (parsed by `0x000044A0`):
  - **Request**: `[0x2A ('*')] [u16le payload_len] [u8 opcode] [args...]`
    - Total wire length = `3 + payload_len` (`payload_len = 1 + len(args)`).
    - Byte `+0`: `0x2A` magic header.
    - Bytes `+1..+2`: `payload_len` (little-endian `u16`).
    - Byte `+3`: `opcode` (`0x00 .. 0x62`).
    - Bytes `+4..`: Command arguments (accessed via `[r7, #4+]` in dispatcher).
  - **Response**: `[0x2A ('*')] [u16le payload_len] [i32le error_code] [data...]`
    - Byte `+0`: `0x2A`.
    - Bytes `+1..+2`: `4 + len(data)`.
    - Bytes `+3..+6`: Signed 32-bit little-endian error code (`0` = success, negative = error).
    - Bytes `+7..`: Command response payload (written to `[r6, #7+]`).
- **Activity LED**: `XDS_ProcessCommand` (`0x00004D60`) toggles the Red LED (`PD2`) on entry and exit of every command.
- **Auto-Stop Guard (`0x00006860`)**:
  - Any command except `0x4A` (`XDS_PING`) resets the keep-alive timer (`0x0001913C`).
  - If `SSI3` / SWO streaming (`0x20029128`) is active and `opcode` is outside `0x56..0x59`, streaming is automatically stopped (`0x000186DC`).

---

## 2. Complete Opcode Table (`0x00 .. 0x62`)

94 of 99 slots are active; `0x4B..0x4F` branch to the default unsupported error handler (`0x000074A8`).

### Core Session, Probe Info & DFU

| Opcode | Case Addr | Callee(s) | Name | Request Args (`+4..`) | Response Data (`+7..`) |
|---|---|---|---|---|---|
| `0x00` | `0x6954` | `0x23CD0` (`memcpy`) | `XDS_ECHO` | `u32 len`, `u8 data[len]` | Echoes `data[len]` |
| `0x01` | `0x6966` | `0x0EFE0` | `XDS_CONNECT` | — | — (clears TAP state `0x20026D50`) |
| `0x02` | `0x6974` | `0x0DC38` | `XDS_DISCONNECT` | — | — (tri-states JTAG/SWD pins) |
| `0x03` | `0x697C` | inline | `XDS_VERSION` | — | `u32le 0x03000045` (`45 00 00 03`), `u16le hw_version` (`23 00`) |
| `0x08` | `0x69CA` | `0x0DED8` | `XDS_GET_POWER` | — | `u32le vcc_mv` (`3300` on LP-XDS110ET) |
| `0x0E` | `0x6BF4` | `0x0DF30` | `XDS_SET_SRST` | `u8 state` (`0`=assert low, `1`=deassert high) | — (`PA6` on `hw=0x23`) |
| `0x26` | `0x6EB4` | `0x25F58` | `XDS_ENTER_DFU` | — | *(No reply; detaches USB & jumps to `0x0000002C`)* |
| `0x27` | `0x6ED0` | `0x1EDB0` | `XDS_READ_CONFIG` | `u32 param` | Flash config/serial info |
| `0x28` | `0x6EE2` | `0x0EEF4` | `XDS_CONNECT_ET` | — | — (connects session without driving JTAG) |
| `0x29` | `0x6EE8` | `0x181F4` | `XDS_GPIO_GET` | `u32 flags` | `u16le pin_state` |
| `0x2A` | `0x6EF8` | `0x1824C` | `XDS_GPIO_SET` | `u32 flags`, `u16le mask_val` | — (aux GPIO/power pins) |
| `0x49` | `0x715E` | `0x19130`, `0x6324/48` | `XDS_LED_OR_KEEPALIVE` | `u8 subcmd`, `u32 arg` | — |
| `0x4A` | `0x71E0` | `0x19148` | `XDS_PING` | — | — (keep-alive; does not reset `0x1913C`) |

### JTAG & cJTAG (2-Wire / 4-Wire)

| Opcode | Case Addr | Callee(s) | Name | Request Args / Notes |
|---|---|---|---|---|
| `0x04` | `0x699A` | `0x0EE20` | `XDS_SET_TCK_DELAY` | `u32 delay` (sets TCK half-period delay) |
| `0x05` | `0x69A4` | `0x0DE18` | `XDS_SET_TRST` | `u8 state` (`0`=assert nTRST, `1`=deassert nTRST; **`>=2` returns `-122`**) |
| `0x06` | `0x69AE` | `0x0DE8C` | `XDS_CYCLE_TCK` | `u32 cycles` |
| `0x07` | `0x69BE` | inline | `XDS_SET_JTAG_MODE` | `u32 mode` |
| `0x09` | `0x69E0` | `0x07634` (via `0x200017F0`) | `XDS_JTAG_GOTO_STATE` | `u8 target_state` (`+4`, 0..9), `u8 path` (`+8`, 0..3). Uses 10×10×4 TAP table @ `0x0003605C` |
| `0x0A` | `0x69EE` | JTAG scan | `XDS_JTAG_SCAN_IR` | Bit count, pre/post pad, end state, TDI data → TDO data |
| `0x0B` | `0x6A4E` | JTAG scan | `XDS_JTAG_SCAN_DR` | Bit count, pre/post pad, end state, TDI data → TDO data |
| `0x0C` | `0x6AD4` | JTAG scan | `XDS_JTAG_SCAN_RAW` | Raw TMS/TDI bit stream → TDO |
| `0x0D` | `0x6B66` | `0x0DEE4` | `XDS_JTAG_EXEC` | Multi-command JTAG sequence |
| `0x0F` | `0x6BFE` | inline | `CJTAG_INIT` | — |
| `0x10` | `0x6C02` | `0x157E0` | `CJTAG_CONNECT` | — |
| `0x11` | `0x6C0A` | `0x157E4` | `CJTAG_DISCONNECT` | — |
| `0x12` | `0x6C12` | `0x15828` | `CJTAG_ESCAPE` | Zero-bit / escape sequence |
| `0x2B` | `0x6F02` | `0x111FC` | `CJTAG_SET_FORMAT` | `u32 format` |
| `0x2C` | `0x6F0A` | `0x112B0` | `CJTAG_RESET` | — |
| `0x2D` | `0x6F10` | `0x0E0D4` | `JTAG_SCAN_EXT_A` | Extended JTAG/cJTAG scan |
| `0x2E` | `0x6F30` | `0x0E3F4` | `JTAG_SCAN_EXT_B` | Extended JTAG/cJTAG scan |
| `0x2F` | `0x6F4E` | `0x0E5EC` | `JTAG_FLUSH` | — |
| `0x32` | `0x6F54` | `0x0E8E0` | `JTAG_SET_PIN_STATE` | `u32 pin_mask`, `u8 val` |
| `0x39` | `0x7086` | `0x0E648` | `JTAG_SET_MODE_EX` | `u8 mode` |
| `0x45` | `0x70C8` | `0x0F108` | `JTAG_SET_FREQ_EX` | `u32 freq_or_div` |

### SWD, ARM DAP (ADIv5 / ADIv6) & Target Memory

| Opcode | Case Addr | Callee(s) | Name | Notes |
|---|---|---|---|---|
| `0x13` | `0x6C1A` | inline/SWD | `SWD_READ_MEM_32` | `u8 ap`, `u8 flags`, `u8 size`, addr/count |
| `0x14` | `0x6C70` | inline/SWD | `SWD_WRITE_MEM_32` | `u8 ap`, `u8 flags`, `u8 size`, addr/data |
| `0x15` | `0x6D6A` | `0x16570` | `SWD_READ_REG` | Read DP/AP register |
| `0x16` | `0x6D86` | `0x165AC` | `SWD_WRITE_REG` | Write DP/AP register |
| `0x17` | `0x6D9C` | `0x15454` | `SWD_CONNECT` | JTAG-to-SWD switch sequence (`0xE79E`) |
| `0x18` | `0x6DA2` | `0x153A8` | `SWD_DISCONNECT` | SWD-to-JTAG / line idle |
| `0x19` | `0x6DA8` | `0x14F8C` | `SWD_CLEAR_ERRORS` | Clear SWD sticky errors |
| `0x1A` | `0x6DAE` | `0x14FA4` | `SWD_DORMANT_WAKE` | ADIv5.2/ADIv6 dormant wake sequence |
| `0x1B` | `0x6DB4` | inline | `SWD_SET_CONFIG` | `u8 p0`, `u8 p1` |
| `0x1C` | `0x6DC2` | inline | `SWD_BATCH_TRANSFER` | Batched DAP read/write |
| `0x33..0x38` | `0x6F5E..0x7064` | `0x178A4..0x180B8` | `ARM_MEM_ACCESS_*` | Block memory read/write & AP banked transfers |
| `0x3A..0x3C` | `0x7094..0x70BC` | `0x16F5C..0x17114` | `DAP_MULTI_DROP_*` | SWD multi-drop target select (`TARGETSEL`) |
| `0x3D..0x3E` | `0x6CC2..0x6D18` | inline | `SWD_MEM_BLOCK_EX` | Extended SWD block transfer |
| `0x3F..0x40` | `0x70D0..0x70F4` | `0x2112A`, `0x2135C` | `I2C_BID_READ/WRITE` | Board-ID I2C register read/write |
| `0x47..0x48` | `0x7130..0x7146` | `0x174E0`, `0x17630` | `DAP_REG_POLL_*` | Wait/poll target memory or register |
| `0x52..0x55` | `0x6FAC..0x6FE0` | `0x17B76`, `0x17BA0` | `DSP_OR_EXT_MEM_*` | Extended memory R/W (`0x52`/`0x54` share handler; `0x53`/`0x55` share handler) |
| `0x5A..0x60` | `0x7236..0x7458` | `0x155D8..0x165AC` | `ADIv6_64BIT_*` | 64-bit address / ADIv6 AP & memory read/write (`error -1002` if mode != 6) |
| `0x61..0x62` | `0x7494..0x749C` | `0x16EE8`, `0x16E7C` | `DAP_EXT_CTRL` | `u8 arg` |

### EnergyTrace & Profiling (`0x1D..0x25`, `0x30`, `0x31`, `0x41..0x44`, `0x46`)

| Opcode | Case Addr | Callee | Name | Request Args (`+4..`) | Response Data (`+7..`) |
|---|---|---|---|---|---|
| `0x1D` | `0x6E00` | `0x1C680` | `ET_SETUP` | `u8 mode` (`0`=Std, `1`=HDR, `2`=Alt), `u8 dig_en` (`0`=analog, `2`=ET++), `u32le sample_freq`, `u8 trigger_mode` | — |
| `0x1E` | `0x6E3A` | `0x1C98C` | `ET_CALIBRATE` | `u16le cal_idx` (`0`=zero, `1`=40.2mA, `2`=90.4mA, `4`=1.5mA) | `i32le adc_sum`, `u32le count` (`5`) |
| `0x1F` | `0x6E4E` | `0x1CA10` | `ET_START` | — | — (begins streaming on Bulk 1 IN `0x87`) |
| `0x20` | `0x6E54` | `0x1CA94` | `ET_STOP` | — | — (`-644` if already cleaned up) |
| `0x21` | `0x6E5A` | `0x1C898` | `ET_CLEANUP` | — | — (clears vtable, bypasses shunt `PM0=0`) |
| `0x22` | `0x6E60` | `0x1CB58` | `ET_DCDC_GET_MCU_VERSION` | — | `u32le version` (I2C1, `hw=0x21` only) |
| `0x23` | `0x6E6E` | `0x1CD6C` | `ET_DCDC_POWER_DOWN_MCU` | — | — (I2C1, `hw=0x21` only) |
| `0x24` | `0x6E74` | `0x1CF14` | `ET_DCDC_SET_VCC` | `u16le mv` | — (I2C1, `hw=0x21` only) |
| `0x25` | `0x6E7C` | `0x1CC04` | `ET_DCDC_RESTART_MCU` | — | — (I2C1, `hw=0x21` only) |
| `0x30` | `0x6E20` | `0x1C858` | `ET_SETUP_RANGE` | `u8 range` (`0`=low/default, `1`=high on `hw=0x21`) | — |
| `0x31` | `0x6E28` | `0x1C864` | `ET_SETUP_DIG` | `u8 dig_type` (`0`, `1`, `3`) | — |
| `0x41` | `0x6E94` | `0x1C580` | `ET_TIMER1_SETUP` | `u8 cfg`, `u32le param` | — |
| `0x42` | `0x6EA8` | `0x1C4C4` | `ET_TIMER1_START` | — | — |
| `0x43` | `0x6EAE` | `0x1C4F4` | `ET_TIMER1_STOP` | — | — |
| `0x44` | `0x6E30` | `0x1C870` | `ET_SETUP_DIG_PARAMS` | `u8 count`, `u8 params[count]` | — |
| `0x46` | `0x6E82` | `0x1C904` | `ET_HARDWARE_INFO` | — | `u32le hw_type` (`2`=HDR), `u8 caps` (`0x07` in Std cfg, `0x02` in cfg 3/4) |

### Target SPI / SWO Streaming (`SSI3` on `PQ0..PQ3`) & Misc

| Opcode | Case Addr | Callee | Name | Request Args | Response Data |
|---|---|---|---|---|---|
| `0x50` | `0x71C0` | `0x18C1C` | `SWO_UART_START` | `u8 cfg` | — |
| `0x51` | `0x71CE` | `0x18C38` | `SWO_UART_STOP` | `u8 cfg` | — |
| `0x56` | `0x71F0` | `0x1858C` | `SSI3_STREAM_START` | `u8 mode`, `u8 div` | — (enables `SSI3` on `PQ0..PQ3`, sets `0x20029128=1`) |
| `0x57` | `0x71FA` | `0x186DC` | `SSI3_STREAM_STOP` | — | — |
| `0x58` | `0x7200` | `0x18B04` | `SSI3_WRITE` | `u8 a0`, `u8 a1`, `u8 data[len-3]` | — |
| `0x59` | `0x721E` | `0x18730` | `SSI3_READ` | `u16le count` | `u8 data[count]` (+3 bytes) |

---

## 3. Common Firmware Error Codes

| Hex | Signed Decimal | Meaning / Observed Location |
|---|---|---|
| `0x00000000` | `0` | Success (`OK`) |
| `0xFFFFFFFF` | `-1` | Generic failure (e.g., external ET pod not detected on `hw=0x21` @ `0x00020142`) |
| `0xFFFFFF86` | `-122` | Invalid parameter (`XDS_SET_TRST` with arg >= 2 @ `0x0000DE72`) |
| `0xFFFFFD80` | `-640` | Unsupported EnergyTrace hardware / invalid sample rate (`0x0001CA78`, `0x0001FE6E`, `0x0002013E`) |
| `0xFFFFFD7C` | `-644` | EnergyTrace not initialized / vtable is NULL (`ET_Start`/`ET_Stop` after `ET_Cleanup` @ `0x0001CAB4`) |
| `0xFFFFFD77` | `-649` | DCDC MCU version mismatch (`0x00020148`: `~0x288`) |
| `0xFFFFFD62` | `-670` | Hardware board does not support EnergyTrace (`ET_HardwareInfo` @ `0x0001C94E`) |
| `0xFFFFFD61` | `-671` | EnergyTrace unavailable in Drag-n-Drop config (`0xAA550002` @ `0x0001C952`) |
| `0xFFFFFC16` | `-1002` | Invalid debug protocol mode for ADIv6 command (`0x00007450`, `0x00007490`) |

---

## 4. Resolution of Open Items from Root `PROTOCOL.md`

1. **Connect-time `0x05 <u32 2>` and `0x09 <u32 0x449, u32 1>`**:
   - `0x05` is `XDS_SET_TRST(u8 state)`: passing `2` is rejected with `-122` (`0xFFFFFF86`) — TI's `jscxds110` probes whether TRST tri-state/mode-2 is supported.
   - `0x09` is `XDS_JTAG_GOTO_STATE(u8 state, u8 path)`: moves the JTAG TAP state machine via the lookup table at `0x0003605C` (note the firmware reads `ldrb` at `+4` and `+8`, so only the low byte of each `u32` is used).
2. **Repeated `Stop (0x20)` / `Cleanup (0x21)` returning `-644`**:
   - Confirmed harmless: `ET_Cleanup` zeros `g_et_ops` (`0x2002C7DC`). Subsequent `ET_Stop`/`ET_Cleanup` check `g_et_ops.stop == NULL` and immediately return `-644` (`0xFFFFFD7C`).
3. **Status byte in 256 kS/s EnergyTrace stream (`0xd0, 0x30, 0x70, ...`)**:
   - Directly clocked out of `ADS127L01` as the 4th byte of each 32-bit SPI conversion frame (see `ENERGYTRACE_DEEP_DIVE.md`). The firmware's own calibration code (`0x00020338`) completely ignores byte 3 and uses only bytes 0..2 as a signed 24-bit big-endian integer!
4. **Bulk 1 OUT (`0x06`) behavior**:
   - Implemented at `0x00004DBC` as a pure byte-reversal loopback echo back onto Bulk 1 IN (`0x87`) for USB throughput/integrity self-testing.
