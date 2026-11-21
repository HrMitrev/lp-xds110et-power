/*
 * bmp_bridge.h - C99-safe interface between main.c / bmp_mode.c and Black Magic Debug.
 */

#ifndef BMP_BRIDGE_H
#define BMP_BRIDGE_H

#include <stdbool.h>

/* Initialize Black Magic Debug bridge and TAP state */
void bmp_bridge_init(void);

/*
 * Cooperative super-loop poll when BMP mode is active:
 *   - Returns false immediately when CDC1 DTR is deasserted and no target is attached
 *     (or when idle with no pending RX bytes), allowing main() to execute __WFI().
 *   - Otherwise processes target polling and/or one GDB RSP packet while pumping
 *     app_background_poll() so EnergyTrace HDR, UART0, and XDS CMD 0x63 never stall.
 */
bool bmp_bridge_poll(void);

/* Cleanly detach any active BMP target and tri-state SWD pins on mode exit */
void bmp_bridge_on_mode_exit(void);

#endif /* BMP_BRIDGE_H */
