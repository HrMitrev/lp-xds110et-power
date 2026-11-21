/*
 * cmsis_dap.c - ARM CMSIS-DAP 1.0 HID command processor for LP-XDS110ET.
 *
 * Uses MSP432E4 DriverLib (MAP_GPIO*) exclusively for SWD/JTAG pin setup on
 * Port A (PA2=SWCLK/TCK, PA3=SWDIO/TMS, PA4=TDO, PA5=TDI, PA6=nSRST) and the
 * U10 SN74AVC4T245 level-shifter direction pin PC5 (XDS_TMS_DIR: HIGH=A->B out,
 * LOW=B->A in).
 */

#include "cmsis_dap.h"

#include <stdio.h>
#include <string.h>

#include "board_lp_xds110et.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "usb/usb_device.h"

/* CMSIS-DAP command IDs */
#define DAP_CMD_INFO               0x00U
#define DAP_CMD_HOST_STATUS        0x01U
#define DAP_CMD_CONNECT            0x02U
#define DAP_CMD_DISCONNECT         0x03U
#define DAP_CMD_TRANSFER_CONFIGURE 0x04U
#define DAP_CMD_WRITE_ABORT        0x08U
#define DAP_CMD_DELAY              0x09U
#define DAP_CMD_RESET_TARGET       0x0AU
#define DAP_CMD_SWJ_PINS           0x10U
#define DAP_CMD_SWJ_CLOCK          0x11U
#define DAP_CMD_SWD_CONFIGURE      0x13U
#define DAP_CMD_INVALID            0xFFU

/* CMSIS-DAP status & port constants */
#define DAP_STATUS_OK    0x00U
#define DAP_STATUS_ERROR 0xFFU

#define DAP_PORT_AUTODETECT 0U
#define DAP_PORT_DISABLED   0U
#define DAP_PORT_SWD        1U

/* DAP_Info IDs */
#define DAP_INFO_VENDOR            0x01U
#define DAP_INFO_PRODUCT           0x02U
#define DAP_INFO_SER_NUM           0x03U
#define DAP_INFO_FW_VER            0x04U
#define DAP_INFO_DEVICE_VENDOR     0x05U
#define DAP_INFO_DEVICE_NAME       0x06U
#define DAP_INFO_BOARD_VENDOR      0x07U
#define DAP_INFO_BOARD_NAME        0x08U
#define DAP_INFO_PRODUCT_FW_VER    0x09U
#define DAP_INFO_CAPABILITIES      0xF0U
#define DAP_INFO_TEST_DOMAIN_TIMER 0xF1U
#define DAP_INFO_PACKET_COUNT      0xFEU
#define DAP_INFO_PACKET_SIZE       0xFFU

/* Capabilities byte 0: bit 0 = SWD supported */
#define DAP_CAP_SWD 0x01U

/* DAP_SWJ_Pins bit definitions */
#define DAP_PIN_SWCLK_TCK 0x01U
#define DAP_PIN_SWDIO_TMS 0x02U
#define DAP_PIN_TDI       0x04U
#define DAP_PIN_TDO       0x08U
#define DAP_PIN_NRESET    0x80U
#define DAP_PIN_READ_MASK (DAP_PIN_SWCLK_TCK | DAP_PIN_SWDIO_TMS | DAP_PIN_TDI | DAP_PIN_TDO | DAP_PIN_NRESET)

/* DAP_HostStatus types */
#define DAP_HOST_STATUS_CONNECT 0U
#define DAP_HOST_STATUS_RUNNING 1U

#define DAP_SWJ_PIN_WAIT_MAX_US 3000000U
#define DAP_DEFAULT_CLOCK_HZ    1000000U
#define DAP_PROTOCOL_VERSION    "1.1.0"

static char g_dap_serial[BOARD_SERIAL_LEN + 1U];
static char g_dap_product_fw_ver[16];

static uint8_t g_dap_port = DAP_PORT_DISABLED;
static bool g_dap_host_connected = false;
static uint32_t g_dap_clock_hz = DAP_DEFAULT_CLOCK_HZ;
static uint8_t g_dap_idle_cycles = 0U;
static uint16_t g_dap_wait_retry = 100U;
static uint16_t g_dap_match_retry = 0U;
static uint8_t g_dap_swd_turnaround = 1U;
static bool g_dap_swd_data_phase = false;

static uint16_t read_u16le(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8U);
}

static uint32_t read_u32le(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8U) | ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
}

