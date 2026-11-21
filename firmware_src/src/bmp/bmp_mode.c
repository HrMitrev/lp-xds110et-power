/*
 * bmp_mode.c - Runtime probe mode manager & cooperative Black Magic Debug bridge.
 *
 * Replaces upstream blackmagic/src/main.c so LP-XDS110ET retains ownership of
 * main() while serving BMP's GDB RSP server cleanly over CDC1 (ttyACM1).
 */

#include "bmp_mode.h"

#include <stddef.h>
#include <string.h>

#include "board_lp_xds110et.h"
#include "dap/cmsis_dap.h"
#include "dap/swd.h"
#include "dap/xds_swd.h"
#include "protocol/xds_cmd.h"
#include "usb/usb_device.h"

#if (FW_BMP_GDB_COMPILED != 0)
#include "command.h"
#include "exception.h"
#include "gdb_if.h"
#include "gdb_main.h"
#include "gdb_packet.h"
#include "general.h"
#include "jtagtap.h"
#include "morse.h"
#include "platform.h"
#ifdef ENABLE_RTT
#include "rtt.h"
#include "rtt_if.h"
#endif
#include "swd.h"
#include "target.h"
#endif

/* Peek window & partial-prefix timeout for GDB/BMP handshake auto-detection on CDC1 */
#define BMP_AUTODETECT_PEEK_MAX   32U
#define BMP_AUTODETECT_CTRL_MAX   4U
#define BMP_AUTODETECT_TIMEOUT_MS 200U

typedef enum {
    GDB_DETECT_NONE = 0,
    GDB_DETECT_PARTIAL = 1,
    GDB_DETECT_MATCH = 2,
} gdb_detect_result_t;

typedef struct {
    uint8_t mode;
    bmp_bg_poll_fn_t bg_poll;
    bool detect_pending;
    uint32_t detect_start_ms;
} bmp_mode_state_t;

static bmp_mode_state_t s_bmp_mode = {
    .mode = (FW_DEFAULT_BMP_MODE != 0U) ? PROBE_MODE_BMP : PROBE_MODE_NORMAL,
    .bg_poll = NULL,
};

static void bmp_mode_enter_bmp(bool flush_rx)
{
    /* Release any active Normal-mode SWD owner before handing pins to BMP */
    cmsis_dap_force_disconnect();
    if (xds_swd_is_active()) {
        (void)xds_swd_disconnect();
    }
    swd_disconnect();
    if (flush_rx) {
        usb_cdc_rx_flush(USB_CDC_PORT_AUX);
    }
    usb_cdc_tx_flush(USB_CDC_PORT_AUX);
    s_bmp_mode.detect_pending = false;
    s_bmp_mode.mode = PROBE_MODE_BMP;
}

#if (FW_BMP_GDB_COMPILED != 0)

/* Known initial packet prefixes sent by GDB, LLDB, and Black Magic remote (BMDA) */
static const char *const k_gdb_signatures[] = {
    "$qSupported:",
    "$QStartNoAckMode#",
    "$qTStatus#",
    "$qHostInfo#",
    "$qRcmd,",
    "$vMustReplyEmpty#",
    "$Hg0#df",
    "$?#3f",
    "$!#21",
    "!GA#",
    "!HA#",
};

static bool bmp_is_gdb_leading_ctrl(uint8_t ch)
{
    return (ch == (uint8_t)'+') || (ch == (uint8_t)'-') || (ch == 0x03U);
}

