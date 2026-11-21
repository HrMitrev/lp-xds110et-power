/*
 * main.c - Open-source bare-metal firmware entry point for TI LP-XDS110ET.
 *
 * Phase 0 (BSP & Failsafe bringup):
 *   - Fixed MSP432E401Y vector table (CAN1 @ IRQ 39, USB0 @ IRQ 42)
 *   - LP-XDS110ET BSP (120 MHz clock, HW ID, USB3300 ULPI detect, LEDs PD2/PD3)
 *   - Boot-time double-reset DFU failsafe (jumps to preserved 0x0000002C vector)
 */

#include <stdbool.h>
#include <stdint.h>

#include "board_lp_xds110et.h"
#include "dfu_jump.h"

/*
 * Firmware version marker matching XDS_VERSION (CMD 0x03) wire format:
 * [build=0x01, patch=0x00, minor=0x00, major=0x03] = 3.0.0.01
 */
__attribute__((used, section(".rodata.version"))) const uint8_t g_firmware_version[4] = {0x01U, 0x00U, 0x00U, 0x03U};

int main(void)
{
    char serial_number[BOARD_SERIAL_LEN + 1U];

    board_init();
    dfu_failsafe_check();

    board_read_serial(serial_number);
    (void)serial_number;

    uint16_t hw_version = board_get_hw_version();
    bool ulpi_present = board_has_ulpi_phy();

    /* Indicate healthy LP-XDS110ET hardware detection via Green/Red LED pattern */
    bool board_ok = (hw_version == BOARD_HW_VERSION_LP_XDS110ET) && ulpi_present;

    while (1) {
        if (board_ok) {
            /* Steady Green ready heartbeat */
            board_led_set(true, false);
            board_delay_ms(900U);
            board_led_set(false, false);
            board_delay_ms(100U);
        } else {
            /* Fast Red fault blink if board ID or ULPI PHY was not detected */
            board_led_set(false, true);
            board_delay_ms(150U);
            board_led_set(false, false);
            board_delay_ms(150U);
        }
    }

    return 0;
}
