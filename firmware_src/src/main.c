/*
 * main.c - Open-source bare-metal firmware entry point for TI LP-XDS110ET.
 *
 * Implemented subsystems (MCU: MSP432E401YTPDT @ 120 MHz, hw_version = 0x23):
 *   - LP-XDS110ET BSP & 1 kHz SysTick uptime timebase
 *   - Boot-time double-reset DFU failsafe
 *   - Boot-time EnergyTrace HDR calibration (ADS127L01 + SSI1 + uDMA Ch5/Ch24)
 *   - USB3300-EZK ULPI USB 2.1 HS composite stack (0451:BEF3 + BOS / MS OS 2.0)
 *   - XDS110 0x2A command channel (Interface 2): control, EnergyTrace, Aux GPIO,
 *     OpenOCD native SWD (OCD_DAP_REQUEST 0x3A), and TI UniFlash DSLite (CMAPI + SACI)
 *   - Dual CDC ACM Virtual COM Ports (UART0 App UART + UART7 Aux/SWO RX)
 *   - Target Aux GPIOs (PF0..PF2 + PJ0..PJ1 level-shifter dir, opcodes 0x29/0x2A)
 *   - CMSIS-DAP v2.0.0 WinUSB Bulk + fast SWD wire engine (Interface 5, 0x05/0x86)
 */

#include <stdbool.h>
#include <stdint.h>

#include "board_lp_xds110et.h"
#include "dap/cmsis_dap.h"
#include "dfu_jump.h"
#include "energytrace/et_hdr.h"
#include "gpio/aux_gpio.h"
#include "protocol/xds_cmd.h"
#include "uart/cdc_uart.h"
#include "usb/usb_device.h"

/* Firmware version components (BCD: 3.0.0.45 -> 0x03000045) */
#define FW_VERSION_MAJOR 0x03U
#define FW_VERSION_MINOR 0x00U
#define FW_VERSION_PATCH 0x00U
#define FW_VERSION_BUILD 0x45U

/*
 * Firmware version marker matching XDS_VERSION (CMD 0x03) wire format:
 * [build, patch, minor, major]
 */
__attribute__((used, section(".rodata.version"))) const uint8_t g_firmware_version[4] = {
    FW_VERSION_BUILD,
    FW_VERSION_PATCH,
    FW_VERSION_MINOR,
    FW_VERSION_MAJOR,
};

int main(void)
{
    board_init();
    dfu_failsafe_check();

    uint16_t hw_version = board_get_hw_version();
    bool ulpi_present = board_has_ulpi_phy();
    bool board_ok = (hw_version == BOARD_HW_VERSION_LP_XDS110ET) && ulpi_present;

    if (board_ok) {
        et_hdr_init();
        et_hdr_boot_calibrate();
    }

    xds_cmd_init(g_firmware_version);
    cmsis_dap_init(g_firmware_version);
    aux_gpio_init();
    cdc_uart_init();
    usb_device_init(g_firmware_version);

    bool led_on = false;
    uint32_t last_toggle_ms = board_get_uptime_ms();

    while (1) {
        bool did_work = xds_cmd_poll();
        if (cmsis_dap_poll()) {
            did_work = true;
        }
        if (cdc_uart_poll()) {
            did_work = true;
        }
        uint32_t now_ms = board_get_uptime_ms();

        if (!board_ok) {
            /* Fast Red fault blink if board ID or ULPI PHY was not detected */
            if ((uint32_t)(now_ms - last_toggle_ms) >= 150U) {
                last_toggle_ms = now_ms;
                led_on = !led_on;
                board_led_red_set(led_on);
            }
        } else if (usb_device_is_configured()) {
            /* Solid Green when USB host has enumerated and configured the probe */
            board_led_green_set(true);
        } else {
            /* Slow Green blink while awaiting USB host enumeration */
            if ((uint32_t)(now_ms - last_toggle_ms) >= 500U) {
                last_toggle_ms = now_ms;
                led_on = !led_on;
                board_led_green_set(led_on);
            }
        }

        if (!did_work) {
            __WFI();
        }
    }

    return 0;
}
