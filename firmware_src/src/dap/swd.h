/*
 * swd.h - Fast ARM Serial Wire Debug (SWD) wire driver for LP-XDS110ET.
 *
 * Drives Port A debug pins (PA2=SWCLK, PA3=SWDIO, PA4=TDO, PA5=TDI, PA6=nSRST)
 * and the U10 SN74AVC4T245 bidirectional level-shifter direction control on
 * PC5 (XDS_TMS_DIR: HIGH = A->B probe-to-target, LOW = B->A target-to-probe).
 */

#ifndef SWD_H
#define SWD_H

#include <stdbool.h>
#include <stdint.h>

/* CMSIS-DAP SWD transfer request bit definitions */
#define SWD_REQ_APnDP (1U << 0U)
#define SWD_REQ_RnW   (1U << 1U)
#define SWD_REQ_A2    (1U << 2U)
#define SWD_REQ_A3    (1U << 3U)

/* SWD ACK codes (3-bit LSB-first wire values) */
#define SWD_ACK_OK    0x01U
#define SWD_ACK_WAIT  0x02U
#define SWD_ACK_FAULT 0x04U
#define SWD_ACK_ERROR 0x08U /* Parity error on received data */

/* CMSIS-DAP DAP_SWJ_Pins bit definitions */
#define SWD_PIN_SWCLK_TCK 0x01U
#define SWD_PIN_SWDIO_TMS 0x02U
#define SWD_PIN_TDI       0x04U
#define SWD_PIN_TDO       0x08U
#define SWD_PIN_NRESET    0x80U
#define SWD_PIN_READ_MASK (SWD_PIN_SWCLK_TCK | SWD_PIN_SWDIO_TMS | SWD_PIN_TDI | SWD_PIN_TDO | SWD_PIN_NRESET)

/* Initialize DWT cycle counter and place SWD/JTAG lines into safe high-Z idle */
void swd_init(void);

/* Configure Port A & PC5 (U10 TMS_DIR) for active SWD operation */
void swd_connect(void);

/* Release Port A & PC5 (U10 TMS_DIR) back to safe high-Z input state */
void swd_disconnect(void);

/* Set target SWCLK frequency in Hz (updates DWT half-period cycle delay) */
void swd_set_clock_hz(uint32_t clock_hz);

/* Configure SWD turnaround cycles (1..4), dataPhase-on-WAIT/FAULT, and idle cycles */
void swd_configure(uint8_t turnaround_cycles, bool data_phase, uint8_t idle_cycles);

/* Read current logic levels of SWCLK/SWDIO/TDI/TDO/nRESET */
uint8_t swd_pins_read(void);

/* Drive selected SWJ pins (pin_sel bitmask) to pin_out values */
void swd_pins_write(uint8_t pin_out, uint8_t pin_sel);

/* Shift out bit_count (1..256) LSB-first bits on SWDIO (DAP_SWJ_Sequence) */
void swd_sequence(uint32_t bit_count, const uint8_t *data);

/* Clock count idle SWCLK cycles with SWDIO LOW (ensures trailing AP/DP writes commit) */
void swd_idle_cycles(uint32_t count);

/*
 * Execute a single SWD DP/AP read or write transaction.
 *   request: low 4 bits = [A3, A2, RnW, APnDP]
 *   data:    pointer to u32 write value (RnW=0) or read destination (RnW=1, may be NULL)
 * Returns 3-bit ACK (SWD_ACK_OK / WAIT / FAULT / 0x07) or SWD_ACK_ERROR on parity mismatch.
 */
uint8_t swd_transfer(uint8_t request, uint32_t *data);

#endif /* SWD_H */