static void write_u16le(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
}

/* ---- SWD / JTAG Hardware Pin Control ---- */

static void dap_pins_release(void)
{
    const uint8_t jtag_pins = BOARD_JTAG_TCK_PIN | BOARD_JTAG_TMS_PIN | BOARD_JTAG_TDO_PIN | BOARD_JTAG_TDI_PIN;

    /* Put PA2..PA5 into high-Z input BEFORE switching U10 TMS_DIR LOW (B->A) */
    MAP_GPIOPinTypeGPIOInput(BOARD_JTAG_PORT_BASE, jtag_pins);
    MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, jtag_pins, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);
    MAP_GPIOPinWrite(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, 0U);

    board_srst_set(false);
}

static void dap_pins_connect_swd(void)
{
    /* Switch U10 TMS_DIR HIGH (A->B) before driving PA3 (SWDIO) as output */
    MAP_GPIOPinWrite(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, BOARD_TMS_DIR_PIN);

    const uint8_t out_pins = BOARD_JTAG_TCK_PIN | BOARD_JTAG_TMS_PIN | BOARD_JTAG_TDI_PIN;
    MAP_GPIOPinWrite(BOARD_JTAG_PORT_BASE, out_pins, out_pins);
    MAP_GPIOPinTypeGPIOOutput(BOARD_JTAG_PORT_BASE, out_pins);
    MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, out_pins, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

    MAP_GPIOPinTypeGPIOInput(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TDO_PIN);
    MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TDO_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

    board_srst_set(false);
}

static uint8_t dap_pins_read(void)
{
    const uint8_t read_mask =
        BOARD_JTAG_TCK_PIN | BOARD_JTAG_TMS_PIN | BOARD_JTAG_TDO_PIN | BOARD_JTAG_TDI_PIN | BOARD_TARGET_SRST_PIN;
    uint32_t raw = MAP_GPIOPinRead(BOARD_JTAG_PORT_BASE, read_mask);
    uint8_t pins = 0U;

    if ((raw & BOARD_JTAG_TCK_PIN) != 0U) {
        pins |= DAP_PIN_SWCLK_TCK;
    }
    if ((raw & BOARD_JTAG_TMS_PIN) != 0U) {
        pins |= DAP_PIN_SWDIO_TMS;
    }
    if ((raw & BOARD_JTAG_TDI_PIN) != 0U) {
        pins |= DAP_PIN_TDI;
    }
    if ((raw & BOARD_JTAG_TDO_PIN) != 0U) {
        pins |= DAP_PIN_TDO;
    }
    if ((raw & BOARD_TARGET_SRST_PIN) != 0U) {
        pins |= DAP_PIN_NRESET;
    }
    return pins;
}

/* ---- CMSIS-DAP Command Handlers ---- */

static void dap_write_info_string(uint8_t *resp, const char *str)
{
    uint32_t len = (uint32_t)strlen(str) + 1U;
    if (len > (USB_HID_PKT_SIZE - 2U)) {
        len = USB_HID_PKT_SIZE - 2U;
    }
    resp[1] = (uint8_t)len;
    memcpy(&resp[2], str, len);
    resp[2U + len - 1U] = '\0';
}

static void dap_handle_info(const uint8_t *req, uint8_t *resp)
{
    uint8_t info_id = req[1];
    resp[0] = DAP_CMD_INFO;
    resp[1] = 0U;

    switch (info_id) {
    case DAP_INFO_VENDOR:
        dap_write_info_string(resp, "Texas Instruments");
        break;

    case DAP_INFO_PRODUCT:
        dap_write_info_string(resp, "XDS110 OpenFW CMSIS-DAP");
        break;

    case DAP_INFO_SER_NUM:
        dap_write_info_string(resp, g_dap_serial);
        break;

    case DAP_INFO_FW_VER:
        dap_write_info_string(resp, DAP_PROTOCOL_VERSION);
        break;

    case DAP_INFO_BOARD_VENDOR:
        dap_write_info_string(resp, "Texas Instruments");
        break;

    case DAP_INFO_BOARD_NAME:
        dap_write_info_string(resp, "LP-XDS110ET");
        break;

    case DAP_INFO_PRODUCT_FW_VER:
        dap_write_info_string(resp, g_dap_product_fw_ver);
        break;

    case DAP_INFO_CAPABILITIES:
        resp[1] = 1U;
        resp[2] = DAP_CAP_SWD;
        break;

    case DAP_INFO_PACKET_COUNT:
        resp[1] = 1U;
        resp[2] = USB_HID_QUEUE_DEPTH;
        break;

    case DAP_INFO_PACKET_SIZE:
        resp[1] = 2U;
        write_u16le(&resp[2], USB_HID_PKT_SIZE);
        break;

    case DAP_INFO_DEVICE_VENDOR:
    case DAP_INFO_DEVICE_NAME:
    case DAP_INFO_TEST_DOMAIN_TIMER:
    default:
        resp[1] = 0U;
        break;
    }
}

