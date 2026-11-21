/*
 * dfu_jump.h - Safe re-entry into the preserved TI USB DFU bootloader.
 *
 * Provides both explicit command-triggered DFU entry (CMD 0x26) and a
 * boot-time double-reset failsafe so open-source firmware can always return
 * to USB DFU mode without external JTAG recovery.
 */

#ifndef DFU_JUMP_H
#define DFU_JUMP_H

#include <stdint.h>

/*
 * Check the .noinit double-reset cookie at boot.
 * If reset occurs twice within the boot window (~400 ms), immediately
 * transitions into the preserved TI USB DFU bootloader at 0x00000000.
 */
void dfu_failsafe_check(void);

/*
 * Cleanly disables interrupts/SysTick/USB, configures USB0 clock for the
 * bootloader, waits for host detach recognition, and jumps to the bootloader's
 * SVCall re-entry vector (0x0000002C). Does not return.
 */
void dfu_jump_enter(void) __attribute__((noreturn));

#endif /* DFU_JUMP_H */
