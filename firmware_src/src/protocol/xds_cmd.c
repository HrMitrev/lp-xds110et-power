/*
 * xds_cmd.c - XDS110 Bulk Command Protocol (0x2A framing) and opcode dispatcher.
 *
 * Serves Interface 2 (Bulk OUT 0x02 / Bulk IN 0x83):
 *   - Core session, probe version/power, nSRST, keep-alive, and DFU entry
 *   - EnergyTrace HDR 256 kS/s configuration, calibration, and streaming control
 *   - Target Aux GPIOs (0x29 / 0x2A) and probe-identify LED blink
 *   - Native XDS110 SWD for OpenOCD (`interface/xds110.cfg`, 0x3A) and TI
 *     UniFlash `DSLite` (CMAPI 0x0F..0x18 / 0x3D..0x3E / 0x5A + SACI 0x50..0x51)
 * plus Bulk 1 OUT -> IN byte-reversal loopback when EnergyTrace is idle.
 */

#include "xds_cmd.h"

#include <string.h>

#include "bmp/bmp_mode.h"
#include "board_lp_xds110et.h"
#include "dap/xds_swd.h"
#include "dfu_jump.h"
#include "energytrace/et_hdr.h"
#include "gpio/aux_gpio.h"
#include "usb/usb_device.h"

typedef struct {
    uint8_t fw_version[4];
    bool session_connected;
    uint8_t req_buf[XDS_MAX_FRAME_SIZE];
    uint8_t resp_buf[XDS_MAX_FRAME_SIZE];
    uint8_t loopback_buf[512];
} xds_cmd_state_t;

static xds_cmd_state_t s_xds_cmd = {0};

static void xds_send_response(int32_t err_code, uint16_t data_len)
{
    uint16_t payload_len = (uint16_t)(4U + data_len);
    s_xds_cmd.resp_buf[0] = XDS_FRAME_MAGIC;
    write_u16le(&s_xds_cmd.resp_buf[1], payload_len);
    write_i32le(&s_xds_cmd.resp_buf[3], err_code);

    usb_cmd_tx_write(s_xds_cmd.resp_buf, (uint32_t)(XDS_REQ_HEADER_SIZE + payload_len));
}

