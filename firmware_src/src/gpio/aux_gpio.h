/*
 * aux_gpio.h - LP-XDS110ET Target Auxiliary GPIO driver (XDS110 opcodes 0x29/0x2A).
 *
 * Controls PF0..PF2 (XDS_GPIO3, XDS_GPIO1, XDS_GPIO2) and the U49 SN74AVC4T245
 * bidirectional level-shifter direction pins PJ0 (XDS_DIR_GPIO1) and PJ1
 * (XDS_DIR_GPIO2):
 *   Bit 0 (0x01): PF0 -> XDS_GPIO3 (U8 fixed A->B output)
 *   Bit 1 (0x02): PF1 -> XDS_GPIO1 (U49 bidir, PJ0 HIGH=output / LOW=input)
 *   Bit 2 (0x04): PF2 -> XDS_GPIO2 (U49 bidir, PJ1 HIGH=output / LOW=input)
 */

#ifndef AUX_GPIO_H
#define AUX_GPIO_H

#include <stdint.h>

/* XDS110 GPIO subcommand codes (verified against firmware_3.0.0.45.bin 0x181F4/0x1824C) */
#define AUX_GPIO_SUBCMD_VALUE      0x80000000U /* Read/write pin logic levels */
#define AUX_GPIO_SUBCMD_DIR        0x80000001U /* Read/write pin direction (1=out, 0=in) */
#define AUX_GPIO_SUBCMD_BLINK_LED  0x80000010U /* Alternate Red/Green LEDs to identify probe */
#define AUX_GPIO_SUBCMD_SWD_SWITCH 0x80008000U /* Reserved for Phase 4 JTAG/SWD switch */

/* Initialize PF0..PF2 as safe inputs and PJ0..PJ1 LOW (B->A target-to-probe input) */
void aux_gpio_init(void);

/* Execute XDS_GPIO_GET (opcode 0x29): populates *out_val (u16le) and returns error code */
int32_t aux_gpio_get(uint32_t subcmd, uint16_t *out_val);

/* Execute XDS_GPIO_SET (opcode 0x2A): mask_val = (mask << 8) | val; returns error code */
int32_t aux_gpio_set(uint32_t subcmd, uint16_t mask_val);

#endif /* AUX_GPIO_H */