static void dap_handle_host_status(const uint8_t *req, uint8_t *resp)
{
    uint8_t type = req[1];
    bool active = (req[2] & 0x01U) != 0U;

    resp[0] = DAP_CMD_HOST_STATUS;
    if (type == DAP_HOST_STATUS_CONNECT) {
        g_dap_host_connected = active;
        resp[1] = DAP_STATUS_OK;
    } else if (type == DAP_HOST_STATUS_RUNNING) {
        board_led_red_set(active);
        resp[1] = DAP_STATUS_OK;
    } else {
        resp[1] = DAP_STATUS_ERROR;
    }
}

static void dap_handle_connect(const uint8_t *req, uint8_t *resp)
{
    uint8_t port = req[1];
    resp[0] = DAP_CMD_CONNECT;

    if ((port == DAP_PORT_AUTODETECT) || (port == DAP_PORT_SWD)) {
        dap_pins_connect_swd();
        g_dap_port = DAP_PORT_SWD;
        resp[1] = DAP_PORT_SWD;
    } else {
        g_dap_port = DAP_PORT_DISABLED;
        resp[1] = DAP_PORT_DISABLED;
    }
}

static void dap_handle_disconnect(uint8_t *resp)
{
    dap_pins_release();
    g_dap_port = DAP_PORT_DISABLED;
    resp[0] = DAP_CMD_DISCONNECT;
    resp[1] = DAP_STATUS_OK;
}

static void dap_handle_transfer_configure(const uint8_t *req, uint8_t *resp)
{
    g_dap_idle_cycles = req[1];
    g_dap_wait_retry = read_u16le(&req[2]);
    g_dap_match_retry = read_u16le(&req[4]);

    (void)g_dap_idle_cycles;
    (void)g_dap_wait_retry;
    (void)g_dap_match_retry;

    resp[0] = DAP_CMD_TRANSFER_CONFIGURE;
    resp[1] = DAP_STATUS_OK;
}

static void dap_handle_swj_pins(const uint8_t *req, uint8_t *resp)
{
    uint8_t pin_out = req[1];
    uint8_t pin_sel = req[2];
    uint32_t pin_wait_us = read_u32le(&req[3]);

    if ((pin_sel & DAP_PIN_SWCLK_TCK) != 0U) {
        MAP_GPIOPinWrite(
            BOARD_JTAG_PORT_BASE, BOARD_JTAG_TCK_PIN, ((pin_out & DAP_PIN_SWCLK_TCK) != 0U) ? BOARD_JTAG_TCK_PIN : 0U);
    }
    if ((pin_sel & DAP_PIN_SWDIO_TMS) != 0U) {
        MAP_GPIOPinWrite(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, BOARD_TMS_DIR_PIN);
        MAP_GPIOPinWrite(
            BOARD_JTAG_PORT_BASE, BOARD_JTAG_TMS_PIN, ((pin_out & DAP_PIN_SWDIO_TMS) != 0U) ? BOARD_JTAG_TMS_PIN : 0U);
    }
    if ((pin_sel & DAP_PIN_TDI) != 0U) {
        MAP_GPIOPinWrite(
            BOARD_JTAG_PORT_BASE, BOARD_JTAG_TDI_PIN, ((pin_out & DAP_PIN_TDI) != 0U) ? BOARD_JTAG_TDI_PIN : 0U);
    }
    if ((pin_sel & DAP_PIN_NRESET) != 0U) {
        board_srst_set((pin_out & DAP_PIN_NRESET) == 0U);
    }

    uint8_t check_mask = (uint8_t)(pin_sel & DAP_PIN_READ_MASK);
    if ((pin_wait_us > 0U) && (check_mask != 0U)) {
        if (pin_wait_us > DAP_SWJ_PIN_WAIT_MAX_US) {
            pin_wait_us = DAP_SWJ_PIN_WAIT_MAX_US;
        }
        uint32_t start_ms = board_get_uptime_ms();
        uint32_t wait_ms = (pin_wait_us + 999U) / 1000U;
        while (((dap_pins_read() ^ pin_out) & check_mask) != 0U) {
            if ((uint32_t)(board_get_uptime_ms() - start_ms) >= wait_ms) {
                break;
            }
            board_delay_us(10U);
        }
    }

    resp[0] = DAP_CMD_SWJ_PINS;
    resp[1] = dap_pins_read();
}

