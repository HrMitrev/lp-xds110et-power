/*
 * cdc_uart.c - LP-XDS110ET USB CDC ACM <-> UART0 / UART7 hardware bridge.
 *
 * Uses MSP432E4 DriverLib (MAP_UART*) exclusively:
 *   - UART0 (PA0 = XDS_RX, PA1 = XDS_TX) <-> CDC 0 (App UART, full-duplex)
 *   - UART7 (PC4 = XDS_SWO, RX-only)     <-> CDC 1 (Aux / SWO UART)
 *
 * Interrupts (UART0_IRQn, UART7_IRQn) run at priority 1 (same as GPIOB, above
 * SSI1/USB0 = 2) because each ISR only drains/fills the 16-byte UART FIFO into
 * local lock-free rings (< 1 us) without touching USB0 or SSI1, preventing
 * high-baud RX FIFO overruns during 256 kS/s EnergyTrace USB transfers.
 */

#include "cdc_uart.h"

#include <stdint.h>

#include "board_lp_xds110et.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/uart.h"
#include "usb/usb_device.h"

/* Power-of-two ring buffer sizes */
#define CDC_UART_RX_RING_SIZE 2048U
#define CDC_UART_TX_RING_SIZE 512U
#define CDC_UART_PUMP_CHUNK   256U

/* Clamped baud bounds safe for 120 MHz UART clock (IBRD <= 25000, HSE <= clk/10) */
#define CDC_UART_DEFAULT_BAUD 115200U
#define CDC_UART_MIN_BAUD     300U
#define CDC_UART_MAX_BAUD     12000000U

/* Same priority as GPIOB (1), above SSI1/USB0 (2); ISRs touch only local rings */
#define CDC_UART_IRQ_PRIO 1U

/*
 * Short (< 1 us) BASEPRI critical section masking priority >= CDC_UART_IRQ_PRIO.
 * Prevents lower-priority SSI1/USB0 (prio 2) from preempting main() while UART0
 * is masked, eliminating hardware RX FIFO overruns at >= 921.6 kbps.
 */
static inline uint32_t uart_crit_enter(void)
{
    uint32_t prev = __get_BASEPRI();
    uint32_t mask = CDC_UART_IRQ_PRIO << (8U - __NVIC_PRIO_BITS);
    if ((prev == 0U) || (mask < prev)) {
        __set_BASEPRI(mask);
    }
    return prev;
}

static inline void uart_crit_exit(uint32_t prev)
{
    __set_BASEPRI(prev);
}

typedef struct {
    uint8_t *buf;
    uint32_t mask;
    volatile uint32_t head;
    volatile uint32_t tail;
} uart_ring_t;

static uint8_t g_uart_rx_storage[USB_CDC_PORT_COUNT][CDC_UART_RX_RING_SIZE];
static uint8_t g_uart0_tx_storage[CDC_UART_TX_RING_SIZE];

static uart_ring_t g_uart_rx_rings[USB_CDC_PORT_COUNT] = {
    {g_uart_rx_storage[0], CDC_UART_RX_RING_SIZE - 1U, 0U, 0U},
    {g_uart_rx_storage[1], CDC_UART_RX_RING_SIZE - 1U, 0U, 0U},
};
static uart_ring_t g_uart0_tx_ring = {g_uart0_tx_storage, CDC_UART_TX_RING_SIZE - 1U, 0U, 0U};

static const uint32_t g_uart_bases[USB_CDC_PORT_COUNT] = {BOARD_APP_UART_BASE, BOARD_AUX_UART_BASE};
static const IRQn_Type g_uart_irqs[USB_CDC_PORT_COUNT] = {UART0_IRQn, UART7_IRQn};

/* ---- Lock-Free Single-Byte / Block Ring Helpers ---- */

static inline uint32_t uart_ring_used(const uart_ring_t *ring)
{
    return (ring->head - ring->tail) & ring->mask;
}

static inline uint32_t uart_ring_free(const uart_ring_t *ring)
{
    return ring->mask - uart_ring_used(ring);
}

static inline void uart_ring_reset(uart_ring_t *ring)
{
    ring->head = 0U;
    ring->tail = 0U;
}

static inline void uart_ring_discard(uart_ring_t *ring)
{
    /* Consumer-only update: safe while producer ISR updates head */
    ring->tail = ring->head;
}

