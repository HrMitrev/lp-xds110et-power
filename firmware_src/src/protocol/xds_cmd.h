/*
 * xds_cmd.h - XDS110 Bulk Command Protocol (0x2A framing) and dispatcher.
 *
 * Wire format on Interface 2 (Bulk OUT 0x02 / Bulk IN 0x83):
 *   Request:  '*'(0x2A) | u16le payload_len | u8 opcode | args[payload_len - 1]
 *   Response: '*'(0x2A) | u16le payload_len | i32le error | data[payload_len - 4]
 */

#ifndef XDS_CMD_H
#define XDS_CMD_H

#include <stdbool.h>
#include <stdint.h>

/* Framing limits */
#define XDS_FRAME_MAGIC       0x2AU
#define XDS_REQ_HEADER_SIZE   3U
#define XDS_RESP_HEADER_SIZE  7U /* magic(1) + len(2) + error(4) */
#define XDS_MAX_FRAME_SIZE    0x1100U
#define XDS_MAX_RESP_DATA_LEN (XDS_MAX_FRAME_SIZE - XDS_RESP_HEADER_SIZE)

/* XDS110 command opcodes (verified against firmware_3.0.0.45.bin TBH table) */
#define XDS_CMD_ECHO        0x00U
#define XDS_CMD_CONNECT     0x01U
#define XDS_CMD_DISCONNECT  0x02U
#define XDS_CMD_VERSION     0x03U
#define XDS_CMD_GET_POWER   0x08U
#define XDS_CMD_SET_SRST    0x0EU
#define XDS_CMD_ET_SETUP    0x1DU
#define XDS_CMD_ET_CAL      0x1EU
#define XDS_CMD_ET_START    0x1FU
#define XDS_CMD_ET_STOP     0x20U
#define XDS_CMD_ET_CLEANUP  0x21U
#define XDS_CMD_ENTER_DFU   0x26U
#define XDS_CMD_CONNECT_ET  0x28U
#define XDS_CMD_ET_RANGE    0x30U
#define XDS_CMD_ET_DIG      0x31U
#define XDS_CMD_ET_DIG_ARGS 0x44U
#define XDS_CMD_ET_HWINFO   0x46U
#define XDS_CMD_PING        0x4AU

/* Standard signed error codes returned in response bytes [3..6] */
#define XDS_ERR_OK             0
#define XDS_ERR_GENERIC        (-1)
#define XDS_ERR_INVALID_PARAM  (-122)
#define XDS_ERR_UNSUPPORTED    (-122)
#define XDS_ERR_ET_UNSUPPORTED (-640)
#define XDS_ERR_ET_NOT_INIT    (-644)

/* Initialize XDS command processor with 4-byte [build, patch, minor, major] version */
void xds_cmd_init(const uint8_t fw_version[4]);

/*
 * Non-blocking super-loop poll: parses and executes any complete 0x2A command
 * frame on Bulk 0 and handles Bulk 1 OUT loopback test packets.
 * Returns true if any USB work was processed.
 */
bool xds_cmd_poll(void);

/* Query whether a debug or EnergyTrace host session is currently connected */
bool xds_cmd_is_connected(void);

#endif /* XDS_CMD_H */
