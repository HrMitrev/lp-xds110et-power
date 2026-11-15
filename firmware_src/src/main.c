/*
 * main.c - Bare-metal application for MSP432E401Y (XDS110 / LP-XDS110ET)
 *
 * Demonstrates:
 * - Direct ROM API table utilization (MAP_SysCtlClockFreqSet, MAP_GPIOPinWrite)
 * - Clock configuration (120 MHz via 25MHz crystal & PLL)
 * - Embedded CMD_VERSION protocol version marker (3.0.0.01)
 * - LED blink / super-loop execution
 */

#include <stdint.h>
#include <stdbool.h>

#include "ti/devices/msp432e4/inc/msp.h"
#include "driverlib/driverlib.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"

/*
 * Firmware version marker matching probe CMD_VERSION (0x03) format:
 * [build=0x01, patch=0x00, minor=0x00, major=0x03] = 3.0.0.01
 */
__attribute__((used, section(".rodata.version")))
const uint8_t g_firmware_version[4] = { 0x01, 0x00, 0x00, 0x03 };

int main(void)
{
    uint32_t sys_clock;

    /* Configure system clock to 120 MHz using 25 MHz external crystal and PLL */
    sys_clock = MAP_SysCtlClockFreqSet(
        (SYSCTL_XTAL_25MHZ | SYSCTL_OSC_MAIN | SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480),
        120000000
    );

    /* Enable GPIO Peripheral for Port N (LaunchPad LED D1/D2) */
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);
    while (!MAP_SysCtlPeripheralReady(SYSCTL_PERIPH_GPION)) {}

    /* Configure PN0 (LED D1) and PN1 (LED D2) as output pins */
    MAP_GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    /* Main event / super-loop */
    while (1)
    {
        /* Turn on LED D1, turn off LED D2 */
        MAP_GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1, GPIO_PIN_0);
        MAP_SysCtlDelay(sys_clock / 6); /* ~500ms delay */

        /* Turn off LED D1, turn on LED D2 */
        MAP_GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1, GPIO_PIN_1);
        MAP_SysCtlDelay(sys_clock / 6);
    }

    return 0;
}