static void dap_handle_swj_clock(const uint8_t *req, uint8_t *resp)
{
    uint32_t clock_hz = read_u32le(&req[1]);
    resp[0] = DAP_CMD_SWJ_CLOCK;
    if (clock_hz == 0U) {
        resp[1] = DAP_STATUS_ERROR;
        return;
    }
    g_dap_clock_hz = clock_hz;
    (void)g_dap_clock_hz;
    resp[1] = DAP_STATUS_OK;
}

static void dap_handle_swd_configure(const uint8_t *req, uint8_t *resp)
{
    uint8_t cfg = req[1];
    g_dap_swd_turnaround = (uint8_t)((cfg & 0x03U) + 1U);
    g_dap_swd_data_phase = (cfg & 0x04U) != 0U;

    (void)g_dap_swd_turnaround;
    (void)g_dap_swd_data_phase;

    resp[0] = DAP_CMD_SWD_CONFIGURE;
    resp[1] = DAP_STATUS_OK;
}

static void dap_process_packet(const uint8_t req[USB_HID_PKT_SIZE], uint8_t resp[USB_HID_PKT_SIZE])
{
    memset(resp, 0, USB_HID_PKT_SIZE);
    uint8_t cmd = req[0];

    switch (cmd) {
    case DAP_CMD_INFO:
        dap_handle_info(req, resp);
        break;

    case DAP_CMD_HOST_STATUS:
        dap_handle_host_status(req, resp);
        break;

    case DAP_CMD_CONNECT:
        dap_handle_connect(req, resp);
        break;

    case DAP_CMD_DISCONNECT:
        dap_handle_disconnect(resp);
        break;

    case DAP_CMD_TRANSFER_CONFIGURE:
        dap_handle_transfer_configure(req, resp);
        break;

    case DAP_CMD_WRITE_ABORT:
        resp[0] = DAP_CMD_WRITE_ABORT;
        resp[1] = DAP_STATUS_ERROR;
        break;

    case DAP_CMD_DELAY: {
        uint16_t delay_us = read_u16le(&req[1]);
        board_delay_us(delay_us);
        resp[0] = DAP_CMD_DELAY;
        resp[1] = DAP_STATUS_OK;
        break;
    }

    case DAP_CMD_RESET_TARGET:
        resp[0] = DAP_CMD_RESET_TARGET;
        resp[1] = DAP_STATUS_OK;
        resp[2] = 0U; /* No device-specific reset sequence executed */
        break;

    case DAP_CMD_SWJ_PINS:
        dap_handle_swj_pins(req, resp);
        break;

    case DAP_CMD_SWJ_CLOCK:
        dap_handle_swj_clock(req, resp);
        break;

    case DAP_CMD_SWD_CONFIGURE:
        dap_handle_swd_configure(req, resp);
        break;

    default:
        resp[0] = DAP_CMD_INVALID;
        break;
    }
}

/* ---- Public API ---- */

void cmsis_dap_init(const uint8_t fw_version[4])
{
    board_read_serial(g_dap_serial);
    snprintf(g_dap_product_fw_ver, sizeof(g_dap_product_fw_ver), "%02u.%02u.%02u.%02u", (unsigned)fw_version[3],
        (unsigned)fw_version[2], (unsigned)fw_version[1], (unsigned)fw_version[0]);

    g_dap_port = DAP_PORT_DISABLED;
    g_dap_host_connected = false;
    (void)g_dap_host_connected;
    dap_pins_release();
}

bool cmsis_dap_poll(void)
{
    if (!usb_hid_rx_available() || !usb_hid_tx_space()) {
        return false;
    }

    uint8_t req[USB_HID_PKT_SIZE];
    uint8_t resp[USB_HID_PKT_SIZE];
    if (!usb_hid_rx_pop(req)) {
        return false;
    }

    dap_process_packet(req, resp);
    (void)usb_hid_tx_push(resp);
    return true;
}
