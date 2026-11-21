/*
 * bmp_platform.c - Black Magic Debug platform HAL for TI LP-XDS110ET.
 *
 * Bridges BMP's platform_support.h / timing.h hooks onto the LP-XDS110ET BSP
 * (SysTick 1 kHz uptime, PA6 nSRST, DFU jump, SWD frequency control, and a
 * stack-guarded _sbrk heap allocator).
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "board_lp_xds110et.h"
#include "dap/swd.h"
#include "dfu_jump.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "general.h"
#include "platform.h"

/* SWCLK delay loop count used by bmp_swdptap.c / bmp_jtagtap.c (UINT32_MAX = max speed) */
uint32_t target_clk_divider = 2U;
static uint32_t g_bmp_max_freq_hz = 4000000U;

/* Linker-provided heap bounds (16 KiB stack reserved at top of 256 KiB SRAM) */
extern uint8_t __heap_start__;
extern uint8_t __heap_end__;
static uint8_t *g_heap_cur = &__heap_start__;

void platform_init(void)
{
    /* Board, SysTick, and USB composite stack are already initialized by main() */
}

uint32_t platform_time_ms(void)
{
    return board_get_uptime_ms();
}

void platform_delay(uint32_t ms)
{
    uint32_t start = board_get_uptime_ms();
    while ((uint32_t)(board_get_uptime_ms() - start) < ms) {
        app_background_poll();
    }
}

int platform_hwversion(void)
{
    return (int)board_get_hw_version();
}

uint32_t platform_target_voltage_sense(void)
{
    /* LP-XDS110ET supplies regulated 3.3 V (BMP units: 0.1 V -> 33) */
    return 33U;
}

const char *platform_target_voltage(void)
{
    return "3.3V";
}

void platform_nrst_set_val(bool assert)
{
    board_srst_set(assert);
}

bool platform_nrst_get_val(void)
{
    /* Active-low nSRST on PA6: returns true when asserted (LOW) */
    return MAP_GPIOPinRead(BOARD_JTAG_PORT_BASE, BOARD_TARGET_SRST_PIN) == 0U;
}

bool platform_target_get_power(void)
{
    return true;
}

bool platform_target_set_power(bool power)
{
    (void)power;
    return false;
}

void platform_request_boot(void)
{
    dfu_jump_enter();
}

void platform_max_frequency_set(uint32_t freq)
{
    if (freq == 0U) {
        target_clk_divider = UINT32_MAX;
        g_bmp_max_freq_hz = 10000000U;
        return;
    }

    uint32_t sys_clk = board_get_sys_clock_hz();
    uint32_t half_cycles = sys_clk / (2U * freq);
    if (half_cycles <= 6U) {
        target_clk_divider = UINT32_MAX;
        g_bmp_max_freq_hz = 10000000U;
    } else {
        target_clk_divider = (half_cycles - 6U) / 4U;
        g_bmp_max_freq_hz = freq;
    }
}

uint32_t platform_max_frequency_get(void)
{
    return g_bmp_max_freq_hz;
}

void platform_target_clk_output_enable(bool enable)
{
    if (enable) {
        swd_connect();
    } else {
        swd_disconnect();
    }
}

void platform_ospeed_update(uint32_t frequency)
{
    (void)frequency;
}

bool platform_spi_init(spi_bus_e bus)
{
    (void)bus;
    return false;
}

bool platform_spi_deinit(spi_bus_e bus)
{
    (void)bus;
    return false;
}

bool platform_spi_chip_select(uint8_t device_select)
{
    (void)device_select;
    return false;
}

uint8_t platform_spi_xfer(spi_bus_e bus, uint8_t value)
{
    (void)bus;
    (void)value;
    return 0xFFU;
}

void debug_serial_send_stdout(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}

/* Stack-guarded newlib _sbrk allocator and minimal nosys stubs for BMP */
__attribute__((used)) void *_sbrk(ptrdiff_t incr)
{
    uint8_t *next = g_heap_cur + incr;
    if ((next < &__heap_start__) || (next > &__heap_end__)) {
        errno = ENOMEM;
        return (void *)-1;
    }

    void *prev = g_heap_cur;
    g_heap_cur = next;
    return prev;
}

__attribute__((used)) int _write(int file, const void *ptr, size_t len)
{
    (void)file;
    (void)ptr;
    return (int)len;
}

__attribute__((used)) int _read(int file, void *ptr, size_t len)
{
    (void)file;
    (void)ptr;
    return (int)len;
}

__attribute__((used)) int _close(int file)
{
    (void)file;
    return 0;
}

__attribute__((used)) off_t _lseek(int file, off_t offset, int whence)
{
    (void)file;
    (void)offset;
    (void)whence;
    return 0;
}

__attribute__((used)) int _fstat(int file, struct stat *st)
{
    (void)file;
    memset(st, 0, sizeof(*st));
    return 0;
}

__attribute__((used)) int _isatty(int file)
{
    (void)file;
    return 1;
}

__attribute__((used)) pid_t _getpid(void)
{
    return 1;
}

__attribute__((used)) int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    return 0;
}

__attribute__((weak)) void __aeabi_unwind_cpp_pr0(void)
{
}

__attribute__((weak)) void __aeabi_unwind_cpp_pr1(void)
{
}
