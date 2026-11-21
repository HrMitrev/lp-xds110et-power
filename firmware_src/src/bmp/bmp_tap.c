/*
 * bmp_tap.c - High-speed SWD & 4-wire JTAG TAP drivers for Black Magic Debug.
 *
 * Drives PA2 (TCK/SWCLK), PA3 (TMS/SWDIO, via U10 SN74AVC2T245 PC5 TMS_DIR),
 * PA5 (TDI), and samples PA4 (TDO) using single-instruction DriverLib
 * bit-masked GPIO accesses (HWREG) on LP-XDS110ET.
 */

#include "board_lp_xds110et.h"
#include "dap/swd.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/types.h"
#include "general.h"
#include "jtagtap.h"
#include "maths_utils.h"
#include "platform.h"
#include "swd.h"
#include "timing.h"

#define BMP_REG_TCK     (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TCK_PIN << 2U))
#define BMP_REG_TMS     (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TMS_PIN << 2U))
#define BMP_REG_TDO     (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TDO_PIN << 2U))
#define BMP_REG_TDI     (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TDI_PIN << 2U))
#define BMP_REG_TMS_DIR (BOARD_TMS_DIR_PORT_BASE + ((uint32_t)BOARD_TMS_DIR_PIN << 2U))

typedef enum {
    BMP_SWDIO_FLOAT = 0,
    BMP_SWDIO_DRIVE,
} bmp_swdio_dir_t;

typedef struct {
    bmp_swdio_dir_t swdio_dir;
} bmp_tap_state_t;

/* Required external dispatch tables populated for upstream BMP swd.h / jtagtap.h */
swd_proc_s swd_proc;
jtag_proc_s jtag_proc;

static bmp_tap_state_t s_bmp_tap = {
    .swdio_dir = BMP_SWDIO_FLOAT,
};

static inline __attribute__((always_inline)) void bmp_tap_delay(uint32_t loops)
{
    if (loops == UINT32_MAX) {
        __NOP();
        __NOP();
        return;
    }
    for (uint32_t i = 0U; i < loops; i++) {
        __NOP();
    }
}

static inline __attribute__((always_inline)) void bmp_swdio_float(void)
{
    /* Release PA3 to input BEFORE switching U10 TMS_DIR LOW (B->A) */
    GPIOA->DIR &= ~BOARD_JTAG_TMS_PIN;
    HWREG(BMP_REG_TMS_DIR) = 0U;
}

static inline __attribute__((always_inline)) void bmp_swdio_drive(void)
{
    /* Switch U10 TMS_DIR HIGH (A->B) BEFORE enabling MCU output on PA3 */
    HWREG(BMP_REG_TMS_DIR) = BOARD_TMS_DIR_PIN;
    GPIOA->DIR |= BOARD_JTAG_TMS_PIN;
}

void bmp_tap_clk_output_enable(bool enable)
{
    if (enable) {
        HWREG(BMP_REG_TCK) = 0U;
        MAP_GPIOPinTypeGPIOOutput(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TCK_PIN);
        MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TCK_PIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
    } else {
        MAP_GPIOPinTypeGPIOInput(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TCK_PIN);
        MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TCK_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);
    }
}

/* ---- SWD TAP (swdptap) ---- */

__attribute__((optimize("O2"))) static void bmp_swd_turnaround(bmp_swdio_dir_t dir)
{
    if (dir == s_bmp_tap.swdio_dir) {
        return;
    }
    s_bmp_tap.swdio_dir = dir;

    uint32_t loops = target_clk_divider;
    if (dir == BMP_SWDIO_FLOAT) {
        bmp_swdio_float();
    }
    bmp_tap_delay(loops);
    HWREG(BMP_REG_TCK) = BOARD_JTAG_TCK_PIN;
    bmp_tap_delay(loops);
    HWREG(BMP_REG_TCK) = 0U;
    if (dir == BMP_SWDIO_DRIVE) {
        bmp_swdio_drive();
    }
}

__attribute__((optimize("O2"))) static uint32_t bmp_swd_seq_in(size_t clock_cycles)
{
    bmp_swd_turnaround(BMP_SWDIO_FLOAT);
    if (clock_cycles == 0U) {
        return 0U;
    }

    uint32_t loops = target_clk_divider;
    uint32_t value = 0U;

    if (loops == UINT32_MAX) {
        for (size_t i = clock_cycles; i > 0U; i--) {
            __NOP();
            __NOP();
            __NOP();
            uint32_t raw = HWREG(BMP_REG_TMS);
            HWREG(BMP_REG_TCK) = BOARD_JTAG_TCK_PIN;
            value = (value >> 1U) | (raw << 28U); /* PA3 (bit 3) -> bit 31 */
            __NOP();
            HWREG(BMP_REG_TCK) = 0U;
        }
    } else {
        for (size_t i = clock_cycles; i > 0U; i--) {
            bmp_tap_delay(loops);
            uint32_t raw = HWREG(BMP_REG_TMS);
            HWREG(BMP_REG_TCK) = BOARD_JTAG_TCK_PIN;
            value = (value >> 1U) | (raw << 28U);
            bmp_tap_delay(loops);
            HWREG(BMP_REG_TCK) = 0U;
        }
    }

    if (clock_cycles < 32U) {
        value >>= (32U - clock_cycles);
    }
    return value;
}

