/*
 * bmp_gdb_if.c - Black Magic Debug GDB RSP transport over USB CDC1 (ttyACM1).
 *
 * Bridges gdb_if.h directly to usb_cdc_* (USB_CDC_PORT_AUX) while pumping
 * app_background_poll() in every wait loop so EnergyTrace HDR (256 kS/s),
 * CDC0 App UART (ttyACM0), and XDS110 control/mode-switch (0x63) never block.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bmp_mode.h"
#include "board_lp_xds110et.h"
#include "gdb_if.h"
#include "general.h"
#include "platform.h"
#include "usb/usb_device.h"

#define BMP_GDB_TX_BUF_SIZE 64U

static uint8_t g_gdb_tx_buf[BMP_GDB_TX_BUF_SIZE];
static uint32_t g_gdb_tx_count = 0U;

static inline bool bmp_gdb_link_open(void)
{
    return bmp_mode_is_active() && usb_device_is_configured() && usb_cdc_is_dtr_asserted(USB_CDC_PORT_AUX);
}

int gdb_if_init(void)
{
    g_gdb_tx_count = 0U;
    return 0;
}

void gdb_if_putchar(char c, bool flush)
{
    g_gdb_tx_buf[g_gdb_tx_count++] = (uint8_t)c;
    if (flush || (g_gdb_tx_count >= BMP_GDB_TX_BUF_SIZE)) {
        gdb_if_flush(flush);
    }
}

void gdb_if_flush(bool force)
{
    (void)force;
    if (g_gdb_tx_count == 0U) {
        return;
    }

    uint32_t offset = 0U;
    while (offset < g_gdb_tx_count) {
        if (!bmp_gdb_link_open()) {
            g_gdb_tx_count = 0U;
            return;
        }
        uint32_t space = usb_cdc_tx_space(USB_CDC_PORT_AUX);
        if (space == 0U) {
            app_background_poll();
            continue;
        }
        uint32_t rem = g_gdb_tx_count - offset;
        uint32_t chunk = (rem < space) ? rem : space;
        offset += usb_cdc_tx_write(USB_CDC_PORT_AUX, &g_gdb_tx_buf[offset], chunk);
    }

    g_gdb_tx_count = 0U;
}

char gdb_if_getchar(void)
{
    while (usb_cdc_rx_available(USB_CDC_PORT_AUX) == 0U) {
        if (!bmp_gdb_link_open()) {
            return '\x04';
        }
        app_background_poll();
        if (usb_cdc_rx_available(USB_CDC_PORT_AUX) == 0U) {
            __WFI();
        }
    }

    uint8_t ch = 0U;
    if (usb_cdc_rx_read(USB_CDC_PORT_AUX, &ch, 1U) == 0U) {
        return '\x04';
    }
    return (char)ch;
}

char gdb_if_getchar_to(uint32_t timeout)
{
    if (timeout == 0U) {
        app_background_poll();
        if (!bmp_gdb_link_open()) {
            return '\x04';
        }
        uint8_t ch = 0U;
        if (usb_cdc_rx_read(USB_CDC_PORT_AUX, &ch, 1U) > 0U) {
            return (char)ch;
        }
        return (char)-1;
    }

    platform_timeout_s rx_timeout;
    platform_timeout_set(&rx_timeout, timeout);

    while (usb_cdc_rx_available(USB_CDC_PORT_AUX) == 0U) {
        if (!bmp_gdb_link_open()) {
            return '\x04';
        }
        if (platform_timeout_is_expired(&rx_timeout)) {
            return (char)-1;
        }
        app_background_poll();
    }

    uint8_t ch = 0U;
    if (usb_cdc_rx_read(USB_CDC_PORT_AUX, &ch, 1U) == 0U) {
        return '\x04';
    }
    return (char)ch;
}
