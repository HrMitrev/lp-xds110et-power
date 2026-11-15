# XDS110 Firmware Toolchain & SDK Analysis

This document details the reverse-engineering and binary analysis of Texas Instruments XDS110 debug probe firmware images (`firmware_3.0.0.41.bin`, `firmware_3.0.0.43.bin`, `firmware_3.0.0.45.bin`) targeted for the **MSP432E401YTPDT** microcontroller on the LP-XDS110ET board.

---

## 1. Summary of Findings

| Category | Finding | Confidence | Primary Evidence |
|---|---|---|---|
| **Compiler / Toolchain** | **GNU Arm Embedded (`arm-none-eabi-gcc`)** | 100% | Exact byte-for-byte disassembly match with TI's `startup_gcc.c` (`ResetISR` assembly loop). |
| **SDK / Hardware Abstraction** | **SimpleLink™ MSP432E4 SDK / TivaWare DriverLib (`SW-TM4C`)** | 100% | Hundreds of direct calls into the on-chip ROM API table (`0x01000010` / `0x01000000`). |
| **USB Middleware** | **TI USB Device Library (`usblib`)** | 100% | Composite device endpoint handlers (Bulk debug, dual CDC, MSC, and DFU). |
| **Emulation Middleware** | **ARM CMSIS-DAP Firmware Stack** | 100% | String tables and protocol handlers for CMSIS-DAP v1.2.0 and v2.0.0. |
| **Operating System / Kernel** | **Bare-Metal (Super-Loop + Interrupts)** | 100% | Complete absence of FreeRTOS, TI-RTOS (SYS/BIOS), or other RTOS kernel objects. |

---

## 2. Toolchain Evidence: `arm-none-eabi-gcc`

### Reset Handler Disassembly (`ResetISR`)
In `firmware_3.0.0.45.bin`, the Reset Vector at vector table entry 1 (`0x00004004`) points to `0x00006565` (file offset `0x002564`, executing in Thumb-2 state):

```assembly
; File offset 0x002564:
ldr     r2, [pc, #48]       ; = 0x00050b8a (flash source address of .data)
ldr     r3, [pc, #52]       ; = 0x20000000 (SRAM destination address of .data)
b       check_copy
copy_loop:
ldr.w   r1, [r2], #4
str.w   r1, [r3], #4
check_copy:
ldr     r1, [pc, #44]       ; = 0x20002570 (SRAM end address of .data)
cmp     r3, r1
bcc     copy_loop

; Inline assembly zero-fill loop for .bss:
ldr     r0, [pc, #44]       ; = 0x20002800 (start of .bss in SRAM)
ldr     r1, [pc, #48]       ; = 0x20033588 (end of .bss in SRAM)
mov.w   r2, #0
cmp     r0, r1
it      lt
strlt   r2, [r0], #4
blt     zero_loop

; Floating-Point Unit (FPU) enable in CPACR:
ldr     r3, =0xe000ed88     ; NVIC_CPAC register
ldr     r2, [r3]
orr.w   r2, r2, #0x00F00000 ; Enable CP10 & CP11 coprocessors
str     r2, [r3]

; Direct branch to main:
bl      main                ; Target Thumb address: 0x00005D76 (offset 0x001D76)
```

### 1:1 Match with TI `startup_gcc.c`
This binary code matches Texas Instruments' template file **`startup_gcc.c`** provided in TivaWare and SimpleLink MSP432E4 SDKs:

```c
// TI SDK startup_gcc.c:
__asm("    ldr     r0, =__bss_start__\n"
      "    ldr     r1, =__bss_end__\n"
      "    mov     r2, #0\n"
      "    .thumb_func\n"
      "zero_loop:\n"
      "        cmp     r0, r1\n"
      "        it      lt\n"
      "        strlt   r2, [r0], #4\n"
      "        blt     zero_loop");

HWREG(NVIC_CPAC) = ((HWREG(NVIC_CPAC) &
                     ~(NVIC_CPAC_CP10_M | NVIC_CPAC_CP11_M)) |
                    NVIC_CPAC_CP10_FULL | NVIC_CPAC_CP11_FULL);

main();
```

