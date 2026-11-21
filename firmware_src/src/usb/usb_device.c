/*
 * usb_device.c - MSP432E401Y USB0 High-Speed (USB3300 ULPI) device stack.
 *
 * Implements:
 *   - USB3300-EZK ULPI pin mux, external 60 MHz clock, and HS PHY configuration
 *   - USB 2.0 Standard EP0 control transfer state machine + per-port CDC ACM
 *   - Bulk 0 (OUT 0x02 / IN 0x83) XDS110 command ring buffers
 *   - Bulk 1 (OUT 0x06 / IN 0x87) 64 KiB EnergyTrace streaming ring buffer
 *   - CDC 0  (OUT 0x01 / IN 0x82 / INT 0x81) App UART ring buffers
 *   - CDC 1  (OUT 0x04 / IN 0x85 / INT 0x84) Aux/SWO UART ring buffers
 */

#include "usb_device.h"

#include <string.h>

#include "board_lp_xds110et.h"
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/usb.h"
#include "usb_descriptors.h"

/* Standard USB setup request codes */
#define USB_REQ_GET_STATUS        0x00U
#define USB_REQ_CLEAR_FEATURE     0x01U
#define USB_REQ_SET_FEATURE       0x03U
#define USB_REQ_SET_ADDRESS       0x05U
#define USB_REQ_GET_DESCRIPTOR    0x06U
#define USB_REQ_GET_CONFIGURATION 0x08U
#define USB_REQ_SET_CONFIGURATION 0x09U
#define USB_REQ_GET_INTERFACE     0x0AU
#define USB_REQ_SET_INTERFACE     0x0BU

/* Request type bitfields */
#define USB_REQ_TYPE_MASK      0x60U
#define USB_REQ_TYPE_STANDARD  0x00U
#define USB_REQ_TYPE_CLASS     0x20U
#define USB_REQ_TYPE_VENDOR    0x40U
#define USB_REQ_DIR_IN_MASK    0x80U
#define USB_REQ_RECIPIENT_MASK 0x1FU
#define USB_REQ_RECIPIENT_DEV  0x00U
#define USB_REQ_RECIPIENT_IF   0x01U
#define USB_REQ_RECIPIENT_EP   0x02U

/* CDC ACM class requests */
#define USB_CDC_SET_LINE_CODING        0x20U
#define USB_CDC_GET_LINE_CODING        0x21U
#define USB_CDC_SET_CONTROL_LINE_STATE 0x22U
#define USB_CDC_SEND_BREAK             0x23U
#define USB_CDC_LINE_CODING_LEN        7U

/* USB3300 ULPI PHY register addresses */
#define ULPI_REG_FUNC_CTRL 0x04U
#define ULPI_REG_IF_CTRL   0x07U

/*
 * MSP432E4 USB0 4,096-byte FIFO RAM layout (all addresses naturally aligned):
 *   0..63      EP0 Control (64 B, fixed hardware)
 *   64..79     CDC0 INT IN  0x81 (16 B)
 *   80..95     CDC1 INT IN  0x84 (16 B)
 *   96..127    CDC1 IN      0x85 (32 B, software-capped <=32 B/pkt)
 *   128..255   CDC0 IN      0x82 (128 B, software-capped <=128 B/pkt)
 *   256..511   CMD IN       0x83 (256 B, software-capped <=256 B/pkt)
 *   512..1023  CMD OUT      0x02 (512 B)
 *   1024..1535 DATA IN      0x87 (512 B, EnergyTrace streaming)
 *   1536..2047 DATA OUT     0x06 (512 B)
 *   2048..2559 CDC0 OUT     0x01 (512 B)
 *   2560..3071 CDC1 OUT     0x04 (512 B)
 *   3072..3583 DAP v2 OUT   0x05 (512 B, CMSIS-DAP v2 WinUSB)
 *   3584..4095 DAP v2 IN    0x86 (512 B, CMSIS-DAP v2 WinUSB)
 * Total used: 4,096 / 4,096 bytes.
 */
#define USB_FIFO_ADDR_CDC0_INT 64U
#define USB_FIFO_ADDR_CDC1_INT 80U
#define USB_FIFO_ADDR_CDC1_IN  96U
#define USB_FIFO_ADDR_CDC0_IN  128U
#define USB_FIFO_ADDR_CMD_IN   256U
#define USB_FIFO_ADDR_CMD_OUT  512U
#define USB_FIFO_ADDR_DATA_IN  1024U
#define USB_FIFO_ADDR_DATA_OUT 1536U
#define USB_FIFO_ADDR_CDC0_OUT 2048U
#define USB_FIFO_ADDR_CDC1_OUT 2560U
#define USB_FIFO_ADDR_DAP_OUT  3072U
#define USB_FIFO_ADDR_DAP_IN   3584U
#define USB_CMD_IN_FIFO_BYTES  256U
#define USB_CDC0_IN_FIFO_BYTES 128U
#define USB_CDC1_IN_FIFO_BYTES 32U

typedef enum {
    EP0_STATE_IDLE = 0,
    EP0_STATE_TX_DATA,
    EP0_STATE_RX_DATA,
    EP0_STATE_STATUS_SET_ADDR,
} ep0_state_t;

typedef struct {
    uint8_t *buf;
    uint32_t mask;
    volatile uint32_t head;
    volatile uint32_t tail;
} usb_ring_t;

/* Backing storage for Bulk 0 (CMD), Bulk 1 (EnergyTrace Data), and CDC 0/1 */
static uint8_t g_cmd_rx_storage[USB_CMD_RX_BUF_SIZE];
static uint8_t g_cmd_tx_storage[USB_CMD_TX_BUF_SIZE];
static uint8_t g_data_rx_storage[USB_DATA_RX_BUF_SIZE];
static uint8_t g_data_tx_storage[USB_DATA_TX_BUF_SIZE];
static uint8_t g_cdc_rx_storage[USB_CDC_PORT_COUNT][USB_CDC_RX_BUF_SIZE];
static uint8_t g_cdc_tx_storage[USB_CDC_PORT_COUNT][USB_CDC_TX_BUF_SIZE];

static usb_ring_t g_cmd_rx_ring = {g_cmd_rx_storage, USB_CMD_RX_BUF_SIZE - 1U, 0U, 0U};
static usb_ring_t g_cmd_tx_ring = {g_cmd_tx_storage, USB_CMD_TX_BUF_SIZE - 1U, 0U, 0U};
static usb_ring_t g_data_rx_ring = {g_data_rx_storage, USB_DATA_RX_BUF_SIZE - 1U, 0U, 0U};
static usb_ring_t g_data_tx_ring = {g_data_tx_storage, USB_DATA_TX_BUF_SIZE - 1U, 0U, 0U};
static usb_ring_t g_cdc_rx_rings[USB_CDC_PORT_COUNT] = {
    {g_cdc_rx_storage[0], USB_CDC_RX_BUF_SIZE - 1U, 0U, 0U},
    {g_cdc_rx_storage[1], USB_CDC_RX_BUF_SIZE - 1U, 0U, 0U},
};
static usb_ring_t g_cdc_tx_rings[USB_CDC_PORT_COUNT] = {
    {g_cdc_tx_storage[0], USB_CDC_TX_BUF_SIZE - 1U, 0U, 0U},
    {g_cdc_tx_storage[1], USB_CDC_TX_BUF_SIZE - 1U, 0U, 0U},
};

