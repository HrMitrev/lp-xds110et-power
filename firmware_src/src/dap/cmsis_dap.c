/*
 * cmsis_dap.c - ARM CMSIS-DAP v2.0.0 WinUSB Bulk command processor for LP-XDS110ET.
 *
 * Implements the CMSIS-DAP v2.0.0 command set over USB Interface 5 (Bulk 0x05/0x86):
 *   - 0x00 DAP_Info, 0x01 DAP_HostStatus, 0x02 DAP_Connect, 0x03 DAP_Disconnect
 *   - 0x04 DAP_TransferConfigure, 0x05 DAP_Transfer, 0x06 DAP_TransferBlock
 *   - 0x07 DAP_TransferAbort, 0x08 DAP_WriteABORT, 0x09 DAP_Delay, 0x0A DAP_ResetTarget
 *   - 0x10 DAP_SWJ_Pins, 0x11 DAP_SWJ_Clock, 0x12 DAP_SWJ_Sequence, 0x13 DAP_SWD_Configure
 *   - 0x7E DAP_QueueCommands, 0x7F DAP_ExecuteCommands (Atomic Commands)
 */

#include "cmsis_dap.h"

#include <stdio.h>
#include <string.h>

#include "bmp/bmp_mode.h"
#include "board_lp_xds110et.h"
#include "swd.h"
#include "usb/usb_device.h"

/* CMSIS-DAP command IDs */
#define DAP_CMD_INFO               0x00U
#define DAP_CMD_HOST_STATUS        0x01U
#define DAP_CMD_CONNECT            0x02U
#define DAP_CMD_DISCONNECT         0x03U
#define DAP_CMD_TRANSFER_CONFIGURE 0x04U
#define DAP_CMD_TRANSFER           0x05U
#define DAP_CMD_TRANSFER_BLOCK     0x06U
#define DAP_CMD_TRANSFER_ABORT     0x07U
#define DAP_CMD_WRITE_ABORT        0x08U
#define DAP_CMD_DELAY              0x09U
#define DAP_CMD_RESET_TARGET       0x0AU
#define DAP_CMD_SWJ_PINS           0x10U
#define DAP_CMD_SWJ_CLOCK          0x11U
#define DAP_CMD_SWJ_SEQUENCE       0x12U
#define DAP_CMD_SWD_CONFIGURE      0x13U
#define DAP_CMD_QUEUE_COMMANDS     0x7EU
#define DAP_CMD_EXECUTE_COMMANDS   0x7FU
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

/* Capabilities byte 0: bit 0 = SWD, bit 4 = Atomic Commands (0x7F/0x7E) */
#define DAP_CAP_SWD             (1U << 0U)
#define DAP_CAP_ATOMIC_COMMANDS (1U << 4U)

/* DAP_Transfer request flags & response status bits */
#define DAP_TRANSFER_APnDP      (1U << 0U)
#define DAP_TRANSFER_RnW        (1U << 1U)
#define DAP_TRANSFER_A2         (1U << 2U)
#define DAP_TRANSFER_A3         (1U << 3U)
#define DAP_TRANSFER_MATCH_VAL  (1U << 4U)
#define DAP_TRANSFER_MATCH_MASK (1U << 5U)

#define DAP_TRANSFER_MISMATCH (1U << 4U)

/* DP register SWD request encodings */
#define DAP_DP_ABORT_WRITE 0x00U /* DP Write A[3:2]=00 */
#define DAP_DP_RDBUFF_READ (DAP_TRANSFER_RnW | DAP_TRANSFER_A2 | DAP_TRANSFER_A3)

/* DAP_HostStatus types */
#define DAP_HOST_STATUS_CONNECT 0U
#define DAP_HOST_STATUS_RUNNING 1U

#define DAP_SWJ_PIN_WAIT_MAX_US 3000000U
#define DAP_PROTOCOL_VERSION    "2.0.0"

static char g_dap_serial[BOARD_SERIAL_LEN + 1U];
static char g_dap_product_fw_ver[16];
static uint8_t g_dap_req_buf[USB_DAP_MAX_PKT_SIZE];
static uint8_t g_dap_resp_buf[USB_DAP_MAX_PKT_SIZE];

