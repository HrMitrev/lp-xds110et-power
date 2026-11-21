/*
 * bmp_jtagtap.c - 4-wire JTAG TAP bit-bang driver for Black Magic Debug on LP-XDS110ET.
 *
 * Drives PA2 (TCK), PA3 (TMS, via U10 PC5 TMS_DIR=HIGH), PA5 (TDI) and samples
 * PA4 (TDO) using DriverLib bit-masked GPIO accesses.
 */

#include "board_lp_xds110et.h"
#include "dap/swd.h"
#include "driverlib/types.h"
#include "general.h"
#include "jtagtap.h"
#include "platform.h"
#include "timing.h"

#define BMP_JTAG_REG_TCK (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TCK_PIN << 2U))
#define BMP_JTAG_REG_TMS (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TMS_PIN << 2U))
#define BMP_JTAG_REG_TDO (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TDO_PIN << 2U))
#define BMP_JTAG_REG_TDI (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TDI_PIN << 2U))

jtag_proc_s jtag_proc;

static inline void bmp_jtag_delay(void)
{
    uint32_t loops = target_clk_divider;
    if (loops == UINT32_MAX) {
        __NOP();
        __NOP();
        return;
    }
    for (uint32_t i = 0U; i < loops; i++) {
        __NOP();
    }
}

static bool bmp_jtag_next(const bool tms, const bool tdi)
{
    HWREG(BMP_JTAG_REG_TMS) = tms ? BOARD_JTAG_TMS_PIN : 0U;
    HWREG(BMP_JTAG_REG_TDI) = tdi ? BOARD_JTAG_TDI_PIN : 0U;
    bmp_jtag_delay();
    HWREG(BMP_JTAG_REG_TCK) = BOARD_JTAG_TCK_PIN;
    bmp_jtag_delay();
    bool tdo = (HWREG(BMP_JTAG_REG_TDO) != 0U);
    HWREG(BMP_JTAG_REG_TCK) = 0U;
    return tdo;
}

static void bmp_jtag_tms_seq(uint32_t tms_states, size_t clock_cycles)
{
    for (size_t i = 0U; i < clock_cycles; i++) {
        (void)bmp_jtag_next((tms_states & 1U) != 0U, true);
        tms_states >>= 1U;
    }
}

static void bmp_jtag_tdi_tdo_seq(
    uint8_t *data_out, const bool final_tms, const uint8_t *data_in, size_t clock_cycles)
{
    uint8_t out_byte = 0U;
    for (size_t i = 0U; i < clock_cycles; i++) {
        bool tms = final_tms && ((i + 1U) == clock_cycles);
        bool tdi = (data_in != NULL) ? (((data_in[i >> 3U] >> (i & 7U)) & 1U) != 0U) : false;
        bool tdo = bmp_jtag_next(tms, tdi);
        if (tdo) {
            out_byte |= (uint8_t)(1U << (i & 7U));
        }
        if (((i & 7U) == 7U) || ((i + 1U) == clock_cycles)) {
            if (data_out != NULL) {
                data_out[i >> 3U] = out_byte;
            }
            out_byte = 0U;
        }
    }
}

static void bmp_jtag_tdi_seq(const bool final_tms, const uint8_t *data_in, size_t clock_cycles)
{
    bmp_jtag_tdi_tdo_seq(NULL, final_tms, data_in, clock_cycles);
}

static void bmp_jtag_cycle(const bool tms, const bool tdi, const size_t clock_cycles)
{
    for (size_t i = 0U; i < clock_cycles; i++) {
        (void)bmp_jtag_next(tms, tdi);
    }
}

static void bmp_jtag_reset(void)
{
    jtagtap_soft_reset();
}

void jtagtap_init(void)
{
    swd_connect();
    HWREG(BMP_JTAG_REG_TCK) = 0U;

    jtag_proc.jtagtap_reset = bmp_jtag_reset;
    jtag_proc.jtagtap_next = bmp_jtag_next;
    jtag_proc.jtagtap_tms_seq = bmp_jtag_tms_seq;
    jtag_proc.jtagtap_tdi_tdo_seq = bmp_jtag_tdi_tdo_seq;
    jtag_proc.jtagtap_tdi_seq = bmp_jtag_tdi_seq;
    jtag_proc.jtagtap_cycle = bmp_jtag_cycle;
    jtag_proc.tap_idle_cycles = 1U;
}
