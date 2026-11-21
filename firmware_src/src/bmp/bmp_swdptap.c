/*
 * bmp_swdptap.c - High-speed SWD TAP driver for Black Magic Debug on LP-XDS110ET.
 *
 * Uses DriverLib bit-masked GPIO writes (HWREG) on PA2 (SWCLK), PA3 (SWDIO),
 * and PC5 (U10 SN74AVC2T245 TMS_DIR) with glitch-free direction turnaround
 * matching BMP's falling-edge-to-falling-edge SWD timing contract.
 */

#include "board_lp_xds110et.h"
#include "dap/swd.h"
#include "driverlib/types.h"
#include "general.h"
#include "maths_utils.h"
#include "platform.h"
#include "swd.h"
#include "timing.h"

#define BMP_REG_SWCLK   (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TCK_PIN << 2U))
#define BMP_REG_SWDIO   (BOARD_JTAG_PORT_BASE + ((uint32_t)BOARD_JTAG_TMS_PIN << 2U))
#define BMP_REG_TMS_DIR (BOARD_TMS_DIR_PORT_BASE + ((uint32_t)BOARD_TMS_DIR_PIN << 2U))

typedef enum {
    BMP_SWDIO_FLOAT = 0,
    BMP_SWDIO_DRIVE,
} bmp_swdio_dir_t;

swd_proc_s swd_proc;
static bmp_swdio_dir_t g_swdio_dir = BMP_SWDIO_FLOAT;

static inline __attribute__((always_inline)) void bmp_swd_delay(uint32_t loops)
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

__attribute__((optimize("O2"))) static void bmp_swd_turnaround(bmp_swdio_dir_t dir)
{
    if (dir == g_swdio_dir) {
        return;
    }
    g_swdio_dir = dir;

    uint32_t loops = target_clk_divider;
    if (dir == BMP_SWDIO_FLOAT) {
        bmp_swdio_float();
    }
    bmp_swd_delay(loops);
    HWREG(BMP_REG_SWCLK) = BOARD_JTAG_TCK_PIN;
    bmp_swd_delay(loops);
    HWREG(BMP_REG_SWCLK) = 0U;
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
            uint32_t raw = HWREG(BMP_REG_SWDIO);
            HWREG(BMP_REG_SWCLK) = BOARD_JTAG_TCK_PIN;
            value = (value >> 1U) | (raw << 28U); /* PA3 (bit 3) -> bit 31 */
            __NOP();
            HWREG(BMP_REG_SWCLK) = 0U;
        }
    } else {
        for (size_t i = clock_cycles; i > 0U; i--) {
            bmp_swd_delay(loops);
            uint32_t raw = HWREG(BMP_REG_SWDIO);
            HWREG(BMP_REG_SWCLK) = BOARD_JTAG_TCK_PIN;
            value = (value >> 1U) | (raw << 28U);
            bmp_swd_delay(loops);
            HWREG(BMP_REG_SWCLK) = 0U;
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

    bmp_swd_delay(loops);
    bool bit = (HWREG(BMP_REG_SWDIO) != 0U);
    HWREG(BMP_REG_SWCLK) = BOARD_JTAG_TCK_PIN;
    bmp_swd_delay(loops);
    HWREG(BMP_REG_SWCLK) = 0U;

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
            HWREG(BMP_REG_SWDIO) = ((value & 1U) != 0U) ? BOARD_JTAG_TMS_PIN : 0U;
            __NOP();
            __NOP();
            HWREG(BMP_REG_SWCLK) = BOARD_JTAG_TCK_PIN;
            value >>= 1U;
            __NOP();
            HWREG(BMP_REG_SWCLK) = 0U;
        }
    } else {
        for (size_t i = clock_cycles; i > 0U; i--) {
            HWREG(BMP_REG_SWDIO) = ((value & 1U) != 0U) ? BOARD_JTAG_TMS_PIN : 0U;
            bmp_swd_delay(loops);
            HWREG(BMP_REG_SWCLK) = BOARD_JTAG_TCK_PIN;
            value >>= 1U;
            bmp_swd_delay(loops);
            HWREG(BMP_REG_SWCLK) = 0U;
        }
    }
}

__attribute__((optimize("O2"))) static void bmp_swd_seq_out_parity(uint32_t tms_states, size_t clock_cycles)
{
    bool parity = calculate_odd_parity(tms_states);
    bmp_swd_seq_out(tms_states, clock_cycles);

    uint32_t loops = target_clk_divider;
    HWREG(BMP_REG_SWDIO) = parity ? BOARD_JTAG_TMS_PIN : 0U;
    bmp_swd_delay(loops);
    HWREG(BMP_REG_SWCLK) = BOARD_JTAG_TCK_PIN;
    bmp_swd_delay(loops);
    HWREG(BMP_REG_SWCLK) = 0U;
}

void swdptap_init(void)
{
    swd_connect();
    HWREG(BMP_REG_SWCLK) = 0U;
    g_swdio_dir = BMP_SWDIO_DRIVE;

    swd_proc.seq_in = bmp_swd_seq_in;
    swd_proc.seq_in_parity = bmp_swd_seq_in_parity;
    swd_proc.seq_out = bmp_swd_seq_out;
    swd_proc.seq_out_parity = bmp_swd_seq_out_parity;
}