static void xds_dispatch_frame(uint8_t opcode, const uint8_t *args, uint16_t args_len)
{
    uint8_t *resp_data = &s_xds_cmd.resp_buf[XDS_RESP_HEADER_SIZE];
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
        s_xds_cmd.session_connected = true;
        break;

    case XDS_CMD_DISCONNECT:
        if (et_hdr_is_running()) {
            (void)et_hdr_cleanup();
        }
        if (xds_swd_is_active()) {
            (void)xds_swd_disconnect();
        }
        s_xds_cmd.session_connected = false;
        break;

    case XDS_CMD_VERSION:
        memcpy(resp_data, s_xds_cmd.fw_version, sizeof(s_xds_cmd.fw_version));
        write_u16le(&resp_data[4], board_get_hw_version());
        resp_len = 6U;
        break;

    case XDS_CMD_SET_TCK:
        if (args_len < 4U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        err = xds_swd_set_tck_delay(read_u32le(args));
        break;

    case XDS_CMD_SET_TRST:
        if ((args_len < 1U) || (args[0] > 1U)) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        break;

    case XDS_CMD_GET_TRST:
        resp_data[0] = 1U;
        resp_len = 1U;
        break;

    case XDS_CMD_CYCLE_TCK:
        if (args_len < 4U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        err = xds_swd_cycle_tck(read_u32le(args));
        break;

    case XDS_CMD_GET_POWER:
        write_u32le(resp_data, BOARD_TARGET_VCC_MV);
        resp_len = 4U;
        break;

    case XDS_CMD_JTAG_GOTO_STATE:
        if (args_len < 8U) {
            err = XDS_ERR_INVALID_PARAM;
        }
        break;

    case XDS_CMD_SET_SRST:
        if ((args_len < 1U) || (args[0] > 1U)) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        board_srst_set(args[0] == 0U);
        break;

    case XDS_CMD_CMAPI_CONNECT: {
        uint32_t idcode = 0U;
        err = xds_swd_cmapi_connect(&idcode);
        write_u32le(resp_data, idcode);
        resp_len = 4U;
        break;
    }

    case XDS_CMD_CMAPI_DISCONNECT:
        err = xds_swd_cmapi_disconnect();
        break;

    case XDS_CMD_CMAPI_ACQUIRE:
        err = xds_swd_cmapi_acquire();
        break;

    case XDS_CMD_CMAPI_RELEASE:
        err = xds_swd_cmapi_release();
        break;

    case XDS_CMD_CMAPI_READ_MEM: {
        if ((args_len != 9U) && (args_len != 13U)) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        bool has_csw = (args_len == 13U);
        uint32_t csw = has_csw ? read_u32le(&args[9]) : 0U;
        uint16_t byte_cnt = read_u16le(&args[7]);
        err = xds_swd_cmapi_read_mem(args[0], args[1], args[2], read_u32le(&args[3]), byte_cnt, has_csw, csw, resp_data,
            XDS_MAX_RESP_DATA_LEN, &resp_len);
        break;
    }

    case XDS_CMD_CMAPI_WRITE_MEM: {
        if (args_len < 9U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        uint16_t byte_cnt = read_u16le(&args[7]);
        uint16_t hdr_len = (uint16_t)(args_len - byte_cnt);
        if ((byte_cnt > args_len) || ((hdr_len != 9U) && (hdr_len != 13U))) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        bool has_csw = (hdr_len == 13U);
        uint32_t csw = has_csw ? read_u32le(&args[9]) : 0U;
        err = xds_swd_cmapi_write_mem(
            args[0], args[1], args[2], read_u32le(&args[3]), byte_cnt, has_csw, csw, &args[hdr_len]);
        break;
    }

    case XDS_CMD_CMAPI_REG_READ: {
        uint32_t reg_val = 0U;
        if (args_len < 3U) {
            err = XDS_ERR_INVALID_PARAM;
        } else {
            err = xds_swd_cmapi_read_reg(args[0], args[1], args[2], &reg_val);
        }
        write_u32le(resp_data, reg_val);
        resp_len = 4U;
        break;
    }

    case XDS_CMD_CMAPI_REG_WRITE:
        if (args_len < 7U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        err = xds_swd_cmapi_write_reg(args[0], args[1], args[2], read_u32le(&args[3]));
        break;

    case XDS_CMD_SWD_CONNECT:
        err = xds_swd_connect();
        break;

    case XDS_CMD_SWD_DISCONNECT:
        err = xds_swd_disconnect();
        break;

    case XDS_CMD_SWO_ROUTE_ENABLE:
    case XDS_CMD_SWO_ROUTE_DISABLE:
        /* TI XDS110 SWO/UART routing toggles; accepted as no-ops on LP-XDS110ET */
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

    case XDS_CMD_CJTAG_CONNECT:
        if (args_len < 4U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        break;

    case XDS_CMD_CJTAG_DISCONNECT:
        break;

    case XDS_CMD_OCD_DAP_REQUEST:
        err = xds_swd_ocd_dap_request(args, args_len, resp_data, XDS_MAX_RESP_DATA_LEN, &resp_len);
        break;

    case XDS_CMD_CMAPI_READ_MEM_64: {
        if ((args_len != 13U) && (args_len != 17U)) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        bool has_csw = (args_len == 17U);
        uint32_t csw = has_csw ? read_u32le(&args[13]) : 0U;
        uint16_t byte_cnt = read_u16le(&args[11]);
        err = xds_swd_cmapi_read_mem(args[0], args[1], args[2], read_u32le(&args[3]), byte_cnt, has_csw, csw, resp_data,
            XDS_MAX_RESP_DATA_LEN, &resp_len);
        break;
    }

    case XDS_CMD_CMAPI_WRITE_MEM_64: {
        if (args_len < 13U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        uint16_t byte_cnt = read_u16le(&args[11]);
        uint16_t hdr_len = (uint16_t)(args_len - byte_cnt);
        if ((byte_cnt > args_len) || ((hdr_len != 13U) && (hdr_len != 17U))) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        bool has_csw = (hdr_len == 17U);
        uint32_t csw = has_csw ? read_u32le(&args[13]) : 0U;
        err = xds_swd_cmapi_write_mem(
            args[0], args[1], args[2], read_u32le(&args[3]), byte_cnt, has_csw, csw, &args[hdr_len]);
        break;
    }

    case XDS_CMD_SET_SUPPLY:
        if (args_len < 5U) {
            err = XDS_ERR_INVALID_PARAM;
        }
        break;

    case XDS_CMD_JTAG_SET_MODE_EX:
        if (args_len < 1U) {
            err = XDS_ERR_INVALID_PARAM;
        }
        break;

    case XDS_CMD_JTAG_SET_FREQ_EX:
        if (args_len < 4U) {
            err = XDS_ERR_INVALID_PARAM;
        }
        break;

    case XDS_CMD_SET_OPTION:
        if ((args_len < 8U) || (args[0] > 5U)) {
            err = XDS_ERR_INVALID_PARAM;
        }
        break;

    case XDS_CMD_PING:
        break;

    case XDS_CMD_SACI_WRITE: {
        if (args_len < 3U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        uint16_t word_cnt = read_u16le(&args[1]);
        if ((uint32_t)args_len != (3U + ((uint32_t)word_cnt * 4U))) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        err = xds_swd_saci_write(args[0], word_cnt, &args[3]);
        break;
    }

    case XDS_CMD_SACI_READ: {
        if (args_len < 3U) {
            err = XDS_ERR_INVALID_PARAM;
            break;
        }
        uint16_t word_cnt = read_u16le(&args[1]);
        err = xds_swd_saci_read(args[0], word_cnt, resp_data, XDS_MAX_RESP_DATA_LEN, &resp_len);
        break;
    }

    case XDS_CMD_CMAPI_CONNECT_EX: {
        uint32_t idcode = 0U;
        if (args_len < 4U) {
            err = XDS_ERR_INVALID_PARAM;
        } else {
            err = xds_swd_cmapi_connect(&idcode);
        }
        write_u32le(resp_data, idcode);
        resp_len = 4U;
        break;
    }

    case XDS_CMD_PROBE_MODE: {
        if ((args_len >= 1U) && (args[0] != PROBE_MODE_QUERY)) {
            err = bmp_mode_set(args[0]);
            if (err != XDS_ERR_OK) {
                break;
            }
        }
        resp_data[0] = bmp_mode_get();
        resp_data[1] = bmp_mode_get_caps();
        resp_len = 2U;
        break;
    }

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

    usb_cmd_rx_read(s_xds_cmd.req_buf, frame_len);

    board_led_red_set(true);
    xds_dispatch_frame(s_xds_cmd.req_buf[3], &s_xds_cmd.req_buf[4], (uint16_t)(payload_len - 1U));
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

    uint32_t chunk = (avail > sizeof(s_xds_cmd.loopback_buf)) ? sizeof(s_xds_cmd.loopback_buf) : avail;
    if (chunk > usb_data_tx_space()) {
        return false;
    }

    uint32_t got = usb_data_rx_read(s_xds_cmd.loopback_buf, chunk);
    if (got == 0U) {
        return false;
    }

    /* Reverse received bytes and echo back on Bulk 1 IN (matches firmware 0x00004DBC) */
    for (uint32_t i = 0U, j = got - 1U; i < j; i++, j--) {
        uint8_t tmp = s_xds_cmd.loopback_buf[i];
        s_xds_cmd.loopback_buf[i] = s_xds_cmd.loopback_buf[j];
        s_xds_cmd.loopback_buf[j] = tmp;
    }

    usb_data_tx_write(s_xds_cmd.loopback_buf, got);
    return true;
}

void xds_cmd_init(const uint8_t fw_version[4])
{
    memcpy(s_xds_cmd.fw_version, fw_version, sizeof(s_xds_cmd.fw_version));
    s_xds_cmd.session_connected = false;
    xds_swd_init();
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
    return s_xds_cmd.session_connected;
}
