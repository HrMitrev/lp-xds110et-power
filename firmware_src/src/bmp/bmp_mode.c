/*
 * bmp_mode.c - Runtime probe mode manager & cooperative Black Magic Debug bridge.
 *
 * Replaces upstream blackmagic/src/main.c so LP-XDS110ET retains ownership of
 * main() while serving BMP's GDB RSP server cleanly over CDC1 (ttyACM1).
 */

#include "bmp_mode.h"

#include <stddef.h>

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

typedef struct {
    uint8_t mode;
    bmp_bg_poll_fn_t bg_poll;
} bmp_mode_state_t;

static bmp_mode_state_t s_bmp_mode = {
    .mode = (FW_DEFAULT_BMP_MODE != 0U) ? PROBE_MODE_BMP : PROBE_MODE_NORMAL,
    .bg_poll = NULL,
};

#if (FW_BMP_GDB_COMPILED != 0)

static void bmp_gdb_on_exit(void)
{
    if (cur_target != NULL) {
        target_list_free();
        cur_target = NULL;
    }
    gdb_target_running = false;
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
        /* Release any active Normal-mode SWD owner before handing pins to BMP */
        cmsis_dap_force_disconnect();
        if (xds_swd_is_active()) {
            (void)xds_swd_disconnect();
        }
        swd_disconnect();
        usb_cdc_rx_flush(USB_CDC_PORT_AUX);
        usb_cdc_tx_flush(USB_CDC_PORT_AUX);
        s_bmp_mode.mode = PROBE_MODE_BMP;
    } else {
        /* Detach any active BMP target, tri-state SWD pins, and flush CDC1 */
#if (FW_BMP_GDB_COMPILED != 0)
        bmp_gdb_on_exit();
#else
        swd_disconnect();
#endif
        usb_cdc_rx_flush(USB_CDC_PORT_AUX);
        usb_cdc_tx_flush(USB_CDC_PORT_AUX);
        s_bmp_mode.mode = PROBE_MODE_NORMAL;
    }

    return XDS_ERR_OK;
}

bool bmp_mode_poll(void)
{
#if (FW_BMP_GDB_COMPILED != 0)
    if (!bmp_mode_is_active()) {
        return false;
    }

    bool host_ready = usb_device_is_configured() && usb_cdc_is_dtr_asserted(USB_CDC_PORT_AUX);
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