static inline void uart_ring_push_byte(uart_ring_t *ring, uint8_t byte)
{
    uint32_t next_head = (ring->head + 1U) & ring->mask;
    if (next_head != ring->tail) {
        ring->buf[ring->head] = byte;
        ring->head = next_head;
    }
}

static inline uint8_t uart_ring_pop_byte(uart_ring_t *ring)
{
    uint8_t byte = ring->buf[ring->tail];
    ring->tail = (ring->tail + 1U) & ring->mask;
    return byte;
}

static uint32_t uart_ring_pop_chunk(uart_ring_t *ring, uint8_t *dst, uint32_t max_len)
{
    uint32_t avail = uart_ring_used(ring);
    if (avail > max_len) {
        avail = max_len;
    }
    for (uint32_t i = 0U; i < avail; i++) {
        dst[i] = uart_ring_pop_byte(ring);
    }
    return avail;
}

/* ---- UART Hardware Configuration ---- */

static uint32_t cdc_coding_to_uart_config(const usb_cdc_line_coding_t *coding)
{
    uint32_t cfg = 0U;

    switch (coding->data_bits) {
    case 5U:
        cfg |= UART_CONFIG_WLEN_5;
        break;
    case 6U:
        cfg |= UART_CONFIG_WLEN_6;
        break;
    case 7U:
        cfg |= UART_CONFIG_WLEN_7;
        break;
    case 8U:
    default:
        cfg |= UART_CONFIG_WLEN_8;
        break;
    }

    if (coding->stop_bits == 2U) {
        cfg |= UART_CONFIG_STOP_TWO;
    } else {
        cfg |= UART_CONFIG_STOP_ONE;
    }

    switch (coding->parity) {
    case 1U:
        cfg |= UART_CONFIG_PAR_ODD;
        break;
    case 2U:
        cfg |= UART_CONFIG_PAR_EVEN;
        break;
    case 3U:
        cfg |= UART_CONFIG_PAR_ONE;
        break;
    case 4U:
        cfg |= UART_CONFIG_PAR_ZERO;
        break;
    case 0U:
    default:
        cfg |= UART_CONFIG_PAR_NONE;
        break;
    }

    return cfg;
}

static uint32_t cdc_clamp_baud(uint32_t baud)
{
    if (baud == 0U) {
        return CDC_UART_DEFAULT_BAUD;
    }
    if (baud < CDC_UART_MIN_BAUD) {
        return CDC_UART_MIN_BAUD;
    }
    if (baud > CDC_UART_MAX_BAUD) {
        return CDC_UART_MAX_BAUD;
    }
    return baud;
}

static void uart0_tx_fill_fifo(void)
{
    while ((uart_ring_used(&g_uart0_tx_ring) > 0U) && MAP_UARTSpaceAvail(BOARD_APP_UART_BASE)) {
        uint8_t ch = uart_ring_pop_byte(&g_uart0_tx_ring);
        MAP_UARTCharPutNonBlocking(BOARD_APP_UART_BASE, ch);
    }
}

static void cdc_uart_apply_line_coding(uint8_t port, const usb_cdc_line_coding_t *coding)
{
    if (port >= USB_CDC_PORT_COUNT) {
        return;
    }

    uint32_t base = g_uart_bases[port];
    IRQn_Type irqn = g_uart_irqs[port];
    uint32_t baud = cdc_clamp_baud(coding->baud_rate);
    uint32_t cfg = cdc_coding_to_uart_config(coding);

    NVIC_DisableIRQ(irqn);

    MAP_UARTConfigSetExpClk(base, board_get_sys_clock_hz(), baud, cfg);
    MAP_UARTFIFOLevelSet(base, UART_FIFO_TX2_8, UART_FIFO_RX2_8);
    MAP_UARTTxIntModeSet(base, UART_TXINT_MODE_FIFO);
    MAP_UARTRxErrorClear(base);
    MAP_UARTIntClear(base, UART_INT_RX | UART_INT_RT | UART_INT_TX);
    MAP_UARTIntEnable(base, UART_INT_RX | UART_INT_RT);

    if (port == USB_CDC_PORT_APP) {
        uart0_tx_fill_fifo();
        if (uart_ring_used(&g_uart0_tx_ring) > 0U) {
            MAP_UARTIntEnable(base, UART_INT_TX);
        }
    }

    NVIC_EnableIRQ(irqn);
}