static gdb_detect_result_t bmp_match_handshake(const uint8_t *buf, uint32_t len)
{
    /* Skip up to a few leading ACK/NACK/Ctrl-C bytes before '$' or '!' */
    uint32_t off = 0U;
    while ((off < len) && (off < BMP_AUTODETECT_CTRL_MAX) && bmp_is_gdb_leading_ctrl(buf[off])) {
        off++;
    }
    if (off == len) {
        return (len <= BMP_AUTODETECT_CTRL_MAX) ? GDB_DETECT_PARTIAL : GDB_DETECT_NONE;
    }

    const uint8_t *payload = &buf[off];
    uint32_t rem = len - off;
    bool saw_partial = false;

    for (uint32_t i = 0U; i < (sizeof(k_gdb_signatures) / sizeof(k_gdb_signatures[0])); i++) {
        uint32_t sig_len = (uint32_t)strlen(k_gdb_signatures[i]);
        if (rem >= sig_len) {
            if (memcmp(payload, k_gdb_signatures[i], sig_len) == 0) {
                return GDB_DETECT_MATCH;
            }
        } else if (memcmp(payload, k_gdb_signatures[i], rem) == 0) {
            saw_partial = true;
        }
    }

    return saw_partial ? GDB_DETECT_PARTIAL : GDB_DETECT_NONE;
}

static void bmp_discard_cdc_aux(uint32_t max_bytes)
{
    uint8_t sink[BMP_AUTODETECT_PEEK_MAX];
    uint32_t to_drop = (max_bytes > sizeof(sink)) ? sizeof(sink) : max_bytes;
    (void)usb_cdc_rx_read(USB_CDC_PORT_AUX, sink, to_drop);
    s_bmp_mode.detect_pending = false;
}

static bool bmp_autodetect_poll(bool host_ready)
{
    uint32_t avail = usb_cdc_rx_available(USB_CDC_PORT_AUX);
    if (avail == 0U) {
        s_bmp_mode.detect_pending = false;
        return false;
    }

    if (!host_ready) {
        bmp_discard_cdc_aux(avail);
        return true;
    }

    uint8_t buf[BMP_AUTODETECT_PEEK_MAX];
    uint32_t peek_len = usb_cdc_rx_peek(USB_CDC_PORT_AUX, buf, sizeof(buf));
    gdb_detect_result_t res = bmp_match_handshake(buf, peek_len);

    if (res == GDB_DETECT_MATCH) {
        /* Switch into BMP mode while keeping the recognized packet intact in CDC1 RX */
        bmp_mode_enter_bmp(false);
        return true;
    }

    if (res == GDB_DETECT_PARTIAL) {
        uint32_t now_ms = board_get_uptime_ms();
        if (!s_bmp_mode.detect_pending) {
            s_bmp_mode.detect_pending = true;
            s_bmp_mode.detect_start_ms = now_ms;
            return false;
        }
        if ((uint32_t)(now_ms - s_bmp_mode.detect_start_ms) < BMP_AUTODETECT_TIMEOUT_MS) {
            return false;
        }
    }

    /* Non-GDB data or timed-out incomplete prefix: drain & discard */
    bmp_discard_cdc_aux(peek_len);
    return true;
}

static void bmp_gdb_on_exit(void)
{
    if (cur_target != NULL) {
        target_list_free();
        cur_target = NULL;
    }
    gdb_target_running = false;
    gdb_set_noackmode(false);
#ifdef ENABLE_RTT
    rtt_enabled = false;
    rtt_found = false;
#endif
    swd_disconnect();
}

static void bmp_gdb_poll_step(void)
{
    SET_IDLE_STATE(false);
    while (gdb_target_running && (cur_target != NULL)) {
        gdb_poll_target();
        bmp_mode_background_poll();

        if (!bmp_mode_is_active()) {
            target_list_free();
            gdb_target_running = false;
            return;
        }
        if (!gdb_target_running || (cur_target == NULL)) {
            break;
        }

        char c = gdb_if_getchar_to(0U);
        if ((c == '\x03') || (c == '\x04')) {
            target_halt_request(cur_target);
        }
#ifdef ENABLE_RTT
        else if (rtt_enabled) {
            poll_rtt(cur_target);
        }
#endif
        platform_pace_poll();
    }

    if (!bmp_mode_is_active()) {
        return;
    }

    SET_IDLE_STATE(true);
    const gdb_packet_s *const packet = gdb_packet_receive();
    if (!bmp_mode_is_active()) {
        return;
    }
    if ((packet->data[0] != '\x04') || (cur_target != NULL)) {
        SET_IDLE_STATE(false);
    }
    gdb_main(packet);
}

#endif /* FW_BMP_GDB_COMPILED */

