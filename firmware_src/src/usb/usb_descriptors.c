/*
 * usb_descriptors.c - USB 2.0 High-Speed composite descriptor definitions.
 *
 * Dynamically populates the product version string and 8-character flash serial
 * number into UTF-16LE string descriptors and adjusts bulk endpoint max packet
 * sizes between 512 B (HS) and 64 B (FS) upon USB bus reset.
 */

#include "usb_descriptors.h"

#include <stdio.h>
#include <string.h>

#define U16_LO(x) ((uint8_t)((x) & 0xFFU))
#define U16_HI(x) ((uint8_t)(((x) >> 8U) & 0xFFU))

#define USB_CONFIG_DESC_TOTAL_LEN 219U

/* Minimal 64-byte vendor HID report descriptor (CMSIS-DAP 1.0 compatible) */
static const uint8_t g_hid_report_desc[] = {
    0x06U,
    0x00U,
    0xFFU, /* Usage Page (Vendor Defined 0xFF00) */
    0x09U,
    0x01U, /* Usage (0x01) */
    0xA1U,
    0x01U, /* Collection (Application) */
    0x15U,
    0x00U, /*   Logical Minimum (0) */
    0x26U,
    0xFFU,
    0x00U, /*   Logical Maximum (255) */
    0x75U,
    0x08U, /*   Report Size (8 bits) */
    0x95U,
    0x40U, /*   Report Count (64 bytes) */
    0x09U,
    0x01U, /*   Usage (0x01) */
    0x81U,
    0x02U, /*   Input (Data, Var, Abs) */
    0x95U,
    0x40U, /*   Report Count (64 bytes) */
    0x09U,
    0x01U, /*   Usage (0x01) */
    0x91U,
    0x02U, /*   Output (Data, Var, Abs) */
    0xC0U, /* End Collection */
};

static const uint8_t g_device_desc[] = {
    18U,                         /* bLength */
    USB_DESC_TYPE_DEVICE,        /* bDescriptorType */
    U16_LO(USB_BCD_VERSION_2_0), /* bcdUSB lo */
    U16_HI(USB_BCD_VERSION_2_0), /* bcdUSB hi */
    0xEFU,                       /* bDeviceClass (Misc / IAD Composite) */
    0x02U,                       /* bDeviceSubClass (Common Class) */
    0x01U,                       /* bDeviceProtocol (IAD) */
    USB_EP0_MPS,                 /* bMaxPacketSize0 */
    U16_LO(USB_VID_TI),          /* idVendor lo */
    U16_HI(USB_VID_TI),          /* idVendor hi */
    U16_LO(USB_PID_XDS110),      /* idProduct lo */
    U16_HI(USB_PID_XDS110),      /* idProduct hi */
    0x00U,
    0x01U, /* bcdDevice (1.00) */
    1U,    /* iManufacturer */
    2U,    /* iProduct */
    3U,    /* iSerialNumber */
    1U,    /* bNumConfigurations */
};

static const uint8_t g_device_qualifier_desc[] = {
    10U,                         /* bLength */
    USB_DESC_TYPE_DEV_QUALIFIER, /* bDescriptorType */
    U16_LO(USB_BCD_VERSION_2_0), /* bcdUSB lo */
    U16_HI(USB_BCD_VERSION_2_0), /* bcdUSB hi */
    0xEFU,                       /* bDeviceClass */
    0x02U,                       /* bDeviceSubClass */
    0x01U,                       /* bDeviceProtocol */
    USB_EP0_MPS,                 /* bMaxPacketSize0 */
    1U,                          /* bNumConfigurations */
    0U,                          /* bReserved */
};