/* CMSIS-DAP v2 WinUSB Bulk variable-length packet queues */
typedef struct {
    uint16_t len;
    uint8_t data[USB_DAP_MAX_PKT_SIZE];
} usb_dap_pkt_t;

static usb_dap_pkt_t g_dap_rx_queue[USB_DAP_QUEUE_DEPTH];
static usb_dap_pkt_t g_dap_tx_queue[USB_DAP_QUEUE_DEPTH];
static volatile uint8_t g_dap_rx_head = 0U;
static volatile uint8_t g_dap_rx_tail = 0U;
static volatile uint8_t g_dap_rx_count = 0U;
static volatile uint8_t g_dap_tx_head = 0U;
static volatile uint8_t g_dap_tx_tail = 0U;
static volatile uint8_t g_dap_tx_count = 0U;

/* Device & endpoint state */
static volatile bool g_usb_configured = false;
static volatile bool g_usb_high_speed = true;
static volatile uint16_t g_bulk_mps = USB_BULK_MPS_HS;
static volatile bool g_cmd_tx_busy = false;
static volatile bool g_cmd_tx_need_zlp = false;
static volatile bool g_data_tx_busy = false;
static volatile bool g_cdc_tx_busy[USB_CDC_PORT_COUNT] = {false, false};
static volatile bool g_dap_tx_busy = false;

static ep0_state_t g_ep0_state = EP0_STATE_IDLE;
static uint8_t g_pending_addr = 0U;
static const uint8_t *g_ep0_tx_ptr = NULL;
static uint16_t g_ep0_tx_rem = 0U;
static bool g_ep0_tx_need_zlp = false;
static uint8_t g_ep0_status_buf[2];
static uint8_t g_ep0_rx_scratch[16];
static uint16_t g_ep0_rx_rem = 0U;
static uint8_t g_ep0_rx_cdc_port = USB_CDC_PORT_APP;

/* Per-port CDC ACM state (defaults: 115200 8N1) */
static usb_cdc_line_coding_t g_cdc_coding[USB_CDC_PORT_COUNT] = {
    {115200U, 0U, 0U, 8U},
    {115200U, 0U, 0U, 8U},
};
static uint8_t g_cdc_coding_wire[USB_CDC_LINE_CODING_LEN];
static volatile bool g_cdc_coding_dirty[USB_CDC_PORT_COUNT] = {false, false};
static volatile uint16_t g_cdc_ctrl_state[USB_CDC_PORT_COUNT] = {0U, 0U};

static const uint32_t g_cdc_out_ep[USB_CDC_PORT_COUNT] = {USB_EP_1, USB_EP_4};
static const uint32_t g_cdc_in_ep[USB_CDC_PORT_COUNT] = {USB_EP_2, USB_EP_5};
static const uint32_t g_cdc_in_fifo_bytes[USB_CDC_PORT_COUNT] = {USB_CDC0_IN_FIFO_BYTES, USB_CDC1_IN_FIFO_BYTES};

/* Shared NVIC priority for USB0 and EnergyTrace SSI1 (must match et_hdr.c) */
#define USB_IRQ_PRIO 2U

/*
 * Nestable critical section masking USB0 (prio 2) and SSI1 (prio 2) via
 * Cortex-M4 BASEPRI while keeping higher-priority GPIOB/UART0/UART7 (prio 1)
 * completely unmasked.
 */
static inline uint32_t usb_crit_enter(void)
{
    uint32_t prev = __get_BASEPRI();
    uint32_t mask = USB_IRQ_PRIO << (8U - __NVIC_PRIO_BITS);
    if ((prev == 0U) || (mask < prev)) {
        __set_BASEPRI(mask);
    }
    return prev;
}

static inline void usb_crit_exit(uint32_t prev)
{
    __set_BASEPRI(prev);
}

/* ---- Ring buffer helpers ---- */

static inline uint32_t ring_used(const usb_ring_t *ring)
{
    return (ring->head - ring->tail) & ring->mask;
}

static inline uint32_t ring_free(const usb_ring_t *ring)
{
    return ring->mask - ring_used(ring);
}

static void ring_reset(usb_ring_t *ring)
{
    ring->head = 0U;
    ring->tail = 0U;
}

static uint32_t ring_push(usb_ring_t *ring, const uint8_t *src, uint32_t len)
{
    uint32_t avail = ring_free(ring);
    if (len > avail) {
        len = avail;
    }
    uint32_t head = ring->head;
    uint32_t size = ring->mask + 1U;
    uint32_t first = size - head;
    if (first > len) {
        first = len;
    }
    memcpy(&ring->buf[head], src, first);
    if (len > first) {
        memcpy(&ring->buf[0], src + first, len - first);
    }
    ring->head = (head + len) & ring->mask;
    return len;
}

static uint32_t ring_peek(const usb_ring_t *ring, uint8_t *dst, uint32_t len)
{
    uint32_t avail = ring_used(ring);
    if (len > avail) {
        len = avail;
    }
    uint32_t tail = ring->tail;
    uint32_t size = ring->mask + 1U;
    uint32_t first = size - tail;
    if (first > len) {
        first = len;
    }
    memcpy(dst, &ring->buf[tail], first);
    if (len > first) {
        memcpy(dst + first, &ring->buf[0], len - first);
    }
    return len;
}

static uint32_t ring_pop(usb_ring_t *ring, uint8_t *dst, uint32_t len)
{
    uint32_t got = ring_peek(ring, dst, len);
    ring->tail = (ring->tail + got) & ring->mask;
    return got;
}

/* ---- CDC Line Coding Serialization ---- */

static bool usb_iface_to_cdc_port(uint16_t iface, uint8_t *out_port)
{
    if ((iface == USB_IF_CDC0_CTRL) || (iface == USB_IF_CDC0_DATA)) {
        *out_port = USB_CDC_PORT_APP;
        return true;
    }
    if ((iface == USB_IF_CDC1_CTRL) || (iface == USB_IF_CDC1_DATA)) {
        *out_port = USB_CDC_PORT_AUX;
        return true;
    }
    return false;
}