/* ---- UART Interrupt Handlers ---- */

static void uart_rx_drain_isr(uint32_t base, uart_ring_t *rx_ring)
{
    while (MAP_UARTCharsAvail(base)) {
        int32_t ch = MAP_UARTCharGetNonBlocking(base);
        if (ch >= 0) {
            uart_ring_push_byte(rx_ring, (uint8_t)ch);
        }
    }

    if (MAP_UARTRxErrorGet(base) != 0U) {
        MAP_UARTRxErrorClear(base);
    }
}

void uart0_irq_handler(void)
{
    uint32_t status = MAP_UARTIntStatus(BOARD_APP_UART_BASE, true);
    MAP_UARTIntClear(BOARD_APP_UART_BASE, status);

    if ((status & (UART_INT_RX | UART_INT_RT)) != 0U) {
        uart_rx_drain_isr(BOARD_APP_UART_BASE, &g_uart_rx_rings[USB_CDC_PORT_APP]);
    }

    if ((status & UART_INT_TX) != 0U) {
        uart0_tx_fill_fifo();
        if (uart_ring_used(&g_uart0_tx_ring) == 0U) {
            MAP_UARTIntDisable(BOARD_APP_UART_BASE, UART_INT_TX);
        }
    }
}

void uart7_irq_handler(void)
{
    uint32_t status = MAP_UARTIntStatus(BOARD_AUX_UART_BASE, true);
    MAP_UARTIntClear(BOARD_AUX_UART_BASE, status);

    if ((status & (UART_INT_RX | UART_INT_RT)) != 0U) {
        uart_rx_drain_isr(BOARD_AUX_UART_BASE, &g_uart_rx_rings[USB_CDC_PORT_AUX]);
    }
}

/* ---- Public API ---- */

