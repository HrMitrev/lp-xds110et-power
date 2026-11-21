/*
 * xds_swd.c - Native XDS110 Interface 2 SWD, CMAPI, SACI & OpenOCD DAP handlers.
 *
 * Reuses the fast DWT-paced SWD wire driver in swd.c to serve both OpenOCD's
 * native `interface/xds110.cfg` SWD driver and TI UniFlash `DSLite` over USB
 * Interface 2 (Bulk 0x02/0x83).
 */

#include "xds_swd.h"

#include <stddef.h>

#include "protocol/xds_cmd.h"
#include "swd.h"

/* XDS110 Fast-TCK special delay values (firmware >= 3.0.0.00) */
#define XDS_FAST_TCK_DELAY_14000_KHZ 0U
#define XDS_FAST_TCK_DELAY_12000_KHZ 0xFFFFFFFEU
#define XDS_FAST_TCK_DELAY_10000_KHZ 0xFFFFFFFDU
#define XDS_FAST_TCK_DELAY_8500_KHZ  1U
#define XDS_FAST_TCK_DELAY_5500_KHZ  2U
#define XDS_FAST_TCK_SLOPE_HZ        17100000U
#define XDS_MIN_SWD_CLOCK_HZ         10000U

/* CMAPI register types and DP/AP addresses */
#define XDS_DAP_AP           0U
#define XDS_DAP_DP           1U
#define XDS_DP_ABORT_REQ     0x00U /* DP Write A[3:2]=00 */
#define XDS_DP_IDCODE_REQ    SWD_REQ_RnW
#define XDS_DP_SELECT_REQ    SWD_REQ_A3
#define XDS_DP_RDBUFF_REQ    (SWD_REQ_RnW | SWD_REQ_A2 | SWD_REQ_A3)
#define XDS_AP_CSW_ADDR      0x00U
#define XDS_AP_TAR_ADDR      0x04U
#define XDS_AP_DRW_ADDR      0x0CU
#define XDS_AP_CSW_CACHE_MAX 8U

/* Raw SWD header byte flags used in OCD_DAP_REQUEST (0x3A) */
#define XDS_SWD_CMD_RNW (1U << 2U)

/* WAIT retry count, SACI mailbox poll cap, and TCK cycle cap */
#define XDS_SWD_WAIT_RETRIES    1000U
#define XDS_SACI_POLL_RETRIES   50000U
#define XDS_CYCLE_TCK_MAX       200000U
#define XDS_SWD_TRAILING_CLKS   8U
#define XDS_DP_ABORT_CLEAR_ALL  0x0000001EU
#define XDS_DP_ABORT_DAP_AND_CL 0x0000001FU
#define XDS_AP_CSW_SPROT_BIT    0x40000000U

/* TI SEC-AP (SACI) mailbox register offsets */
#define XDS_SACI_REG_TXD   0x00U
#define XDS_SACI_REG_TXCTL 0x04U
#define XDS_SACI_REG_RXD   0x08U
#define XDS_SACI_REG_RXCTL 0x0CU

static bool g_xds_swd_active = false;
static bool g_xds_cmapi_connected = false;
static bool g_xds_select_valid = false;
static uint32_t g_xds_select_cache = 0U;
static uint32_t g_xds_csw_cache[XDS_AP_CSW_CACHE_MAX];
static bool g_xds_csw_valid[XDS_AP_CSW_CACHE_MAX];

static uint16_t read_u16le(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8U);
}

static void write_u16le(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
}

static uint32_t read_u32le(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8U) | ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
}