static void usb_pack_line_coding(const usb_cdc_line_coding_t *coding, uint8_t out[USB_CDC_LINE_CODING_LEN])
{
    out[0] = (uint8_t)(coding->baud_rate & 0xFFU);
    out[1] = (uint8_t)((coding->baud_rate >> 8U) & 0xFFU);
    out[2] = (uint8_t)((coding->baud_rate >> 16U) & 0xFFU);
    out[3] = (uint8_t)((coding->baud_rate >> 24U) & 0xFFU);
    out[4] = coding->stop_bits;
    out[5] = coding->parity;
    out[6] = coding->data_bits;
}

static void usb_unpack_line_coding(const uint8_t raw[USB_CDC_LINE_CODING_LEN], usb_cdc_line_coding_t *out)
{
    out->baud_rate =
        (uint32_t)raw[0] | ((uint32_t)raw[1] << 8U) | ((uint32_t)raw[2] << 16U) | ((uint32_t)raw[3] << 24U);
    out->stop_bits = raw[4];
    out->parity = raw[5];
    out->data_bits = raw[6];
}

/* ---- ULPI & Hardware PHY Setup ---- */

static void usb_init_ulpi_pins(void)
{
    /* PB0 (USB0ID) and PB1 (USB0VBUS) as analog */
    MAP_GPIOPinTypeUSBAnalog(GPIO_PORTB_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    /* PB2 (USB0STP) and PB3 (USB0CLK) */
    MAP_GPIOPinConfigure(GPIO_PB2_USB0STP);
    MAP_GPIOPinConfigure(GPIO_PB3_USB0CLK);
    MAP_GPIOPinTypeUSBDigital(BOARD_ULPI_CTRL_PORT_BASE, BOARD_ULPI_STP_PIN | BOARD_ULPI_CLK_PIN);
    MAP_GPIOPadConfigSet(
        BOARD_ULPI_CTRL_PORT_BASE, BOARD_ULPI_STP_PIN | BOARD_ULPI_CLK_PIN, GPIO_STRENGTH_12MA, GPIO_PIN_TYPE_STD);

    /* PP2 (USB0NXT), PP3 (USB0DIR), PP4 (USB0D7), PP5 (USB0D6) */
    MAP_GPIOPinConfigure(GPIO_PP2_USB0NXT);
    MAP_GPIOPinConfigure(GPIO_PP3_USB0DIR);
    MAP_GPIOPinConfigure(GPIO_PP4_USB0D7);
    MAP_GPIOPinConfigure(GPIO_PP5_USB0D6);
    MAP_GPIOPinTypeUSBDigital(BOARD_ULPI_DATA_HI_BASE, BOARD_ULPI_HI_PINS);
    MAP_GPIOPadConfigSet(BOARD_ULPI_DATA_HI_BASE, BOARD_ULPI_HI_PINS, GPIO_STRENGTH_12MA, GPIO_PIN_TYPE_STD);

    /* PL0..PL5 (USB0D0..USB0D5) */
    MAP_GPIOPinConfigure(GPIO_PL0_USB0D0);
    MAP_GPIOPinConfigure(GPIO_PL1_USB0D1);
    MAP_GPIOPinConfigure(GPIO_PL2_USB0D2);
    MAP_GPIOPinConfigure(GPIO_PL3_USB0D3);
    MAP_GPIOPinConfigure(GPIO_PL4_USB0D4);
    MAP_GPIOPinConfigure(GPIO_PL5_USB0D5);
    MAP_GPIOPinTypeUSBDigital(BOARD_ULPI_DATA_LO_BASE, BOARD_ULPI_DATA_LO_PINS);
    MAP_GPIOPadConfigSet(BOARD_ULPI_DATA_LO_BASE, BOARD_ULPI_DATA_LO_PINS, GPIO_STRENGTH_12MA, GPIO_PIN_TYPE_STD);
}

static void usb_configure_endpoints(void)
{
    uint32_t mps = g_bulk_mps;
    uint32_t fifo_full = g_usb_high_speed ? USB_FIFO_SZ_512 : USB_FIFO_SZ_64;
    uint32_t fifo_cmd_in = g_usb_high_speed ? USB_FIFO_SZ_256 : USB_FIFO_SZ_64;
    uint32_t fifo_cdc0_in = g_usb_high_speed ? USB_FIFO_SZ_128 : USB_FIFO_SZ_64;

    /* CDC0 / CDC1 Interrupt IN (EP1 IN 0x81, EP4 IN 0x84) */
    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_1, USB_CDC_INT_MPS, USB_EP_MODE_INT | USB_EP_DEV_IN);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_1, USB_FIFO_ADDR_CDC0_INT, USB_FIFO_SZ_16, USB_EP_DEV_IN);

    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_4, USB_CDC_INT_MPS, USB_EP_MODE_INT | USB_EP_DEV_IN);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_4, USB_FIFO_ADDR_CDC1_INT, USB_FIFO_SZ_16, USB_EP_DEV_IN);

    /* Bulk 0 OUT (EP2, 0x02) & IN (EP3, 0x83) — XDS110 Command Port */
    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_2, mps, USB_EP_MODE_BULK | USB_EP_DEV_OUT);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_2, USB_FIFO_ADDR_CMD_OUT, fifo_full, USB_EP_DEV_OUT);

    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_3, mps, USB_EP_MODE_BULK | USB_EP_DEV_IN);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_3, USB_FIFO_ADDR_CMD_IN, fifo_cmd_in, USB_EP_DEV_IN);

    /* Bulk 1 OUT (EP6, 0x06) & IN (EP7, 0x87) — EnergyTrace Data Port */
    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_6, mps, USB_EP_MODE_BULK | USB_EP_DEV_OUT);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_6, USB_FIFO_ADDR_DATA_OUT, fifo_full, USB_EP_DEV_OUT);

    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_7, mps, USB_EP_MODE_BULK | USB_EP_DEV_IN);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_7, USB_FIFO_ADDR_DATA_IN, fifo_full, USB_EP_DEV_IN);

    /* CDC0 Bulk OUT (EP1, 0x01) & IN (EP2, 0x82) — Application UART */
    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_1, mps, USB_EP_MODE_BULK | USB_EP_DEV_OUT);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_1, USB_FIFO_ADDR_CDC0_OUT, fifo_full, USB_EP_DEV_OUT);

    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_2, mps, USB_EP_MODE_BULK | USB_EP_DEV_IN);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_2, USB_FIFO_ADDR_CDC0_IN, fifo_cdc0_in, USB_EP_DEV_IN);

    /* CDC1 Bulk OUT (EP4, 0x04) & IN (EP5, 0x85) — Aux / SWO UART */
    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_4, mps, USB_EP_MODE_BULK | USB_EP_DEV_OUT);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_4, USB_FIFO_ADDR_CDC1_OUT, fifo_full, USB_EP_DEV_OUT);

    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_5, mps, USB_EP_MODE_BULK | USB_EP_DEV_IN);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_5, USB_FIFO_ADDR_CDC1_IN, USB_FIFO_SZ_32, USB_EP_DEV_IN);

    /* CMSIS-DAP v2 WinUSB Bulk OUT (EP5, 0x05) & IN (EP6, 0x86) */
    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_5, mps, USB_EP_MODE_BULK | USB_EP_DEV_OUT);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_5, USB_FIFO_ADDR_DAP_OUT, fifo_full, USB_EP_DEV_OUT);

    MAP_USBDevEndpointConfigSet(USB0_BASE, USB_EP_6, mps, USB_EP_MODE_BULK | USB_EP_DEV_IN);
    MAP_USBFIFOConfigSet(USB0_BASE, USB_EP_6, USB_FIFO_ADDR_DAP_IN, fifo_full, USB_EP_DEV_IN);

    g_cmd_tx_busy = false;
    g_cmd_tx_need_zlp = false;
    g_data_tx_busy = false;
    g_cdc_tx_busy[0] = false;
    g_cdc_tx_busy[1] = false;
    g_dap_tx_busy = false;
}

