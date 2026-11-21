/*
 * board.c - LP-XDS110ET Board Support Package implementation.
 *
 * Configures system clock, GPIO peripherals, board hardware ID detection,
 * USB3300 ULPI PHY presence detection, status LEDs, target nSRST, and safe
 * default states for the EnergyTrace analog front-end switches.
 */

#include "board_lp_xds110et.h"

#include <string.h>

#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/systick.h"

static uint32_t g_sys_clock_hz = 0U;
static uint32_t g_flash_size_bytes = 0U;
static uint32_t g_config_flash_addr = 0x000FC000U;
static uint16_t g_hw_version = 0U;
static bool g_has_ulpi_phy = false;
static volatile uint32_t g_uptime_ms = 0U;

void systick_handler(void)
{
    g_uptime_ms++;
}

static void board_init_systick(void)
{
    MAP_SysTickPeriodSet(g_sys_clock_hz / 1000U);
    MAP_SysTickIntEnable();
    MAP_SysTickEnable();
    MAP_IntMasterEnable();
}

static void board_enable_peripheral(uint32_t periph)
{
    MAP_SysCtlPeripheralEnable(periph);
    while (!MAP_SysCtlPeripheralReady(periph)) {
    }
}

static void board_init_leds(void)
{
    board_enable_peripheral(BOARD_LED_PORT_PERIPH);
    MAP_GPIOPinTypeGPIOOutput(BOARD_LED_PORT_BASE, BOARD_LED_ALL_PINS);
    MAP_GPIOPadConfigSet(BOARD_LED_PORT_BASE, BOARD_LED_ALL_PINS, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
    board_led_green_set(false);
    board_led_red_set(false);
}

static void board_detect_ulpi_phy(void)
{
    /*
     * Probe USB3300-EZK presence on PP2 (USBNXT): configure with weak pull-up.
     * An idle USB3300 actively drives USBNXT LOW; unpopulated boards read HIGH.
     */
    MAP_GPIOPinTypeGPIOInput(BOARD_ULPI_DATA_HI_BASE, BOARD_ULPI_NXT_PIN);
    MAP_GPIOPadConfigSet(BOARD_ULPI_DATA_HI_BASE, BOARD_ULPI_NXT_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
    board_delay_us(10U);

    uint32_t nxt_val = MAP_GPIOPinRead(BOARD_ULPI_DATA_HI_BASE, BOARD_ULPI_NXT_PIN);
    g_has_ulpi_phy = (nxt_val == 0U);
}

static void board_detect_hw_version(void)
{
    MAP_GPIOPinTypeGPIOInput(BOARD_HWID_PORT_BASE, BOARD_HWID_PINS);
    MAP_GPIOPadConfigSet(BOARD_HWID_PORT_BASE, BOARD_HWID_PINS, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
    board_delay_us(10U);

    uint32_t strap_nibble = (MAP_GPIOPinRead(BOARD_HWID_PORT_BASE, BOARD_HWID_PINS) >> 4U) & 0x0FU;
    g_hw_version = (uint16_t)(0x20U | strap_nibble);
}

static void board_init_safe_defaults(void)
{
    /* Release target nSRST (PA6) high with weak pull-up */
    MAP_GPIOPinTypeGPIOOutput(BOARD_JTAG_PORT_BASE, BOARD_TARGET_SRST_PIN);
    MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, BOARD_TARGET_SRST_PIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD_WPU);
    board_srst_set(false);

    /* Set TMS level-shifter direction (PC5) high (output) as default */
    MAP_GPIOPinTypeGPIOOutput(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN);
    MAP_GPIOPadConfigSet(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
    MAP_GPIOPinWrite(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, BOARD_TMS_DIR_PIN);

    /*
     * Safe defaults for EnergyTrace HDR analog front-end:
     *   - PM0 (ET_BYPASS)    = LOW  (1 ohm shunt bypassed; low-impedance supply)
     *   - PM1 (ET_CALIBRATE) = LOW  (target LP_VDD connected, cal bank disconnected)
     *   - PM2 (ADS START)    = LOW  (ADC idle)
     *   - PM4/PM5/PC6 (CAL)  = LOW  (all calibration resistors disconnected)
     *   - PN3 (ADS nRESET)   = HIGH (ADC held out of reset)
     */
    const uint8_t et_portm_pins =
        BOARD_ET_BYPASS_PIN | BOARD_ET_CALIBRATE_PIN | BOARD_ET_ADC_START_PIN | BOARD_ET_CAL0_PIN | BOARD_ET_CAL1_PIN;
    MAP_GPIOPinTypeGPIOOutput(BOARD_ET_SW_PORT_BASE, et_portm_pins);
    MAP_GPIOPinWrite(BOARD_ET_SW_PORT_BASE, et_portm_pins, 0U);

    MAP_GPIOPinTypeGPIOOutput(BOARD_ET_CAL2_PORT_BASE, BOARD_ET_CAL2_PIN);
    MAP_GPIOPinWrite(BOARD_ET_CAL2_PORT_BASE, BOARD_ET_CAL2_PIN, 0U);

    MAP_GPIOPinTypeGPIOOutput(BOARD_ET_RESET_PORT, BOARD_ET_ADC_NRESET_PIN);
    MAP_GPIOPinWrite(BOARD_ET_RESET_PORT, BOARD_ET_ADC_NRESET_PIN, BOARD_ET_ADC_NRESET_PIN);
}

void board_init(void)
{
    g_sys_clock_hz = MAP_SysCtlClockFreqSet(BOARD_SYS_CLOCK_CONFIG, BOARD_SYS_CLOCK_HZ);

    g_flash_size_bytes = MAP_SysCtlFlashSizeGet();
    uint32_t sector_size = MAP_SysCtlFlashSectorSizeGet();
    if (g_flash_size_bytes > sector_size) {
        g_config_flash_addr = g_flash_size_bytes - sector_size;
    }

    board_init_leds();

    /* Enable all GPIO ports used on LP-XDS110ET */
    static const uint32_t gpio_periphs[] = {
        SYSCTL_PERIPH_GPIOA,
        SYSCTL_PERIPH_GPIOB,
        SYSCTL_PERIPH_GPIOC,
        SYSCTL_PERIPH_GPIOE,
        SYSCTL_PERIPH_GPIOF,
        SYSCTL_PERIPH_GPIOJ,
        SYSCTL_PERIPH_GPIOK,
        SYSCTL_PERIPH_GPIOL,
        SYSCTL_PERIPH_GPIOM,
        SYSCTL_PERIPH_GPION,
        SYSCTL_PERIPH_GPIOP,
        SYSCTL_PERIPH_GPIOQ,
    };
    for (uint32_t i = 0U; i < (sizeof(gpio_periphs) / sizeof(gpio_periphs[0])); i++) {
        board_enable_peripheral(gpio_periphs[i]);
    }

    board_detect_ulpi_phy();
    board_detect_hw_version();
    board_init_safe_defaults();
    board_init_systick();
}

uint32_t board_get_sys_clock_hz(void)
{
    return g_sys_clock_hz;
}

uint32_t board_get_flash_size_bytes(void)
{
    return g_flash_size_bytes;
}

uint16_t board_get_hw_version(void)
{
    return g_hw_version;
}

bool board_has_ulpi_phy(void)
{
    return g_has_ulpi_phy;
}

void board_read_serial(char out_serial[BOARD_SERIAL_LEN + 1U])
{
    const char *flash_serial = (const char *)g_config_flash_addr;
    bool valid = true;

    for (uint32_t i = 0U; i < BOARD_SERIAL_LEN; i++) {
        uint8_t ch = (uint8_t)flash_serial[i];
        if ((ch < 0x20U) || (ch > 0x7EU)) {
            valid = false;
            break;
        }
        out_serial[i] = (char)ch;
    }

    if (!valid) {
        memcpy(out_serial, "NOserial", BOARD_SERIAL_LEN);
    }
    out_serial[BOARD_SERIAL_LEN] = '\0';
}

uint32_t board_read_config_word(void)
{
    const uint32_t *config_word_ptr = (const uint32_t *)(g_config_flash_addr + BOARD_CONFIG_WORD_OFFSET);
    uint32_t word = *config_word_ptr;

    if ((word & 0xFFFF0000U) != 0xAA550000U) {
        return BOARD_CONFIG_STD;
    }
    return word;
}

void board_led_green_set(bool on)
{
    MAP_GPIOPinWrite(BOARD_LED_PORT_BASE, BOARD_LED_GREEN_PIN, on ? BOARD_LED_GREEN_PIN : 0U);
}

void board_led_red_set(bool on)
{
    MAP_GPIOPinWrite(BOARD_LED_PORT_BASE, BOARD_LED_RED_PIN, on ? BOARD_LED_RED_PIN : 0U);
}

void board_srst_set(bool assert_low)
{
    uint8_t val = assert_low ? 0U : BOARD_TARGET_SRST_PIN;
    MAP_GPIOPinWrite(BOARD_JTAG_PORT_BASE, BOARD_TARGET_SRST_PIN, val);
}

uint32_t board_get_uptime_ms(void)
{
    return g_uptime_ms;
}

void board_delay_ms(uint32_t ms)
{
    /* SysCtlDelay executes 3 CPU cycles per iteration */
    uint32_t cycles_per_ms = g_sys_clock_hz / 3000U;
    while (ms > 0U) {
        MAP_SysCtlDelay(cycles_per_ms);
        ms--;
    }
}

void board_delay_us(uint32_t us)
{
    uint32_t cycles_per_us = g_sys_clock_hz / 3000000U;
    if ((us > 0U) && (cycles_per_us > 0U)) {
        MAP_SysCtlDelay(us * cycles_per_us);
    }
}
