/*
 * board_lp_xds110et.h - LP-XDS110ET (MCU101 Rev B) Board Support Package.
 *
 * Pinout and peripheral assignments verified against docs/LP-XDS110ET_SCH.pdf
 * and rev-eng/HARDWARE_MAP.md. All hardware access uses MSP432E4 SDK calls.
 */

#ifndef BOARD_LP_XDS110ET_H
#define BOARD_LP_XDS110ET_H

#include <stdbool.h>
#include <stdint.h>

#include "ti/devices/msp432e4/inc/msp.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/sysctl.h"

/* System clock configuration (16 MHz crystal Y3 -> 480 MHz VCO -> 120 MHz SYSCLK) */
#define BOARD_SYS_CLOCK_HZ     120000000U
#define BOARD_SYS_CLOCK_CONFIG (SYSCTL_XTAL_16MHZ | SYSCTL_OSC_MAIN | SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480)

/* Expected hardware version ID for LP-XDS110ET (0x20 | PK7..PK4 strap = 0x23) */
#define BOARD_HW_VERSION_LP_XDS110ET 0x0023U
#define BOARD_TARGET_VCC_MV          3300U

/* Flash config / serial block layout (last 16 KiB flash sector) */
#define BOARD_SERIAL_LEN          8U
#define BOARD_CONFIG_WORD_OFFSET  0x10U
#define BOARD_CONFIG_STD          0xAA550001U
#define BOARD_CONFIG_DRAG_DROP    0xAA550002U
#define BOARD_CONFIG_CMSIS_DAP_V2 0xAA550004U

/* Status LEDs on Port D (active-high on LP-XDS110ET: CR4 Red = PD2, CR5 Green = PD3) */
#define BOARD_LED_PORT_PERIPH SYSCTL_PERIPH_GPIOD
#define BOARD_LED_PORT_BASE   GPIO_PORTD_BASE
#define BOARD_LED_RED_PIN     GPIO_PIN_2
#define BOARD_LED_GREEN_PIN   GPIO_PIN_3
#define BOARD_LED_ALL_PINS    (BOARD_LED_RED_PIN | BOARD_LED_GREEN_PIN)

/* 4-bit Hardware ID resistor straps (PK4..PK7) */
#define BOARD_HWID_PORT_PERIPH SYSCTL_PERIPH_GPIOK
#define BOARD_HWID_PORT_BASE   GPIO_PORTK_BASE
#define BOARD_HWID_PINS        (GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7)

/* Target JTAG / SWD / Reset & Level Shifter Direction (Port A & Port C) */
#define BOARD_JTAG_PORT_PERIPH    SYSCTL_PERIPH_GPIOA
#define BOARD_JTAG_PORT_BASE      GPIO_PORTA_BASE
#define BOARD_JTAG_TCK_PIN        GPIO_PIN_2
#define BOARD_JTAG_TMS_PIN        GPIO_PIN_3
#define BOARD_JTAG_TDO_PIN        GPIO_PIN_4
#define BOARD_JTAG_TDI_PIN        GPIO_PIN_5
#define BOARD_TARGET_SRST_PIN     GPIO_PIN_6
#define BOARD_TMS_DIR_PORT_PERIPH SYSCTL_PERIPH_GPIOC
#define BOARD_TMS_DIR_PORT_BASE   GPIO_PORTC_BASE
#define BOARD_TMS_DIR_PIN         GPIO_PIN_5

/* USB3300-EZK External ULPI High-Speed PHY pins */
#define BOARD_ULPI_CTRL_PORT_BASE GPIO_PORTB_BASE
#define BOARD_ULPI_STP_PIN        GPIO_PIN_2
#define BOARD_ULPI_CLK_PIN        GPIO_PIN_3
#define BOARD_ULPI_DATA_LO_BASE   GPIO_PORTL_BASE
#define BOARD_ULPI_DATA_LO_PINS   (GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5)
#define BOARD_ULPI_DATA_HI_BASE   GPIO_PORTP_BASE
#define BOARD_ULPI_NXT_PIN        GPIO_PIN_2
#define BOARD_ULPI_DIR_PIN        GPIO_PIN_3
#define BOARD_ULPI_D7_PIN         GPIO_PIN_4
#define BOARD_ULPI_D6_PIN         GPIO_PIN_5
#define BOARD_ULPI_HI_PINS        (BOARD_ULPI_NXT_PIN | BOARD_ULPI_DIR_PIN | BOARD_ULPI_D7_PIN | BOARD_ULPI_D6_PIN)