/* ---- CMSIS-DAP v2 Bulk IN/OUT Packet Queue Helpers ---- */

static void dap_queues_reset(void)
{
    g_dap_rx_head = 0U;
    g_dap_rx_tail = 0U;
    g_dap_rx_count = 0U;
    g_dap_tx_head = 0U;
    g_dap_tx_tail = 0U;
    g_dap_tx_count = 0U;
    g_dap_tx_busy = false;
}

static void usb_kick_dap_in(void)
{
    if (!g_usb_configured || g_dap_tx_busy || (g_dap_tx_count == 0U)) {
        return;
    }

    uint8_t tail = g_dap_tx_tail;
    uint16_t pkt_len = g_dap_tx_queue[tail].len;
    g_dap_tx_tail = (uint8_t)((tail + 1U) % USB_DAP_QUEUE_DEPTH);
    g_dap_tx_count--;
    g_dap_tx_busy = true;

    MAP_USBEndpointDataPut(USB0_BASE, USB_EP_6, g_dap_tx_queue[tail].data, pkt_len);
    MAP_USBEndpointDataSend(USB0_BASE, USB_EP_6, USB_TRANS_IN);
}

static void usb_handle_dap_out(void)
{
    uint32_t avail = MAP_USBEndpointDataAvail(USB0_BASE, USB_EP_5);
    if (avail == 0U) {
        MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_5, false);
        return;
    }

    /* Hold packet in hardware FIFO (USB NAK flow control) if queue is full */
    if (g_dap_rx_count >= USB_DAP_QUEUE_DEPTH) {
        return;
    }

    uint8_t head = g_dap_rx_head;
    uint32_t read_len = (avail > USB_DAP_MAX_PKT_SIZE) ? USB_DAP_MAX_PKT_SIZE : avail;
    MAP_USBEndpointDataGet(USB0_BASE, USB_EP_5, g_dap_rx_queue[head].data, &read_len);
    MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_5, false);
    g_dap_rx_queue[head].len = (uint16_t)read_len;

    g_dap_rx_head = (uint8_t)((head + 1U) % USB_DAP_QUEUE_DEPTH);
    g_dap_rx_count++;
}

/* ---- Bulk IN/OUT Pump Helpers ---- */

static void usb_kick_bulk_in(
    uint32_t ep, usb_ring_t *ring, volatile bool *busy_flag, volatile bool *need_zlp, uint32_t max_chunk)
{
    if (!g_usb_configured || *busy_flag) {
        return;
    }

    uint32_t avail = ring_used(ring);
    if (avail == 0U) {
        if ((need_zlp != NULL) && *need_zlp) {
            *need_zlp = false;
            *busy_flag = true;
            MAP_USBEndpointDataSend(USB0_BASE, ep, USB_TRANS_IN);
        }
        return;
    }

    uint32_t limit = (max_chunk < g_bulk_mps) ? max_chunk : g_bulk_mps;
    uint32_t chunk = (avail > limit) ? limit : avail;

    uint8_t pkt[USB_BULK_MPS_HS];
    ring_pop(ring, pkt, chunk);

    *busy_flag = true;
    MAP_USBEndpointDataPut(USB0_BASE, ep, pkt, chunk);
    MAP_USBEndpointDataSend(USB0_BASE, ep, USB_TRANS_IN);
}

static void usb_handle_bulk_out(uint32_t ep, usb_ring_t *ring)
{
    uint32_t avail = MAP_USBEndpointDataAvail(USB0_BASE, ep);
    if (avail == 0U) {
        MAP_USBDevEndpointDataAck(USB0_BASE, ep, false);
        return;
    }

    /* Keep packet in hardware FIFO (USB NAK flow control) if ring lacks space */
    if (avail > ring_free(ring)) {
        return;
    }

    uint8_t pkt[USB_BULK_MPS_HS];
    uint32_t read_len = (avail > sizeof(pkt)) ? sizeof(pkt) : avail;
    MAP_USBEndpointDataGet(USB0_BASE, ep, pkt, &read_len);
    MAP_USBDevEndpointDataAck(USB0_BASE, ep, false);
    ring_push(ring, pkt, read_len);
}

/* ---- EP0 Control Transfer State Machine ---- */

static void ep0_send_next_chunk(void)
{
    uint32_t chunk = (g_ep0_tx_rem > USB_EP0_MPS) ? USB_EP0_MPS : g_ep0_tx_rem;
    bool is_last = (g_ep0_tx_rem < USB_EP0_MPS) || ((g_ep0_tx_rem == USB_EP0_MPS) && !g_ep0_tx_need_zlp);

    if (chunk > 0U) {
        MAP_USBEndpointDataPut(USB0_BASE, USB_EP_0, (uint8_t *)g_ep0_tx_ptr, chunk);
        g_ep0_tx_ptr += chunk;
        g_ep0_tx_rem = (uint16_t)(g_ep0_tx_rem - chunk);
    }

    if (is_last) {
        g_ep0_state = EP0_STATE_IDLE;
        g_ep0_tx_need_zlp = false;
        MAP_USBEndpointDataSend(USB0_BASE, USB_EP_0, USB_TRANS_IN_LAST);
    } else {
        g_ep0_state = EP0_STATE_TX_DATA;
        MAP_USBEndpointDataSend(USB0_BASE, USB_EP_0, USB_TRANS_IN);
    }
}

