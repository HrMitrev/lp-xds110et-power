/*
 * swd.c - ARM ADIv5.2 Serial Wire Debug (SWD) bit-bang engine for LP-XDS110ET.
 *
 * Uses MSP432E4 DriverLib (MAP_GPIO*) for atomic bit-masked pin writes/reads,
 * CMSIS GPIOA->DIR for fast single-cycle SWDIO turnaround, and Cortex-M4
 * DWT->CYCCNT for cycle-accurate SWCLK half-period pacing.
 */

#include "swd.h"

#include <stddef.h>

#include "board_lp_xds110et.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"

/* Approximate fixed GPIO call overhead per SWCLK half-cycle at 120 MHz */
#define SWD_GPIO_OVERHEAD_CYCLES 16U
#define SWD_DEFAULT_CLOCK_HZ     1000000U

static uint32_t g_swd_half_cycles = 44U;
static uint8_t g_swd_turnaround = 1U;
static bool g_swd_data_phase = false;
static uint8_t g_swd_idle_cycles = 0U;

static inline void swd_delay_half(void)
{
    uint32_t cycles = g_swd_half_cycles;
    if (cycles > 0U) {
        uint32_t start = DWT->CYCCNT;
        while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
        }
    }
}

static inline uint32_t swd_parity32(uint32_t val)
{
    val ^= val >> 16U;
    val ^= val >> 8U;
    val ^= val >> 4U;
    val &= 0x0FU;
    return (0x6996U >> val) & 1U;
}

static inline void swd_clk_low(void)
{
    MAP_GPIOPinWrite(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TCK_PIN, 0U);
}

static inline void swd_clk_high(void)
{
    MAP_GPIOPinWrite(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TCK_PIN, BOARD_JTAG_TCK_PIN);
}

static inline void swd_dio_write(uint32_t bit)
{
    MAP_GPIOPinWrite(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TMS_PIN, (bit != 0U) ? BOARD_JTAG_TMS_PIN : 0U);
}

static inline uint32_t swd_dio_read(void)
{
    return (MAP_GPIOPinRead(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TMS_PIN) != 0U) ? 1U : 0U;
}

static inline void swd_dio_dir_out(void)
{
    /* Switch U10 TMS_DIR HIGH (A->B) BEFORE enabling MCU output on PA3 */
    MAP_GPIOPinWrite(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, BOARD_TMS_DIR_PIN);
    GPIOA->DIR |= BOARD_JTAG_TMS_PIN;
}

static inline void swd_dio_dir_in(void)
{
    /* Release PA3 to input BEFORE switching U10 TMS_DIR LOW (B->A) */
    GPIOA->DIR &= ~BOARD_JTAG_TMS_PIN;
    MAP_GPIOPinWrite(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, 0U);
}

static inline void swd_write_bit(uint32_t bit)
{
    swd_clk_low();
    swd_dio_write(bit);
    swd_delay_half();
    swd_clk_high();
    swd_delay_half();
}

static inline uint32_t swd_read_bit(void)
{
    swd_clk_low();
    swd_delay_half();
    uint32_t bit = swd_dio_read();
    swd_clk_high();
    swd_delay_half();
    return bit;
}

static inline void swd_clock_cycle(void)
{
    swd_clk_low();
    swd_delay_half();
    swd_clk_high();
    swd_delay_half();
}

/* ---- Public API ---- */

void swd_init(void)
{
    /* Enable Cortex-M4 DWT cycle counter for accurate SWCLK pacing */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    g_swd_turnaround = 1U;
    g_swd_data_phase = false;
    g_swd_idle_cycles = 0U;
    swd_set_clock_hz(SWD_DEFAULT_CLOCK_HZ);
    swd_disconnect();
}

void swd_connect(void)
{
    /* Switch U10 TMS_DIR HIGH (A->B) before driving PA3 (SWDIO) as output */
    MAP_GPIOPinWrite(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, BOARD_TMS_DIR_PIN);

    const uint8_t out_pins = BOARD_JTAG_TCK_PIN | BOARD_JTAG_TMS_PIN | BOARD_JTAG_TDI_PIN;
    /* Idle SWCLK LOW per ARM ADIv5 SWD spec (also prevents latching BOOT0=1 on STM32G0) */
    MAP_GPIOPinWrite(BOARD_JTAG_PORT_BASE, out_pins, BOARD_JTAG_TMS_PIN | BOARD_JTAG_TDI_PIN);
    MAP_GPIOPinTypeGPIOOutput(BOARD_JTAG_PORT_BASE, out_pins);
    MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, out_pins, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

    MAP_GPIOPinTypeGPIOInput(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TDO_PIN);
    MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, BOARD_JTAG_TDO_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
}

