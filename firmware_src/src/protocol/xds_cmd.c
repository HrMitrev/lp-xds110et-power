/*
 * xds_cmd.c - XDS110 Bulk Command Protocol framing and core opcode handlers.
 *
 * Implements Phase 1B core control commands:
 *   0x00 XDS_ECHO       - Echo test payload
 *   0x01 XDS_CONNECT    - Open debug session
 *   0x02 XDS_DISCONNECT - Close debug / EnergyTrace session
 *   0x03 XDS_VERSION    - Firmware version (4 B) + HW ID (u16le)
 *   0x08 XDS_GET_POWER  - Target supply voltage (3300 mV)
 *   0x0E XDS_SET_SRST   - Assert (0) / deassert (1) target nSRST on PA6
 *   0x26 XDS_ENTER_DFU  - Tear down USB and jump to TI Flash DFU bootloader
 *   0x28 XDS_CONNECT_ET - Open EnergyTrace session
 *   0x4A XDS_PING       - Session keep-alive
 * plus Bulk 1 OUT -> IN byte-reversal loopback (matching firmware 0x00004DBC).
 */

#include "xds_cmd.h"

#include <string.h>

#include "board_lp_xds110et.h"
#include "dfu_jump.h"
#include "energytrace/et_hdr.h"
#include "gpio/aux_gpio.h"
#include "usb/usb_device.h"

static uint8_t g_fw_version[4];
static bool g_session_connected = false;

static uint8_t g_req_buf[XDS_MAX_FRAME_SIZE];
static uint8_t g_resp_buf[XDS_MAX_FRAME_SIZE];
static uint8_t g_loopback_buf[512];

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