static void ep0_start_tx(const uint8_t *data, uint16_t data_len, uint16_t req_len)
{
    uint16_t send_len = (data_len < req_len) ? data_len : req_len;
    g_ep0_tx_ptr = data;
    g_ep0_tx_rem = send_len;
    g_ep0_tx_need_zlp = (send_len < req_len) && ((send_len % USB_EP0_MPS) == 0U);

    MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, false);
    ep0_send_next_chunk();
}

static void ep0_stall(void)
{
    g_ep0_state = EP0_STATE_IDLE;
    MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, false);
    MAP_USBDevEndpointStall(USB0_BASE, USB_EP_0, USB_EP_DEV_IN);
}

static void ep0_handle_standard_request(const uint8_t setup[8])
{
    uint8_t bm_req_type = setup[0];
    uint8_t b_request = setup[1];
    uint16_t w_value = (uint16_t)setup[2] | ((uint16_t)setup[3] << 8U);
    uint16_t w_index = (uint16_t)setup[4] | ((uint16_t)setup[5] << 8U);
    uint16_t w_length = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8U);
    uint8_t recipient = bm_req_type & USB_REQ_RECIPIENT_MASK;

    switch (b_request) {
    case USB_REQ_GET_STATUS:
        g_ep0_status_buf[0] = 0U;
        g_ep0_status_buf[1] = 0U;
        ep0_start_tx(g_ep0_status_buf, 2U, w_length);
        break;

    case USB_REQ_CLEAR_FEATURE:
    case USB_REQ_SET_FEATURE:
        if ((recipient == USB_REQ_RECIPIENT_EP) && (w_value == 0U)) {
            uint8_t ep_num = (uint8_t)(w_index & 0x0FU);
            uint32_t ep_dir = ((w_index & 0x80U) != 0U) ? USB_EP_DEV_IN : USB_EP_DEV_OUT;
            if ((ep_num >= 1U) && (ep_num <= 7U)) {
                uint32_t ep_id = (uint32_t)ep_num << 4U;
                if (b_request == USB_REQ_CLEAR_FEATURE) {
                    MAP_USBDevEndpointStallClear(USB0_BASE, ep_id, ep_dir);
                    MAP_USBEndpointDataToggleClear(USB0_BASE, ep_id, ep_dir);
                } else {
                    MAP_USBDevEndpointStall(USB0_BASE, ep_id, ep_dir);
                }
            }
        }
        MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, true);
        g_ep0_state = EP0_STATE_IDLE;
        break;

    case USB_REQ_SET_ADDRESS:
        g_pending_addr = (uint8_t)(w_value & 0x7FU);
        g_ep0_state = EP0_STATE_STATUS_SET_ADDR;
        MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, true);
        break;

    case USB_REQ_GET_DESCRIPTOR: {
        uint8_t desc_type = (uint8_t)(w_value >> 8U);
        uint8_t desc_idx = (uint8_t)(w_value & 0xFFU);
        const uint8_t *desc_ptr = NULL;
        uint16_t desc_len = 0U;

        if (usb_descriptors_get(desc_type, desc_idx, &desc_ptr, &desc_len)) {
            ep0_start_tx(desc_ptr, desc_len, w_length);
        } else {
            ep0_stall();
        }
        break;
    }

    case USB_REQ_GET_CONFIGURATION:
        g_ep0_status_buf[0] = g_usb_configured ? USB_CONFIG_VALUE : 0U;
        ep0_start_tx(g_ep0_status_buf, 1U, w_length);
        break;

    case USB_REQ_SET_CONFIGURATION:
        if ((w_value == 0U) || (w_value == USB_CONFIG_VALUE)) {
            g_usb_configured = (w_value == USB_CONFIG_VALUE);
            if (g_usb_configured) {
                usb_configure_endpoints();
            }
            MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, true);
            g_ep0_state = EP0_STATE_IDLE;
        } else {
            ep0_stall();
        }
        break;

    case USB_REQ_GET_INTERFACE:
        g_ep0_status_buf[0] = 0U;
        ep0_start_tx(g_ep0_status_buf, 1U, w_length);
        break;

    case USB_REQ_SET_INTERFACE:
        MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, true);
        g_ep0_state = EP0_STATE_IDLE;
        break;

    default:
        ep0_stall();
        break;
    }
}

static void ep0_handle_class_request(const uint8_t setup[8])
{
    uint8_t b_request = setup[1];
    uint16_t w_value = (uint16_t)setup[2] | ((uint16_t)setup[3] << 8U);
    uint16_t w_index = (uint16_t)setup[4] | ((uint16_t)setup[5] << 8U);
    uint16_t w_length = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8U);
    uint8_t cdc_port = USB_CDC_PORT_APP;

    switch (b_request) {
    case USB_CDC_GET_LINE_CODING:
        if (!usb_iface_to_cdc_port(w_index, &cdc_port)) {
            ep0_stall();
            break;
        }
        usb_pack_line_coding(&g_cdc_coding[cdc_port], g_cdc_coding_wire);
        ep0_start_tx(g_cdc_coding_wire, USB_CDC_LINE_CODING_LEN, w_length);
        break;

    case USB_CDC_SET_LINE_CODING:
        if (!usb_iface_to_cdc_port(w_index, &cdc_port)) {
            ep0_stall();
            break;
        }
        if ((w_length > 0U) && (w_length <= sizeof(g_ep0_rx_scratch))) {
            g_ep0_rx_cdc_port = cdc_port;
            g_ep0_rx_rem = w_length;
            g_ep0_state = EP0_STATE_RX_DATA;
            MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, false);
        } else {
            MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, true);
            g_ep0_state = EP0_STATE_IDLE;
        }
        break;

    case USB_CDC_SET_CONTROL_LINE_STATE:
        if (usb_iface_to_cdc_port(w_index, &cdc_port)) {
            g_cdc_ctrl_state[cdc_port] = w_value;
        }
        MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, true);
        g_ep0_state = EP0_STATE_IDLE;
        break;

    case USB_CDC_SEND_BREAK:
        MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, true);
        g_ep0_state = EP0_STATE_IDLE;
        break;

    default:
        ep0_stall();
        break;
    }
}

static void ep0_handle_vendor_request(const uint8_t setup[8])
{
    uint8_t bm_req_type = setup[0];
    uint8_t b_request = setup[1];
    uint16_t w_value = (uint16_t)setup[2] | ((uint16_t)setup[3] << 8U);
    uint16_t w_index = (uint16_t)setup[4] | ((uint16_t)setup[5] << 8U);
    uint16_t w_length = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8U);

    if (((bm_req_type & USB_REQ_DIR_IN_MASK) != 0U) && (b_request == USB_MS_OS_20_VENDOR_CODE) && (w_value == 0U) &&
        (w_index == USB_MS_OS_20_DESC_INDEX)) {
        const uint8_t *desc_ptr = NULL;
        uint16_t desc_len = 0U;
        usb_descriptors_get_ms_os_20(&desc_ptr, &desc_len);
        ep0_start_tx(desc_ptr, desc_len, w_length);
        return;
    }

    ep0_stall();
}

