# EnergyTrace HDR Deep Dive (`LP-XDS110ET`, `hw_version = 0x23`)

Complete hardware and firmware reconstruction of the 256 kS/s High Dynamic Range (HDR) power measurement engine on **LP-XDS110ET**, synthesized from `docs/LP-XDS110ET_SCH.pdf` (Sheets 3–4) and `proprietary_fw/firmware_3.0.0.45.bin` (`0x0001C680 – 0x00020560`).

---

## 1. Analog & Mixed-Signal Hardware Signal Chain

```
ET_VDD (3.30 V, U46 TPS79601DR)
   │
   ├──[ U33 TS3A24159 (PM0 = ET_BYPASS) ]─── Bypass path when PM0 = LOW
   │
   └──[ R107 = 1.0 Ω Precision Shunt ]
         │  (differential voltage across R107 = I_target × 1.0 Ω)
         ├──> U36 INA118UB (R137 = 2.61 kΩ → Gain = 1 + 50k/2.61k = 20.157×)
         │       └──> U37 THS4551IRUN (fully-differential ADC driver, VCM = ~0.44 V via U41 OPA320)
         │               └──> U35 ADS127L01IPBS (24-bit ΔΣ ADC, VREF = 2.500 V via U40 REF6025)
         │                      • CLK = 8.192 MHz (16.384 MHz Y5 ÷ 2 via U34 SN74LVC2G74)
         │                      • Straps: HR=1, OSR1:0=00 (OSR=32), FILTER1:0=10, FSMODE=0, FORMAT=0
         │                      • Output Data Rate: f_DATA = 8.192 MHz / 32 = 256,000 Hz (3.90625 µs/sample)
         │                      • Control: START = PM2, nRESET/nPWDN = PN3
         │                      • SPI: SCLK = PB5 (SSI1CLK @ 15 MHz), nDRDY = PB4, DIN = PE4, DOUT = PE5
         │
         └──[ U38 TS3A24159 (PM1 = ET_CALIBRATE) ]
                 ├── PM1 = LOW  ──> LP_VDD (Target LaunchPad Supply)
                 └── PM1 = HIGH ──> U39 TS3A4751RUCR Calibration Resistor Bank (Target Isolated):
                                      • IN1 (PM4 = ET_CAL0): R123 = 82 Ω   → ~40.24 mA @ 3.3 V
                                      • IN2 (PM5 = ET_CAL1): R125 = 36.5 Ω → ~90.41 mA @ 3.3 V
                                      • IN3 (PC6 = ET_CAL2): R130 = 2.2 kΩ →  ~1.50 mA @ 3.3 V
```

### Theoretical ADC Transfer Function

- Full-scale differential input of `ADS127L01`: $\pm V_{\text{REF}} = \pm 2.500\text{ V}$ across 24-bit two's complement ($\pm 2^{23} = \pm 8{,}388{,}608$ codes) → **$298.023\text{ nV/LSB}$**.
- Across `R107 = 1.0 Ω` and `INA118` gain $G = 20.1571$:
  $$\text{Nominal Sensitivity} = \frac{2.500\text{ V}}{2^{23} \times 20.1571 \times 1.0\ \Omega} \approx \mathbf{14.785\text{ nA/LSB}}$$
- **Cross-check with `PROTOCOL.md` empirical values**: Observed slopes were **`14.6626 nA/LSB`** (low range) and **`15.0262 nA/LSB`** (high range) — matching the schematic calculation within ~1% (exact variation comes from `THS4551` resistor tolerances, `R107`, `R137`, and `U33`/`U38` switch $R_{\text{ON}}$)!

---

## 2. Firmware Polymorphic Vtable (`g_et_ops` @ `0x2002C7DC`)

`ET_Setup` (`CMD 0x1D` @ `0x0001C680`) stores the request struct into `g_et_cfg` (`0x20001850`):
- `+0x00`: `u8 mode` (`0` = Standard DCDC pulse, `1` = **HDR `ADS127L01`**, `2` = Mode 2 external)
- `+0x01`: `u8 dig_en` (`0` = analog only → `0x68` packets, `2` = EnergyTrace++ analog+digital → `0x6A` packets)
- `+0x04`: `u32 sample_freq` (e.g., `48000` Hz for digital Timer3 sampling; echoed in `0x35` stream header)
- `+0x08`: `u8 trigger_mode` (`1` or `4` sets `0x2002C70D=1`; `3` or `4` enables `PF3` GPIO trigger on `hw=0x21`)
- `+0x09`: `u8 range` (set by `CMD 0x30 ET_Setup_Range`)
- `+0x0A`: `u8 dig_type` (set by `CMD 0x31 ET_Setup_Dig`: `0`, `1`, or `3`)

For **Mode 1 (LP-XDS110ET HDR)**, `g_et_ops` (`0x2002C7DC`) is populated as:

| Offset | Function | Address | Purpose |
|---|---|---|---|
| `+0x00` | `setup()` | `0x000200A9` | Resets `ADS127L01` (`PN3`), configures `SSI1` @ 15 MHz & `PB4`, enables shunt (`PM0=1`), flushes Bulk 1 TX |
| `+0x04` | `start()` | `0x0001FD0D` | Sends `0x35` 3-byte sync header, primes ping-pong headers (`0x68`/`0x6A`), starts uDMA Ch5 & Ch24, drives `PM2` (`START=1`) |
| `+0x08` | `stop()` | `0x0001FEBD` | Clears `PM2` (`START=0`), disables `GPIOB`/`SSI1`/`Timer3` interrupts and uDMA channels |
| `+0x0C` | `cleanup()` | `0x0001FFB1` | Stops DMA, resets `PM0=0` (bypasses 1 Ω shunt so target gets low-impedance power!) |
| `+0x14` | `dig_tick()` | `0x00020E05` / `0x00020F25` | `TIMER3` ISR for EnergyTrace++ digital state capture (`dig_type` 1 or 3) |
| `+0x18` | `calibrate()` | `0x00020175` | Returns boot-cached `(adc_sum, count=5)` from `0x2002C878[slot]` |

### `ET_HardwareInfo` (`CMD 0x46` @ `0x0001C904`)

- Returns `(u32le hw_type, u8 capabilities)`:
  - For `hw_version == 0x23` (LP-XDS110ET): `hw_type = 2` (HDR), `capabilities = 0x07` (in Standard config `0xAA550001`) or `0x02` (in configs `0xAA550003`/`0xAA550004`).
  - Returns error `-671` (`0xFFFFFD61`) if probe is in Drag-n-Drop config (`0xAA550002`).

---

## 3. Zero-CPU-Overhead Hardware DMA Pipeline (256 kS/s)

Instead of taking a CPU interrupt every 3.9 µs (256,000 IRQs/sec), TI chains **two hardware uDMA channels** so the CPU only takes **1 interrupt per millisecond (1 kHz)**:

### Step A — `SSI1` Configuration (`0x00020020`)
- Peripheral: `SYSCTL_PERIPH_SSI1` (`0x40009000`)
- Pins: `PB5` (`SSI1CLK`), `PE4` (`SSI1XDAT0` = MOSI), `PE5` (`SSI1XDAT1` = MISO)
- Mode: `SSIConfigSetExpClk(SSI1_BASE, 120000000, SSI_FRF_MOTO_MODE_2, SSI_MODE_MASTER, 15000000, 8)`
  - Note: `0x00FA0000` = `16,384,000` requested → integer divisor 8 from 120 MHz = **15.0 MHz SCLK** (`CPOL=1, CPHA=0`, 8-bit frames).
- `PB4` (`ADS127L01 nDRDY`): Configured as **GPIO input** (`GPIO_FALLING_EDGE` = `0`, 8 mA / pull-down/up `0x66, 0x0C`) with **`GPIODMATriggerEnable(GPIO_PORTB_AHB, GPIO_PIN_4)`** (`0x0001FA60`).

### Step B — TX Trigger Channel: uDMA Channel 5 (`UDMA_CH5_GPIOB`, `0x0001FA08`)
- Triggered automatically by hardware on **every falling edge of `PB4` (`nDRDY` @ 256 kHz)**!
- Config: `UDMA_SIZE_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_NONE | UDMA_ARB_4` (`0xCC008000`)
- Ping-Pong (`0x00030005` Primary + `0x00030025` Alternate), **1,024 bytes** per half (`256 samples × 4 bytes/sample`).
- Source: dummy byte at `0x2002C876` / `0x2002C877`; Destination: `SSI1_DR` (`0x40009008`).
- Each `nDRDY` edge autonomously clocks out **4 bytes (32 SCLK cycles @ 15 MHz = 2.13 µs < 3.91 µs sample period)** from `ADS127L01`!
- `GPIOB` ISR (`0x0001FB60`) fires only once every 1,024 bytes (every 1 ms) to re-arm the finished ping or pong descriptor.

### Step C — RX Capture Channel: uDMA Channel 24 (`UDMA_CH24_SSI1RX`, `0x0001F990`)
- Config: `UDMA_SIZE_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_8 | UDMA_ARB_4` (`0x0C008000`)
- Ping-Pong (`0x18` Primary + `0x38` Alternate), **1,024 bytes** per half.
- Source: `SSI1_DR` (`0x40009008`).
- Destination: Directly into the payload portion of **two pre-framed 1,030-byte (`0x406`) packet buffers** starting at `0x2002C899`:
  - **Ping packet**: header `0x2002C899..0x2002C89E` (6 B) + DMA payload `0x2002C89F..0x2002CC9E` (1,024 B)
  - **Pong packet**: header `0x2002CC9F..0x2002CCA4` (6 B) + DMA payload `0x2002CCA5..0x2002D0A4` (1,024 B)
