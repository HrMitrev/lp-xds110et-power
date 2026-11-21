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

/* Set to 1 when the Black Magic Debug core is linked in */
#ifndef FW_BMP_GDB_COMPILED
#define FW_BMP_GDB_COMPILED 0U
#endif

/* Global boot-time default mode macro (defaults to Normal; override with -DFW_DEFAULT_BMP_MODE=1) */
#ifndef FW_DEFAULT_BMP_MODE
#define FW_DEFAULT_BMP_MODE 0U
#endif

/* Wire values for XDS_CMD_PROBE_MODE (opcode 0x63) */
#define PROBE_MODE_NORMAL 0x00U
#define PROBE_MODE_BMP    0x01U
#define PROBE_MODE_QUERY  0xFFU

/* Capability flags returned in byte 1 of opcode 0x63 response */
#define PROBE_MODE_CAP_SWITCH      (1U << 0U) /* Runtime mode switching supported */
#define PROBE_MODE_CAP_GDB_SERVER  (1U << 1U) /* BMP GDB RSP server compiled in */
#define PROBE_MODE_CAP_DEFAULT_BMP (1U << 2U) /* Firmware boots in BMP mode by default */

/* Cooperative background tick callback invoked during BMP wait/delay loops */
typedef void (*bmp_bg_poll_fn_t)(void);

/* Initialize probe mode to FW_DEFAULT_BMP_MODE, register bg_poll callback, and init BMP TAPs */
void bmp_mode_init(bmp_bg_poll_fn_t bg_poll);

/* Invoke the registered background poll callback (safe no-op if NULL) */
void bmp_mode_background_poll(void);

/* Query current probe mode (PROBE_MODE_NORMAL or PROBE_MODE_BMP) */
uint8_t bmp_mode_get(void);

/* Query probe mode capability bitmask */
uint8_t bmp_mode_get_caps(void);

/* Convenience check: returns true when probe is in PROBE_MODE_BMP */
bool bmp_mode_is_active(void);

/* Returns true when BMP mode is active and RTT has claimed CDC0 (ttyACM0) */
bool bmp_rtt_is_active(void);

/*
 * Switch probe mode cleanly (disconnects active SWD session and flushes CDC1).
 * Returns XDS_ERR_OK on success or XDS_ERR_INVALID_PARAM for unknown mode.
 */
int32_t bmp_mode_set(uint8_t mode);

/*
 * Cooperative super-loop poll for the BMP subsystem:
 *   - In PROBE_MODE_NORMAL (when BMP is compiled in): inspects incoming CDC1
 *     (ttyBmpGdb) bytes and automatically switches to PROBE_MODE_BMP as soon as
 *     a valid GDB RSP ($...#XX) or BMP remote handshake (!GA# / !HA#) packet
 *     is recognized, preserving the triggering packet for immediate handling.
 *     Non-GDB traffic on CDC1 OUT is drained and discarded.
 *   - In PROBE_MODE_BMP: returns false immediately when idle (no DTR or no
 *     pending RX bytes and target not running) so main() can __WFI(); otherwise
 *     processes target polling and/or one GDB RSP packet while pumping the
 *     registered background callback.
 */
bool bmp_mode_poll(void);

#endif /* BMP_MODE_H */