static void ep0_handle_interrupt(void)
{
    uint32_t ep0_status = MAP_USBEndpointStatus(USB0_BASE, USB_EP_0);

    if ((ep0_status & USB_DEV_EP0_SENT_STALL) != 0U) {
        MAP_USBDevEndpointStatusClear(USB0_BASE, USB_EP_0, USB_DEV_EP0_SENT_STALL);
        g_ep0_state = EP0_STATE_IDLE;
    }

    if ((ep0_status & USB_DEV_EP0_SETUP_END) != 0U) {
        MAP_USBDevEndpointStatusClear(USB0_BASE, USB_EP_0, USB_DEV_EP0_SETUP_END);
        g_ep0_state = EP0_STATE_IDLE;
    }

    if (g_ep0_state == EP0_STATE_STATUS_SET_ADDR) {
        MAP_USBDevAddrSet(USB0_BASE, g_pending_addr);
        g_ep0_state = EP0_STATE_IDLE;
        return;
    }

    if (g_ep0_state == EP0_STATE_TX_DATA) {
        ep0_send_next_chunk();
        return;
    }

    if ((ep0_status & USB_DEV_EP0_OUT_PKTRDY) == 0U) {
        return;
    }

    if (g_ep0_state == EP0_STATE_RX_DATA) {
        uint32_t got = sizeof(g_ep0_rx_scratch);
        MAP_USBEndpointDataGet(USB0_BASE, USB_EP_0, g_ep0_rx_scratch, &got);
        if ((got >= USB_CDC_LINE_CODING_LEN) && (g_ep0_rx_rem >= USB_CDC_LINE_CODING_LEN) &&
            (g_ep0_rx_cdc_port < USB_CDC_PORT_COUNT)) {
            usb_unpack_line_coding(g_ep0_rx_scratch, &g_cdc_coding[g_ep0_rx_cdc_port]);
            g_cdc_coding_dirty[g_ep0_rx_cdc_port] = true;
        }
        g_ep0_rx_rem = (got >= g_ep0_rx_rem) ? 0U : (uint16_t)(g_ep0_rx_rem - got);
        bool is_last = (g_ep0_rx_rem == 0U);
        if (is_last) {
            g_ep0_state = EP0_STATE_IDLE;
        }
        MAP_USBDevEndpointDataAck(USB0_BASE, USB_EP_0, is_last);
        return;
    }

    /* Fresh 8-byte SETUP packet */
    uint8_t setup[8];
    uint32_t setup_len = sizeof(setup);
    MAP_USBEndpointDataGet(USB0_BASE, USB_EP_0, setup, &setup_len);
    if (setup_len != sizeof(setup)) {
        ep0_stall();
        return;
    }

    uint8_t req_type = setup[0] & USB_REQ_TYPE_MASK;
    if (req_type == USB_REQ_TYPE_STANDARD) {
        ep0_handle_standard_request(setup);
    } else if (req_type == USB_REQ_TYPE_CLASS) {
        ep0_handle_class_request(setup);
    } else if (req_type == USB_REQ_TYPE_VENDOR) {
        ep0_handle_vendor_request(setup);
    } else {
        ep0_stall();
    }
}

/* ---- USB0 Hardware Interrupt Handler (Vector 58 / IRQ 42) ---- */

void usb0_irq_handler(void)
{
    uint32_t ctrl_status = MAP_USBIntStatusControl(USB0_BASE);
    uint32_t ep_status = MAP_USBIntStatusEndpoint(USB0_BASE);

    if ((ctrl_status & USB_INTCTRL_RESET) != 0U) {
        g_usb_configured = false;
        g_ep0_state = EP0_STATE_IDLE;
        g_pending_addr = 0U;
        g_cmd_tx_busy = false;
        g_cmd_tx_need_zlp = false;
        g_data_tx_busy = false;
        g_cdc_tx_busy[0] = false;
        g_cdc_tx_busy[1] = false;
        g_cdc_ctrl_state[0] = 0U;
        g_cdc_ctrl_state[1] = 0U;

        ring_reset(&g_cmd_rx_ring);
        ring_reset(&g_cmd_tx_ring);
        ring_reset(&g_data_rx_ring);
        ring_reset(&g_data_tx_ring);
        ring_reset(&g_cdc_rx_rings[0]);
        ring_reset(&g_cdc_rx_rings[1]);
        ring_reset(&g_cdc_tx_rings[0]);
        ring_reset(&g_cdc_tx_rings[1]);
        dap_queues_reset();

        g_usb_high_speed = (MAP_USBDevSpeedGet(USB0_BASE) == USB_HIGH_SPEED);
        g_bulk_mps = g_usb_high_speed ? USB_BULK_MPS_HS : USB_BULK_MPS_FS;
        usb_descriptors_set_speed(g_usb_high_speed);
    }

    if ((ctrl_status & USB_INTCTRL_DISCONNECT) != 0U) {
        g_usb_configured = false;
        g_cmd_tx_busy = false;
        g_data_tx_busy = false;
        g_cdc_tx_busy[0] = false;
        g_cdc_tx_busy[1] = false;
        g_cdc_ctrl_state[0] = 0U;
        g_cdc_ctrl_state[1] = 0U;
        dap_queues_reset();
    }

    if ((ep_status & USB_INTEP_0) != 0U) {
        ep0_handle_interrupt();
    }

    /* Bulk 0 — XDS110 Command (OUT 0x02 / IN 0x83) */
    if ((ep_status & USB_INTEP_DEV_OUT_2) != 0U) {
        usb_handle_bulk_out(USB_EP_2, &g_cmd_rx_ring);
    }

    if ((ep_status & USB_INTEP_DEV_IN_3) != 0U) {
        g_cmd_tx_busy = false;
        usb_kick_bulk_in(USB_EP_3, &g_cmd_tx_ring, &g_cmd_tx_busy, &g_cmd_tx_need_zlp, USB_CMD_IN_FIFO_BYTES);
    }

    /* Bulk 1 — EnergyTrace Data (OUT 0x06 / IN 0x87) */
    if ((ep_status & USB_INTEP_DEV_OUT_6) != 0U) {
        usb_handle_bulk_out(USB_EP_6, &g_data_rx_ring);
    }

    if ((ep_status & USB_INTEP_DEV_IN_7) != 0U) {
        g_data_tx_busy = false;
        usb_kick_bulk_in(USB_EP_7, &g_data_tx_ring, &g_data_tx_busy, NULL, USB_BULK_MPS_HS);
    }

    /* CDC 0 — App UART (OUT 0x01 / IN 0x82) */
    if ((ep_status & USB_INTEP_DEV_OUT_1) != 0U) {
        usb_handle_bulk_out(USB_EP_1, &g_cdc_rx_rings[USB_CDC_PORT_APP]);
    }

    if ((ep_status & USB_INTEP_DEV_IN_2) != 0U) {
        g_cdc_tx_busy[USB_CDC_PORT_APP] = false;
        usb_kick_bulk_in(USB_EP_2, &g_cdc_tx_rings[USB_CDC_PORT_APP], &g_cdc_tx_busy[USB_CDC_PORT_APP], NULL,
            USB_CDC0_IN_FIFO_BYTES);
    }

    /* CDC 1 — Aux / SWO UART (OUT 0x04 / IN 0x85) */
    if ((ep_status & USB_INTEP_DEV_OUT_4) != 0U) {
        usb_handle_bulk_out(USB_EP_4, &g_cdc_rx_rings[USB_CDC_PORT_AUX]);
    }

    if ((ep_status & USB_INTEP_DEV_IN_5) != 0U) {
        g_cdc_tx_busy[USB_CDC_PORT_AUX] = false;
        usb_kick_bulk_in(USB_EP_5, &g_cdc_tx_rings[USB_CDC_PORT_AUX], &g_cdc_tx_busy[USB_CDC_PORT_AUX], NULL,
            USB_CDC1_IN_FIFO_BYTES);
    }

    /* CMSIS-DAP v2 WinUSB Bulk (OUT 0x05 / IN 0x86) */
    if ((ep_status & USB_INTEP_DEV_OUT_5) != 0U) {
        usb_handle_dap_out();
    }

    if ((ep_status & USB_INTEP_DEV_IN_6) != 0U) {
        g_dap_tx_busy = false;
        usb_kick_dap_in();
    }
}