- `SSI1` ISR (`0x0001FBC4`) fires once every 1 ms when a 1,024-byte half completes:
  1. Calls `USBBufferWrite(&Bulk1_TX_Buf, packet_ptr, 0x406)` (copies 1,030 bytes into the 96 KiB USB ring buffer at `0x2000764C`).
  2. If `dig_en != 0` (`0x6A` mode), also writes `dig_len` into header bytes `+4..+5` and appends the digital buffer (`0x2002D0C4`).
  3. Increments the 8-bit sequence counter (`0x2002D0A5`), writes it to the buffer header `+1`, and re-arms the uDMA RX ping/pong descriptor!

---

## 4. USB Stream Packet Formats (Bulk 1 IN `0x87`)

1. **Stream Start Sync Header** (sent once at `ET_Start`, `0x0001FD16`, **3 bytes**):
   - `[0x35] [sample_freq & 0xFF] [(sample_freq >> 8) & 0xFF]`
   - Example (`48000 = 0xBB80`): `35 80 BB`.
2. **Analog Data Packet** (`dig_en == 0`, **1,030 bytes = `0x406`**, 1 packet/ms):
   - Byte `0`: `0x68` (packet type: HDR Analog)
   - Byte `1`: `u8 seq` (wrapping 0..255 sequence number)
   - Bytes `2..3`: `u16le 0x0400` (1,024 payload bytes = 256 samples)
   - Bytes `4..5`: `u16le 0x0000` (0 digital bytes)
   - Bytes `6..1029`: `256 × 4-byte` raw `ADS127L01` SPI frames:
     - Bytes `0..2`: **Signed 24-bit big-endian two's-complement ADC code** (`val = (b0<<16)|(b1<<8)|b2; if val >= 0x800000: val -= 0x1000000`).
     - Byte `3`: Raw 4th byte clocked from `ADS127L01 DOUT` (8-bit ADC STATUS/flags byte; low nibble is always `0x0` in hardware mode; ignored by firmware calibration and optional for current calculation).
3. **Analog + Digital Packet** (`dig_en == 2`, EnergyTrace++, **`1030 + N` bytes**):
   - Byte `0`: `0x6A`
   - Byte `1`: `u8 seq`
   - Bytes `2..3`: `u16le 0x0400`
   - Bytes `4..5`: `u16le N` (number of appended digital trace bytes)
   - Bytes `6..1029`: 256 × 4-byte ADC samples
   - Bytes `1030..1030+N-1`: Digital trace payload (`0x2002D0C4`).
4. **Trigger-Wait Keep-Alive Packet** (only when GPIO trigger mode is armed on `hw=0x21`, **2 bytes**):
   - `[0xB8] [u8 seq]`.

---

## 5. Boot-Time Calibration Algorithm (`0x00020420` & `0x000201F0`)

On **LP-XDS110ET (`hw=0x23`)**, calibration runs **once at boot** (`0x00005A78 – 0x00005AC0`) before USB enumeration:

1. Asserts target `nSRST` (`PA6 = 0`) so target cannot draw transient current.
2. Drives `PM1` (`ET_CALIBRATE = 1`) to disconnect `LP_VDD` and connect the `U39` calibration resistor bank; waits 300 ms.
3. Drives `PM0` (`ET_BYPASS = 1`) to route current through the 1 Ω shunt.
4. Resets `ADS127L01` (`PN3` LOW→HIGH) and configures `SSI1`.
5. Calls `ET_HDR_MeasureCalPoint(idx, &cache[slot], count=5)` (`0x000201F0`) for four points:

| `cal_idx` | Cache Slot (`0x2002C878`) | GPIO Driven (`0x00020266`) | Resistor (`U39`) | Nominal Current (@ 3.30 V) |
|---|---|---|---|---|
| `0` | `[0]` | `PM4=0, PM5=0, PC6=0` | Open circuit | `0.00 mA` (Zero offset) |
| `1` | `[1]` | `PM4=1` (`ET_CAL0`) | `R123 = 82 Ω` | `40.24 mA` |
| `2` | `[2]` | `PM5=1` (`ET_CAL1`) | `R125 = 36.5 Ω` | `90.41 mA` |
| `4` | `[3]` | `PC6=1` (`ET_CAL2`) | `R130 = 2.2 kΩ` | `1.50 mA` |

6. Inside each `ET_HDR_MeasureCalPoint` (`count = 5` iterations):
   - Waits settling delay (`DelayCycles`), arms uDMA Ch5 & Ch24 for 2×1024-byte buffers (512 samples total), asserts `PM2` (`START=1`), waits until both buffers finish (`0x2002C898 == 1`), clears `PM2` (`START=0`).
   - **Discards the first 256 samples** (ADC digital filter settling) and averages the **second 256 samples** (signed 24-bit big-endian sum ÷ 256, rounding toward zero at `0x0002035E`).
   - Accumulates all 5 averages into `cache[slot]`.
7. Clears `PM0 = 0` (shunt bypassed), clears `PM1 = 0` (reconnects `LP_VDD`), waits 300 ms, deasserts `nSRST` (`PA6 = 1`).
8. During runtime `ET_Calibrate(idx)` (`CMD 0x1E`), the firmware simply returns `(cache[slot], 5)` — zero target interruption!