static void write_u32le(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((val >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

static int32_t xds_swd_ack_to_err(uint8_t ack)
{
    switch (ack) {
    case SWD_ACK_OK:
        return XDS_ERR_OK;
    case SWD_ACK_WAIT:
        return XDS_ERR_SWD_WAIT;
    case SWD_ACK_FAULT:
        return XDS_ERR_SWD_FAULT;
    case SWD_ACK_ERROR:
        return XDS_ERR_SWD_PARITY;
    default:
        return XDS_ERR_SWD_PROTOCOL;
    }
}

static uint8_t xds_swd_xfer_retry(uint8_t swd_req, uint32_t *data)
{
    uint32_t retry = XDS_SWD_WAIT_RETRIES;
    uint8_t ack;
    do {
        ack = swd_transfer(swd_req, data);
        if (ack != SWD_ACK_WAIT) {
            break;
        }
    } while (retry-- > 0U);
    return ack;
}

static uint8_t xds_swd_cmapi_xfer_slow(uint8_t swd_req, uint32_t *data, uint8_t ack)
{
    if (ack == SWD_ACK_WAIT) {
        ack = xds_swd_xfer_retry(swd_req, data);
    }
    if (ack == SWD_ACK_OK) {
        return SWD_ACK_OK;
    }

    bool is_dp_idcode_or_abort = ((swd_req & SWD_REQ_APnDP) == 0U) && ((swd_req & 0x0CU) == 0U);
    bool is_dp_ctrlstat_read = (swd_req == (uint8_t)(SWD_REQ_RnW | SWD_REQ_A2));

    if ((ack != SWD_ACK_WAIT) && (ack != SWD_ACK_FAULT) && !is_dp_idcode_or_abort) {
        /* Protocol/parity glitch: read DPIDR once to resync and retry once */
        uint32_t idcode = 0U;
        (void)swd_transfer(XDS_DP_IDCODE_REQ, &idcode);
        ack = xds_swd_xfer_retry(swd_req, data);
    }

    if ((ack == SWD_ACK_FAULT) && !is_dp_idcode_or_abort && !is_dp_ctrlstat_read) {
        uint32_t stat = 0U;
        uint32_t abort_val = XDS_DP_ABORT_CLEAR_ALL;
        (void)swd_transfer((uint8_t)(SWD_REQ_RnW | SWD_REQ_A2), &stat);
        (void)swd_transfer(XDS_DP_ABORT_REQ, &abort_val);
        swd_idle_cycles(XDS_SWD_TRAILING_CLKS);
    } else if ((ack == SWD_ACK_WAIT) && !is_dp_idcode_or_abort && !is_dp_ctrlstat_read) {
        uint32_t abort_val = XDS_DP_ABORT_DAP_AND_CL;
        (void)swd_transfer(XDS_DP_ABORT_REQ, &abort_val);
        swd_idle_cycles(XDS_SWD_TRAILING_CLKS);
    }

    return ack;
}

/*
 * Fast-path CMAPI SWD transfer with automatic sticky-error / WAIT-abort recovery
 * (matching TI XDS110 CMAPI behavior so a single bus fault does not leave
 * DP.CTRL/STAT.STICKYERR latched across subsequent AP/DP commands).
 */
static inline uint8_t xds_swd_cmapi_xfer(uint8_t swd_req, uint32_t *data)
{
    uint8_t ack = swd_transfer(swd_req, data);
    if (ack == SWD_ACK_OK) {
        return SWD_ACK_OK;
    }
    return xds_swd_cmapi_xfer_slow(swd_req, data, ack);
}

static void xds_swd_send_jtag_to_swd(void)
{
    /* ARM ADIv5 JTAG-to-SWD switch: >=50 1s, 0xE79E (LSB first), >=50 1s, 8 idle 0s */
    static const uint8_t jtag_to_swd_seq[] = {
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU, /* 56 high bits */
        0x9EU,
        0xE7U, /* 0xE79E LSB first */
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU, /* 56 high bits (line reset) */
        0x00U, /* 8 idle low bits */
    };
    swd_sequence((uint32_t)sizeof(jtag_to_swd_seq) * 8U, jtag_to_swd_seq);
}

static void xds_swd_send_line_reset(void)
{
    static const uint8_t line_reset_seq[] = {
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU, /* 56 high bits */
        0x00U, /* 8 idle low bits */
    };
    swd_sequence((uint32_t)sizeof(line_reset_seq) * 8U, line_reset_seq);
}

static void xds_swd_send_swd_to_jtag(void)
{
    /* ARM ADIv5 SWD-to-JTAG switch: >=50 1s, 0xE73C (LSB first), >=5 1s */
    static const uint8_t swd_to_jtag_seq[] = {
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU, /* 56 high bits */
        0x3CU,
        0xE7U, /* 0xE73C LSB first */
        0xFFU, /* 8 high bits */
    };
    swd_sequence((uint32_t)sizeof(swd_to_jtag_seq) * 8U, swd_to_jtag_seq);
}

static void xds_swd_send_dormant_to_swd(void)
{
    /* ARM ADIv5.2 Selection Alert (128-bit) + 4 low bits + 8-bit SWD activation (0x1A) */
    static const uint8_t dormant_seq[] = {
        0xFFU,
        0x92U,
        0xF3U,
        0x09U,
        0x62U,
        0x95U,
        0x2DU,
        0x85U,
        0x86U,
        0xE9U,
        0xAFU,
        0xDDU,
        0xE3U,
        0xA2U,
        0x0EU,
        0xBCU,
        0x19U,
        0xA0U,
        0x01U,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0xFFU,
        0x00U,
    };
    swd_sequence((uint32_t)sizeof(dormant_seq) * 8U, dormant_seq);
}

static int32_t xds_swd_select_ap_bank(uint8_t ap_num, uint8_t addr)
{
    uint32_t sel = ((uint32_t)ap_num << 24U) | ((uint32_t)addr & 0xF0U);
    if (g_xds_select_valid && (g_xds_select_cache == sel)) {
        return XDS_ERR_OK;
    }

    uint32_t wval = sel;
    uint8_t ack = xds_swd_cmapi_xfer(XDS_DP_SELECT_REQ, &wval);
    if (ack != SWD_ACK_OK) {
        g_xds_select_valid = false;
        return xds_swd_ack_to_err(ack);
    }

    g_xds_select_cache = sel;
    g_xds_select_valid = true;
    return XDS_ERR_OK;
}

static void xds_swd_clear_caches(void)
{
    g_xds_select_valid = false;
    g_xds_select_cache = 0U;
    for (uint32_t i = 0U; i < XDS_AP_CSW_CACHE_MAX; i++) {
        g_xds_csw_valid[i] = false;
        g_xds_csw_cache[i] = 0U;
    }
}

/* ---- Public API ---- */

void xds_swd_init(void)
{
    g_xds_swd_active = false;
    g_xds_cmapi_connected = false;
    xds_swd_clear_caches();
}

int32_t xds_swd_set_tck_delay(uint32_t delay_count)
{
    uint32_t freq_hz;

    switch (delay_count) {
    case XDS_FAST_TCK_DELAY_14000_KHZ:
        freq_hz = 14000000U;
        break;
    case XDS_FAST_TCK_DELAY_12000_KHZ:
        freq_hz = 12000000U;
        break;
    case XDS_FAST_TCK_DELAY_10000_KHZ:
        freq_hz = 10000000U;
        break;
    case XDS_FAST_TCK_DELAY_8500_KHZ:
        freq_hz = 8500000U;
        break;
    case XDS_FAST_TCK_DELAY_5500_KHZ:
        freq_hz = 5500000U;
        break;
    default:
        freq_hz = XDS_FAST_TCK_SLOPE_HZ / (delay_count + 1U);
        if (freq_hz < XDS_MIN_SWD_CLOCK_HZ) {
            freq_hz = XDS_MIN_SWD_CLOCK_HZ;
        }
        break;
    }

    swd_set_clock_hz(freq_hz);
    return XDS_ERR_OK;
}

int32_t xds_swd_cycle_tck(uint32_t count)
{
    if (count > XDS_CYCLE_TCK_MAX) {
        count = XDS_CYCLE_TCK_MAX;
    }
    if (g_xds_swd_active && (count > 0U)) {
        swd_idle_cycles(count);
    }
    return XDS_ERR_OK;
}

int32_t xds_swd_connect(void)
{
    swd_connect();
    swd_configure(1U, false, 0U);
    xds_swd_send_jtag_to_swd();

    /*
     * Read DPIDR immediately after switching to SWD (required by SW-DP v2
     * targets such as CC2340R5 before any DP/AP write); if unresponsive, try
     * ADIv5.2 Dormant-to-SWD wakeup and re-read DPIDR.
     */
    uint32_t idcode = 0U;
    uint8_t ack = xds_swd_xfer_retry(XDS_DP_IDCODE_REQ, &idcode);
    if (ack != SWD_ACK_OK) {
        xds_swd_send_dormant_to_swd();
        (void)xds_swd_xfer_retry(XDS_DP_IDCODE_REQ, &idcode);
    }

    g_xds_swd_active = true;
    xds_swd_clear_caches();
    return XDS_ERR_OK;
}

int32_t xds_swd_disconnect(void)
{
    if (g_xds_swd_active) {
        xds_swd_send_swd_to_jtag();
        swd_disconnect();
    }

    g_xds_swd_active = false;
    g_xds_cmapi_connected = false;
    xds_swd_clear_caches();
    return XDS_ERR_OK;
}

int32_t xds_swd_cmapi_connect(uint32_t *out_idcode)
{
    *out_idcode = 0U;
    if (!g_xds_swd_active) {
        (void)xds_swd_connect();
    }

    uint32_t idcode = 0U;
    uint8_t ack = xds_swd_xfer_retry(XDS_DP_IDCODE_REQ, &idcode);
    if (ack != SWD_ACK_OK) {
        /* Retry after an explicit SWD line reset */
        xds_swd_send_line_reset();
        ack = xds_swd_xfer_retry(XDS_DP_IDCODE_REQ, &idcode);
    }
    if (ack != SWD_ACK_OK) {
        /* Retry after ADIv5.2 Dormant-to-SWD wake (required for SW-DP v2 targets) */
        xds_swd_send_dormant_to_swd();
        ack = xds_swd_xfer_retry(XDS_DP_IDCODE_REQ, &idcode);
    }

    if (ack != SWD_ACK_OK) {
        g_xds_cmapi_connected = false;
        return xds_swd_ack_to_err(ack);
    }

    /* Clear sticky errors and power up debug & system domains (matches TI CMAPI_CONNECT) */
    uint32_t abort_val = XDS_DP_ABORT_CLEAR_ALL;
    (void)xds_swd_xfer_retry(XDS_DP_ABORT_REQ, &abort_val);

    uint32_t ctrl_req = 0x50000000U; /* CSYSPWRUPREQ | CDBGPWRUPREQ */
    ack = xds_swd_xfer_retry(SWD_REQ_A2, &ctrl_req);
    if (ack == SWD_ACK_OK) {
        swd_idle_cycles(XDS_SWD_TRAILING_CLKS);
        for (uint32_t i = 0U; i < 200U; i++) {
            uint32_t stat = 0U;
            if (xds_swd_xfer_retry((uint8_t)(SWD_REQ_RnW | SWD_REQ_A2), &stat) != SWD_ACK_OK) {
                break;
            }
            if ((stat & 0xA0000000U) == 0xA0000000U) {
                break;
            }
        }
    }

    g_xds_cmapi_connected = true;
    xds_swd_clear_caches();
    *out_idcode = idcode;
    return XDS_ERR_OK;
}

int32_t xds_swd_cmapi_disconnect(void)
{
    g_xds_cmapi_connected = false;
    return XDS_ERR_OK;
}

int32_t xds_swd_cmapi_acquire(void)
{
    if (!g_xds_cmapi_connected) {
        return XDS_ERR_SWD_PROTOCOL;
    }
    return XDS_ERR_OK;
}

int32_t xds_swd_cmapi_release(void)
{
    return XDS_ERR_OK;
}

int32_t xds_swd_cmapi_read_reg(uint8_t type, uint8_t ap_num, uint8_t addr, uint32_t *out_val)
{
    *out_val = 0U;
    if (!g_xds_swd_active) {
        return XDS_ERR_SWD_PROTOCOL;
    }

    uint8_t reg_bits = addr & 0x0CU;
    if (type == XDS_DAP_AP) {
        int32_t sel_err = xds_swd_select_ap_bank(ap_num, addr);
        if (sel_err != XDS_ERR_OK) {
            return sel_err;
        }
        uint8_t ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | SWD_REQ_RnW | reg_bits), NULL);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
        uint32_t val = 0U;
        ack = xds_swd_cmapi_xfer(XDS_DP_RDBUFF_REQ, &val);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
        *out_val = val;
        return XDS_ERR_OK;
    }

    if (type == XDS_DAP_DP) {
        uint32_t val = 0U;
        uint8_t ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_RnW | reg_bits), &val);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
        *out_val = val;
        return XDS_ERR_OK;
    }

    return XDS_ERR_INVALID_PARAM;
}