/* ---- Public API ---- */

void usb_device_init(const uint8_t fw_version[4])
{
    char serial[BOARD_SERIAL_LEN + 1U];
    board_read_serial(serial);
    usb_descriptors_init(fw_version, serial);

    MAP_SysCtlPeripheralReset(SYSCTL_PERIPH_USB0);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_USB0);
    while (!MAP_SysCtlPeripheralReady(SYSCTL_PERIPH_USB0)) {
    }

    if (board_has_ulpi_phy()) {
        usb_init_ulpi_pins();

        /* USB3300-EZK supplies external 60 MHz ULPI clock on PB3 */
        MAP_USBClockEnable(USB0_BASE, 1U, USB_CLOCK_EXTERNAL);
        MAP_USBULPIEnable(USB0_BASE);

        /* Configure USB3300 PHY for 8-bit SDR High-Speed mode */
        uint8_t func_ctrl = MAP_USBULPIRegRead(USB0_BASE, ULPI_REG_FUNC_CTRL);
        MAP_USBULPIRegWrite(USB0_BASE, ULPI_REG_FUNC_CTRL, (uint8_t)(func_ctrl & 0xFCU));

        uint8_t if_ctrl = MAP_USBULPIRegRead(USB0_BASE, ULPI_REG_IF_CTRL);
        MAP_USBULPIRegWrite(USB0_BASE, ULPI_REG_IF_CTRL, (uint8_t)(if_ctrl & 0x8FU));

        MAP_USBHighSpeed(USB0_BASE, true);
    } else {
        /* Fallback for boards without external ULPI PHY: 480 MHz VCO / 8 = 60 MHz */
        MAP_GPIOPinTypeUSBAnalog(GPIO_PORTL_BASE, GPIO_PIN_6 | GPIO_PIN_7);
        MAP_USBClockEnable(USB0_BASE, 8U, USB_CLOCK_INTERNAL);
        MAP_USBULPIDisable(USB0_BASE);
        MAP_USBHighSpeed(USB0_BASE, false);
    }

    MAP_USBDevMode(USB0_BASE);

    /* Clear any stale status before enabling interrupts */
    (void)MAP_USBIntStatusControl(USB0_BASE);
    (void)MAP_USBIntStatusEndpoint(USB0_BASE);

    MAP_USBIntEnableControl(
        USB0_BASE, USB_INTCTRL_RESET | USB_INTCTRL_DISCONNECT | USB_INTCTRL_SUSPEND | USB_INTCTRL_RESUME);
    MAP_USBIntEnableEndpoint(USB0_BASE,
        USB_INTEP_0 | USB_INTEP_DEV_OUT_1 | USB_INTEP_DEV_IN_2 | USB_INTEP_DEV_OUT_2 | USB_INTEP_DEV_IN_3 |
            USB_INTEP_DEV_OUT_4 | USB_INTEP_DEV_IN_5 | USB_INTEP_DEV_OUT_5 | USB_INTEP_DEV_OUT_6 | USB_INTEP_DEV_IN_6 |
            USB_INTEP_DEV_IN_7);

    NVIC_SetPriority(USB0_IRQn, USB_IRQ_PRIO);
    NVIC_ClearPendingIRQ(USB0_IRQn);
    NVIC_EnableIRQ(USB0_IRQn);

    MAP_USBDevConnect(USB0_BASE);
}

void usb_device_disconnect(void)
{
    NVIC_DisableIRQ(USB0_IRQn);
    MAP_USBDevDisconnect(USB0_BASE);
    g_usb_configured = false;
    g_cmd_tx_busy = false;
    g_data_tx_busy = false;
    g_cdc_tx_busy[0] = false;
    g_cdc_tx_busy[1] = false;
    g_cdc_ctrl_state[0] = 0U;
    g_cdc_ctrl_state[1] = 0U;
    dap_queues_reset();
}

bool usb_device_is_configured(void)
{
    return g_usb_configured;
}

bool usb_device_is_high_speed(void)
{
    return g_usb_high_speed;
}

uint32_t usb_cmd_rx_available(void)
{
    return ring_used(&g_cmd_rx_ring);
}

uint32_t usb_cmd_rx_peek(uint8_t *dst, uint32_t len)
{
    return ring_peek(&g_cmd_rx_ring, dst, len);
}

uint32_t usb_cmd_rx_read(uint8_t *dst, uint32_t len)
{
    uint32_t crit = usb_crit_enter();
    uint32_t got = ring_pop(&g_cmd_rx_ring, dst, len);
    /* Drain any packet held by NAK back-pressure now that space freed up */
    if (MAP_USBEndpointDataAvail(USB0_BASE, USB_EP_2) > 0U) {
        usb_handle_bulk_out(USB_EP_2, &g_cmd_rx_ring);
    }
    usb_crit_exit(crit);
    return got;
}

void usb_cmd_rx_flush(void)
{
    uint32_t crit = usb_crit_enter();
    ring_reset(&g_cmd_rx_ring);
    usb_crit_exit(crit);
}

