/*
 * cdc_uart.h - Dual Virtual COM Port hardware UART bridge for LP-XDS110ET.
 *
 * Bridges USB CDC ACM ports to on-chip UART peripherals:
 *   - CDC 0 (USB_CDC_PORT_APP, /dev/ttyACM0) <-> UART0 (PA0 RX, PA1 TX, full-duplex)
 *   - CDC 1 (USB_CDC_PORT_AUX, /dev/ttyACM1) <-> UART7 (PC4 RX-only, Aux / SWO)
 */

#ifndef CDC_UART_H
#define CDC_UART_H

#include <stdbool.h>

/* Initialize UART0 (PA0/PA1) and UART7 (PC4 RX-only) with default 115200 8N1 */
void cdc_uart_init(void);

/*
 * Service line-coding updates and pump bytes between USB CDC rings and UARTs.
 * Returns true if any work was performed in this iteration.
 */
bool cdc_uart_poll(void);

#endif /* CDC_UART_H */
