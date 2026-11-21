/*
 * cmsis_dap.h - ARM CMSIS-DAP 1.0 HID protocol handler for LP-XDS110ET.
 *
 * Runs over USB Interface 5 (HID Interrupt OUT 0x05 / IN 0x86, 64-byte packets).
 *
 * Phase 4A implements HID packet framing, discovery, status, clock, transfer
 * configuration, and direct SWJ pin control (0x00..0x04, 0x09..0x0A, 0x10..0x11,
 * 0x13). Phase 4B will add the fast GPIO_PORTA_AHB SWD wire engine (0x05, 0x06,
 * 0x08, 0x12).
 */

#ifndef CMSIS_DAP_H
#define CMSIS_DAP_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize CMSIS-DAP state and place JTAG/SWD lines into safe high-Z idle */
void cmsis_dap_init(const uint8_t fw_version[4]);

/*
 * Non-blocking super-loop poll: pops and executes any pending 64-byte HID
 * CMSIS-DAP command report and pushes the 64-byte response report.
 * Returns true if a packet was processed.
 */
bool cmsis_dap_poll(void);

#endif /* CMSIS_DAP_H */
