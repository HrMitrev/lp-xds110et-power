/*
 * bmp_mode.c - Runtime probe mode manager (Normal vs Black Magic Probe mode).
 */

#include "bmp_mode.h"

#include "bmp_bridge.h"
#include "dap/cmsis_dap.h"
#include "dap/swd.h"
#include "dap/xds_swd.h"
#include "protocol/xds_cmd.h"
#include "usb/usb_device.h"

static uint8_t g_probe_mode = (FW_DEFAULT_BMP_MODE != 0U) ? PROBE_MODE_BMP : PROBE_MODE_NORMAL;

void bmp_mode_init(void)
{
    g_probe_mode = (FW_DEFAULT_BMP_MODE != 0U) ? PROBE_MODE_BMP : PROBE_MODE_NORMAL;
}

uint8_t bmp_mode_get(void)
{
    return g_probe_mode;
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
    return g_probe_mode == PROBE_MODE_BMP;
}

int32_t bmp_mode_set(uint8_t mode)
{
    if ((mode != PROBE_MODE_NORMAL) && (mode != PROBE_MODE_BMP)) {
        return XDS_ERR_INVALID_PARAM;
    }

    if (mode == g_probe_mode) {
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
        g_probe_mode = PROBE_MODE_BMP;
    } else {
        /* Detach any active BMP target, tri-state SWD pins, and flush CDC1 */
        bmp_bridge_on_mode_exit();
        usb_cdc_rx_flush(USB_CDC_PORT_AUX);
        usb_cdc_tx_flush(USB_CDC_PORT_AUX);
        g_probe_mode = PROBE_MODE_NORMAL;
    }

    return XDS_ERR_OK;
}