void swd_disconnect(void)
{
    const uint8_t jtag_pins = BOARD_JTAG_TCK_PIN | BOARD_JTAG_TMS_PIN | BOARD_JTAG_TDO_PIN | BOARD_JTAG_TDI_PIN;

    /* Put PA2..PA5 into high-Z input BEFORE switching U10 TMS_DIR LOW (B->A) */
    MAP_GPIOPinTypeGPIOInput(BOARD_JTAG_PORT_BASE, jtag_pins);
    MAP_GPIOPadConfigSet(BOARD_JTAG_PORT_BASE, jtag_pins, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD);
    MAP_GPIOPinWrite(BOARD_TMS_DIR_PORT_BASE, BOARD_TMS_DIR_PIN, 0U);

    board_srst_set(false);
}

void swd_set_clock_hz(uint32_t clock_hz)
{
    if (clock_hz == 0U) {
        return;
    }

    uint32_t sys_clk = board_get_sys_clock_hz();
    uint32_t half_cycles = sys_clk / (2U * clock_hz);
    if (half_cycles > SWD_GPIO_OVERHEAD_CYCLES) {
        g_swd_half_cycles = half_cycles - SWD_GPIO_OVERHEAD_CYCLES;
    } else {
        g_swd_half_cycles = 0U;
    }
}

void swd_configure(uint8_t turnaround_cycles, bool data_phase, uint8_t idle_cycles)
{
    if ((turnaround_cycles >= 1U) && (turnaround_cycles <= 4U)) {
        g_swd_turnaround = turnaround_cycles;
    }
    g_swd_data_phase = data_phase;
    g_swd_idle_cycles = idle_cycles;
}

uint8_t swd_pins_read(void)
{
    const uint8_t read_mask =
        BOARD_JTAG_TCK_PIN | BOARD_JTAG_TMS_PIN | BOARD_JTAG_TDO_PIN | BOARD_JTAG_TDI_PIN | BOARD_TARGET_SRST_PIN;
    uint32_t raw = MAP_GPIOPinRead(BOARD_JTAG_PORT_BASE, read_mask);
    uint8_t pins = 0U;

    if ((raw & BOARD_JTAG_TCK_PIN) != 0U) {
        pins |= SWD_PIN_SWCLK_TCK;
    }
    if ((raw & BOARD_JTAG_TMS_PIN) != 0U) {
        pins |= SWD_PIN_SWDIO_TMS;
    }
    if ((raw & BOARD_JTAG_TDI_PIN) != 0U) {
        pins |= SWD_PIN_TDI;
    }
    if ((raw & BOARD_JTAG_TDO_PIN) != 0U) {
        pins |= SWD_PIN_TDO;
    }
    if ((raw & BOARD_TARGET_SRST_PIN) != 0U) {
        pins |= SWD_PIN_NRESET;
    }
    return pins;
}

void swd_pins_write(uint8_t pin_out, uint8_t pin_sel)
{
    if ((pin_sel & SWD_PIN_SWCLK_TCK) != 0U) {
        MAP_GPIOPinWrite(
            BOARD_JTAG_PORT_BASE, BOARD_JTAG_TCK_PIN, ((pin_out & SWD_PIN_SWCLK_TCK) != 0U) ? BOARD_JTAG_TCK_PIN : 0U);
    }
    if ((pin_sel & SWD_PIN_SWDIO_TMS) != 0U) {
        swd_dio_dir_out();
        MAP_GPIOPinWrite(
            BOARD_JTAG_PORT_BASE, BOARD_JTAG_TMS_PIN, ((pin_out & SWD_PIN_SWDIO_TMS) != 0U) ? BOARD_JTAG_TMS_PIN : 0U);
    }
    if ((pin_sel & SWD_PIN_TDI) != 0U) {
        MAP_GPIOPinWrite(
            BOARD_JTAG_PORT_BASE, BOARD_JTAG_TDI_PIN, ((pin_out & SWD_PIN_TDI) != 0U) ? BOARD_JTAG_TDI_PIN : 0U);
    }
    if ((pin_sel & SWD_PIN_NRESET) != 0U) {
        board_srst_set((pin_out & SWD_PIN_NRESET) == 0U);
    }
}