int32_t xds_swd_cmapi_write_reg(uint8_t type, uint8_t ap_num, uint8_t addr, uint32_t val)
{
    if (!g_xds_swd_active) {
        return XDS_ERR_SWD_PROTOCOL;
    }

    uint8_t reg_bits = addr & 0x0CU;
    if (type == XDS_DAP_AP) {
        int32_t sel_err = xds_swd_select_ap_bank(ap_num, addr);
        if (sel_err != XDS_ERR_OK) {
            return sel_err;
        }
        uint32_t wval = val;
        uint8_t ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | reg_bits), &wval);
        if (ack != SWD_ACK_OK) {
            if ((addr == XDS_AP_CSW_ADDR) && (ap_num < XDS_AP_CSW_CACHE_MAX)) {
                g_xds_csw_valid[ap_num] = false;
            }
            return xds_swd_ack_to_err(ack);
        }
        if ((addr == XDS_AP_CSW_ADDR) && (ap_num < XDS_AP_CSW_CACHE_MAX)) {
            g_xds_csw_cache[ap_num] = val;
            g_xds_csw_valid[ap_num] = true;
        }
        swd_idle_cycles(XDS_SWD_TRAILING_CLKS);
        return XDS_ERR_OK;
    }

    if (type == XDS_DAP_DP) {
        uint32_t wval = val;
        uint8_t ack = xds_swd_cmapi_xfer(reg_bits, &wval);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
        if (reg_bits == XDS_DP_SELECT_REQ) {
            g_xds_select_cache = val;
            g_xds_select_valid = true;
        }
        swd_idle_cycles(XDS_SWD_TRAILING_CLKS);
        return XDS_ERR_OK;
    }

    return XDS_ERR_INVALID_PARAM;
}

