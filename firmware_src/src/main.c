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

    /* Indicate healthy LP-XDS110ET hardware detection via non-blocking LED heartbeat */
    bool board_ok = (hw_version == BOARD_HW_VERSION_LP_XDS110ET) && ulpi_present;
    bool led_on = true;
    uint32_t last_toggle_ms = board_get_uptime_ms();

    if (board_ok) {
        board_led_green_set(true);
    } else {
        board_led_red_set(true);
    }

    while (1) {
        uint32_t now_ms = board_get_uptime_ms();
        uint32_t interval_ms = board_ok ? (led_on ? 900U : 100U) : 150U;

        if ((uint32_t)(now_ms - last_toggle_ms) >= interval_ms) {
            last_toggle_ms = now_ms;
            led_on = !led_on;
            if (board_ok) {
                board_led_green_set(led_on);
            } else {
                board_led_red_set(led_on);
            }
        }

        __WFI();
    }

    return 0;
}
