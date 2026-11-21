/*
 * bmp_platform.c - Black Magic Debug platform HAL, CDC1 GDB & CDC0 RTT transport.
 *
 * Implements four clean layers for Black Magic Debug on LP-XDS110ET:
 *   1. Platform HAL (platform_support.h / timing.h): SysTick uptime, nSRST,
 *      DFU jump, SWCLK output enable, and SWD/JTAG clock frequency scaling.
 *   2. GDB RSP Transport (gdb_if.h) over USB CDC1 (ttyACM1), pumping
 *      app_background_poll() in every wait loop so EnergyTrace HDR (256 kS/s),
 *      CDC0 App UART (ttyACM0), and XDS110 opcode 0x63 never stall.
 *   3. SEGGER RTT Transport (rtt_if.h) over USB CDC0 (ttyACM0) when enabled.
 *   4. Stack-guarded newlib _sbrk heap allocator & minimal nosys syscall stubs.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "bmp_mode.h"
#include "board_lp_xds110et.h"
#include "dfu_jump.h"
#include "gdb_if.h"
#include "general.h"
#include "platform.h"
#ifdef ENABLE_RTT
#include "rtt.h"
#include "rtt_if.h"
#endif
#include "usb/usb_device.h"

#define BMP_GDB_TX_BUF_SIZE 64U

/* SWCLK delay loop count required as an external symbol by BMP timing.h / bmp_tap.c */
uint32_t target_clk_divider = 2U;

/* Linker-provided heap bounds (16 KiB stack reserved at top of 256 KiB SRAM) */
extern uint8_t __heap_start__;
extern uint8_t __heap_end__;

typedef struct {
    uint32_t max_freq_hz;

    struct {
        uint8_t buf[BMP_GDB_TX_BUF_SIZE];
        uint32_t count;
    } gdb_tx;
#ifdef ENABLE_RTT
    struct {
        uint8_t buf[RTT_DOWN_BUF_SIZE];
        uint32_t head;
        uint32_t tail;
    } rtt_down;
#endif
    uint8_t *heap_cur;
} bmp_platform_state_t;

static bmp_platform_state_t s_bmp_plat;

/* ---- 1. Platform HAL (platform_support.h / timing.h) ---- */

void platform_init(void)
{
    /* Board, SysTick, and USB composite stack are already initialized by main() */
    s_bmp_plat.max_freq_hz = 4000000U;
    if (s_bmp_plat.heap_cur == NULL) {
        s_bmp_plat.heap_cur = &__heap_start__;
    }
}

uint32_t platform_time_ms(void)
{
    return board_get_uptime_ms();
}

void platform_delay(uint32_t ms)
{
    uint32_t start = board_get_uptime_ms();
    while ((uint32_t)(board_get_uptime_ms() - start) < ms) {
        bmp_mode_background_poll();
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
    return board_srst_get();
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
        s_bmp_plat.max_freq_hz = 10000000U;
        return;
    }

    uint32_t sys_clk = board_get_sys_clock_hz();
    uint32_t half_cycles = sys_clk / (2U * freq);
    if (half_cycles <= 6U) {
        target_clk_divider = UINT32_MAX;
        s_bmp_plat.max_freq_hz = 10000000U;
    } else {
        target_clk_divider = (half_cycles - 6U) / 4U;
        s_bmp_plat.max_freq_hz = freq;
    }
}

uint32_t platform_max_frequency_get(void)
{
    return s_bmp_plat.max_freq_hz;
}

void platform_target_clk_output_enable(bool enable)
{
    /* Toggle only SWCLK/TCK output without disturbing nSRST or SWDIO direction state */
    bmp_tap_clk_output_enable(enable);
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

/* ---- 2. GDB RSP Transport (gdb_if.h) over USB CDC1 (ttyACM1) ---- */

static inline bool bmp_gdb_link_open(void)
{
    return bmp_mode_is_active() && usb_device_is_configured() && usb_cdc_is_dtr_asserted(USB_CDC_PORT_AUX);
}

int gdb_if_init(void)
{
    s_bmp_plat.gdb_tx.count = 0U;
    return 0;
}

void gdb_if_putchar(char c, bool flush)
{
    s_bmp_plat.gdb_tx.buf[s_bmp_plat.gdb_tx.count++] = (uint8_t)c;
    if (flush || (s_bmp_plat.gdb_tx.count >= BMP_GDB_TX_BUF_SIZE)) {
        gdb_if_flush(flush);
    }
}

void gdb_if_flush(bool force)
{
    (void)force;
    if (s_bmp_plat.gdb_tx.count == 0U) {
        return;
    }

    uint32_t offset = 0U;
    while (offset < s_bmp_plat.gdb_tx.count) {
        if (!bmp_gdb_link_open()) {
            s_bmp_plat.gdb_tx.count = 0U;
            return;
        }
        uint32_t space = usb_cdc_tx_space(USB_CDC_PORT_AUX);
        if (space == 0U) {
            bmp_mode_background_poll();
            continue;
        }
        uint32_t rem = s_bmp_plat.gdb_tx.count - offset;
        uint32_t chunk = (rem < space) ? rem : space;
        offset += usb_cdc_tx_write(USB_CDC_PORT_AUX, &s_bmp_plat.gdb_tx.buf[offset], chunk);
    }

    s_bmp_plat.gdb_tx.count = 0U;
}

char gdb_if_getchar(void)
{
    while (usb_cdc_rx_available(USB_CDC_PORT_AUX) == 0U) {
        if (!bmp_gdb_link_open()) {
            return '\x04';
        }
        bmp_mode_background_poll();
        if (usb_cdc_rx_available(USB_CDC_PORT_AUX) == 0U) {
            __WFI();
        }
    }

    uint8_t ch = 0U;
    if (usb_cdc_rx_read(USB_CDC_PORT_AUX, &ch, 1U) == 0U) {
        return '\x04';
    }
    return (char)ch;
}

char gdb_if_getchar_to(uint32_t timeout)
{
    if (timeout == 0U) {
        bmp_mode_background_poll();
        if (!bmp_gdb_link_open()) {
            return '\x04';
        }
        uint8_t ch = 0U;
        if (usb_cdc_rx_read(USB_CDC_PORT_AUX, &ch, 1U) > 0U) {
            return (char)ch;
        }
        return (char)-1;
    }

    platform_timeout_s rx_timeout;
    platform_timeout_set(&rx_timeout, timeout);

    while (usb_cdc_rx_available(USB_CDC_PORT_AUX) == 0U) {
        if (!bmp_gdb_link_open()) {
            return '\x04';
        }
        if (platform_timeout_is_expired(&rx_timeout)) {
            return (char)-1;
        }
        bmp_mode_background_poll();
    }

    uint8_t ch = 0U;
    if (usb_cdc_rx_read(USB_CDC_PORT_AUX, &ch, 1U) == 0U) {
        return '\x04';
    }
    return (char)ch;
}

#ifdef ENABLE_RTT
/* ---- 3. SEGGER RTT Transport (rtt_if.h) over USB CDC0 (ttyACM0) ---- */

int rtt_if_init(void)
{
    s_bmp_plat.rtt_down.head = 0U;
    s_bmp_plat.rtt_down.tail = 0U;
    return 0;
}

int rtt_if_exit(void)
{
    s_bmp_plat.rtt_down.head = 0U;
    s_bmp_plat.rtt_down.tail = 0U;
    return 0;
}

void rtt_load_recv_buf(const char *data_buf, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        uint32_t next_head = (s_bmp_plat.rtt_down.head + 1U) % RTT_DOWN_BUF_SIZE;
        if (next_head == s_bmp_plat.rtt_down.tail) {
            break;
        }
        s_bmp_plat.rtt_down.buf[s_bmp_plat.rtt_down.head] = (uint8_t)data_buf[i];
        s_bmp_plat.rtt_down.head = next_head;
    }
}