static int32_t xds_swd_prepare_mem_csw(
    uint8_t ap_num, uint8_t access_size, uint8_t flags, bool has_csw, uint32_t csw_override)
{
    uint32_t base_csw = 0U;
    bool had_cached = (ap_num < XDS_AP_CSW_CACHE_MAX) && g_xds_csw_valid[ap_num];
    if (had_cached) {
        base_csw = g_xds_csw_cache[ap_num];
    } else if (has_csw) {
        base_csw = csw_override;
        if (ap_num < XDS_AP_CSW_CACHE_MAX) {
            g_xds_csw_cache[ap_num] = base_csw;
            g_xds_csw_valid[ap_num] = true;
        }
    } else {
        int32_t r_err = xds_swd_cmapi_read_reg(XDS_DAP_AP, ap_num, XDS_AP_CSW_ADDR, &base_csw);
        if (r_err != XDS_ERR_OK) {
            return r_err;
        }
        if (ap_num < XDS_AP_CSW_CACHE_MAX) {
            g_xds_csw_cache[ap_num] = base_csw;
            g_xds_csw_valid[ap_num] = true;
        }
    }

    bool addr_inc = (flags & 0x01U) != 0U;
    bool secure_prot = (flags & 0x02U) != 0U;
    uint32_t size_bits = (access_size == 4U) ? 2U : ((access_size == 2U) ? 1U : 0U);
    uint32_t csw = (base_csw & ~0x3FU) | XDS_AP_CSW_SPROT_BIT | size_bits | (addr_inc ? 0x10U : 0U);
    if (secure_prot) {
        csw &= ~XDS_AP_CSW_SPROT_BIT;
    }

    int32_t sel_err = xds_swd_select_ap_bank(ap_num, XDS_AP_CSW_ADDR);
    if (sel_err != XDS_ERR_OK) {
        return sel_err;
    }

    if (had_cached && (csw == g_xds_csw_cache[ap_num])) {
        return XDS_ERR_OK;
    }

    uint32_t wval = csw;
    uint8_t ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | XDS_AP_CSW_ADDR), &wval);
    if (ack != SWD_ACK_OK) {
        if (ap_num < XDS_AP_CSW_CACHE_MAX) {
            g_xds_csw_valid[ap_num] = false;
        }
        return xds_swd_ack_to_err(ack);
    }

    if (ap_num < XDS_AP_CSW_CACHE_MAX) {
        g_xds_csw_cache[ap_num] = csw;
        g_xds_csw_valid[ap_num] = true;
    }
    return XDS_ERR_OK;
}

