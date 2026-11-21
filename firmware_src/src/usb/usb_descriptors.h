/*
 * usb_descriptors.h - USB 2.1 High-Speed composite descriptors for LP-XDS110ET.
 *
 * Exposes VID:PID 0x0451:0xBEF3 with the XDS110 interface layout plus
 * CMSIS-DAP v2 (WinUSB Bulk) on Interface 5:
 *   - Interface 0/1: CDC0 Application UART (INT 0x81, OUT 0x01, IN 0x82)
 *   - Interface 2:   Vendor Bulk Debug/CMD (OUT 0x02, IN 0x83)
 *   - Interface 3/4: CDC1 Aux/SWO UART (INT 0x84, OUT 0x04, IN 0x85)
 *   - Interface 5:   Vendor Bulk CMSIS-DAP v2 WinUSB (OUT 0x05, IN 0x86)
 *   - Interface 6:   Vendor Bulk EnergyTrace Data (OUT 0x06, IN 0x87)
 */

#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include <stdbool.h>
#include <stdint.h>

/* USB identifiers matching TI XDS110 Standard configuration */
#define USB_VID_TI          0x0451U
#define USB_PID_XDS110      0xBEF3U
#define USB_BCD_VERSION_2_1 0x0210U /* USB 2.10 required for BOS / MS OS 2.0 */

/* Standard USB descriptor types */
#define USB_DESC_TYPE_DEVICE          0x01U
#define USB_DESC_TYPE_CONFIGURATION   0x02U
#define USB_DESC_TYPE_STRING          0x03U
#define USB_DESC_TYPE_INTERFACE       0x04U
#define USB_DESC_TYPE_ENDPOINT        0x05U
#define USB_DESC_TYPE_DEV_QUALIFIER   0x06U
#define USB_DESC_TYPE_OTHER_SPEED_CFG 0x07U
#define USB_DESC_TYPE_IAD             0x0BU
#define USB_DESC_TYPE_BOS             0x0FU
#define USB_DESC_TYPE_DEV_CAPABILITY  0x10U
#define USB_DESC_TYPE_CS_INTERFACE    0x24U

/* Microsoft OS 2.0 vendor request constants (WinUSB automatic binding) */
#define USB_MS_OS_20_VENDOR_CODE 0x20U
#define USB_MS_OS_20_DESC_INDEX  0x0007U

/* Interface indices */
#define USB_IF_CDC0_CTRL 0U
#define USB_IF_CDC0_DATA 1U
#define USB_IF_XDS_CMD   2U
#define USB_IF_CDC1_CTRL 3U
#define USB_IF_CDC1_DATA 4U
#define USB_IF_CMSIS_DAP 5U
#define USB_IF_ET_DATA   6U
#define USB_NUM_IFACES   7U

/* Endpoint addresses */
#define USB_EP_ADDR_CDC0_INT 0x81U
#define USB_EP_ADDR_CDC0_OUT 0x01U
#define USB_EP_ADDR_CDC0_IN  0x82U
#define USB_EP_ADDR_CMD_OUT  0x02U
#define USB_EP_ADDR_CMD_IN   0x83U
#define USB_EP_ADDR_CDC1_INT 0x84U
#define USB_EP_ADDR_CDC1_OUT 0x04U
#define USB_EP_ADDR_CDC1_IN  0x85U
#define USB_EP_ADDR_DAP_OUT  0x05U
#define USB_EP_ADDR_DAP_IN   0x86U
#define USB_EP_ADDR_DATA_OUT 0x06U
#define USB_EP_ADDR_DATA_IN  0x87U

/* Endpoint max packet sizes */
#define USB_EP0_MPS      64U
#define USB_BULK_MPS_HS  512U
#define USB_BULK_MPS_FS  64U
#define USB_CDC_INT_MPS  16U
#define USB_CONFIG_VALUE 1U

/* Initialize dynamic UTF-16LE product and serial string descriptors */
void usb_descriptors_init(const uint8_t fw_version[4], const char *serial_ascii);

/* Update bulk endpoint wMaxPacketSize fields for High-Speed (512 B) or Full-Speed (64 B) */
void usb_descriptors_set_speed(bool high_speed);

/* Look up standard descriptors (Device, Config, String, Qualifier, Other-Speed, BOS) */
bool usb_descriptors_get(uint8_t desc_type, uint8_t desc_index, const uint8_t **out_data, uint16_t *out_len);

/* Look up Microsoft OS 2.0 Descriptor Set (WinUSB + CMSIS-DAP v2 GUID for Interface 5) */
void usb_descriptors_get_ms_os_20(const uint8_t **out_data, uint16_t *out_len);

#endif /* USB_DESCRIPTORS_H */