uint32_t usb_cmd_tx_space(void)
{
    return ring_free(&g_cmd_tx_ring);
}

uint32_t usb_cmd_tx_write(const uint8_t *src, uint32_t len)
{
    uint32_t crit = usb_crit_enter();
    uint32_t written = ring_push(&g_cmd_tx_ring, src, len);
    if ((written > 0U) && (USB_CMD_IN_FIFO_BYTES >= g_bulk_mps) && ((written % g_bulk_mps) == 0U)) {
        g_cmd_tx_need_zlp = true;
    }
    usb_kick_bulk_in(USB_EP_3, &g_cmd_tx_ring, &g_cmd_tx_busy, &g_cmd_tx_need_zlp, USB_CMD_IN_FIFO_BYTES);
    usb_crit_exit(crit);
    return written;
}

uint32_t usb_data_rx_available(void)
{
    return ring_used(&g_data_rx_ring);
}

uint32_t usb_data_rx_read(uint8_t *dst, uint32_t len)
{
    uint32_t crit = usb_crit_enter();
    uint32_t got = ring_pop(&g_data_rx_ring, dst, len);
    if (MAP_USBEndpointDataAvail(USB0_BASE, USB_EP_6) > 0U) {
        usb_handle_bulk_out(USB_EP_6, &g_data_rx_ring);
    }
    usb_crit_exit(crit);
    return got;
}

uint32_t usb_data_tx_space(void)
{
    return ring_free(&g_data_tx_ring);
}

uint32_t usb_data_tx_write(const uint8_t *src, uint32_t len)
{
    uint32_t crit = usb_crit_enter();
    uint32_t written = ring_push(&g_data_tx_ring, src, len);
    usb_kick_bulk_in(USB_EP_7, &g_data_tx_ring, &g_data_tx_busy, NULL, USB_BULK_MPS_HS);
    usb_crit_exit(crit);
    return written;
}

void usb_data_tx_flush(void)
{
    uint32_t crit = usb_crit_enter();
    ring_reset(&g_data_tx_ring);
    usb_crit_exit(crit);
}

uint32_t usb_cdc_rx_available(uint8_t port)
{
    if (port >= USB_CDC_PORT_COUNT) {
        return 0U;
    }
    return ring_used(&g_cdc_rx_rings[port]);
}

uint32_t usb_cdc_rx_read(uint8_t port, uint8_t *dst, uint32_t len)
{
    if (port >= USB_CDC_PORT_COUNT) {
        return 0U;
    }
    uint32_t crit = usb_crit_enter();
    uint32_t got = ring_pop(&g_cdc_rx_rings[port], dst, len);
    uint32_t ep = g_cdc_out_ep[port];
    if (MAP_USBEndpointDataAvail(USB0_BASE, ep) > 0U) {
        usb_handle_bulk_out(ep, &g_cdc_rx_rings[port]);
    }
    usb_crit_exit(crit);
    return got;
}

uint32_t usb_cdc_tx_space(uint8_t port)
{
    if (port >= USB_CDC_PORT_COUNT) {
        return 0U;
    }
    return ring_free(&g_cdc_tx_rings[port]);
}

uint32_t usb_cdc_tx_write(uint8_t port, const uint8_t *src, uint32_t len)
{
    if (port >= USB_CDC_PORT_COUNT) {
        return 0U;
    }
    uint32_t crit = usb_crit_enter();
    uint32_t written = ring_push(&g_cdc_tx_rings[port], src, len);
    usb_kick_bulk_in(g_cdc_in_ep[port], &g_cdc_tx_rings[port], &g_cdc_tx_busy[port], NULL, g_cdc_in_fifo_bytes[port]);
    usb_crit_exit(crit);
    return written;
}

bool usb_cdc_take_line_coding(uint8_t port, usb_cdc_line_coding_t *out_coding)
{
    if (port >= USB_CDC_PORT_COUNT) {
        return false;
    }
    uint32_t crit = usb_crit_enter();
    bool changed = g_cdc_coding_dirty[port];
    if (changed) {
        g_cdc_coding_dirty[port] = false;
        *out_coding = g_cdc_coding[port];
    }
    usb_crit_exit(crit);
    return changed;
}

void usb_cdc_get_line_coding(uint8_t port, usb_cdc_line_coding_t *out_coding)
{
    if (port >= USB_CDC_PORT_COUNT) {
        return;
    }
    uint32_t crit = usb_crit_enter();
    *out_coding = g_cdc_coding[port];
    usb_crit_exit(crit);
}

bool usb_cdc_is_dtr_asserted(uint8_t port)
{
    if (port >= USB_CDC_PORT_COUNT) {
        return false;
    }
    return (g_cdc_ctrl_state[port] & 0x0001U) != 0U;
}

uint16_t usb_dap_packet_size(void)
{
    return g_bulk_mps;
}

bool usb_dap_rx_available(void)
{
    return (g_dap_rx_count > 0U);
}

uint16_t usb_dap_rx_pop(uint8_t *out_pkt, uint16_t max_len)
{
    uint32_t crit = usb_crit_enter();
    if (g_dap_rx_count == 0U) {
        usb_crit_exit(crit);
        return 0U;
    }

    uint8_t tail = g_dap_rx_tail;
    uint16_t pkt_len = g_dap_rx_queue[tail].len;
    if (pkt_len > max_len) {
        pkt_len = max_len;
    }
    memcpy(out_pkt, g_dap_rx_queue[tail].data, pkt_len);
    g_dap_rx_tail = (uint8_t)((tail + 1U) % USB_DAP_QUEUE_DEPTH);
    g_dap_rx_count--;

    /* Drain any DAP Bulk OUT packet held in hardware FIFO by NAK back-pressure */
    if (MAP_USBEndpointDataAvail(USB0_BASE, USB_EP_5) > 0U) {
        usb_handle_dap_out();
    }

    usb_crit_exit(crit);
    return pkt_len;
}

bool usb_dap_tx_space(void)
{
    return (g_dap_tx_count < USB_DAP_QUEUE_DEPTH);
}

bool usb_dap_tx_push(const uint8_t *pkt, uint16_t len)
{
    if ((len == 0U) || (len > g_bulk_mps)) {
        return false;
    }

    uint32_t crit = usb_crit_enter();
    if (g_dap_tx_count >= USB_DAP_QUEUE_DEPTH) {
        usb_crit_exit(crit);
        return false;
    }

    uint8_t head = g_dap_tx_head;
    memcpy(g_dap_tx_queue[head].data, pkt, len);
    g_dap_tx_queue[head].len = len;
    g_dap_tx_head = (uint8_t)((head + 1U) % USB_DAP_QUEUE_DEPTH);
    g_dap_tx_count++;

    usb_kick_dap_in();
    usb_crit_exit(crit);
    return true;
}
