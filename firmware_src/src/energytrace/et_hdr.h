/*
 * et_hdr.h - LP-XDS110ET EnergyTrace HDR (256 kS/s ADS127L01) engine.
 *
 * Implements the hardware driver and state machine for the onboard
 * ADS127L01IPBS 24-bit delta-sigma ADC, INA118UB/THS4551 analog front-end,
 * TS3A24159/TS3A4751 calibration switches, and zero-CPU-overhead ping-pong
 * uDMA pipeline (PB4 nDRDY -> uDMA Ch5 -> SSI1_DR -> uDMA Ch24 -> Bulk 1 IN).
 */

#ifndef ET_HDR_H
#define ET_HDR_H

#include <stdbool.h>
#include <stdint.h>

/* EnergyTrace hardware type & capability flags (CMD 0x46 ET_HARDWARE_INFO) */
#define ET_HW_TYPE_HDR          2U
#define ET_HW_CAPS_STD          0x07U
#define ET_HW_CAPS_ALT          0x02U
#define ET_ERR_OK               0
#define ET_ERR_UNSUPPORTED_MODE (-640)
#define ET_ERR_NOT_INITIALIZED  (-644)
#define ET_ERR_UNSUPPORTED_HW   (-670)
#define ET_ERR_INVALID_CONFIG   (-671)

/* EnergyTrace operating modes (CMD 0x1D mode argument) */
#define ET_MODE_STANDARD 0U
#define ET_MODE_HDR      1U
#define ET_MODE_ALT      2U

/* Calibration point indices (CMD 0x1E cal_idx argument) */
#define ET_CAL_IDX_ZERO_MA 0U /* Open circuit (0.00 mA offset) */
#define ET_CAL_IDX_40_2_MA 1U /* ET_CAL0 (PM4): R123 = 82 ohm (~40.24 mA) */
#define ET_CAL_IDX_90_4_MA 2U /* ET_CAL1 (PM5): R125 = 36.5 ohm (~90.41 mA) */
#define ET_CAL_IDX_1_5_MA  4U /* ET_CAL2 (PC6): R130 = 2.2 kohm (~1.50 mA) */
#define ET_CAL_SAMPLE_CNT  5U

/* Stream framing constants (Bulk 1 IN 0x87) */
#define ET_STREAM_SYNC_MAGIC    0x35U
#define ET_STREAM_PKT_ANALOG    0x68U
#define ET_STREAM_PKT_HEADER_SZ 6U
#define ET_STREAM_SAMPLES_PKT   256U
#define ET_STREAM_PAYLOAD_BYTES (ET_STREAM_SAMPLES_PKT * 4U) /* 1024 bytes */
#define ET_STREAM_PKT_SIZE      (ET_STREAM_PKT_HEADER_SZ + ET_STREAM_PAYLOAD_BYTES)

/* Initialize uDMA, SSI1, PB4 nDRDY trigger, and ADS127L01 reset */
void et_hdr_init(void);

/*
 * Run one-shot boot-time calibration (matches firmware_3.0.0.45.bin 0x00020420):
 * asserts target nSRST, isolates LP_VDD via PM1, measures cal points 0/1/2/4,
 * caches results in SRAM, restores LP_VDD, and releases nSRST.
 */
void et_hdr_boot_calibrate(void);

/* Query EnergyTrace hardware info for CMD 0x46 */
int32_t et_hdr_get_hw_info(uint32_t *out_hw_type, uint8_t *out_caps);

/* Configure optional range (CMD 0x30) and digital trace mode (CMD 0x31) */
int32_t et_hdr_set_range(uint8_t range);
int32_t et_hdr_set_dig(uint8_t dig_type);

/* Setup EnergyTrace session (CMD 0x1D) */
int32_t et_hdr_setup(uint8_t mode, uint8_t dig_en, uint32_t sample_freq, uint8_t trigger_mode);

/* Return boot-cached calibration (sum, count=5) for CMD 0x1E */
int32_t et_hdr_get_calibration(uint16_t cal_idx, int32_t *out_sum, uint32_t *out_count);

/* Start 256 kS/s DMA streaming to Bulk 1 IN (CMD 0x1F) */
int32_t et_hdr_start(void);

/* Stop DMA streaming and ADC conversion (CMD 0x20) */
int32_t et_hdr_stop(void);

/* Tear down session and bypass 1 ohm shunt (CMD 0x21) */
int32_t et_hdr_cleanup(void);

/* Query whether EnergyTrace HDR is currently streaming */
bool et_hdr_is_running(void);

#endif /* ET_HDR_H */
