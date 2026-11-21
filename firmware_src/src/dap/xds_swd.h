/*
 * xds_swd.h - Native XDS110 Interface 2 SWD & OpenOCD DAP command handlers.
 *
 * Implements the XDS110 Bulk 0 (0x2A framing) opcodes required by OpenOCD's
 * native `interface/xds110.cfg` SWD driver:
 *   - 0x04 XDS_SET_TCK       (SWCLK delay / speed control)
 *   - 0x05 XDS_SET_TRST      (nTRST state validation)
 *   - 0x07 XDS_CYCLE_TCK     (clock TCK/SWCLK cycles)
 *   - 0x0F CMAPI_CONNECT     (read SW-DP IDCODE)
 *   - 0x10 CMAPI_DISCONNECT  (close CMAPI session)
 *   - 0x11 CMAPI_ACQUIRE     (acquire CMAPI session)
 *   - 0x12 CMAPI_RELEASE     (release CMAPI session)
 *   - 0x15 CMAPI_REG_READ    (legacy single DP/AP register read)
 *   - 0x16 CMAPI_REG_WRITE   (legacy single DP/AP register write)
 *   - 0x17 SWD_CONNECT       (JTAG-to-SWD switch sequence)
 *   - 0x18 SWD_DISCONNECT    (SWD-to-JTAG & pin tri-state)
 *   - 0x19 SWD_CLEAR_ERRORS  (DP ABORT sticky error clear)
 *   - 0x3A OCD_DAP_REQUEST   (batched raw SWD register read/write queue)
 */

#ifndef XDS_SWD_H
#define XDS_SWD_H

#include <stdbool.h>
#include <stdint.h>

/* XDS110 SWD API error codes (matching OpenOCD src/jtag/drivers/xds110.c) */
#define XDS_ERR_SWD_WAIT     (-613)
#define XDS_ERR_SWD_FAULT    (-614)
#define XDS_ERR_SWD_PROTOCOL (-615)
#define XDS_ERR_SWD_PARITY   (-616)

/* Reset native XDS110 SWD session state */
void xds_swd_init(void);

/* Configure SWCLK frequency from XDS110 fast-TCK delay count (opcode 0x04) */
int32_t xds_swd_set_tck_delay(uint32_t delay_count);

/* Toggle SWCLK/TCK for count cycles (opcode 0x07) */
int32_t xds_swd_cycle_tck(uint32_t count);

/* Switch target into SWD mode with ARM JTAG-to-SWD sequence (opcode 0x17) */
int32_t xds_swd_connect(void);

/* Switch target out of SWD mode and tri-state debug pins (opcode 0x18) */
int32_t xds_swd_disconnect(void);

/* Clear sticky SW-DP errors via DP ABORT write (opcode 0x19) */
int32_t xds_swd_clear_errors(void);

/* Execute ARM ADIv5.2 Dormant-to-SWD selection alert wake sequence (opcode 0x1A) */
int32_t xds_swd_dormant_wake(void);

/* CMAPI lifecycle & discrete register / memory access (opcodes 0x0F..0x16, 0x5A) */
int32_t xds_swd_cmapi_connect(uint32_t *out_idcode);
int32_t xds_swd_cmapi_disconnect(void);
int32_t xds_swd_cmapi_acquire(void);
int32_t xds_swd_cmapi_release(void);
int32_t xds_swd_cmapi_read_reg(uint8_t type, uint8_t ap_num, uint8_t addr, uint32_t *out_val);
int32_t xds_swd_cmapi_write_reg(uint8_t type, uint8_t ap_num, uint8_t addr, uint32_t val);
int32_t xds_swd_cmapi_read_mem(uint8_t access_size, uint8_t flags, uint8_t ap_num, uint32_t addr, uint16_t byte_count,
    bool has_csw, uint32_t csw_override, uint8_t *out_data, uint16_t out_max, uint16_t *out_len);
int32_t xds_swd_cmapi_write_mem(uint8_t access_size, uint8_t flags, uint8_t ap_num, uint32_t addr, uint16_t byte_count,
    bool has_csw, uint32_t csw_override, const uint8_t *in_data);

/*
 * Execute a null-terminated batch of raw SWD read/write commands (opcode 0x3A).
 * Writes 4-byte little-endian results for every read command into resp_data and
 * sets *out_resp_len to total_reads * 4 (zero-padded on early wire error so
 * response length always matches OpenOCD's expected payload size).
 */
int32_t xds_swd_ocd_dap_request(
    const uint8_t *req, uint16_t req_len, uint8_t *resp_data, uint16_t resp_max, uint16_t *out_resp_len);

/* Query whether native XDS110 SWD mode is currently active */
bool xds_swd_is_active(void);

#endif /* XDS_SWD_H */