__attribute__((optimize("O2"))) static bool bmp_swd_seq_in_parity(uint32_t *ret, size_t clock_cycles)
{
    uint32_t result = bmp_swd_seq_in(clock_cycles);
    uint32_t loops = target_clk_divider;

    bmp_tap_delay(loops);
    bool bit = (HWREG(BMP_REG_TMS) != 0U);
    HWREG(BMP_REG_TCK) = BOARD_JTAG_TCK_PIN;
    bmp_tap_delay(loops);
    HWREG(BMP_REG_TCK) = 0U;

    /* Terminate read phase with turnaround back to Probe->Target */
    bmp_swd_turnaround(BMP_SWDIO_DRIVE);

    bool parity = calculate_odd_parity(result);
    *ret = result;
    return parity == bit;
}

__attribute__((optimize("O2"))) static void bmp_swd_seq_out(uint32_t tms_states, size_t clock_cycles)
{
    bmp_swd_turnaround(BMP_SWDIO_DRIVE);
    if (clock_cycles == 0U) {
        return;
    }

    uint32_t loops = target_clk_divider;
    uint32_t value = tms_states;

    if (loops == UINT32_MAX) {
        for (size_t i = clock_cycles; i > 0U; i--) {
            HWREG(BMP_REG_TMS) = ((value & 1U) != 0U) ? BOARD_JTAG_TMS_PIN : 0U;
            __NOP();
            __NOP();
            HWREG(BMP_REG_TCK) = BOARD_JTAG_TCK_PIN;
            value >>= 1U;
            __NOP();
            HWREG(BMP_REG_TCK) = 0U;
        }
    } else {
        for (size_t i = clock_cycles; i > 0U; i--) {
            HWREG(BMP_REG_TMS) = ((value & 1U) != 0U) ? BOARD_JTAG_TMS_PIN : 0U;
            bmp_tap_delay(loops);
            HWREG(BMP_REG_TCK) = BOARD_JTAG_TCK_PIN;
            value >>= 1U;
            bmp_tap_delay(loops);
            HWREG(BMP_REG_TCK) = 0U;
        }
    }
}

__attribute__((optimize("O2"))) static void bmp_swd_seq_out_parity(uint32_t tms_states, size_t clock_cycles)
{
    bool parity = calculate_odd_parity(tms_states);
    bmp_swd_seq_out(tms_states, clock_cycles);

    uint32_t loops = target_clk_divider;
    HWREG(BMP_REG_TMS) = parity ? BOARD_JTAG_TMS_PIN : 0U;
    bmp_tap_delay(loops);
    HWREG(BMP_REG_TCK) = BOARD_JTAG_TCK_PIN;
    bmp_tap_delay(loops);
    HWREG(BMP_REG_TCK) = 0U;
}

void swdptap_init(void)
{
    swd_connect();
    HWREG(BMP_REG_TCK) = 0U;
    s_bmp_tap.swdio_dir = BMP_SWDIO_DRIVE;

    swd_proc.seq_in = bmp_swd_seq_in;
    swd_proc.seq_in_parity = bmp_swd_seq_in_parity;
    swd_proc.seq_out = bmp_swd_seq_out;
    swd_proc.seq_out_parity = bmp_swd_seq_out_parity;
}

/* ---- 4-Wire JTAG TAP (jtagtap) ---- */

static bool bmp_jtag_next(const bool tms, const bool tdi)
{
    uint32_t loops = target_clk_divider;
    HWREG(BMP_REG_TMS) = tms ? BOARD_JTAG_TMS_PIN : 0U;
    HWREG(BMP_REG_TDI) = tdi ? BOARD_JTAG_TDI_PIN : 0U;
    bmp_tap_delay(loops);
    HWREG(BMP_REG_TCK) = BOARD_JTAG_TCK_PIN;
    bmp_tap_delay(loops);
    bool tdo = (HWREG(BMP_REG_TDO) != 0U);
    HWREG(BMP_REG_TCK) = 0U;
    return tdo;
}

static void bmp_jtag_tms_seq(uint32_t tms_states, size_t clock_cycles)
{
    for (size_t i = 0U; i < clock_cycles; i++) {
        (void)bmp_jtag_next((tms_states & 1U) != 0U, true);
        tms_states >>= 1U;
    }
}

static void bmp_jtag_tdi_tdo_seq(uint8_t *data_out, const bool final_tms, const uint8_t *data_in, size_t clock_cycles)
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
    HWREG(BMP_REG_TCK) = 0U;
    s_bmp_tap.swdio_dir = BMP_SWDIO_DRIVE;

    jtag_proc.jtagtap_reset = bmp_jtag_reset;
    jtag_proc.jtagtap_next = bmp_jtag_next;
    jtag_proc.jtagtap_tms_seq = bmp_jtag_tms_seq;
    jtag_proc.jtagtap_tdi_tdo_seq = bmp_jtag_tdi_tdo_seq;
    jtag_proc.jtagtap_tdi_seq = bmp_jtag_tdi_seq;
    jtag_proc.jtagtap_cycle = bmp_jtag_cycle;
    jtag_proc.tap_idle_cycles = 1U;
}