static uint8_t g_dap_port = DAP_PORT_DISABLED;
static bool g_dap_host_connected = false;
static uint8_t g_dap_idle_cycles = 0U;
static uint16_t g_dap_wait_retry = 100U;
static uint16_t g_dap_match_retry = 0U;
static uint32_t g_dap_match_mask = 0xFFFFFFFFU;
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

static void write_u32le(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((val >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

/* Execute SWD transfer with CMSIS-DAP WAIT retry loop */
static uint8_t dap_swd_xfer_retry(uint8_t req, uint32_t *data)
{
    uint32_t retry = g_dap_wait_retry;
    uint8_t ack;
    do {
        ack = swd_transfer(req, data);
        if (ack != SWD_ACK_WAIT) {
            break;
        }
    } while (retry-- > 0U);
    return ack;
}

/* Per ARM ADIv5 B4.2.3, ensure >= 8 trailing SWCLK cycles (SWDIO=0) after a final write */
static void dap_swd_flush_trailing_write(void)
{
    if (g_dap_idle_cycles < 8U) {
        swd_idle_cycles(8U - g_dap_idle_cycles);
    }
}

/* ---- CMSIS-DAP Command Handlers ---- */

static uint32_t dap_write_info_string(uint8_t *resp, uint32_t resp_max, const char *str)
{
    if (resp_max < 2U) {
        return 0U;
    }
    uint32_t len = (uint32_t)strlen(str) + 1U;
    if (len > (resp_max - 2U)) {
        len = resp_max - 2U;
    }
    resp[1] = (uint8_t)len;
    if (len > 0U) {
        memcpy(&resp[2], str, len);
        resp[2U + len - 1U] = '\0';
    }
    return 2U + len;
}

static uint32_t dap_handle_info(const uint8_t *req, uint8_t *resp, uint32_t resp_max)
{
    uint8_t info_id = req[1];
    resp[0] = DAP_CMD_INFO;
    resp[1] = 0U;

    switch (info_id) {
    case DAP_INFO_VENDOR:
        return dap_write_info_string(resp, resp_max, "Texas Instruments");

    case DAP_INFO_PRODUCT:
        return dap_write_info_string(resp, resp_max, "XDS110 OpenFW CMSIS-DAP");

    case DAP_INFO_SER_NUM:
        return dap_write_info_string(resp, resp_max, g_dap_serial);

    case DAP_INFO_FW_VER:
        return dap_write_info_string(resp, resp_max, DAP_PROTOCOL_VERSION);

    case DAP_INFO_BOARD_VENDOR:
        return dap_write_info_string(resp, resp_max, "Texas Instruments");

    case DAP_INFO_BOARD_NAME:
        return dap_write_info_string(resp, resp_max, "LP-XDS110ET");

    case DAP_INFO_PRODUCT_FW_VER:
        return dap_write_info_string(resp, resp_max, g_dap_product_fw_ver);

    case DAP_INFO_CAPABILITIES:
        if (resp_max >= 3U) {
            resp[1] = 1U;
            resp[2] = DAP_CAP_SWD | DAP_CAP_ATOMIC_COMMANDS;
            return 3U;
        }
        return 2U;

    case DAP_INFO_PACKET_COUNT:
        if (resp_max >= 3U) {
            resp[1] = 1U;
            resp[2] = USB_DAP_QUEUE_DEPTH;
            return 3U;
        }
        return 2U;

    case DAP_INFO_PACKET_SIZE:
        if (resp_max >= 4U) {
            resp[1] = 2U;
            write_u16le(&resp[2], usb_dap_packet_size());
            return 4U;
        }
        return 2U;

    case DAP_INFO_DEVICE_VENDOR:
    case DAP_INFO_DEVICE_NAME:
    case DAP_INFO_TEST_DOMAIN_TIMER:
    default:
        resp[1] = 0U;
        return 2U;
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

    if (bmp_mode_is_active()) {
        g_dap_port = DAP_PORT_DISABLED;
        resp[1] = DAP_PORT_DISABLED;
        return;
    }

    if ((port == DAP_PORT_AUTODETECT) || (port == DAP_PORT_SWD)) {
        swd_connect();
        g_dap_port = DAP_PORT_SWD;
        resp[1] = DAP_PORT_SWD;
    } else {
        g_dap_port = DAP_PORT_DISABLED;
        resp[1] = DAP_PORT_DISABLED;
    }
}

void cmsis_dap_force_disconnect(void)
{
    if (g_dap_port != DAP_PORT_DISABLED) {
        swd_disconnect();
        g_dap_port = DAP_PORT_DISABLED;
    }
}

static void dap_handle_disconnect(uint8_t *resp)
{
    swd_disconnect();
    g_dap_port = DAP_PORT_DISABLED;
    resp[0] = DAP_CMD_DISCONNECT;
    resp[1] = DAP_STATUS_OK;
}

static void dap_handle_transfer_configure(const uint8_t *req, uint8_t *resp)
{
    g_dap_idle_cycles = req[1];
    g_dap_wait_retry = read_u16le(&req[2]);
    g_dap_match_retry = read_u16le(&req[4]);

    swd_configure(g_dap_swd_turnaround, g_dap_swd_data_phase, g_dap_idle_cycles);

    resp[0] = DAP_CMD_TRANSFER_CONFIGURE;
    resp[1] = DAP_STATUS_OK;
}

static void dap_handle_transfer(const uint8_t *req, uint32_t req_max, uint8_t *resp, uint32_t resp_max,
    uint32_t *out_req_used, uint32_t *out_resp_used)
{
    uint8_t xfer_count = req[2];
    uint32_t req_off = 3U;
    uint32_t resp_off = 3U;
    uint8_t resp_count = 0U;
    uint8_t resp_val = 0U;
    bool post_read = false;
    bool last_was_write = false;
    uint8_t executed = 0U;

    resp[0] = DAP_CMD_TRANSFER;

    if (g_dap_port == DAP_PORT_SWD) {
        for (; executed < xfer_count; executed++) {
            if (req_off >= req_max) {
                break;
            }
            uint8_t xfer_req = req[req_off++];

            if ((xfer_req & DAP_TRANSFER_RnW) != 0U) {
                last_was_write = false;
                /* Read transfer */
                if (post_read) {
                    if (((xfer_req & DAP_TRANSFER_APnDP) != 0U) && ((xfer_req & DAP_TRANSFER_MATCH_VAL) == 0U)) {
                        /* Pipelined AP read: returns previous AP read while posting new one */
                        uint32_t val = 0U;
                        resp_val = dap_swd_xfer_retry(xfer_req, &val);
                        if (resp_val != SWD_ACK_OK) {
                            post_read = false;
                            executed++;
                            break;
                        }
                        if ((resp_off + 4U) <= resp_max) {
                            write_u32le(&resp[resp_off], val);
                            resp_off += 4U;
                        }
                        resp_count++;
                        continue;
                    }

                    /* Flush previous posted AP read before DP read or value-match read */
                    uint32_t prev_val = 0U;
                    resp_val = dap_swd_xfer_retry(DAP_DP_RDBUFF_READ, &prev_val);
                    post_read = false;
                    if (resp_val != SWD_ACK_OK) {
                        if ((xfer_req & DAP_TRANSFER_MATCH_VAL) != 0U) {
                            req_off = ((req_off + 4U) <= req_max) ? (req_off + 4U) : req_max;
                        }
                        executed++;
                        break;
                    }
                    if ((resp_off + 4U) <= resp_max) {
                        write_u32le(&resp[resp_off], prev_val);
                        resp_off += 4U;
                    }
                    resp_count++;
                }

                if ((xfer_req & DAP_TRANSFER_MATCH_VAL) != 0U) {
                    if ((req_off + 4U) > req_max) {
                        executed++;
                        break;
                    }
                    uint32_t match_val = read_u32le(&req[req_off]);
                    req_off += 4U;

                    if ((xfer_req & DAP_TRANSFER_APnDP) != 0U) {
                        /* Post initial AP read before polling */
                        resp_val = dap_swd_xfer_retry(xfer_req, NULL);
                        if (resp_val != SWD_ACK_OK) {
                            executed++;
                            break;
                        }
                    }

                    uint32_t val = 0U;
                    uint32_t retry = g_dap_match_retry;
                    do {
                        resp_val = dap_swd_xfer_retry(xfer_req, &val);
                        if (resp_val != SWD_ACK_OK) {
                            break;
                        }
                        if ((val & g_dap_match_mask) == match_val) {
                            break;
                        }
                    } while (retry-- > 0U);

                    if (resp_val != SWD_ACK_OK) {
                        executed++;
                        break;
                    }
                    if ((val & g_dap_match_mask) != match_val) {
                        resp_val |= DAP_TRANSFER_MISMATCH;
                        executed++;
                        break;
                    }
                } else if ((xfer_req & DAP_TRANSFER_APnDP) != 0U) {
                    /* Initial AP read: post it and count it once its data is collected */
                    resp_val = dap_swd_xfer_retry(xfer_req, NULL);
                    if (resp_val != SWD_ACK_OK) {
                        executed++;
                        break;
                    }
                    post_read = true;
                    continue;
                } else {
                    /* Direct DP read */
                    uint32_t val = 0U;
                    resp_val = dap_swd_xfer_retry(xfer_req, &val);
                    if (resp_val != SWD_ACK_OK) {
                        executed++;
                        break;
                    }
                    if ((resp_off + 4U) <= resp_max) {
                        write_u32le(&resp[resp_off], val);
                        resp_off += 4U;
                    }
                }
            } else {
                /* Write transfer (or MATCH_MASK pseudo-write) */
                if (post_read) {
                    uint32_t prev_val = 0U;
                    resp_val = dap_swd_xfer_retry(DAP_DP_RDBUFF_READ, &prev_val);
                    post_read = false;
                    if (resp_val != SWD_ACK_OK) {
                        req_off = ((req_off + 4U) <= req_max) ? (req_off + 4U) : req_max;
                        executed++;
                        break;
                    }
                    if ((resp_off + 4U) <= resp_max) {
                        write_u32le(&resp[resp_off], prev_val);
                        resp_off += 4U;
                    }
                    resp_count++;
                }

                if ((req_off + 4U) > req_max) {
                    executed++;
                    break;
                }
                uint32_t wdata = read_u32le(&req[req_off]);
                req_off += 4U;

                if ((xfer_req & DAP_TRANSFER_MATCH_MASK) != 0U) {
                    g_dap_match_mask = wdata;
                    resp_val = SWD_ACK_OK;
                } else {
                    resp_val = dap_swd_xfer_retry(xfer_req, &wdata);
                    if (resp_val != SWD_ACK_OK) {
                        last_was_write = false;
                        executed++;
                        break;
                    }
                    last_was_write = true;
                }
            }

            resp_count++;
        }

        /* Flush final posted AP read if the loop completed cleanly */
        if (post_read && (resp_val == SWD_ACK_OK)) {
            uint32_t final_val = 0U;
            resp_val = dap_swd_xfer_retry(DAP_DP_RDBUFF_READ, &final_val);
            if (resp_val == SWD_ACK_OK) {
                if ((resp_off + 4U) <= resp_max) {
                    write_u32le(&resp[resp_off], final_val);
                    resp_off += 4U;
                }
                resp_count++;
            }
        } else if (last_was_write && (resp_val == SWD_ACK_OK)) {
            dap_swd_flush_trailing_write();
        }
    }

    /* Skip any unexecuted request entries so DAP_ExecuteCommands stays aligned */
    while ((executed < xfer_count) && (req_off < req_max)) {
        uint8_t skipped = req[req_off++];
        if (((skipped & DAP_TRANSFER_RnW) == 0U) || ((skipped & DAP_TRANSFER_MATCH_VAL) != 0U)) {
            req_off = ((req_off + 4U) <= req_max) ? (req_off + 4U) : req_max;
        }
        executed++;
    }

    resp[1] = resp_count;
    resp[2] = resp_val;
    *out_req_used = req_off;
    *out_resp_used = resp_off;
}

static void dap_handle_transfer_block(const uint8_t *req, uint32_t req_max, uint8_t *resp, uint32_t resp_max,
    uint32_t *out_req_used, uint32_t *out_resp_used)
{
    uint16_t xfer_count = read_u16le(&req[2]);
    uint8_t xfer_req = req[4];
    uint16_t resp_count = 0U;
    uint8_t resp_val = 0U;
    uint32_t req_off = 5U;
    uint32_t resp_off = 4U;

    resp[0] = DAP_CMD_TRANSFER_BLOCK;

    if ((g_dap_port == DAP_PORT_SWD) && (xfer_count > 0U)) {
        if ((xfer_req & DAP_TRANSFER_RnW) != 0U) {
            /* Block Read */
            if ((xfer_req & DAP_TRANSFER_APnDP) != 0U) {
                resp_val = dap_swd_xfer_retry(xfer_req, NULL);
                if (resp_val == SWD_ACK_OK) {
                    for (uint16_t i = 0U; i < xfer_count; i++) {
                        if ((resp_off + 4U) > resp_max) {
                            break;
                        }
                        uint8_t wire_req = ((i + 1U) < xfer_count) ? xfer_req : DAP_DP_RDBUFF_READ;
                        uint32_t val = 0U;
                        resp_val = dap_swd_xfer_retry(wire_req, &val);
                        if (resp_val != SWD_ACK_OK) {
                            break;
                        }
                        write_u32le(&resp[resp_off], val);
                        resp_off += 4U;
                        resp_count++;
                    }
                }
            } else {
                for (uint16_t i = 0U; i < xfer_count; i++) {
                    if ((resp_off + 4U) > resp_max) {
                        break;
                    }
                    uint32_t val = 0U;
                    resp_val = dap_swd_xfer_retry(xfer_req, &val);
                    if (resp_val != SWD_ACK_OK) {
                        break;
                    }
                    write_u32le(&resp[resp_off], val);
                    resp_off += 4U;
                    resp_count++;
                }
            }
        } else {
            /* Block Write */
            for (uint16_t i = 0U; i < xfer_count; i++) {
                if ((req_off + 4U) > req_max) {
                    break;
                }
                uint32_t wdata = read_u32le(&req[req_off]);
                req_off += 4U;
                resp_val = dap_swd_xfer_retry(xfer_req, &wdata);
                if (resp_val != SWD_ACK_OK) {
                    break;
                }
                resp_count++;
            }
            if ((resp_count > 0U) && (resp_val == SWD_ACK_OK)) {
                dap_swd_flush_trailing_write();
            }
        }
    }

    if ((xfer_req & DAP_TRANSFER_RnW) == 0U) {
        /* Always advance past the full write payload for DAP_ExecuteCommands */
        uint32_t full_req = 5U + ((uint32_t)xfer_count * 4U);
        req_off = (full_req <= req_max) ? full_req : req_max;
    }

    write_u16le(&resp[1], resp_count);
    resp[3] = resp_val;
    *out_req_used = req_off;
    *out_resp_used = resp_off;
}

static void dap_handle_write_abort(const uint8_t *req, uint8_t *resp)
{
    resp[0] = DAP_CMD_WRITE_ABORT;
    if (g_dap_port != DAP_PORT_SWD) {
        resp[1] = DAP_STATUS_ERROR;
        return;
    }

    uint32_t abort_val = read_u32le(&req[2]);
    (void)swd_transfer(DAP_DP_ABORT_WRITE, &abort_val);
    dap_swd_flush_trailing_write();
    resp[1] = DAP_STATUS_OK;
}

static void dap_handle_swj_pins(const uint8_t *req, uint8_t *resp)
{
    uint8_t pin_out = req[1];
    uint8_t pin_sel = req[2];
    uint32_t pin_wait_us = read_u32le(&req[3]);

    swd_pins_write(pin_out, pin_sel);

    uint8_t check_mask = (uint8_t)(pin_sel & SWD_PIN_READ_MASK);
    if ((pin_wait_us > 0U) && (check_mask != 0U)) {
        if (pin_wait_us > DAP_SWJ_PIN_WAIT_MAX_US) {
            pin_wait_us = DAP_SWJ_PIN_WAIT_MAX_US;
        }
        uint32_t start_ms = board_get_uptime_ms();
        uint32_t wait_ms = (pin_wait_us + 999U) / 1000U;
        while (((swd_pins_read() ^ pin_out) & check_mask) != 0U) {
            if ((uint32_t)(board_get_uptime_ms() - start_ms) >= wait_ms) {
                break;
            }
            board_delay_us(10U);
        }
    }

    resp[0] = DAP_CMD_SWJ_PINS;
    resp[1] = swd_pins_read();
}

static void dap_handle_swj_clock(const uint8_t *req, uint8_t *resp)
{
    uint32_t clock_hz = read_u32le(&req[1]);
    resp[0] = DAP_CMD_SWJ_CLOCK;
    if (clock_hz == 0U) {
        resp[1] = DAP_STATUS_ERROR;
        return;
    }
    swd_set_clock_hz(clock_hz);
    resp[1] = DAP_STATUS_OK;
}

static uint32_t dap_handle_swj_sequence(const uint8_t *req, uint32_t req_max, uint8_t *resp)
{
    uint32_t bit_count = (req[1] == 0U) ? 256U : (uint32_t)req[1];
    uint32_t byte_count = (bit_count + 7U) >> 3U;
    resp[0] = DAP_CMD_SWJ_SEQUENCE;

    if ((2U + byte_count) > req_max) {
        resp[1] = DAP_STATUS_ERROR;
        return req_max;
    }

    swd_sequence(bit_count, &req[2]);
    resp[1] = DAP_STATUS_OK;
    return 2U + byte_count;
}

static void dap_handle_swd_configure(const uint8_t *req, uint8_t *resp)
{
    uint8_t cfg = req[1];
    g_dap_swd_turnaround = (uint8_t)((cfg & 0x03U) + 1U);
    g_dap_swd_data_phase = (cfg & 0x04U) != 0U;

    swd_configure(g_dap_swd_turnaround, g_dap_swd_data_phase, g_dap_idle_cycles);

    resp[0] = DAP_CMD_SWD_CONFIGURE;
    resp[1] = DAP_STATUS_OK;
}

/*
 * Execute a single non-compound CMSIS-DAP command.
 * Returns true if the command was recognized (false for DAP_CMD_INVALID or truncated input).
 */
static bool dap_process_single(const uint8_t *req, uint32_t req_max, uint8_t *resp, uint32_t resp_max,
    uint32_t *out_req_used, uint32_t *out_resp_used)
{
    if ((req_max < 1U) || (resp_max < 1U)) {
        *out_req_used = req_max;
        *out_resp_used = 0U;
        return false;
    }

    uint8_t cmd = req[0];
    switch (cmd) {
    case DAP_CMD_INFO:
        if ((req_max < 2U) || (resp_max < 2U)) {
            break;
        }
        *out_req_used = 2U;
        *out_resp_used = dap_handle_info(req, resp, resp_max);
        return true;

    case DAP_CMD_HOST_STATUS:
        if ((req_max < 3U) || (resp_max < 2U)) {
            break;
        }
        dap_handle_host_status(req, resp);
        *out_req_used = 3U;
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_CONNECT:
        if ((req_max < 2U) || (resp_max < 2U)) {
            break;
        }
        dap_handle_connect(req, resp);
        *out_req_used = 2U;
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_DISCONNECT:
        if (resp_max < 2U) {
            break;
        }
        dap_handle_disconnect(resp);
        *out_req_used = 1U;
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_TRANSFER_CONFIGURE:
        if ((req_max < 6U) || (resp_max < 2U)) {
            break;
        }
        dap_handle_transfer_configure(req, resp);
        *out_req_used = 6U;
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_TRANSFER:
        if ((req_max < 3U) || (resp_max < 3U)) {
            break;
        }
        dap_handle_transfer(req, req_max, resp, resp_max, out_req_used, out_resp_used);
        return true;

    case DAP_CMD_TRANSFER_BLOCK:
        if ((req_max < 5U) || (resp_max < 4U)) {
            break;
        }
        dap_handle_transfer_block(req, req_max, resp, resp_max, out_req_used, out_resp_used);
        return true;

    case DAP_CMD_TRANSFER_ABORT:
        /* Per CMSIS-DAP specification, DAP_TransferAbort produces no response */
        *out_req_used = 1U;
        *out_resp_used = 0U;
        return true;

    case DAP_CMD_WRITE_ABORT:
        if ((req_max < 6U) || (resp_max < 2U)) {
            break;
        }
        dap_handle_write_abort(req, resp);
        *out_req_used = 6U;
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_DELAY:
        if ((req_max < 3U) || (resp_max < 2U)) {
            break;
        }
        board_delay_us(read_u16le(&req[1]));
        resp[0] = DAP_CMD_DELAY;
        resp[1] = DAP_STATUS_OK;
        *out_req_used = 3U;
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_RESET_TARGET:
        if (resp_max < 3U) {
            break;
        }
        resp[0] = DAP_CMD_RESET_TARGET;
        resp[1] = DAP_STATUS_OK;
        resp[2] = 0U; /* No device-specific reset sequence executed */
        *out_req_used = 1U;
        *out_resp_used = 3U;
        return true;

    case DAP_CMD_SWJ_PINS:
        if ((req_max < 7U) || (resp_max < 2U)) {
            break;
        }
        dap_handle_swj_pins(req, resp);
        *out_req_used = 7U;
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_SWJ_CLOCK:
        if ((req_max < 5U) || (resp_max < 2U)) {
            break;
        }
        dap_handle_swj_clock(req, resp);
        *out_req_used = 5U;
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_SWJ_SEQUENCE:
        if ((req_max < 2U) || (resp_max < 2U)) {
            break;
        }
        *out_req_used = dap_handle_swj_sequence(req, req_max, resp);
        *out_resp_used = 2U;
        return true;

    case DAP_CMD_SWD_CONFIGURE:
        if ((req_max < 2U) || (resp_max < 2U)) {
            break;
        }
        dap_handle_swd_configure(req, resp);
        *out_req_used = 2U;
        *out_resp_used = 2U;
        return true;

    default:
        break;
    }

    resp[0] = DAP_CMD_INVALID;
    *out_req_used = 1U;
    *out_resp_used = 1U;
    return false;
}

/* Returns exact number of response bytes to send (0 if no response, e.g. TransferAbort) */
static uint16_t dap_process_packet(const uint8_t *req, uint32_t req_len, uint8_t *resp, uint32_t resp_max)
{
    if ((req_len == 0U) || (resp_max == 0U)) {
        return 0U;
    }

    uint8_t cmd = req[0];
    if ((cmd == DAP_CMD_EXECUTE_COMMANDS) || (cmd == DAP_CMD_QUEUE_COMMANDS)) {
        if ((req_len < 2U) || (resp_max < 2U)) {
            resp[0] = DAP_CMD_INVALID;
            return 1U;
        }
        uint8_t num_cmds = req[1];
        uint32_t req_off = 2U;
        uint32_t resp_off = 2U;
        uint8_t executed = 0U;

        resp[0] = cmd;
        while ((executed < num_cmds) && (req_off < req_len) && (resp_off < resp_max)) {
            uint32_t sub_req = 0U;
            uint32_t sub_resp = 0U;
            bool ok = dap_process_single(
                &req[req_off], req_len - req_off, &resp[resp_off], resp_max - resp_off, &sub_req, &sub_resp);
            req_off += sub_req;
            resp_off += sub_resp;
            executed++;
            if (!ok) {
                break;
            }
        }
        resp[1] = executed;
        return (uint16_t)resp_off;
    }

    uint32_t req_used = 0U;
    uint32_t resp_used = 0U;
    (void)dap_process_single(req, req_len, resp, resp_max, &req_used, &resp_used);
    return (uint16_t)resp_used;
}

/* ---- Public API ---- */

void cmsis_dap_init(const uint8_t fw_version[4])
{
    board_read_serial(g_dap_serial);
    snprintf(g_dap_product_fw_ver, sizeof(g_dap_product_fw_ver), "%02x.%02x.%02x.%02x", (unsigned)fw_version[3],
        (unsigned)fw_version[2], (unsigned)fw_version[1], (unsigned)fw_version[0]);

    g_dap_port = DAP_PORT_DISABLED;
    g_dap_host_connected = false;
    g_dap_idle_cycles = 0U;
    g_dap_wait_retry = 100U;
    g_dap_match_retry = 0U;
    g_dap_match_mask = 0xFFFFFFFFU;
    g_dap_swd_turnaround = 1U;
    g_dap_swd_data_phase = false;
    (void)g_dap_host_connected;

    swd_init();
}

bool cmsis_dap_poll(void)
{
    if (!usb_dap_rx_available() || !usb_dap_tx_space()) {
        return false;
    }

    uint16_t max_pkt = usb_dap_packet_size();
    uint16_t req_len = usb_dap_rx_pop(g_dap_req_buf, max_pkt);
    if (req_len == 0U) {
        return false;
    }

    uint16_t resp_len = dap_process_packet(g_dap_req_buf, req_len, g_dap_resp_buf, max_pkt);
    if (resp_len > 0U) {
        (void)usb_dap_tx_push(g_dap_resp_buf, resp_len);
    }
    return true;
}