int32_t xds_swd_cmapi_read_mem(uint8_t access_size, uint8_t flags, uint8_t ap_num, uint32_t addr, uint16_t byte_count,
    bool has_csw, uint32_t csw_override, uint8_t *out_data, uint16_t out_max, uint16_t *out_len)
{
    *out_len = 0U;
    if (!g_xds_swd_active) {
        return XDS_ERR_SWD_PROTOCOL;
    }
    if (((access_size != 1U) && (access_size != 2U) && (access_size != 4U)) || (byte_count > out_max) ||
        ((byte_count % access_size) != 0U)) {
        return XDS_ERR_INVALID_PARAM;
    }
    if (byte_count == 0U) {
        return XDS_ERR_OK;
    }

    bool addr_inc = (flags & 0x01U) != 0U;
    int32_t err = xds_swd_prepare_mem_csw(ap_num, access_size, flags, has_csw, csw_override);
    if (err != XDS_ERR_OK) {
        return err;
    }

    uint32_t cur_addr = addr;
    uint16_t off = 0U;
    while (off < byte_count) {
        /* Re-write TAR at start and at every 1 KiB ADIv5 auto-increment boundary */
        uint32_t tar = cur_addr;
        uint8_t ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | XDS_AP_TAR_ADDR), &tar);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }

        uint32_t chunk_bytes = (uint32_t)byte_count - (uint32_t)off;
        if (addr_inc) {
            uint32_t to_boundary = 1024U - (cur_addr & 1023U);
            if (chunk_bytes > to_boundary) {
                chunk_bytes = to_boundary;
            }
        }
        uint32_t count = chunk_bytes / access_size;

        /* Post initial DRW read */
        ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | SWD_REQ_RnW | XDS_AP_DRW_ADDR), NULL);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }

        for (uint32_t i = 0U; i < count; i++) {
            uint8_t wire_req =
                ((i + 1U) < count) ? (uint8_t)(SWD_REQ_APnDP | SWD_REQ_RnW | XDS_AP_DRW_ADDR) : XDS_DP_RDBUFF_REQ;
            uint32_t word = 0U;
            ack = xds_swd_cmapi_xfer(wire_req, &word);
            if (ack != SWD_ACK_OK) {
                return xds_swd_ack_to_err(ack);
            }

            if (access_size == 4U) {
                write_u32le(&out_data[off], word);
            } else if (access_size == 2U) {
                uint32_t shift = (cur_addr & 2U) * 8U;
                write_u16le(&out_data[off], (uint16_t)((word >> shift) & 0xFFFFU));
            } else {
                uint32_t shift = (cur_addr & 3U) * 8U;
                out_data[off] = (uint8_t)((word >> shift) & 0xFFU);
            }
            off = (uint16_t)(off + access_size);
            if (addr_inc) {
                cur_addr += access_size;
            }
        }
    }

    *out_len = off;
    return XDS_ERR_OK;
}

