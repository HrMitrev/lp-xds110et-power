/*
 * usb_device.h - MSP432E401Y USB0 High-Speed (USB3300 ULPI) device driver.
 *
 * Uses MSP432E4 DriverLib (MAP_USB*) exclusively for ULPI PHY setup, EP0
 * enumeration, Bulk 0 (XDS110 CMD: OUT 0x02 / IN 0x83), and Bulk 1
 * (EnergyTrace Data: OUT 0x06 / IN 0x87).
 */

#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

/* Ring buffer sizes (powers of two for fast lock-free masking) */
#define USB_CMD_RX_BUF_SIZE  8192U
#define USB_CMD_TX_BUF_SIZE  8192U
#define USB_DATA_RX_BUF_SIZE 2048U
#define USB_DATA_TX_BUF_SIZE 65536U

/* Initialize USB3300 ULPI PHY, USB0 controller, descriptors, and attach to bus */
void usb_device_init(const uint8_t fw_version[4]);

/* Detach cleanly from USB bus and mask USB0 interrupts (called before DFU jump) */
void usb_device_disconnect(void);

/* USB link & enumeration state */
bool usb_device_is_configured(void);
bool usb_device_is_high_speed(void);

/* Bulk 0 — XDS110 Command interface (OUT 0x02, IN 0x83) */
uint32_t usb_cmd_rx_available(void);
uint32_t usb_cmd_rx_peek(uint8_t *dst, uint32_t len);
uint32_t usb_cmd_rx_read(uint8_t *dst, uint32_t len);
void usb_cmd_rx_flush(void);
uint32_t usb_cmd_tx_space(void);
uint32_t usb_cmd_tx_write(const uint8_t *src, uint32_t len);

/* Bulk 1 — EnergyTrace Data interface (OUT 0x06, IN 0x87) */
uint32_t usb_data_rx_available(void);
uint32_t usb_data_rx_read(uint8_t *dst, uint32_t len);
uint32_t usb_data_tx_space(void);
uint32_t usb_data_tx_write(const uint8_t *src, uint32_t len);
void usb_data_tx_flush(void);

#endif /* USB_DEVICE_H */
