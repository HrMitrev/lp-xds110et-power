/*
 * bmp_bridge.c - Cooperative Black Magic Debug GDB loop bridge for LP-XDS110ET.
 *
 * Replaces upstream blackmagic/src/main.c so LP-XDS110ET retains ownership of
 * main() while running BMP's GDB RSP server cleanly over CDC1 (ttyACM1).
 */

#include "bmp_bridge.h"

#include "bmp_mode.h"
#include "dap/swd.h"
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
#include "swd.h"
#include "target.h"

static void bmp_poll_step(void)
{
    SET_IDLE_STATE(false);
    while (gdb_target_running && (cur_target != NULL)) {
        gdb_poll_target();
        app_background_poll();

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

void bmp_bridge_init(void)
{
    (void)gdb_if_init();
    swdptap_init();
    jtagtap_init();
    /* Leave target SWD/JTAG lines in high-Z idle until BMP scan actually runs */
    swd_disconnect();
}

void bmp_bridge_on_mode_exit(void)
{
    if (cur_target != NULL) {
        target_list_free();
        cur_target = NULL;
    }
    gdb_target_running = false;
    swd_disconnect();
}

bool bmp_bridge_poll(void)
{
    if (!bmp_mode_is_active()) {
        return false;
    }

    bool host_ready = usb_device_is_configured() && usb_cdc_is_dtr_asserted(USB_CDC_PORT_AUX);
    if (!host_ready) {
        if ((cur_target != NULL) || gdb_target_running) {
            bmp_bridge_on_mode_exit();
            return true;
        }
        return false;
    }

    /* Stay non-blocking in main() super-loop when GDB client is connected but idle */
    if (!gdb_target_running && (usb_cdc_rx_available(USB_CDC_PORT_AUX) == 0U)) {
        return false;
    }

    TRY (EXCEPTION_ALL) {
        bmp_poll_step();
    }
    CATCH () {
    default:
        gdb_put_packet_error(0xffU);
        target_list_free();
        gdb_outf("Uncaught exception: %s\n", exception_frame.msg);
        morse("TARGET LOST.", true);
        break;
    }

    return true;
}

#else

void bmp_bridge_init(void)
{
}

void bmp_bridge_on_mode_exit(void)
{
}

bool bmp_bridge_poll(void)
{
    return false;
}

#endif /* FW_BMP_GDB_COMPILED */