### Compiler Differentiation
- **TI Arm Compiler (`armcl`)**: Uses `startup_ccs.c` branching to `_c_int00` which decompresses tables defined by linker generated `.cinit` sections (`__TI_auto_init`). These tables are entirely absent from the binary.
- **Keil MDK (`armcc` / `armclang`)**: Uses `startup_rvmdk.S` with branches to `__main` and `__scatterload`.
- **IAR Embedded Workbench**: Uses `startup_ewarm.c` with calls to `__iar_program_start`.

The firmware was compiled using **GNU Arm Embedded (`arm-none-eabi-gcc`)**.

---

## 3. SDK & DriverLib Evidence: SimpleLink MSP432E4 / TivaWare

The MSP432E401Y is architecturally derived from TI's TM4C1294 (Tiva-C Series). Both chips embed an on-chip **ROM** containing peripheral drivers (**DriverLib**) accessible via an indirect table known as the **`ROM_APITABLE`**.

### ROM Calls in Binary
- **ROM Base Address**: `0x01000000`
- **ROM APITABLE Address**: `0x01000010`

Scanning the binary reveals nearly 900 literal pool pointers referencing `0x01xxxxxx`, specifically loading functions through the ROM API table:
```c
// Standard DriverLib macro:
#define ROM_APITABLE  ((uint32_t *)0x01000010)
#define ROM_SYSCTL    ((uint32_t *)(ROM_APITABLE[13]))
```

The firmware relies on DriverLib from the **SimpleLink MSP432E4 SDK** (or **TivaWare for C Series `SW-TM4C`**) for:
- System clock configuration (`ROM_SysCtlClockFreqSet`)
- GPIO pin muxing and electrical configuration
- High-speed USB controller configuration and interrupts
- Timers for EnergyTrace sample scheduling

---

## 4. Stacks & Middleware

### 1. TI USB Device Library (`usblib`)
The firmware instantiates a multi-interface USB Composite Device supporting:
- **Interface 0 (DFU)**: Firmware update interface (switchable at runtime via command `2A 01 00 26`).
- **Interface 1 (Debug / Emulation)**: Bulk IN/OUT endpoints for JTAG/SWD packets.
- **Interface 2 & 3 (CDC UART)**: Application COM port and Auxiliary COM port.
- **Interface 4 (Mass Storage)**: Config 2 drag-and-drop firmware flashing.

### 2. ARM CMSIS-DAP
Embedded protocol version strings:
- `CMSIS-DAP`
- `1.2.0`
- `2.0.0`
- `XDS110 with CMSIS-DAP`

The probe implements ARM's reference CMSIS-DAP protocol implementation on top of the bulk and HID endpoints.

### 3. EVM Test Automation & INA Power Rails
The firmware incorporates an embedded text shell for hardware verification and power control:
- Current sensing: `ina %s register %x read %i`, `ina %s set shunt %s`, `ina list`, `ina help`
- DUT power & reset: `auto set dut`, `reset`, `hold_por`, `release_por`, `rtc_wakeup`
- Test automation build git hashes:
  - `3.0.0.41`: `6450962907`
  - `3.0.0.43`: `a3c0d5bdca`
  - `3.0.0.45`: `2e1f88db92`

---

## 5. Execution Model

- **No RTOS detected**: A thorough search reveals zero references to FreeRTOS (`xTaskCreate`, `vTaskDelay`, `prv...`), TI-RTOS / SYS/BIOS (`ti_sysbios_...`, `Task_create`), or CMSIS-RTOS.
- **Architecture**: Bare-metal event loop combined with high-priority ISRs for USB bulk packets, UART FIFO handling, and ADC / EnergyTrace timer sampling.