static void write_u32le(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((val >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

static void write_i32le(uint8_t *dst, int32_t val)
{
    write_u32le(dst, (uint32_t)val);
}

static void xds_send_response(int32_t err_code, uint16_t data_len)
{
    uint16_t payload_len = (uint16_t)(4U + data_len);
    g_resp_buf[0] = XDS_FRAME_MAGIC;
    write_u16le(&g_resp_buf[1], payload_len);
    write_i32le(&g_resp_buf[3], err_code);

    usb_cmd_tx_write(g_resp_buf, (uint32_t)(XDS_REQ_HEADER_SIZE + payload_len));
}

static void xds_dispatch_frame(uint8_t opcode, const uint8_t *args, uint16_t args_len)
{
    uint8_t *resp_data = &g_resp_buf[XDS_RESP_HEADER_SIZE];
    int32_t err = XDS_ERR_OK;
    uint16_t resp_len = 0U;

    switch (opcode) {
    case XDS_CMD_ECHO: {
        if (args_len < 4U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        uint32_t echo_len = read_u32le(args);
        if ((echo_len > (uint32_t)(args_len - 4U)) || (echo_len > XDS_MAX_RESP_DATA_LEN)) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        if (echo_len > 0U) {
            memcpy(resp_data, &args[4], echo_len);
        }
        resp_len = (uint16_t)echo_len;
        break;
    }

    case XDS_CMD_CONNECT:
    case XDS_CMD_CONNECT_ET:
        g_session_connected = true;
        break;

    case XDS_CMD_DISCONNECT:
        if (et_hdr_is_running()) {
            (void)et_hdr_cleanup();
        }
        g_session_connected = false;
        break;

    case XDS_CMD_VERSION:
        memcpy(resp_data, g_fw_version, sizeof(g_fw_version));
        write_u16le(&resp_data[4], board_get_hw_version());
        resp_len = 6U;
        break;

    case XDS_CMD_GET_POWER:
        write_u32le(resp_data, BOARD_TARGET_VCC_MV);
        resp_len = 4U;
        break;

    case XDS_CMD_SET_SRST:
        if ((args_len < 1U) || (args[0] > 1U)) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        board_srst_set(args[0] == 0U);
        break;

    case XDS_CMD_ET_HWINFO: {
        uint32_t hw_type = 0U;
        uint8_t caps = 0U;
        err = et_hdr_get_hw_info(&hw_type, &caps);
        write_u32le(resp_data, hw_type);
        resp_data[4] = caps;
        resp_len = 5U;
        break;
    }

    case XDS_CMD_ET_RANGE:
        if (args_len < 1U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        err = et_hdr_set_range(args[0]);
        break;

    case XDS_CMD_ET_DIG:
        if (args_len < 1U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        err = et_hdr_set_dig(args[0]);
        break;

    case XDS_CMD_ET_DIG_ARGS:
        if (args_len < 1U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        break;

    case XDS_CMD_ET_SETUP:
        if (args_len < 7U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        err = et_hdr_setup(args[0], args[1], read_u32le(&args[2]), args[6]);
        break;

    case XDS_CMD_ET_CAL: {
        if (args_len < 2U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        int32_t cal_sum = 0;
        uint32_t cal_cnt = 0U;
        err = et_hdr_get_calibration(read_u16le(args), &cal_sum, &cal_cnt);
        write_i32le(resp_data, cal_sum);
        write_u32le(&resp_data[4], cal_cnt);
        resp_len = 8U;
        break;
    }

    case XDS_CMD_ET_START:
        err = et_hdr_start();
        break;

    case XDS_CMD_ET_STOP:
        err = et_hdr_stop();
        break;

    case XDS_CMD_ET_CLEANUP:
        err = et_hdr_cleanup();
        break;

    case XDS_CMD_ENTER_DFU:
        (void)et_hdr_cleanup();
        /* No USB reply is sent; host expects immediate disconnect and DFU re-enumeration */
        dfu_jump_enter();
        break;

    case XDS_CMD_GPIO_GET: {
        if (args_len < 4U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        uint16_t gpio_val = 0U;
        err = aux_gpio_get(read_u32le(args), &gpio_val);
        write_u16le(resp_data, gpio_val);
        resp_len = 2U;
        break;
    }

    case XDS_CMD_GPIO_SET:
        if (args_len < 6U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        err = aux_gpio_set(read_u32le(args), read_u16le(&args[4]));
        break;

    case XDS_CMD_PING:
        break;

    default:
        err = XDS_ERR_UNSUPPORTED;
        break;
    }

    xds_send_response(err, resp_len);
}

static bool xds_poll_cmd_channel(void)
{
    uint32_t avail = usb_cmd_rx_available();
    if (avail == 0U) {
        return false;
    }

    /* Resynchronize on 0x2A ('*') magic byte */
    uint8_t hdr[XDS_REQ_HEADER_SIZE];
    usb_cmd_rx_peek(hdr, 1U);
    if (hdr[0] != XDS_FRAME_MAGIC) {
        uint8_t discard = 0U;
        usb_cmd_rx_read(&discard, 1U);
        return true;
    }

    if (avail < XDS_REQ_HEADER_SIZE) {
        return false;
    }

    usb_cmd_rx_peek(hdr, XDS_REQ_HEADER_SIZE);
    uint16_t payload_len = read_u16le(&hdr[1]);
    uint32_t frame_len = (uint32_t)XDS_REQ_HEADER_SIZE + (uint32_t)payload_len;

    if ((payload_len < 1U) || (frame_len > XDS_MAX_FRAME_SIZE)) {
        /* Reject malformed frame header and resync from next byte */
        uint8_t discard = 0U;
        usb_cmd_rx_read(&discard, 1U);
        return true;
    }

    if (avail < frame_len) {
        return false;
    }

    usb_cmd_rx_read(g_req_buf, frame_len);

    board_led_red_set(true);
    xds_dispatch_frame(g_req_buf[3], &g_req_buf[4], (uint16_t)(payload_len - 1U));
    board_led_red_set(false);
    return true;
}

static bool xds_poll_data_loopback(void)
{
    if (et_hdr_is_running()) {
        return false;
    }

    uint32_t avail = usb_data_rx_available();
    if (avail == 0U) {
        return false;
    }

    uint32_t chunk = (avail > sizeof(g_loopback_buf)) ? sizeof(g_loopback_buf) : avail;
    if (chunk > usb_data_tx_space()) {
        return false;
    }

    uint32_t got = usb_data_rx_read(g_loopback_buf, chunk);
    if (got == 0U) {
        return false;
    }

    /* Reverse received bytes and echo back on Bulk 1 IN (matches firmware 0x00004DBC) */
    for (uint32_t i = 0U, j = got - 1U; i < j; i++, j--) {
        uint8_t tmp = g_loopback_buf[i];
        g_loopback_buf[i] = g_loopback_buf[j];
        g_loopback_buf[j] = tmp;
    }

    usb_data_tx_write(g_loopback_buf, got);
    return true;
}

void xds_cmd_init(const uint8_t fw_version[4])
{
    memcpy(g_fw_version, fw_version, sizeof(g_fw_version));
    g_session_connected = false;
}

bool xds_cmd_poll(void)
{
    bool did_work = false;
    if (xds_poll_cmd_channel()) {
        did_work = true;
    }
    if (xds_poll_data_loopback()) {
        did_work = true;
    }
    return did_work;
}

bool xds_cmd_is_connected(void)
{
    return g_session_connected;
}
