/*
 * bmp_mode.h - Runtime probe mode manager & cooperative BMP GDB poll loop.
 *
 * Controls ownership of CDC1 (if03 / ttyACM1) and the target SWD/JTAG pins:
 *   - PROBE_MODE_NORMAL (0): CDC1 = Aux/SWO RX UART (UART7), SWD = CMSIS-DAP v2 / XDS110
 *   - PROBE_MODE_BMP    (1): CDC1 = Black Magic Debug GDB RSP server, SWD/JTAG = BMP
 *
 * In both modes, CDC0 (ttyACM0 App UART) and EnergyTrace HDR (IF 2 / IF 6)
 * remain fully operational with zero USB re-enumeration.
 */

#ifndef BMP_MODE_H
#define BMP_MODE_H

#include <stdbool.h>
#include <stdint.h>

/* Global boot-time default mode macro (override with -DFW_DEFAULT_BMP_MODE=1) */
#ifndef FW_DEFAULT_BMP_MODE
#define FW_DEFAULT_BMP_MODE 0U
#endif

/* Set to 1 when the upstream Black Magic Debug core is linked in */
#ifndef FW_BMP_GDB_COMPILED
#define FW_BMP_GDB_COMPILED 0U
#endif

/* Wire values for XDS_CMD_PROBE_MODE (opcode 0x63) */
#define PROBE_MODE_NORMAL 0x00U
#define PROBE_MODE_BMP    0x01U
#define PROBE_MODE_QUERY  0xFFU

/* Capability flags returned in byte 1 of opcode 0x63 response */
#define PROBE_MODE_CAP_SWITCH      (1U << 0U) /* Runtime mode switching supported */
#define PROBE_MODE_CAP_GDB_SERVER  (1U << 1U) /* BMP GDB RSP server compiled in */
#define PROBE_MODE_CAP_DEFAULT_BMP (1U << 2U) /* Firmware boots in BMP mode by default */

/* Initialize probe mode to FW_DEFAULT_BMP_MODE and initialize BMP TAPs if compiled */
void bmp_mode_init(void);

/* Query current probe mode (PROBE_MODE_NORMAL or PROBE_MODE_BMP) */
uint8_t bmp_mode_get(void);

/* Query probe mode capability bitmask */
uint8_t bmp_mode_get_caps(void);

/* Convenience check: returns true when probe is in PROBE_MODE_BMP */
bool bmp_mode_is_active(void);

/*
 * Switch probe mode cleanly (disconnects active SWD session and flushes CDC1).
 * Returns XDS_ERR_OK on success or XDS_ERR_INVALID_PARAM for unknown mode.
 */
int32_t bmp_mode_set(uint8_t mode);

/*
 * Cooperative super-loop poll when BMP mode is active:
 *   - Returns false immediately when idle (no DTR or no pending RX bytes and target
 *     not running), allowing main() to execute __WFI().
 *   - Otherwise processes target polling and/or one GDB RSP packet while pumping
 *     app_background_poll() so EnergyTrace HDR, UART0, and XDS CMD 0x63 never stall.
 */
bool bmp_mode_poll(void);

#endif /* BMP_MODE_H */
