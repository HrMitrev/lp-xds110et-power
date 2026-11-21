/*
 * xds_swd.h - Native XDS110 Interface 2 SWD, CMAPI, SACI & OpenOCD DAP handlers.
 *
 * Implements the XDS110 Bulk 0 (0x2A framing) opcodes required by OpenOCD's
 * native `interface/xds110.cfg` SWD driver and TI UniFlash `DSLite`:
 *   - 0x04 XDS_SET_TCK         (SWCLK delay / speed control)
 *   - 0x05 / 0x06 TRST         (nTRST state validation / readback)
 *   - 0x07 XDS_CYCLE_TCK       (clock TCK/SWCLK cycles)
 *   - 0x0F..0x12 CMAPI session (connect / disconnect / acquire / release)
 *   - 0x13..0x14 CMAPI_MEM     (32-bit block memory read / write with CSW cache)
 *   - 0x15..0x16 CMAPI_REG     (single DP/AP register read / write)
 *   - 0x17..0x18 SWD_CONNECT   (JTAG-to-SWD / Dormant-to-SWD wake & disconnect)
 *   - 0x3A OCD_DAP_REQUEST     (OpenOCD batched raw SWD register queue)
 *   - 0x3D..0x3E CMAPI_MEM_64  (64-bit address block memory read / write)
 *   - 0x50..0x51 SACI_WRITE/RD (TI SEC-AP SACI mailbox word write / read)
 *   - 0x5A CMAPI_CONNECT_EX    (ADIv5 CMAPI connect with DP power-up)
 */

#ifndef XDS_SWD_H
#define XDS_SWD_H

#include <stdbool.h>
#include <stdint.h>

/* XDS110 SWD API error codes (matching OpenOCD src/jtag/drivers/xds110.c & TI firmware) */
#define XDS_ERR_SWD_WAIT     (-613)
#define XDS_ERR_SWD_FAULT    (-614)
#define XDS_ERR_SWD_PROTOCOL (-615)
#define XDS_ERR_SWD_PARITY   (-616)
#define XDS_ERR_SACI_TIMEOUT (-1141)

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

/* CMAPI lifecycle & discrete register / memory access (opcodes 0x0F..0x16, 0x3D..0x3E, 0x5A) */
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

/* TI SEC-AP SACI mailbox word write / read (opcodes 0x50 / 0x51) */
int32_t xds_swd_saci_write(uint8_t ap_num, uint16_t word_count, const uint8_t *in_words);
int32_t xds_swd_saci_read(uint8_t ap_num, uint16_t word_count, uint8_t *out_words, uint16_t out_max, uint16_t *out_len);

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