void swd_sequence(uint32_t bit_count, const uint8_t *data)
{
    swd_dio_dir_out();

    uint32_t byte_val = 0U;
    for (uint32_t i = 0U; i < bit_count; i++) {
        if ((i & 7U) == 0U) {
            byte_val = *data++;
        }
        swd_write_bit(byte_val & 1U);
        byte_val >>= 1U;
    }
    swd_clk_low();
}

void swd_idle_cycles(uint32_t count)
{
    swd_dio_dir_out();
    for (uint32_t i = 0U; i < count; i++) {
        swd_write_bit(0U);
    }
    swd_clk_low();
    swd_dio_write(0U);
}

uint8_t swd_transfer(uint8_t request, uint32_t *data)
{
    /* 1. Build and transmit 8-bit SWD packet header (LSB first) */
    uint32_t req4 = request & 0x0FU;
    uint32_t parity = swd_parity32(req4);
    uint32_t hdr = 0x81U | (req4 << 1U) | (parity << 5U); /* Start=1, Stop=0, Park=1 */

    swd_dio_dir_out();
    for (uint32_t i = 0U; i < 8U; i++) {
        swd_write_bit(hdr & 1U);
        hdr >>= 1U;
    }

    /* 2. Turnaround: Probe -> Target */
    swd_dio_dir_in();
    for (uint32_t i = 0U; i < g_swd_turnaround; i++) {
        swd_clock_cycle();
    }

    /* 3. Receive 3-bit ACK (LSB first) */
    uint8_t ack = 0U;
    ack |= (uint8_t)(swd_read_bit() << 0U);
    ack |= (uint8_t)(swd_read_bit() << 1U);
    ack |= (uint8_t)(swd_read_bit() << 2U);

    if (ack == SWD_ACK_OK) {
        if ((request & SWD_REQ_RnW) != 0U) {
            /* 4a. Read 32-bit word + parity bit (LSB first) */
            uint32_t val = 0U;
            for (uint32_t i = 0U; i < 32U; i++) {
                val |= (swd_read_bit() << i);
            }
            uint32_t rx_parity = swd_read_bit();

            /* Turnaround: Target -> Probe */
            for (uint32_t i = 0U; i < g_swd_turnaround; i++) {
                swd_clock_cycle();
            }
            swd_dio_dir_out();

            if (rx_parity != swd_parity32(val)) {
                ack = SWD_ACK_ERROR;
            }
            if (data != NULL) {
                *data = val;
            }
        } else {
            /* 4b. Turnaround: Target -> Probe, then write 32-bit word + parity */
            for (uint32_t i = 0U; i < g_swd_turnaround; i++) {
                swd_clock_cycle();
            }
            swd_dio_dir_out();

            uint32_t val = (data != NULL) ? *data : 0U;
            uint32_t tx_parity = swd_parity32(val);
            for (uint32_t i = 0U; i < 32U; i++) {
                swd_write_bit(val & 1U);
                val >>= 1U;
            }
            swd_write_bit(tx_parity);
        }

        /* 5. Append configured idle cycles with SWDIO LOW, then park SWCLK LOW */
        for (uint32_t i = 0U; i < g_swd_idle_cycles; i++) {
            swd_write_bit(0U);
        }
        swd_clk_low();
        swd_dio_write(0U);
        return ack;
    }

    /* Non-OK response (WAIT, FAULT, or protocol error) */
    if ((ack == SWD_ACK_WAIT) || (ack == SWD_ACK_FAULT)) {
        if (g_swd_data_phase && ((request & SWD_REQ_RnW) != 0U)) {
            for (uint32_t i = 0U; i < 33U; i++) {
                swd_clock_cycle();
            }
        }
        for (uint32_t i = 0U; i < g_swd_turnaround; i++) {
            swd_clock_cycle();
        }
        swd_dio_dir_out();
        if (g_swd_data_phase && ((request & SWD_REQ_RnW) == 0U)) {
            for (uint32_t i = 0U; i < 33U; i++) {
                swd_write_bit(0U);
            }
        }
        swd_clk_low();
        swd_dio_write(0U);
        return ack;
    }

    /* Protocol error (e.g. 0x07 no target): clock out 33 dummy cycles and reclaim SWDIO */
    for (uint32_t i = 0U; i < (33U + g_swd_turnaround); i++) {
        swd_clock_cycle();
    }
    swd_dio_dir_out();
    swd_clk_low();
    swd_dio_write(0U);
    return ack;
}