void bmp_mode_init(bmp_bg_poll_fn_t bg_poll)
{
    s_bmp_mode.mode = (FW_DEFAULT_BMP_MODE != 0U) ? PROBE_MODE_BMP : PROBE_MODE_NORMAL;
    s_bmp_mode.bg_poll = bg_poll;
    s_bmp_mode.detect_pending = false;
    s_bmp_mode.detect_start_ms = 0U;

#if (FW_BMP_GDB_COMPILED != 0)
    platform_init();
    (void)gdb_if_init();
#ifdef ENABLE_RTT
    (void)rtt_if_init();
#endif
    swdptap_init();
    jtagtap_init();
    /* Leave target SWD/JTAG lines in high-Z idle until BMP scan actually runs */
    swd_disconnect();
#endif
}

void bmp_mode_background_poll(void)
{
    if (s_bmp_mode.bg_poll != NULL) {
        s_bmp_mode.bg_poll();
    }
}

uint8_t bmp_mode_get(void)
{
    return s_bmp_mode.mode;
}

uint8_t bmp_mode_get_caps(void)
{
    uint8_t caps = PROBE_MODE_CAP_SWITCH;
#if (FW_BMP_GDB_COMPILED != 0)
    caps |= PROBE_MODE_CAP_GDB_SERVER;
#endif
#if (FW_DEFAULT_BMP_MODE != 0)
    caps |= PROBE_MODE_CAP_DEFAULT_BMP;
#endif
    return caps;
}

bool bmp_mode_is_active(void)
{
    return s_bmp_mode.mode == PROBE_MODE_BMP;
}

bool bmp_rtt_is_active(void)
{
#if (FW_BMP_GDB_COMPILED != 0) && defined(ENABLE_RTT)
    return bmp_mode_is_active() && rtt_enabled;
#else
    return false;
#endif
}

int32_t bmp_mode_set(uint8_t mode)
{
    if ((mode != PROBE_MODE_NORMAL) && (mode != PROBE_MODE_BMP)) {
        return XDS_ERR_INVALID_PARAM;
    }

    if (mode == s_bmp_mode.mode) {
        return XDS_ERR_OK;
    }

    if (mode == PROBE_MODE_BMP) {
        bmp_mode_enter_bmp(true);
    } else {
        /* Detach any active BMP target, tri-state SWD pins, and flush CDC1 */
#if (FW_BMP_GDB_COMPILED != 0)
        bmp_gdb_on_exit();
#else
        swd_disconnect();
#endif
        usb_cdc_rx_flush(USB_CDC_PORT_AUX);
        usb_cdc_tx_flush(USB_CDC_PORT_AUX);
        s_bmp_mode.detect_pending = false;
        s_bmp_mode.mode = PROBE_MODE_NORMAL;
    }

    return XDS_ERR_OK;
}

bool bmp_mode_poll(void)
{
#if (FW_BMP_GDB_COMPILED != 0)
    bool host_ready = usb_device_is_configured() && usb_cdc_is_dtr_asserted(USB_CDC_PORT_AUX);

    if (!bmp_mode_is_active()) {
        bool did_detect = bmp_autodetect_poll(host_ready);
        if (!bmp_mode_is_active()) {
            return did_detect;
        }
        /* Fall through immediately to serve the triggering GDB/BMP packet */
    }

    if (!host_ready) {
        if ((cur_target != NULL) || gdb_target_running) {
            bmp_gdb_on_exit();
            return true;
        }
        return false;
    }

    /* Stay non-blocking in main() super-loop when GDB client is connected but idle */
    if (!gdb_target_running && (usb_cdc_rx_available(USB_CDC_PORT_AUX) == 0U)) {
        return false;
    }

    TRY(EXCEPTION_ALL)
    {
        bmp_gdb_poll_step();
    }
    CATCH()
    {
    default:
        gdb_put_packet_error(0xffU);
        target_list_free();
        gdb_outf("Uncaught exception: %s\n", exception_frame.msg);
        morse("TARGET LOST.", true);
        break;
    }

    return true;
#else
    return false;
#endif
}