static void bmp_rtt_drain_cdc0(void)
{
    uint8_t tmp[64];
    while (usb_cdc_rx_available(USB_CDC_PORT_APP) > 0U) {
        uint32_t head = s_bmp_plat.rtt_down.head;
        uint32_t tail = s_bmp_plat.rtt_down.tail;
        uint32_t free_space = (tail <= head) ? (RTT_DOWN_BUF_SIZE - head + tail - 1U) : (tail - head - 1U);
        if (free_space == 0U) {
            if (rtt_flag_skip) {
                (void)usb_cdc_rx_read(USB_CDC_PORT_APP, tmp, sizeof(tmp));
                continue;
            }
            break;
        }
        uint32_t want = (free_space < sizeof(tmp)) ? free_space : (uint32_t)sizeof(tmp);
        uint32_t got = usb_cdc_rx_read(USB_CDC_PORT_APP, tmp, want);
        if (got == 0U) {
            break;
        }
        rtt_load_recv_buf((const char *)tmp, got);
    }
}

bool rtt_nodata(const uint32_t channel)
{
    if (channel != 0U) {
        return true;
    }
    bmp_rtt_drain_cdc0();
    return s_bmp_plat.rtt_down.head == s_bmp_plat.rtt_down.tail;
}

int32_t rtt_getchar(const uint32_t channel)
{
    if (channel != 0U) {
        return -1;
    }
    bmp_rtt_drain_cdc0();
    if (s_bmp_plat.rtt_down.head == s_bmp_plat.rtt_down.tail) {
        return -1;
    }
    uint8_t ch = s_bmp_plat.rtt_down.buf[s_bmp_plat.rtt_down.tail];
    s_bmp_plat.rtt_down.tail = (s_bmp_plat.rtt_down.tail + 1U) % RTT_DOWN_BUF_SIZE;
    return (int32_t)ch;
}

uint32_t rtt_write(const uint32_t channel, const char *buf, uint32_t len)
{
    if ((channel != 0U) || (len == 0U)) {
        return len;
    }
    if (!bmp_mode_is_active() || !usb_device_is_configured() || !usb_cdc_is_dtr_asserted(USB_CDC_PORT_APP)) {
        return len;
    }

    uint32_t offset = 0U;
    uint32_t start_ms = board_get_uptime_ms();
    while (offset < len) {
        uint32_t space = usb_cdc_tx_space(USB_CDC_PORT_APP);
        if (space == 0U) {
            if ((uint32_t)(board_get_uptime_ms() - start_ms) >= 25U) {
                return offset;
            }
            bmp_mode_background_poll();
            continue;
        }
        uint32_t rem = len - offset;
        uint32_t chunk = (rem < space) ? rem : space;
        offset += usb_cdc_tx_write(USB_CDC_PORT_APP, (const uint8_t *)&buf[offset], chunk);
    }
    return len;
}
#endif /* ENABLE_RTT */

/* ---- 4. Stack-Guarded Newlib _sbrk & Minimal Syscall Stubs ---- */

__attribute__((used)) void *_sbrk(ptrdiff_t incr)
{
    if (s_bmp_plat.heap_cur == NULL) {
        s_bmp_plat.heap_cur = &__heap_start__;
    }

    uint8_t *next = s_bmp_plat.heap_cur + incr;
    if ((next < &__heap_start__) || (next > &__heap_end__)) {
        errno = ENOMEM;
        return (void *)-1;
    }

    void *prev = s_bmp_plat.heap_cur;
    s_bmp_plat.heap_cur = next;
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
