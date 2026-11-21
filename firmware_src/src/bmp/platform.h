/*
 * platform.h - Black Magic Debug platform definitions for TI LP-XDS110ET.
 *
 * Included by upstream Black Magic Debug sources via -Isrc/bmp when
 * BMP_ENABLE=1. Maps BMP debug LEDs, timing, and SWD/JTAG hooks onto the
 * LP-XDS110ET BSP without requiring libopencm3.
 */

#ifndef BMP_PLATFORM_H
#define BMP_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "timing.h"

#define NO_LIBOPENCM3     1
#define PLATFORM_IDENT    "(LP-XDS110ET) "
#define RTT_UP_BUF_SIZE   (2048U + 8U)
#define RTT_DOWN_BUF_SIZE 256U

/* Forward declarations avoid pulling msp432e401y.h (SRAM_BASE) into upstream BMP targets */
void board_led_red_set(bool on);
void app_background_poll(void);
void bmp_tap_clk_output_enable(bool enable);

#define SET_RUN_STATE(state) board_led_red_set((state) != 0)
#define SET_IDLE_STATE(state) \
    do {                      \
        (void)(state);        \
    } while (0)
#define SET_ERROR_STATE(state) board_led_red_set((state) != 0)

#endif /* BMP_PLATFORM_H */