int32_t xds_swd_cmapi_write_mem(uint8_t access_size, uint8_t flags, uint8_t ap_num, uint32_t addr, uint16_t byte_count,
    bool has_csw, uint32_t csw_override, const uint8_t *in_data)
{
    if (!g_xds_swd_active) {
        return XDS_ERR_SWD_PROTOCOL;
    }
    if (((access_size != 1U) && (access_size != 2U) && (access_size != 4U)) || ((byte_count % access_size) != 0U)) {
        return XDS_ERR_INVALID_PARAM;
    }
    if (byte_count == 0U) {
        return XDS_ERR_OK;
    }

    bool addr_inc = (flags & 0x01U) != 0U;
    int32_t err = xds_swd_prepare_mem_csw(ap_num, access_size, flags, has_csw, csw_override);
    if (err != XDS_ERR_OK) {
        return err;
    }

    uint32_t cur_addr = addr;
    uint16_t off = 0U;
    while (off < byte_count) {
        uint32_t tar = cur_addr;
        uint8_t ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | XDS_AP_TAR_ADDR), &tar);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }

        uint32_t chunk_bytes = (uint32_t)byte_count - (uint32_t)off;
        if (addr_inc) {
            uint32_t to_boundary = 1024U - (cur_addr & 1023U);
            if (chunk_bytes > to_boundary) {
                chunk_bytes = to_boundary;
            }
        }
        uint32_t count = chunk_bytes / access_size;

        for (uint32_t i = 0U; i < count; i++) {
            uint32_t word;
            if (access_size == 4U) {
                word = read_u32le(&in_data[off]);
            } else if (access_size == 2U) {
                uint32_t shift = (cur_addr & 2U) * 8U;
                word = (uint32_t)read_u16le(&in_data[off]) << shift;
            } else {
                uint32_t shift = (cur_addr & 3U) * 8U;
                word = (uint32_t)in_data[off] << shift;
            }

            ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | XDS_AP_DRW_ADDR), &word);
            if (ack != SWD_ACK_OK) {
                return xds_swd_ack_to_err(ack);
            }
            off = (uint16_t)(off + access_size);
            if (addr_inc) {
                cur_addr += access_size;
            }
        }
    }

    swd_idle_cycles(XDS_SWD_TRAILING_CLKS);
    return XDS_ERR_OK;
}

