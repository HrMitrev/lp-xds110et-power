/*
 * usb_descriptors.c - USB 2.1 High-Speed composite descriptor definitions.
 *
 * Dynamically populates the product version string and 8-character flash serial
 * number into UTF-16LE string descriptors, adjusts bulk endpoint max packet
 * sizes between 512 B (HS) and 64 B (FS) upon USB bus reset, and exposes BOS +
 * Microsoft OS 2.0 descriptors for zero-driver WinUSB CMSIS-DAP v2 binding.
 */

#include "usb_descriptors.h"

#include <stdio.h>
#include <string.h>

#define U16_LO(x) ((uint8_t)((x) & 0xFFU))
#define U16_HI(x) ((uint8_t)(((x) >> 8U) & 0xFFU))
#define U16_CH(c) (uint8_t)(c), 0x00U

#define USB_CONFIG_DESC_TOTAL_LEN 210U
#define USB_BOS_DESC_TOTAL_LEN    40U
#define USB_MS_OS_20_DESC_LEN     178U
#define USB_MS_OS_20_CFG_SUB_LEN  168U
#define USB_MS_OS_20_FUNC_SUB_LEN 160U

static const uint8_t k_device_desc[] = {
    18U,                         /* bLength */
    USB_DESC_TYPE_DEVICE,        /* bDescriptorType */
    U16_LO(USB_BCD_VERSION_2_1), /* bcdUSB lo (2.10 for BOS) */
    U16_HI(USB_BCD_VERSION_2_1), /* bcdUSB hi */
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

static const uint8_t k_device_qualifier_desc[] = {
    10U,                         /* bLength */
    USB_DESC_TYPE_DEV_QUALIFIER, /* bDescriptorType */
    U16_LO(USB_BCD_VERSION_2_1), /* bcdUSB lo */
    U16_HI(USB_BCD_VERSION_2_1), /* bcdUSB hi */
    0xEFU,                       /* bDeviceClass */
    0x02U,                       /* bDeviceSubClass */
    0x01U,                       /* bDeviceProtocol */
    USB_EP0_MPS,                 /* bMaxPacketSize0 */
    1U,                          /* bNumConfigurations */
    0U,                          /* bReserved */
};

/* Binary Device Object Store (BOS) Descriptor with MS OS 2.0 Platform Capability */
static const uint8_t k_bos_desc[] = {
    /* BOS Descriptor Header (5 B) */
    5U,
    USB_DESC_TYPE_BOS,
    U16_LO(USB_BOS_DESC_TOTAL_LEN),
    U16_HI(USB_BOS_DESC_TOTAL_LEN),
    2U, /* bNumDeviceCaps */

    /* Device Capability 1: USB 2.0 Extension (7 B, required by USB 2.1) */
    7U,
    USB_DESC_TYPE_DEV_CAPABILITY,
    0x02U, /* bDevCapabilityType: USB 2.0 Extension */
    0x00U,
    0x00U,
    0x00U,
    0x00U, /* bmAttributes: LPM not supported */

    /* Device Capability 2: Microsoft OS 2.0 Platform Capability (28 B) */
    28U,
    USB_DESC_TYPE_DEV_CAPABILITY,
    0x05U, /* bDevCapabilityType: Platform */
    0x00U, /* bReserved */
    /* PlatformCapabilityUUID: {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} */
    0xDFU,
    0x60U,
    0xDDU,
    0xD8U,
    0x89U,
    0x45U,
    0xC7U,
    0x4CU,
    0x9CU,
    0xD2U,
    0x65U,
    0x9DU,
    0x9EU,
    0x64U,
    0x8AU,
    0x9FU,
    /* dwWindowsVersion: 0x06030000 (Windows 8.1+) */
    0x00U,
    0x00U,
    0x03U,
    0x06U,
    /* wMSOSDescriptorSetTotalLength */
    U16_LO(USB_MS_OS_20_DESC_LEN),
    U16_HI(USB_MS_OS_20_DESC_LEN),
    USB_MS_OS_20_VENDOR_CODE, /* bMS_VendorCode */
    0x00U,                    /* bAltEnumCode */
};

_Static_assert(sizeof(k_bos_desc) == USB_BOS_DESC_TOTAL_LEN, "USB BOS descriptor length mismatch");

/*
 * Microsoft OS 2.0 Descriptor Set binding Interface 5 (CMSIS-DAP v2) to WinUSB
 * with the standard ARM CMSIS-DAP v2 DeviceInterfaceGUID:
 *   {CDB3B5AD-293B-4663-AA36-1AAE46463776}
 */
static const uint8_t k_ms_os_20_desc_set[] = {
    /* Descriptor Set Header (10 B) */
    0x0AU,
    0x00U, /* wLength */
    0x00U,
    0x00U, /* wDescriptorType: MS_OS_20_SET_HEADER_DESCRIPTOR */
    0x00U,
    0x00U,
    0x03U,
    0x06U, /* dwWindowsVersion: 0x06030000 (Windows 8.1+) */
    U16_LO(USB_MS_OS_20_DESC_LEN),
    U16_HI(USB_MS_OS_20_DESC_LEN), /* wTotalLength */

    /* Configuration Subset Header (8 B) */
    0x08U,
    0x00U, /* wLength */
    0x01U,
    0x00U, /* wDescriptorType: MS_OS_20_SUBSET_HEADER_CONFIGURATION */
    0x00U, /* bConfigurationValue (0-based config index per MS OS 2.0 spec) */
    0x00U, /* bReserved */
    U16_LO(USB_MS_OS_20_CFG_SUB_LEN),
    U16_HI(USB_MS_OS_20_CFG_SUB_LEN), /* wTotalLength */

    /* Function Subset Header for Interface 5: CMSIS-DAP v2 (8 B) */
    0x08U,
    0x00U, /* wLength */
    0x02U,
    0x00U,            /* wDescriptorType: MS_OS_20_SUBSET_HEADER_FUNCTION */
    USB_IF_CMSIS_DAP, /* bFirstInterface */
    0x00U,            /* bReserved */
    U16_LO(USB_MS_OS_20_FUNC_SUB_LEN),
    U16_HI(USB_MS_OS_20_FUNC_SUB_LEN), /* wSubsetLength */

    /* Feature Descriptor: Compatible ID = "WINUSB" (20 B) */
    0x14U,
    0x00U, /* wLength */
    0x03U,
    0x00U, /* wDescriptorType: MS_OS_20_FEATURE_COMPATIBLE_ID */
    'W',
    'I',
    'N',
    'U',
    'S',
    'B',
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,

    /* Feature Descriptor: Registry Property DeviceInterfaceGUIDs (132 B) */
    0x84U,
    0x00U, /* wLength (132 B) */
    0x04U,
    0x00U, /* wDescriptorType: MS_OS_20_FEATURE_REG_PROPERTY */
    0x07U,
    0x00U, /* wPropertyDataType: REG_MULTI_SZ */
    0x2AU,
    0x00U, /* wPropertyNameLength (42 B) */
    /* "DeviceInterfaceGUIDs\0" (UTF-16LE) */
    U16_CH('D'),
    U16_CH('e'),
    U16_CH('v'),
    U16_CH('i'),
    U16_CH('c'),
    U16_CH('e'),
    U16_CH('I'),
    U16_CH('n'),
    U16_CH('t'),
    U16_CH('e'),
    U16_CH('r'),
    U16_CH('f'),
    U16_CH('a'),
    U16_CH('c'),
    U16_CH('e'),
    U16_CH('G'),
    U16_CH('U'),
    U16_CH('I'),
    U16_CH('D'),
    U16_CH('s'),
    U16_CH('\0'),
    0x50U,
    0x00U, /* wPropertyDataLength (80 B) */
    /* "{CDB3B5AD-293B-4663-AA36-1AAE46463776}\0\0" (UTF-16LE) */
    U16_CH('{'),
    U16_CH('C'),
    U16_CH('D'),
    U16_CH('B'),
    U16_CH('3'),
    U16_CH('B'),
    U16_CH('5'),
    U16_CH('A'),
    U16_CH('D'),
    U16_CH('-'),
    U16_CH('2'),
    U16_CH('9'),
    U16_CH('3'),
    U16_CH('B'),
    U16_CH('-'),
    U16_CH('4'),
    U16_CH('6'),
    U16_CH('6'),
    U16_CH('3'),
    U16_CH('-'),
    U16_CH('A'),
    U16_CH('A'),
    U16_CH('3'),
    U16_CH('6'),
    U16_CH('-'),
    U16_CH('1'),
    U16_CH('A'),
    U16_CH('A'),
    U16_CH('E'),
    U16_CH('4'),
    U16_CH('6'),
    U16_CH('4'),
    U16_CH('6'),
    U16_CH('3'),
    U16_CH('7'),
    U16_CH('7'),
    U16_CH('6'),
    U16_CH('}'),
    U16_CH('\0'),
    U16_CH('\0'),
};

_Static_assert(sizeof(k_ms_os_20_desc_set) == USB_MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor length mismatch");

/* Configuration descriptor template (copied to s_desc.config_desc and patched on speed change) */
static const uint8_t k_config_desc_template[USB_CONFIG_DESC_TOTAL_LEN] = {
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

    /* ---- Interface 5: CMSIS-DAP v2 WinUSB Bulk ---- */
    /* Interface 5 (9 B) */
    9U,
    USB_DESC_TYPE_INTERFACE,
    USB_IF_CMSIS_DAP,
    0U,
    2U,
    0xFFU, /* Vendor Specific */
    0x00U,
    0x00U,
    6U, /* iInterface: "XDS110 CMSIS-DAP v2" */
    /* Endpoint 0x05: Bulk OUT (7 B, Command) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_DAP_OUT,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,
    /* Endpoint 0x86: Bulk IN (7 B, Response) */
    7U,
    USB_DESC_TYPE_ENDPOINT,
    USB_EP_ADDR_DAP_IN,
    0x02U,
    U16_LO(USB_BULK_MPS_HS),
    U16_HI(USB_BULK_MPS_HS),
    0U,

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

_Static_assert(sizeof(k_config_desc_template) == USB_CONFIG_DESC_TOTAL_LEN, "USB config descriptor length mismatch");

/* Immutable Language ID string descriptor (US English 0x0409) */
static const uint8_t k_str_langid[] = {4U, USB_DESC_TYPE_STRING, 0x09U, 0x04U};

/* Mutable runtime USB descriptors (speed-patched config + UTF-16LE strings) */
typedef struct {
    uint8_t config_desc[USB_CONFIG_DESC_TOTAL_LEN];
    uint8_t other_speed_desc[USB_CONFIG_DESC_TOTAL_LEN];
    uint8_t str_manufacturer[64];
    uint8_t str_product[96];
    uint8_t str_serial[32];
    uint8_t str_ctrl_iface[64];
    uint8_t str_config[64];
    uint8_t str_cmsis[64];
} usb_desc_state_t;

static usb_desc_state_t s_desc;

static const uint8_t *const k_string_table[] = {
    k_str_langid,
    s_desc.str_manufacturer,
    s_desc.str_product,
    s_desc.str_serial,
    s_desc.str_ctrl_iface,
    s_desc.str_config,
    s_desc.str_cmsis,
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
    char product_buf[56];
    snprintf(product_buf, sizeof(product_buf), "XDS110 (%02x.%02x.%02x.%02x) OpenFW CMSIS-DAP", (unsigned)fw_version[3],
        (unsigned)fw_version[2], (unsigned)fw_version[1], (unsigned)fw_version[0]);

    memcpy(s_desc.config_desc, k_config_desc_template, sizeof(s_desc.config_desc));
    usb_encode_utf16le_string(s_desc.str_manufacturer, sizeof(s_desc.str_manufacturer), "Texas Instruments");
    usb_encode_utf16le_string(s_desc.str_product, sizeof(s_desc.str_product), product_buf);
    usb_encode_utf16le_string(s_desc.str_serial, sizeof(s_desc.str_serial), serial_ascii);
    usb_encode_utf16le_string(s_desc.str_ctrl_iface, sizeof(s_desc.str_ctrl_iface), "XDS110 Control Interface");
    usb_encode_utf16le_string(s_desc.str_config, sizeof(s_desc.str_config), "XDS110 Configuration");
    usb_encode_utf16le_string(s_desc.str_cmsis, sizeof(s_desc.str_cmsis), "XDS110 CMSIS-DAP v2");

    usb_descriptors_set_speed(true);
}

void usb_descriptors_set_speed(bool high_speed)
{
    uint16_t active_mps = high_speed ? USB_BULK_MPS_HS : USB_BULK_MPS_FS;
    uint16_t other_mps = high_speed ? USB_BULK_MPS_FS : USB_BULK_MPS_HS;

    usb_patch_bulk_mps(s_desc.config_desc, sizeof(s_desc.config_desc), active_mps);

    memcpy(s_desc.other_speed_desc, s_desc.config_desc, sizeof(s_desc.config_desc));
    s_desc.other_speed_desc[1] = USB_DESC_TYPE_OTHER_SPEED_CFG;
    usb_patch_bulk_mps(s_desc.other_speed_desc, sizeof(s_desc.other_speed_desc), other_mps);
}

bool usb_descriptors_get(uint8_t desc_type, uint8_t desc_index, const uint8_t **out_data, uint16_t *out_len)
{
    switch (desc_type) {
    case USB_DESC_TYPE_DEVICE:
        *out_data = k_device_desc;
        *out_len = (uint16_t)sizeof(k_device_desc);
        return true;

    case USB_DESC_TYPE_CONFIGURATION:
        if (desc_index != 0U) {
            return false;
        }
        *out_data = s_desc.config_desc;
        *out_len = (uint16_t)sizeof(s_desc.config_desc);
        return true;

    case USB_DESC_TYPE_DEV_QUALIFIER:
        *out_data = k_device_qualifier_desc;
        *out_len = (uint16_t)sizeof(k_device_qualifier_desc);
        return true;

    case USB_DESC_TYPE_OTHER_SPEED_CFG:
        if (desc_index != 0U) {
            return false;
        }
        *out_data = s_desc.other_speed_desc;
        *out_len = (uint16_t)sizeof(s_desc.other_speed_desc);
        return true;

    case USB_DESC_TYPE_BOS:
        if (desc_index != 0U) {
            return false;
        }
        *out_data = k_bos_desc;
        *out_len = (uint16_t)sizeof(k_bos_desc);
        return true;

    case USB_DESC_TYPE_STRING:
        if (desc_index >= (sizeof(k_string_table) / sizeof(k_string_table[0]))) {
            return false;
        }
        *out_data = k_string_table[desc_index];
        *out_len = k_string_table[desc_index][0];
        return true;

    default:
        return false;
    }
}

void usb_descriptors_get_ms_os_20(const uint8_t **out_data, uint16_t *out_len)
{
    *out_data = k_ms_os_20_desc_set;
    *out_len = (uint16_t)sizeof(k_ms_os_20_desc_set);
}
