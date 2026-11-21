/*
 * cmsis_dap.h - ARM CMSIS-DAP v2.0.0 WinUSB Bulk protocol handler for LP-XDS110ET.
 *
 * Runs over USB Interface 5 (Vendor Bulk OUT 0x05 / IN 0x86, up to 512-byte HS
 * / 64-byte FS variable-length packets) with SWD and Atomic Commands (0x7E/0x7F).
 */

#ifndef CMSIS_DAP_H
#define CMSIS_DAP_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize CMSIS-DAP state and place JTAG/SWD lines into safe high-Z idle */
void cmsis_dap_init(const uint8_t fw_version[4]);

/*
 * Non-blocking super-loop poll: pops and executes any pending CMSIS-DAP v2
 * Bulk OUT command packet and pushes the exact-length Bulk IN response.
 * Returns true if a packet was processed.
 */
bool cmsis_dap_poll(void);

#endif /* CMSIS_DAP_H */