void cdc_uart_init(void)
{
    uart_ring_reset(&g_uart_rx_rings[USB_CDC_PORT_APP]);
    uart_ring_reset(&g_uart_rx_rings[USB_CDC_PORT_AUX]);
    uart_ring_reset(&g_uart0_tx_ring);

    /* Enable UART0 (CDC0 App UART) and UART7 (CDC1 Aux/SWO RX-only) */
    MAP_SysCtlPeripheralReset(BOARD_APP_UART_PERIPH);
    MAP_SysCtlPeripheralEnable(BOARD_APP_UART_PERIPH);
    while (!MAP_SysCtlPeripheralReady(BOARD_APP_UART_PERIPH)) {
    }

    MAP_SysCtlPeripheralReset(BOARD_AUX_UART_PERIPH);
    MAP_SysCtlPeripheralEnable(BOARD_AUX_UART_PERIPH);
    while (!MAP_SysCtlPeripheralReady(BOARD_AUX_UART_PERIPH)) {
    }

    /* Configure UART0 pins: PA0 (U0RX) + PA1 (U0TX) */
    MAP_GPIOPinConfigure(GPIO_PA0_U0RX);
    MAP_GPIOPinConfigure(GPIO_PA1_U0TX);
    MAP_GPIOPinTypeUART(BOARD_APP_UART_PORT_BASE, BOARD_APP_UART_RX_PIN | BOARD_APP_UART_TX_PIN);
    /* Weak pull-up on RX prevents floating break/framing noise when target is unpowered */
    MAP_GPIOPadConfigSet(BOARD_APP_UART_PORT_BASE, BOARD_APP_UART_RX_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

    /* Configure UART7 pin: PC4 (U7RX only; PC5 is XDS_TMS_DIR and must not be touched) */
    MAP_GPIOPinConfigure(GPIO_PC4_U7RX);
    MAP_GPIOPinTypeUART(BOARD_AUX_UART_PORT_BASE, BOARD_AUX_UART_RX_PIN);
    MAP_GPIOPadConfigSet(BOARD_AUX_UART_PORT_BASE, BOARD_AUX_UART_RX_PIN, GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

    MAP_UARTClockSourceSet(BOARD_APP_UART_BASE, UART_CLOCK_SYSTEM);
    MAP_UARTClockSourceSet(BOARD_AUX_UART_BASE, UART_CLOCK_SYSTEM);

    NVIC_SetPriority(UART0_IRQn, CDC_UART_IRQ_PRIO);
    NVIC_SetPriority(UART7_IRQn, CDC_UART_IRQ_PRIO);
    NVIC_ClearPendingIRQ(UART0_IRQn);
    NVIC_ClearPendingIRQ(UART7_IRQn);

    usb_cdc_line_coding_t coding;
    for (uint8_t port = 0U; port < USB_CDC_PORT_COUNT; port++) {
        usb_cdc_get_line_coding(port, &coding);
        cdc_uart_apply_line_coding(port, &coding);
    }
}

bool cdc_uart_poll(void)
{
    bool did_work = false;
    uint8_t chunk_buf[CDC_UART_PUMP_CHUNK];

    /* 1. Apply any pending USB CDC SET_LINE_CODING changes */
    for (uint8_t port = 0U; port < USB_CDC_PORT_COUNT; port++) {
        usb_cdc_line_coding_t coding;
        if (usb_cdc_take_line_coding(port, &coding)) {
            cdc_uart_apply_line_coding(port, &coding);
            did_work = true;
        }
    }

    if (!usb_device_is_configured()) {
        /* Discard idle UART RX data while USB host is disconnected */
        uart_ring_discard(&g_uart_rx_rings[USB_CDC_PORT_APP]);
        uart_ring_discard(&g_uart_rx_rings[USB_CDC_PORT_AUX]);
        return did_work;
    }

    /* 2. Forward UART0/UART7 RX rings -> USB CDC0/CDC1 IN */
    for (uint8_t port = 0U; port < USB_CDC_PORT_COUNT; port++) {
        uint32_t rx_avail = uart_ring_used(&g_uart_rx_rings[port]);
        if (rx_avail == 0U) {
            continue;
        }
        uint32_t usb_space = usb_cdc_tx_space(port);
        if (usb_space == 0U) {
            continue;
        }
        uint32_t to_send = (rx_avail < usb_space) ? rx_avail : usb_space;
        if (to_send > sizeof(chunk_buf)) {
            to_send = sizeof(chunk_buf);
        }
        uint32_t got = uart_ring_pop_chunk(&g_uart_rx_rings[port], chunk_buf, to_send);
        if (got > 0U) {
            (void)usb_cdc_tx_write(port, chunk_buf, got);
            did_work = true;
        }
    }

    /* 3. Forward USB CDC0 OUT -> UART0 TX ring (lossless USB NAK back-pressure) */
    uint32_t cdc0_avail = usb_cdc_rx_available(USB_CDC_PORT_APP);
    uint32_t tx_space = uart_ring_free(&g_uart0_tx_ring);
    if ((cdc0_avail > 0U) && (tx_space > 0U)) {
        uint32_t to_tx = (cdc0_avail < tx_space) ? cdc0_avail : tx_space;
        if (to_tx > sizeof(chunk_buf)) {
            to_tx = sizeof(chunk_buf);
        }
        uint32_t got = usb_cdc_rx_read(USB_CDC_PORT_APP, chunk_buf, to_tx);
        if (got > 0U) {
            for (uint32_t i = 0U; i < got; i++) {
                uart_ring_push_byte(&g_uart0_tx_ring, chunk_buf[i]);
            }
            uint32_t crit = uart_crit_enter();
            uart0_tx_fill_fifo();
            if (uart_ring_used(&g_uart0_tx_ring) > 0U) {
                MAP_UARTIntEnable(BOARD_APP_UART_BASE, UART_INT_TX);
            }
            uart_crit_exit(crit);
            did_work = true;
        }
    }

    /* 4. Drain & discard CDC1 OUT (UART7 on LP-XDS110ET is RX-only on PC4) */
    uint32_t cdc1_avail = usb_cdc_rx_available(USB_CDC_PORT_AUX);
    if (cdc1_avail > 0U) {
        uint32_t to_drop = (cdc1_avail > sizeof(chunk_buf)) ? sizeof(chunk_buf) : cdc1_avail;
        (void)usb_cdc_rx_read(USB_CDC_PORT_AUX, chunk_buf, to_drop);
        did_work = true;
    }

    return did_work;
}