/* Mutable configuration descriptor (bulk wMaxPacketSize patched on speed change) */
static uint8_t g_config_desc[] = {
    /* Configuration Descriptor (9 B) */
    9U,
    USB_DESC_TYPE_CONFIGURATION,
    U16_LO(USB_CONFIG_DESC_TOTAL_LEN),
    U16_HI(USB_CONFIG_DESC_TOTAL_LEN),
    USB_NUM_IFACES,
    USB_CONFIG_VALUE,
    5U,    /* iConfiguration */
    0x80U, /* bmAttributes: bus-powered */
    250U,  /* bMaxPower: 500 mA */

    /* ---- CDC0: Application UART (Interfaces 0 & 1) ---- */
    /* IAD (8 B) */
    8U,
    USB_DESC_TYPE_IAD,
    USB_IF_CDC0_CTRL,
    2U,
    0x02U,
    0x02U,
    0x01U,
    0U,
    /* Interface 0: CDC0 ACM Control (9 B) */
    9U,
    USB_DESC_TYPE_INTERFACE,
    USB_IF_CDC0_CTRL,
    0U,
    1U,
    0x02U,
    0x02U,
    0x01U,
    0U,
    /* CDC Header Functional Descriptor (5 B) */
    5U,
    USB_DESC_TYPE_CS_INTERFACE,
    0x00U,
    0x10U,
    0x01U,
    /* CDC Call Management Functional Descriptor (5 B) */
    5U,
    USB_DESC_TYPE_CS_INTERFACE,
    0x01U,
    0x00U,
    USB_IF_CDC0_DATA,
    /* CDC ACM Functional Descriptor (4 B) */
    4U,
    USB_DESC_TYPE_CS_INTERFACE,
    0x02U,
    0x02U,
    /* CDC Union Functional Descriptor (5 B) */
    5U,
    USB_DESC_TYPE_CS_INTERFACE,
    0x06U,
    USB_IF_CDC0_CTRL,
    USB_IF_CDC0_DATA,
    /* Endpoint 0x81: Interrupt IN (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_CDC0_INT,
    0x03U,
    U16_LO(USB_CDC_INT_MPS),
    U16_HI(USB_CDC_INT_MPS),
    16U,
    /* Interface 1: CDC0 Data (9 B) */
    9U,
    USB_DESC_TYPE_INTERFACE,
    USB_IF_CDC0_DATA,
    0U,
    2U,
    0x0AU,
    0x00U,
    0x00U,
    0U,
    /* Endpoint 0x01: Bulk OUT (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_CDC0_OUT,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,
    /* Endpoint 0x82: Bulk IN (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_CDC0_IN,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,

    /* ---- Interface 2: XDS110 Debug / Command Bulk Port ---- */
    /* Interface 2 (9 B) */
    9U,
    USB_DESC_TYPE_INTERFACE,
    USB_IF_XDS_CMD,
    0U,
    2U,
    0xFFU,
    0x00U,
    0x00U,
    4U,
    /* Endpoint 0x02: Bulk OUT (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_CMD_OUT,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,
    /* Endpoint 0x83: Bulk IN (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_CMD_IN,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,

    /* ---- CDC1: Auxiliary / SWO UART (Interfaces 3 & 4) ---- */
    /* IAD (8 B) */
    8U,
    USB_DESC_TYPE_IAD,
    USB_IF_CDC1_CTRL,
    2U,
    0x02U,
    0x02U,
    0x01U,
    0U,
    /* Interface 3: CDC1 ACM Control (9 B) */
    9U,
    USB_DESC_TYPE_INTERFACE,
    USB_IF_CDC1_CTRL,
    0U,
    1U,
    0x02U,
    0x02U,
    0x01U,
    0U,
    /* CDC Header Functional Descriptor (5 B) */
    5U,
    USB_DESC_TYPE_CS_INTERFACE,
    0x00U,
    0x10U,
    0x01U,
    /* CDC Call Management Functional Descriptor (5 B) */
    5U,
    USB_DESC_TYPE_CS_INTERFACE,
    0x01U,
    0x00U,
    USB_IF_CDC1_DATA,
    /* CDC ACM Functional Descriptor (4 B) */
    4U,
    USB_DESC_TYPE_CS_INTERFACE,
    0x02U,
    0x02U,
    /* CDC Union Functional Descriptor (5 B) */
    5U,
    USB_DESC_TYPE_CS_INTERFACE,
    0x06U,
    USB_IF_CDC1_CTRL,
    USB_IF_CDC1_DATA,
    /* Endpoint 0x84: Interrupt IN (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_CDC1_INT,
    0x03U,
    U16_LO(USB_CDC_INT_MPS),
    U16_HI(USB_CDC_INT_MPS),
    16U,
    /* Interface 4: CDC1 Data (9 B) */
    9U,
    USB_DESC_TYPE_INTERFACE,
    USB_IF_CDC1_DATA,
    0U,
    2U,
    0x0AU,
    0x00U,
    0x00U,
    0U,
    /* Endpoint 0x04: Bulk OUT (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_CDC1_OUT,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,
    /* Endpoint 0x85: Bulk IN (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_CDC1_IN,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,

    /* ---- Interface 5: HID CMSIS-DAP 1.0 ---- */
    /* Interface 5 (9 B) */
    9U,
    USB_DESC_TYPE_INTERFACE,
    USB_IF_CMSIS_HID,
    0U,
    2U,
    0x03U,
    0x00U,
    0x00U,
    6U,
    /* HID Descriptor (9 B) */
    9U,
    USB_DESC_TYPE_HID,
    0x11U,
    0x01U,
    0x00U,
    1U,
    USB_DESC_TYPE_HID_REPORT,
    U16_LO(sizeof(g_hid_report_desc)),
    U16_HI(sizeof(g_hid_report_desc)),
    /* Endpoint 0x05: Interrupt OUT (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_HID_OUT,
    0x03U,
    U16_LO(USB_HID_INT_MPS),
    U16_HI(USB_HID_INT_MPS),
    1U,
    /* Endpoint 0x86: Interrupt IN (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_HID_IN,
    0x03U,
    U16_LO(USB_HID_INT_MPS),
    U16_HI(USB_HID_INT_MPS),
    1U,

    /* ---- Interface 6: XDS110 EnergyTrace Data Bulk Port ---- */
    /* Interface 6 (9 B) */
    9U,
    USB_DESC_TYPE_INTERFACE,
    USB_IF_ET_DATA,
    0U,
    2U,
    0xFFU,
    0x00U,
    0x00U,
    0U,
    /* Endpoint 0x06: Bulk OUT (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_DATA_OUT,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,
    /* Endpoint 0x87: Bulk IN (7 B) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_DATA_IN,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,
};

_Static_assert(sizeof(g_config_desc) == USB_CONFIG_DESC_TOTAL_LEN, "USB config descriptor length mismatch");

static uint8_t g_other_speed_desc[USB_CONFIG_DESC_TOTAL_LEN];

/* String descriptor buffers (UTF-16LE encoded at init) */
static const uint8_t g_str_langid[] = {4U, USB_DESC_TYPE_STRING, 0x09U, 0x04U};
static uint8_t g_str_manufacturer[64];
static uint8_t g_str_product[96];
static uint8_t g_str_serial[32];
static uint8_t g_str_ctrl_iface[64];
static uint8_t g_str_config[64];
static uint8_t g_str_cmsis[64];

static const uint8_t *const g_string_table[] = {
    g_str_langid,
    g_str_manufacturer,
    g_str_product,
    g_str_serial,
    g_str_ctrl_iface,
    g_str_config,
    g_str_cmsis,
};

static void usb_encode_utf16le_string(uint8_t *dst, uint32_t dst_cap, const char *ascii)
{
    uint32_t len = (uint32_t)strlen(ascii);
    uint32_t max_chars = (dst_cap - 2U) / 2U;
    if (len > max_chars) {
        len = max_chars;
    }

    dst[0] = (uint8_t)(2U + (len * 2U));
    dst[1] = USB_DESC_TYPE_STRING;
    for (uint32_t i = 0U; i < len; i++) {
        dst[2U + (i * 2U)] = (uint8_t)ascii[i];
        dst[3U + (i * 2U)] = 0U;
    }
}

static void usb_patch_bulk_mps(uint8_t *cfg_desc, uint16_t total_len, uint16_t bulk_mps)
{
    uint16_t off = 0U;
    while ((off + 2U) <= total_len) {
        uint8_t desc_len = cfg_desc[off];
        uint8_t desc_type = cfg_desc[off + 1U];
        if ((desc_len < 2U) || ((off + desc_len) > total_len)) {
            break;
        }
        /* Patch Bulk endpoints (bmAttributes[1:0] == 0x02) */
        if ((desc_type == USB_DESC_TYPE_ENDPOINT) && (desc_len >= 7U) && ((cfg_desc[off + 3U] & 0x03U) == 0x02U)) {
            cfg_desc[off + 4U] = U16_LO(bulk_mps);
            cfg_desc[off + 5U] = U16_HI(bulk_mps);
        }
        off = (uint16_t)(off + desc_len);
    }
}

void usb_descriptors_init(const uint8_t fw_version[4], const char *serial_ascii)
{
    char product_buf[48];
    snprintf(product_buf, sizeof(product_buf), "XDS110 (%02u.%02u.%02u.%02u) OpenFW", (unsigned)fw_version[3],
        (unsigned)fw_version[2], (unsigned)fw_version[1], (unsigned)fw_version[0]);

    usb_encode_utf16le_string(g_str_manufacturer, sizeof(g_str_manufacturer), "Texas Instruments");
    usb_encode_utf16le_string(g_str_product, sizeof(g_str_product), product_buf);
    usb_encode_utf16le_string(g_str_serial, sizeof(g_str_serial), serial_ascii);
    usb_encode_utf16le_string(g_str_ctrl_iface, sizeof(g_str_ctrl_iface), "XDS110 Control Interface");
    usb_encode_utf16le_string(g_str_config, sizeof(g_str_config), "XDS110 Configuration");
    usb_encode_utf16le_string(g_str_cmsis, sizeof(g_str_cmsis), "XDS110 CMSIS-DAP");

    usb_descriptors_set_speed(true);
}

void usb_descriptors_set_speed(bool high_speed)
{
    uint16_t active_mps = high_speed ? USB_BULK_MPS_HS : USB_BULK_MPS_FS;
    uint16_t other_mps = high_speed ? USB_BULK_MPS_FS : USB_BULK_MPS_HS;

    usb_patch_bulk_mps(g_config_desc, sizeof(g_config_desc), active_mps);

    memcpy(g_other_speed_desc, g_config_desc, sizeof(g_config_desc));
    g_other_speed_desc[1] = USB_DESC_TYPE_OTHER_SPEED_CFG;
    usb_patch_bulk_mps(g_other_speed_desc, sizeof(g_other_speed_desc), other_mps);
}

bool usb_descriptors_get(uint8_t desc_type, uint8_t desc_index, const uint8_t **out_data, uint16_t *out_len)
{
    switch (desc_type) {
    case USB_DESC_TYPE_DEVICE:
        *out_data = g_device_desc;
        *out_len = (uint16_t)sizeof(g_device_desc);
        return true;

    case USB_DESC_TYPE_CONFIGURATION:
        if (desc_index != 0U) {
            return false;
        }
        *out_data = g_config_desc;
        *out_len = (uint16_t)sizeof(g_config_desc);
        return true;

    case USB_DESC_TYPE_DEV_QUALIFIER:
        *out_data = g_device_qualifier_desc;
        *out_len = (uint16_t)sizeof(g_device_qualifier_desc);
        return true;

    case USB_DESC_TYPE_OTHER_SPEED_CFG:
        if (desc_index != 0U) {
            return false;
        }
        *out_data = g_other_speed_desc;
        *out_len = (uint16_t)sizeof(g_other_speed_desc);
        return true;

    case USB_DESC_TYPE_STRING: {
        uint32_t count = (uint32_t)(sizeof(g_string_table) / sizeof(g_string_table[0]));
        if (desc_index >= count) {
            return false;
        }
        *out_data = g_string_table[desc_index];
        *out_len = g_string_table[desc_index][0];
        return true;
    }

    default:
        return false;
    }
}

bool usb_descriptors_get_hid_report(uint16_t iface, const uint8_t **out_data, uint16_t *out_len)
{
    if (iface != USB_IF_CMSIS_HID) {
        return false;
    }
    *out_data = g_hid_report_desc;
    *out_len = (uint16_t)sizeof(g_hid_report_desc);
    return true;
}
