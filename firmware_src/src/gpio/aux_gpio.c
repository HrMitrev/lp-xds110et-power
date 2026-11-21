/*
 * aux_gpio.c - LP-XDS110ET Target Auxiliary GPIO & Level-Shifter driver.
 *
 * Implements XDS110 opcodes 0x29 (XDS_GPIO_GET) and 0x2A (XDS_GPIO_SET)
 * using MSP432E4 DriverLib (MAP_GPIO*) exclusively.
 *
 * Level-shifter sequencing (U49 SN74AVC4T245RSV, VCCA=XDS_VDD, VCCB=LS_VDD):
 *   - Output -> Input: switch MCU pin PFx to input FIRST, then set PJx LOW (B->A)
 *     so the MCU and U49 never drive the same trace simultaneously.
 *   - Input -> Output: set PJx HIGH (A->B), then configure PFx as output with
 *     its cached logic level pre-loaded.
 */

#include "aux_gpio.h"

#include <stdbool.h>

#include "board_lp_xds110et.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "usb/usb_device.h"

#define AUX_GPIO_VALID_MASK     BOARD_AUX_GPIO_ALL_PINS /* 0x07: PF0 | PF1 | PF2 */
#define AUX_GPIO_BLINK_CYCLES   8U
#define AUX_GPIO_BLINK_DELAY_MS 133U

static uint8_t g_aux_output_mask = 0U;
static uint8_t g_aux_output_val = 0U;

static void aux_gpio_set_dir_pin(uint8_t gpio_bit, bool is_output)
{
    uint8_t dir_pin = 0U;
    if (gpio_bit == BOARD_AUX_GPIO1_PIN) {
        dir_pin = BOARD_AUX_DIR_GPIO1_PIN;
    } else if (gpio_bit == BOARD_AUX_GPIO2_PIN) {
        dir_pin = BOARD_AUX_DIR_GPIO2_PIN;
    }

    if (is_output) {
        /* Switch U49 DIR HIGH (A->B) before enabling MCU output, pre-loading latch */
        if (dir_pin != 0U) {
            MAP_GPIOPinWrite(BOARD_AUX_DIR_PORT_BASE, dir_pin, dir_pin);
        }
        MAP_GPIOPinWrite(BOARD_AUX_GPIO_PORT_BASE, gpio_bit, g_aux_output_val & gpio_bit);
        MAP_GPIOPinTypeGPIOOutput(BOARD_AUX_GPIO_PORT_BASE, gpio_bit);
        MAP_GPIOPadConfigSet(BOARD_AUX_GPIO_PORT_BASE, gpio_bit, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
    } else {
        /* Release MCU pin to input BEFORE switching U49 DIR LOW (B->A) */
        MAP_GPIOPinTypeGPIOInput(BOARD_AUX_GPIO_PORT_BASE, gpio_bit);
        MAP_GPIOPadConfigSet(BOARD_AUX_GPIO_PORT_BASE, gpio_bit, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);
        if (dir_pin != 0U) {
            MAP_GPIOPinWrite(BOARD_AUX_DIR_PORT_BASE, dir_pin, 0U);
        }
    }
}

static void aux_gpio_blink_identify(void)
{
    for (uint32_t i = 0U; i < AUX_GPIO_BLINK_CYCLES; i++) {
        board_led_red_set(true);
        board_led_green_set(false);
        board_delay_ms(AUX_GPIO_BLINK_DELAY_MS);
        board_led_red_set(false);
        board_led_green_set(true);
        board_delay_ms(AUX_GPIO_BLINK_DELAY_MS);
    }
    board_led_red_set(false);
    board_led_green_set(usb_device_is_configured());
}

void aux_gpio_init(void)
{
    g_aux_output_mask = 0U;
    g_aux_output_val = 0U;

    /* Put PF0..PF2 into high-Z input mode first */
    MAP_GPIOPinTypeGPIOInput(BOARD_AUX_GPIO_PORT_BASE, BOARD_AUX_GPIO_ALL_PINS);
    MAP_GPIOPadConfigSet(BOARD_AUX_GPIO_PORT_BASE, BOARD_AUX_GPIO_ALL_PINS, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);

    /* Configure PJ0/PJ1 (XDS_DIR_GPIO1/2) as outputs driven LOW (B->A input) */
    MAP_GPIOPinTypeGPIOOutput(BOARD_AUX_DIR_PORT_BASE, BOARD_AUX_DIR_ALL_PINS);
    MAP_GPIOPadConfigSet(BOARD_AUX_DIR_PORT_BASE, BOARD_AUX_DIR_ALL_PINS, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
    MAP_GPIOPinWrite(BOARD_AUX_DIR_PORT_BASE, BOARD_AUX_DIR_ALL_PINS, 0U);
}

int32_t aux_gpio_get(uint32_t subcmd, uint16_t *out_val)
{
    if (subcmd == AUX_GPIO_SUBCMD_VALUE) {
        uint32_t raw = MAP_GPIOPinRead(BOARD_AUX_GPIO_PORT_BASE, AUX_GPIO_VALID_MASK);
        *out_val = (uint16_t)(raw & AUX_GPIO_VALID_MASK);
    } else if (subcmd == AUX_GPIO_SUBCMD_DIR) {
        *out_val = (uint16_t)(g_aux_output_mask & AUX_GPIO_VALID_MASK);
    } else {
        *out_val = 0U;
    }

    return 0;
}

int32_t aux_gpio_set(uint32_t subcmd, uint16_t mask_val)
{
    uint8_t mask = (uint8_t)((mask_val >> 8U) & AUX_GPIO_VALID_MASK);
    uint8_t val = (uint8_t)(mask_val & AUX_GPIO_VALID_MASK);

    switch (subcmd) {
    case AUX_GPIO_SUBCMD_VALUE: {
        uint8_t eff_mask = (uint8_t)(mask & g_aux_output_mask);
        uint8_t eff_val = (uint8_t)(val & eff_mask);
        g_aux_output_val = (uint8_t)((g_aux_output_val & (uint8_t)(~eff_mask)) | eff_val);
        if (eff_mask != 0U) {
            MAP_GPIOPinWrite(BOARD_AUX_GPIO_PORT_BASE, eff_mask, eff_val);
        }
        break;
    }

    case AUX_GPIO_SUBCMD_DIR: {
        if (mask == 0U) {
            break;
        }
        uint8_t new_out = (uint8_t)((g_aux_output_mask & (uint8_t)(~mask)) | (val & mask));
        g_aux_output_mask = (uint8_t)(new_out & AUX_GPIO_VALID_MASK);

        static const uint8_t pins[3] = {BOARD_AUX_GPIO3_PIN, BOARD_AUX_GPIO1_PIN, BOARD_AUX_GPIO2_PIN};
        for (uint32_t i = 0U; i < 3U; i++) {
            uint8_t bit = pins[i];
            if ((mask & bit) != 0U) {
                aux_gpio_set_dir_pin(bit, (g_aux_output_mask & bit) != 0U);
            }
        }
        break;
    }

    case AUX_GPIO_SUBCMD_BLINK_LED:
        if (mask_val != 0U) {
            aux_gpio_blink_identify();
        }
        break;

    case AUX_GPIO_SUBCMD_SWD_SWITCH:
    default:
        break;
    }

    return 0;
}