static int32_t xds_swd_saci_poll_ctl(uint8_t ctl_reg, bool expect_bit0_set)
{
    uint8_t req = (uint8_t)(SWD_REQ_APnDP | SWD_REQ_RnW | (ctl_reg & 0x0CU));
    for (uint32_t i = 0U; i < XDS_SACI_POLL_RETRIES; i++) {
        uint8_t ack = xds_swd_cmapi_xfer(req, NULL);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
        uint32_t ctl = 0U;
        ack = xds_swd_cmapi_xfer(XDS_DP_RDBUFF_REQ, &ctl);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
        bool bit0 = (ctl & 1U) != 0U;
        if (bit0 == expect_bit0_set) {
            return XDS_ERR_OK;
        }
    }
    return XDS_ERR_SACI_TIMEOUT;
}

int32_t xds_swd_saci_write(uint8_t ap_num, uint16_t word_count, const uint8_t *in_words)
{
    if (!g_xds_swd_active) {
        return XDS_ERR_SWD_PROTOCOL;
    }

    /* All SEC-AP SACI mailbox registers (TXD/TXCTL/RXD/RXCTL) live in AP bank 0x00 */
    int32_t err = xds_swd_select_ap_bank(ap_num, XDS_SACI_REG_TXD);
    if (err != XDS_ERR_OK) {
        return err;
    }

    for (uint16_t i = 0U; i < word_count; i++) {
        err = xds_swd_saci_poll_ctl(XDS_SACI_REG_TXCTL, false);
        if (err != XDS_ERR_OK) {
            return err;
        }
        uint32_t word = read_u32le(&in_words[(uint32_t)i * 4U]);
        uint8_t ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | XDS_SACI_REG_TXD), &word);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
    }

    return xds_swd_saci_poll_ctl(XDS_SACI_REG_TXCTL, false);
}

