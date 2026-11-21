/*
 * dfu_jump.c - Safe re-entry into TI's preserved 16 KiB USB DFU bootloader.
 *
 * Matches the verified teardown sequence from firmware_3.0.0.45.bin
 * (0x00025F58) using MSP432E4 DriverLib and CMSIS NVIC calls.
 */

#include "dfu_jump.h"

#include "board_lp_xds110et.h"
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/systick.h"
#include "driverlib/usb.h"

/*
 * Bootloader SVCall vector slot (index 11 = byte offset 0x2C in the preserved
 * bootloader at flash address 0x00000000, provided by msp432e401y.ld).
 */
#define DFU_USB_INTERNAL_CLK_DIV   8U /* 480 MHz PLL VCO / 8 = 60 MHz */
#define DFU_MAX_EXTERNAL_IRQS      111U
#define DFU_DOUBLE_RESET_MAGIC     0x44465521U /* "DFU!" */
#define DFU_DOUBLE_RESET_WINDOW_MS 400U

extern const uint32_t __dfu_bootloader_entry_vec;

__attribute__((section(".noinit"))) static volatile uint32_t g_dfu_reset_cookie;

void dfu_failsafe_check(void)
{
    if (g_dfu_reset_cookie == DFU_DOUBLE_RESET_MAGIC) {
        g_dfu_reset_cookie = 0U;
        dfu_jump_enter();
    }

    /* Arm the double-reset window briefly while indicating boot on both LEDs */
    g_dfu_reset_cookie = DFU_DOUBLE_RESET_MAGIC;
    board_led_set(true, true);
    board_delay_ms(DFU_DOUBLE_RESET_WINDOW_MS);
    g_dfu_reset_cookie = 0U;
    board_led_set(false, false);
}

void dfu_jump_enter(void)
{
    board_led_set(false, false);

    MAP_IntMasterDisable();
    MAP_SysTickIntDisable();
    MAP_SysTickDisable();

    /* Disable and clear pending state on all external NVIC interrupts via CMSIS */
    for (uint32_t irq = 0U; irq < DFU_MAX_EXTERNAL_IRQS; irq++) {
        NVIC_DisableIRQ((IRQn_Type)irq);
        NVIC_ClearPendingIRQ((IRQn_Type)irq);
    }

    /* Reset USB0 and restore internal 60 MHz clock expected by the DFU bootloader */
    MAP_SysCtlPeripheralReset(SYSCTL_PERIPH_USB0);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_USB0);
    while (!MAP_SysCtlPeripheralReady(SYSCTL_PERIPH_USB0)) {
    }
    MAP_USBClockEnable(USB0_BASE, DFU_USB_INTERNAL_CLK_DIV, USB_CLOCK_INTERNAL);

    /* Hold detached for ~1 second so the USB host detects disconnect cleanly */
    board_delay_ms(1000U);

    MAP_IntMasterEnable();

    /* Branch to TI Flash DFU bootloader re-entry vector at 0x0000002C */
    void (*bootloader_enter)(void) = (void (*)(void))__dfu_bootloader_entry_vec;
    bootloader_enter();

    while (1) {
    }
}