/* EnergyTrace HDR (ADS127L01 + TS3A24159 / TS3A4751 analog switches) */
#define BOARD_ET_SSI_PERIPH     SYSCTL_PERIPH_SSI1
#define BOARD_ET_SSI_BASE       SSI1_BASE
#define BOARD_ET_DRDY_PORT_BASE GPIO_PORTB_BASE
#define BOARD_ET_DRDY_PIN       GPIO_PIN_4
#define BOARD_ET_SCLK_PORT_BASE GPIO_PORTB_BASE
#define BOARD_ET_SCLK_PIN       GPIO_PIN_5
#define BOARD_ET_DATA_PORT_BASE GPIO_PORTE_BASE
#define BOARD_ET_DIN_PIN        GPIO_PIN_4
#define BOARD_ET_DOUT_PIN       GPIO_PIN_5

#define BOARD_ET_SW_PORT_BASE   GPIO_PORTM_BASE
#define BOARD_ET_BYPASS_PIN     GPIO_PIN_0 /* LOW = bypass shunt, HIGH = measure through 1 ohm shunt */
#define BOARD_ET_CALIBRATE_PIN  GPIO_PIN_1 /* LOW = target LP_VDD, HIGH = isolate target for calibration */
#define BOARD_ET_ADC_START_PIN  GPIO_PIN_2 /* ADS127L01 START (HIGH = converting) */
#define BOARD_ET_CAL0_PIN       GPIO_PIN_4 /* 40.2 mA cal switch (R123 = 82 ohm, cal idx 1) */
#define BOARD_ET_CAL1_PIN       GPIO_PIN_5 /* 90.4 mA cal switch (R125 = 36.5 ohm, cal idx 2) */
#define BOARD_ET_CAL2_PORT_BASE GPIO_PORTC_BASE
#define BOARD_ET_CAL2_PIN       GPIO_PIN_6 /* 1.5 mA cal switch (R130 = 2.2 kohm, cal idx 4) */
#define BOARD_ET_RESET_PORT     GPIO_PORTN_BASE
#define BOARD_ET_ADC_NRESET_PIN GPIO_PIN_3 /* ADS127L01 nRESET/nPWDN */

/* Application UART (CDC0 -> UART0) and Aux/SWO UART (CDC1 -> UART7, RX-only) */
#define BOARD_APP_UART_PERIPH    SYSCTL_PERIPH_UART0
#define BOARD_APP_UART_BASE      UART0_BASE
#define BOARD_APP_UART_PORT_BASE GPIO_PORTA_BASE
#define BOARD_APP_UART_RX_PIN    GPIO_PIN_0 /* PA0: XDS_RX (U0RX) */
#define BOARD_APP_UART_TX_PIN    GPIO_PIN_1 /* PA1: XDS_TX (U0TX) */
#define BOARD_AUX_UART_PERIPH    SYSCTL_PERIPH_UART7
#define BOARD_AUX_UART_BASE      UART7_BASE
#define BOARD_AUX_UART_PORT_BASE GPIO_PORTC_BASE
#define BOARD_AUX_UART_RX_PIN    GPIO_PIN_4 /* PC4: XDS_SWO (U7RX; PC5 is XDS_TMS_DIR) */

/* BSP API */
void board_init(void);
uint32_t board_get_sys_clock_hz(void);
uint32_t board_get_flash_size_bytes(void);
uint16_t board_get_hw_version(void);
bool board_has_ulpi_phy(void);
void board_read_serial(char out_serial[BOARD_SERIAL_LEN + 1U]);
uint32_t board_read_config_word(void);

void board_led_green_set(bool on);
void board_led_red_set(bool on);

void board_srst_set(bool assert_low);
uint32_t board_get_uptime_ms(void);
void board_delay_ms(uint32_t ms);
void board_delay_us(uint32_t us);

#endif /* BOARD_LP_XDS110ET_H */