int32_t xds_swd_saci_read(uint8_t ap_num, uint16_t word_count, uint8_t *out_words, uint16_t out_max, uint16_t *out_len)
{
    *out_len = 0U;
    if (!g_xds_swd_active) {
        return XDS_ERR_SWD_PROTOCOL;
    }
    if (((uint32_t)word_count * 4U) > (uint32_t)out_max) {
        return XDS_ERR_INVALID_PARAM;
    }

    int32_t err = xds_swd_select_ap_bank(ap_num, XDS_SACI_REG_RXD);
    if (err != XDS_ERR_OK) {
        return err;
    }

    for (uint16_t i = 0U; i < word_count; i++) {
        err = xds_swd_saci_poll_ctl(XDS_SACI_REG_RXCTL, true);
        if (err != XDS_ERR_OK) {
            return err;
        }
        uint8_t ack = xds_swd_cmapi_xfer((uint8_t)(SWD_REQ_APnDP | SWD_REQ_RnW | XDS_SACI_REG_RXD), NULL);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
        uint32_t word = 0U;
        ack = xds_swd_cmapi_xfer(XDS_DP_RDBUFF_REQ, &word);
        if (ack != SWD_ACK_OK) {
            return xds_swd_ack_to_err(ack);
        }
        write_u32le(&out_words[(uint32_t)i * 4U], word);
    }

    *out_len = (uint16_t)(word_count * 4U);
    return XDS_ERR_OK;
}

int32_t xds_swd_ocd_dap_request(
    const uint8_t *req, uint16_t req_len, uint8_t *resp_data, uint16_t resp_max, uint16_t *out_resp_len)
{
    uint16_t req_off = 0U;
    uint16_t resp_off = 0U;
    int32_t err = g_xds_swd_active ? XDS_ERR_OK : XDS_ERR_SWD_PROTOCOL;
    bool last_was_write = false;

    while (req_off < req_len) {
        uint8_t cmd = req[req_off++];
        if (cmd == 0U) {
            break;
        }

        uint8_t swd_req = (cmd >> 1U) & 0x0FU;
        if ((cmd & XDS_SWD_CMD_RNW) != 0U) {
            uint32_t val = 0U;
            if (err == XDS_ERR_OK) {
                uint8_t ack = xds_swd_xfer_retry(swd_req, &val);
                last_was_write = false;
                if (ack != SWD_ACK_OK) {
                    err = xds_swd_ack_to_err(ack);
                }
            }
            if ((resp_off + 4U) <= resp_max) {
                write_u32le(&resp_data[resp_off], val);
                resp_off = (uint16_t)(resp_off + 4U);
            }
        } else {
            if ((uint32_t)req_off + 4U > (uint32_t)req_len) {
                if (err == XDS_ERR_OK) {
                    err = XDS_ERR_INVALID_PARAM;
                }
                break;
            }
            uint32_t wval = read_u32le(&req[req_off]);
            req_off = (uint16_t)(req_off + 4U);

            if (err == XDS_ERR_OK) {
                uint8_t ack = xds_swd_xfer_retry(swd_req, &wval);
                if (ack != SWD_ACK_OK) {
                    err = xds_swd_ack_to_err(ack);
                    last_was_write = false;
                } else {
                    last_was_write = true;
                    if (swd_req == XDS_DP_SELECT_REQ) {
                        g_xds_select_cache = wval;
                        g_xds_select_valid = true;
                    }
                }
            }
        }
    }

    if ((err == XDS_ERR_OK) && last_was_write) {
        /* Per ARM ADIv5 B4.2.3, clock >= 8 trailing idle SWCLK cycles after a final write */
        swd_idle_cycles(XDS_SWD_TRAILING_CLKS);
    }

    *out_resp_len = resp_off;
    return err;
}

bool xds_swd_is_active(void)
{
    return g_xds_swd_active;
}
