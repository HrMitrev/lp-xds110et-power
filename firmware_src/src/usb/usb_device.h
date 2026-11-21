/*
 * usb_device.h - MSP432E401Y USB0 High-Speed (USB3300 ULPI) device driver.
 *
 * Uses MSP432E4 DriverLib (MAP_USB*) exclusively for ULPI PHY setup, EP0
 * enumeration, Bulk 0 (XDS110 CMD: OUT 0x02 / IN 0x83), Bulk 1
 * (EnergyTrace Data: OUT 0x06 / IN 0x87), dual CDC ACM Virtual COM Ports:
 *   - CDC 0 (App UART): OUT 0x01 / IN 0x82 / INT 0x81 (IF 0/1)
 *   - CDC 1 (Aux/SWO):  OUT 0x04 / IN 0x85 / INT 0x84 (IF 3/4)
 * and HID CMSIS-DAP 1.0:
 *   - HID (CMSIS-DAP):  OUT 0x05 / IN 0x86 (IF 5, 64-byte packets)
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
#define USB_CDC_RX_BUF_SIZE  2048U
#define USB_CDC_TX_BUF_SIZE  8192U

/* HID CMSIS-DAP 1.0 fixed-size packet queue parameters */
#define USB_HID_PKT_SIZE    64U
#define USB_HID_QUEUE_DEPTH 4U

/* CDC port indices */
#define USB_CDC_PORT_APP   0U
#define USB_CDC_PORT_AUX   1U
#define USB_CDC_PORT_COUNT 2U

/* USB CDC ACM line coding parameters (matches 7-byte SET/GET_LINE_CODING wire format) */
typedef struct {
    uint32_t baud_rate;
    uint8_t stop_bits; /* 0 = 1 stop bit, 1 = 1.5, 2 = 2 stop bits */
    uint8_t parity;    /* 0 = None, 1 = Odd, 2 = Even, 3 = Mark, 4 = Space */
    uint8_t data_bits; /* 5, 6, 7, or 8 */
} usb_cdc_line_coding_t;

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

/* CDC ACM Virtual COM Ports (port = USB_CDC_PORT_APP or USB_CDC_PORT_AUX) */
uint32_t usb_cdc_rx_available(uint8_t port);
uint32_t usb_cdc_rx_read(uint8_t port, uint8_t *dst, uint32_t len);
uint32_t usb_cdc_tx_space(uint8_t port);
uint32_t usb_cdc_tx_write(uint8_t port, const uint8_t *src, uint32_t len);
bool usb_cdc_take_line_coding(uint8_t port, usb_cdc_line_coding_t *out_coding);
void usb_cdc_get_line_coding(uint8_t port, usb_cdc_line_coding_t *out_coding);
bool usb_cdc_is_dtr_asserted(uint8_t port);

/* HID CMSIS-DAP 1.0 interface (Interrupt OUT 0x05, Interrupt IN 0x86) */
bool usb_hid_rx_available(void);
bool usb_hid_rx_pop(uint8_t out_pkt[USB_HID_PKT_SIZE]);
bool usb_hid_tx_space(void);
bool usb_hid_tx_push(const uint8_t pkt[USB_HID_PKT_SIZE]);

#endif /* USB_DEVICE_H */
