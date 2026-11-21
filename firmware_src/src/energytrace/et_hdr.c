/*
 * et_hdr.c - LP-XDS110ET EnergyTrace HDR (ADS127L01 + SSI1 + uDMA) driver.
 *
 * Uses MSP432E4 DriverLib exclusively:
 *   - SSI1 Master (Mode 1: CPOL=0, CPHA=1, 8-bit, 15 MHz SCLK)
 *   - PB4 (ADS127L01 nDRDY) falling-edge hardware DMA trigger -> uDMA Ch 5
 *     pushes 4 dummy bytes per sample into SSI1->DR (32 SCLKs = 2.13 us < 3.91 us)
 *   - uDMA Ch 24 (SSI1 RX) ping-pongs 1024-byte frames directly into two
 *     pre-framed 1030-byte (0x68) Bulk 1 IN packet buffers (1 kHz ISR rate).
 */

#include "et_hdr.h"

#include <string.h>

#include "board_lp_xds110et.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/ssi.h"
#include "driverlib/sysctl.h"
#include "driverlib/udma.h"
#include "usb/usb_device.h"

/* SSI1 clock rate: 120 MHz / 8 = 15.0 MHz SCLK (32 clocks = 2.13 us per sample) */
#define ET_SSI_BITRATE_HZ  15000000U
#define ET_CAL_SETTLE_MS   30U
#define ET_BOOT_ISOLATE_MS 300U
#define ET_CAL_CACHE_SLOTS 4U
#define ET_GPIOB_IRQ_PRIO  1U
#define ET_STREAM_IRQ_PRIO 2U /* Shared by SSI1 & USB0 so neither preempts ring/FIFO access */

typedef enum {
    ET_DMA_IDLE = 0,
    ET_DMA_CALIBRATING,
    ET_DMA_STREAMING,
} et_dma_mode_t;

typedef struct {
    /* 1024-byte aligned uDMA channel control table (primary + alternate, 32 channels) */
    uint8_t udma_control_table[1024];
    /* Dummy TX source bytes clocked to ADS127L01 DIN by uDMA Ch 5 */
    uint8_t tx_dummy[2];
    /* Two pre-framed 1030-byte (6 B header + 1024 B payload) ping-pong packet buffers */
    uint8_t pkt_ping[ET_STREAM_PKT_SIZE];
    uint8_t pkt_pong[ET_STREAM_PKT_SIZE];
    /* Boot-cached calibration sums: [0]=0mA, [1]=40.2mA, [2]=90.4mA, [3]=1.5mA */
    int32_t cal_cache[ET_CAL_CACHE_SLOTS];
    bool cal_valid;
    /* Hardware & streaming session state */
    bool hw_initialized;
    bool session_setup;
    volatile et_dma_mode_t dma_mode;
    volatile bool cal_capture_done;
    volatile uint8_t pkt_seq;
    /* Host-configured session parameters */
    uint8_t cfg_range;
    uint8_t cfg_dig_type;
    uint32_t cfg_sample_freq;
} et_hdr_state_t;

static et_hdr_state_t s_et __attribute__((aligned(1024)));

/* ---- Analog Switch & ADC GPIO Helpers ---- */

static void et_switch_shunt_enable(bool enable)
{
    /* PM0 (ET_BYPASS): HIGH routes current through 1 ohm shunt, LOW bypasses shunt */
    MAP_GPIOPinWrite(BOARD_ET_SW_PORT_BASE, BOARD_ET_BYPASS_PIN, enable ? BOARD_ET_BYPASS_PIN : 0U);
}

static void et_switch_calibrate_isolate(bool isolate_target)
{
    /* PM1 (ET_CALIBRATE): HIGH disconnects LP_VDD target & connects cal resistor bank */
    MAP_GPIOPinWrite(BOARD_ET_SW_PORT_BASE, BOARD_ET_CALIBRATE_PIN, isolate_target ? BOARD_ET_CALIBRATE_PIN : 0U);
}

static void et_adc_start_pin_set(bool high)
{
    /* PM2 (ET_PM2): ADS127L01 START pin */
    MAP_GPIOPinWrite(BOARD_ET_SW_PORT_BASE, BOARD_ET_ADC_START_PIN, high ? BOARD_ET_ADC_START_PIN : 0U);
}

static void et_cal_resistors_all_off(void)
{
    MAP_GPIOPinWrite(BOARD_ET_SW_PORT_BASE, BOARD_ET_CAL0_PIN | BOARD_ET_CAL1_PIN, 0U);
    MAP_GPIOPinWrite(BOARD_ET_CAL2_PORT_BASE, BOARD_ET_CAL2_PIN, 0U);
}

static void et_cal_select_resistor(uint16_t cal_idx)
{
    et_cal_resistors_all_off();

    switch (cal_idx) {
    case ET_CAL_IDX_40_2_MA:
        MAP_GPIOPinWrite(BOARD_ET_SW_PORT_BASE, BOARD_ET_CAL0_PIN, BOARD_ET_CAL0_PIN);
        break;
    case ET_CAL_IDX_90_4_MA:
        MAP_GPIOPinWrite(BOARD_ET_SW_PORT_BASE, BOARD_ET_CAL1_PIN, BOARD_ET_CAL1_PIN);
        break;
    case ET_CAL_IDX_1_5_MA:
        MAP_GPIOPinWrite(BOARD_ET_CAL2_PORT_BASE, BOARD_ET_CAL2_PIN, BOARD_ET_CAL2_PIN);
        break;
    case ET_CAL_IDX_ZERO_MA:
    default:
        break;
    }
}

static void et_adc_reset_pulse(void)
{
    /* Pulse PN3 (ADS127L01 nRESET/nPWDN) LOW -> HIGH */
    MAP_GPIOPinWrite(BOARD_ET_RESET_PORT, BOARD_ET_ADC_NRESET_PIN, 0U);
    board_delay_us(200U);
    MAP_GPIOPinWrite(BOARD_ET_RESET_PORT, BOARD_ET_ADC_NRESET_PIN, BOARD_ET_ADC_NRESET_PIN);
    board_delay_ms(2U);
}

static void et_ssi1_drain_rx_fifo(void)
{
    uint32_t dummy = 0U;
    while (MAP_SSIDataGetNonBlocking(BOARD_ET_SSI_BASE, &dummy) != 0) {
    }
}

/* ---- uDMA Ping-Pong Setup & Teardown ---- */

static void et_dma_configure_rx(void)
{
    MAP_uDMAChannelAssign(UDMA_CH24_SSI1RX);
    MAP_uDMAChannelAttributeEnable(UDMA_CH24_SSI1RX, UDMA_ATTR_HIGH_PRIORITY);
    MAP_uDMAChannelAttributeDisable(UDMA_CH24_SSI1RX, UDMA_ATTR_USEBURST | UDMA_ATTR_ALTSELECT | UDMA_ATTR_REQMASK);

    const uint32_t rx_ctrl = UDMA_SIZE_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_8 | UDMA_ARB_4;
    MAP_uDMAChannelControlSet(UDMA_CH24_SSI1RX | UDMA_PRI_SELECT, rx_ctrl);
    MAP_uDMAChannelControlSet(UDMA_CH24_SSI1RX | UDMA_ALT_SELECT, rx_ctrl);

    MAP_uDMAChannelTransferSet(UDMA_CH24_SSI1RX | UDMA_PRI_SELECT, UDMA_MODE_PINGPONG, (void *)&SSI1->DR,
        &s_et.pkt_ping[ET_STREAM_PKT_HEADER_SZ], ET_STREAM_PAYLOAD_BYTES);
    MAP_uDMAChannelTransferSet(UDMA_CH24_SSI1RX | UDMA_ALT_SELECT, UDMA_MODE_PINGPONG, (void *)&SSI1->DR,
        &s_et.pkt_pong[ET_STREAM_PKT_HEADER_SZ], ET_STREAM_PAYLOAD_BYTES);

    MAP_SSIDMAEnable(BOARD_ET_SSI_BASE, SSI_DMA_RX);
    MAP_SSIIntClear(BOARD_ET_SSI_BASE, SSI_DMARX);
    MAP_SSIIntEnable(BOARD_ET_SSI_BASE, SSI_DMARX);
    MAP_uDMAChannelEnable(UDMA_CH24_SSI1RX);
}

static void et_dma_configure_tx_trigger(void)
{
    MAP_uDMAChannelAssign(UDMA_CH5_GPIOB);
    MAP_uDMAChannelAttributeEnable(UDMA_CH5_GPIOB, UDMA_ATTR_USEBURST | UDMA_ATTR_HIGH_PRIORITY);
    MAP_uDMAChannelAttributeDisable(UDMA_CH5_GPIOB, UDMA_ATTR_ALTSELECT | UDMA_ATTR_REQMASK);

    const uint32_t tx_ctrl = UDMA_SIZE_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_NONE | UDMA_ARB_4;
    MAP_uDMAChannelControlSet(UDMA_CH5_GPIOB | UDMA_PRI_SELECT, tx_ctrl);
    MAP_uDMAChannelControlSet(UDMA_CH5_GPIOB | UDMA_ALT_SELECT, tx_ctrl);

    MAP_uDMAChannelTransferSet(UDMA_CH5_GPIOB | UDMA_PRI_SELECT, UDMA_MODE_PINGPONG, &s_et.tx_dummy[0],
        (void *)&SSI1->DR, ET_STREAM_PAYLOAD_BYTES);
    MAP_uDMAChannelTransferSet(UDMA_CH5_GPIOB | UDMA_ALT_SELECT, UDMA_MODE_PINGPONG, &s_et.tx_dummy[1],
        (void *)&SSI1->DR, ET_STREAM_PAYLOAD_BYTES);

    MAP_SSIDMAEnable(BOARD_ET_SSI_BASE, SSI_DMA_TX);
    MAP_uDMAChannelEnable(UDMA_CH5_GPIOB);
    MAP_GPIODMATriggerEnable(BOARD_ET_DRDY_PORT_BASE, BOARD_ET_DRDY_PIN);
}

static void et_dma_stop_all(void)
{
    et_adc_start_pin_set(false);

    NVIC_DisableIRQ(GPIOB_IRQn);
    NVIC_DisableIRQ(SSI1_IRQn);

    MAP_GPIOIntDisable(BOARD_ET_DRDY_PORT_BASE, GPIO_INT_DMA);
    MAP_GPIOIntClear(BOARD_ET_DRDY_PORT_BASE, GPIO_INT_DMA);
    MAP_GPIODMATriggerDisable(BOARD_ET_DRDY_PORT_BASE, BOARD_ET_DRDY_PIN);

    MAP_uDMAChannelDisable(UDMA_CH5_GPIOB);
    MAP_uDMAChannelDisable(UDMA_CH24_SSI1RX);

    MAP_SSIIntDisable(BOARD_ET_SSI_BASE, SSI_DMARX);
    MAP_SSIIntClear(BOARD_ET_SSI_BASE, SSI_DMARX);
    MAP_SSIDMADisable(BOARD_ET_SSI_BASE, SSI_DMA_RX | SSI_DMA_TX);
    MAP_SSIDisable(BOARD_ET_SSI_BASE);

    s_et.dma_mode = ET_DMA_IDLE;
}

static void et_dma_start_capture(et_dma_mode_t mode)
{
    et_dma_stop_all();
    et_ssi1_drain_rx_fifo();

    s_et.dma_mode = mode;
    s_et.cal_capture_done = false;

    et_dma_configure_rx();
    et_dma_configure_tx_trigger();

    MAP_GPIOIntClear(BOARD_ET_DRDY_PORT_BASE, GPIO_INT_DMA);
    MAP_GPIOIntEnable(BOARD_ET_DRDY_PORT_BASE, GPIO_INT_DMA);

    NVIC_ClearPendingIRQ(GPIOB_IRQn);
    NVIC_ClearPendingIRQ(SSI1_IRQn);
    NVIC_EnableIRQ(SSI1_IRQn);
    NVIC_EnableIRQ(GPIOB_IRQn);

    MAP_SSIEnable(BOARD_ET_SSI_BASE);
    et_adc_start_pin_set(true);
}

/* ---- Hardware Interrupt Handlers (Override Weak Vectors in startup_gcc.c) ---- */

void gpiob_irq_handler(void)
{
    MAP_GPIOIntClear(BOARD_ET_DRDY_PORT_BASE, GPIO_INT_DMA);

    if (s_et.dma_mode != ET_DMA_STREAMING) {
        return;
    }

    if (MAP_uDMAChannelModeGet(UDMA_CH5_GPIOB | UDMA_PRI_SELECT) == UDMA_MODE_STOP) {
        MAP_uDMAChannelTransferSet(UDMA_CH5_GPIOB | UDMA_PRI_SELECT, UDMA_MODE_PINGPONG, &s_et.tx_dummy[0],
            (void *)&SSI1->DR, ET_STREAM_PAYLOAD_BYTES);
    }

    if (MAP_uDMAChannelModeGet(UDMA_CH5_GPIOB | UDMA_ALT_SELECT) == UDMA_MODE_STOP) {
        MAP_uDMAChannelTransferSet(UDMA_CH5_GPIOB | UDMA_ALT_SELECT, UDMA_MODE_PINGPONG, &s_et.tx_dummy[1],
            (void *)&SSI1->DR, ET_STREAM_PAYLOAD_BYTES);
    }

    MAP_uDMAChannelEnable(UDMA_CH5_GPIOB);
}

void ssi1_irq_handler(void)
{
    uint32_t status = MAP_SSIIntStatus(BOARD_ET_SSI_BASE, true);
    MAP_SSIIntClear(BOARD_ET_SSI_BASE, status);

    if (s_et.dma_mode == ET_DMA_CALIBRATING) {
        /* Both ping (discarded settling) and pong (measured) finished */
        if (MAP_uDMAChannelModeGet(UDMA_CH24_SSI1RX | UDMA_ALT_SELECT) == UDMA_MODE_STOP) {
            s_et.cal_capture_done = true;
        }
        return;
    }

    if (s_et.dma_mode != ET_DMA_STREAMING) {
        return;
    }

    if (MAP_uDMAChannelModeGet(UDMA_CH24_SSI1RX | UDMA_PRI_SELECT) == UDMA_MODE_STOP) {
        if (usb_data_tx_space() >= ET_STREAM_PKT_SIZE) {
            usb_data_tx_write(s_et.pkt_ping, ET_STREAM_PKT_SIZE);
        }
        s_et.pkt_ping[1] = s_et.pkt_seq++;
        MAP_uDMAChannelTransferSet(UDMA_CH24_SSI1RX | UDMA_PRI_SELECT, UDMA_MODE_PINGPONG, (void *)&SSI1->DR,
            &s_et.pkt_ping[ET_STREAM_PKT_HEADER_SZ], ET_STREAM_PAYLOAD_BYTES);
    }

    if (MAP_uDMAChannelModeGet(UDMA_CH24_SSI1RX | UDMA_ALT_SELECT) == UDMA_MODE_STOP) {
        if (usb_data_tx_space() >= ET_STREAM_PKT_SIZE) {
            usb_data_tx_write(s_et.pkt_pong, ET_STREAM_PKT_SIZE);
        }
        s_et.pkt_pong[1] = s_et.pkt_seq++;
        MAP_uDMAChannelTransferSet(UDMA_CH24_SSI1RX | UDMA_ALT_SELECT, UDMA_MODE_PINGPONG, (void *)&SSI1->DR,
            &s_et.pkt_pong[ET_STREAM_PKT_HEADER_SZ], ET_STREAM_PAYLOAD_BYTES);
    }

    MAP_uDMAChannelEnable(UDMA_CH24_SSI1RX);
}

/* ---- Calibration Measurement Helpers ---- */

static int32_t et_decode_signed24_be(const uint8_t *sample)
{
    uint32_t raw = ((uint32_t)sample[0] << 16U) | ((uint32_t)sample[1] << 8U) | (uint32_t)sample[2];
    if (raw >= 0x00800000U) {
        return (int32_t)(raw - 0x01000000U);
    }
    return (int32_t)raw;
}

static int32_t et_measure_cal_point(uint16_t cal_idx)
{
    et_cal_select_resistor(cal_idx);
    board_delay_ms(ET_CAL_SETTLE_MS);

    int32_t total_sum = 0;

    for (uint32_t iter = 0U; iter < ET_CAL_SAMPLE_CNT; iter++) {
        et_dma_start_capture(ET_DMA_CALIBRATING);

        uint32_t start_ms = board_get_uptime_ms();
        while (!s_et.cal_capture_done) {
            if ((uint32_t)(board_get_uptime_ms() - start_ms) >= 50U) {
                break;
            }
            __WFI();
        }

        et_dma_stop_all();

        /* Discard first 256 samples (ping); average second 256 samples (pong) */
        const uint8_t *samples = &s_et.pkt_pong[ET_STREAM_PKT_HEADER_SZ];
        int32_t burst_sum = 0;
        for (uint32_t i = 0U; i < ET_STREAM_SAMPLES_PKT; i++) {
            burst_sum += et_decode_signed24_be(&samples[i * 4U]);
        }
        total_sum += (burst_sum / (int32_t)ET_STREAM_SAMPLES_PKT);
    }

    et_cal_resistors_all_off();
    return total_sum;
}

static int32_t et_cal_idx_to_slot(uint16_t cal_idx)
{
    switch (cal_idx) {
    case ET_CAL_IDX_ZERO_MA:
        return 0;
    case ET_CAL_IDX_40_2_MA:
        return 1;
    case ET_CAL_IDX_90_4_MA:
        return 2;
    case ET_CAL_IDX_1_5_MA:
        return 3;
    default:
        return -1;
    }
}

static void et_init_packet_headers(void)
{
    s_et.pkt_ping[0] = ET_STREAM_PKT_ANALOG;
    s_et.pkt_ping[1] = s_et.pkt_seq++;
    s_et.pkt_ping[2] = (uint8_t)(ET_STREAM_PAYLOAD_BYTES & 0xFFU);
    s_et.pkt_ping[3] = (uint8_t)((ET_STREAM_PAYLOAD_BYTES >> 8U) & 0xFFU);
    s_et.pkt_ping[4] = 0U;
    s_et.pkt_ping[5] = 0U;

    s_et.pkt_pong[0] = ET_STREAM_PKT_ANALOG;
    s_et.pkt_pong[1] = s_et.pkt_seq++;
    s_et.pkt_pong[2] = (uint8_t)(ET_STREAM_PAYLOAD_BYTES & 0xFFU);
    s_et.pkt_pong[3] = (uint8_t)((ET_STREAM_PAYLOAD_BYTES >> 8U) & 0xFFU);
    s_et.pkt_pong[4] = 0U;
    s_et.pkt_pong[5] = 0U;
}

/* ---- Public API ---- */

void et_hdr_init(void)
{
    /* Enable and configure uDMA controller */
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_UDMA);
    while (!MAP_SysCtlPeripheralReady(SYSCTL_PERIPH_UDMA)) {
    }
    MAP_uDMAEnable();
    MAP_uDMAControlBaseSet(s_et.udma_control_table);

    /* Enable and configure SSI1 for ADS127L01 (SPI Mode 1, 8-bit, 15 MHz) */
    MAP_SysCtlPeripheralEnable(BOARD_ET_SSI_PERIPH);
    while (!MAP_SysCtlPeripheralReady(BOARD_ET_SSI_PERIPH)) {
    }

    MAP_GPIOPinConfigure(GPIO_PB5_SSI1CLK);
    MAP_GPIOPinConfigure(GPIO_PE4_SSI1XDAT0);
    MAP_GPIOPinConfigure(GPIO_PE5_SSI1XDAT1);
    MAP_GPIOPinTypeSSI(BOARD_ET_SCLK_PORT_BASE, BOARD_ET_SCLK_PIN);
    MAP_GPIOPinTypeSSI(BOARD_ET_DATA_PORT_BASE, BOARD_ET_DIN_PIN | BOARD_ET_DOUT_PIN);

    /* Configure PB4 (ADS127L01 nDRDY) as falling-edge GPIO DMA trigger input */
    MAP_GPIOPinTypeGPIOInput(BOARD_ET_DRDY_PORT_BASE, BOARD_ET_DRDY_PIN);
    MAP_GPIOIntTypeSet(BOARD_ET_DRDY_PORT_BASE, BOARD_ET_DRDY_PIN, GPIO_FALLING_EDGE);
    MAP_GPIOPadConfigSet(BOARD_ET_DRDY_PORT_BASE, BOARD_ET_DRDY_PIN, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD_WPD);

    MAP_SSIDisable(BOARD_ET_SSI_BASE);
    MAP_SSIConfigSetExpClk(
        BOARD_ET_SSI_BASE, board_get_sys_clock_hz(), SSI_FRF_MOTO_MODE_1, SSI_MODE_MASTER, ET_SSI_BITRATE_HZ, 8U);

    /*
     * GPIOB (uDMA Ch5 TX trigger) has highest priority and never touches USB.
     * SSI1 and USB0 share the same priority so SSI1 cannot preempt USB0
     * mid-transfer when pushing completed 1030-byte frames to EP7 IN.
     */
    NVIC_SetPriority(GPIOB_IRQn, ET_GPIOB_IRQ_PRIO);
    NVIC_SetPriority(SSI1_IRQn, ET_STREAM_IRQ_PRIO);
    NVIC_SetPriority(USB0_IRQn, ET_STREAM_IRQ_PRIO);

    et_adc_reset_pulse();
    s_et.cfg_sample_freq = 48000U;
    s_et.hw_initialized = true;
}

void et_hdr_boot_calibrate(void)
{
    if (!s_et.hw_initialized) {
        et_hdr_init();
    }

    if (board_get_hw_version() != BOARD_HW_VERSION_LP_XDS110ET) {
        s_et.cal_valid = false;
        return;
    }

    /* Hold target in reset and isolate LP_VDD while measuring onboard cal loads */
    board_srst_set(true);
    et_switch_calibrate_isolate(true);
    board_delay_ms(ET_BOOT_ISOLATE_MS);

    et_switch_shunt_enable(true);
    et_adc_reset_pulse();

    s_et.cal_cache[0] = et_measure_cal_point(ET_CAL_IDX_ZERO_MA);
    s_et.cal_cache[1] = et_measure_cal_point(ET_CAL_IDX_40_2_MA);
    s_et.cal_cache[2] = et_measure_cal_point(ET_CAL_IDX_90_4_MA);
    s_et.cal_cache[3] = et_measure_cal_point(ET_CAL_IDX_1_5_MA);
    s_et.cal_valid = true;

    /* Bypass shunt, reconnect target LP_VDD, settle, then release target reset */
    et_switch_shunt_enable(false);
    et_switch_calibrate_isolate(false);
    board_delay_ms(ET_BOOT_ISOLATE_MS);
    board_srst_set(false);
}

int32_t et_hdr_get_hw_info(uint32_t *out_hw_type, uint8_t *out_caps)
{
    *out_hw_type = 0U;
    *out_caps = 0U;

    if (board_get_hw_version() != BOARD_HW_VERSION_LP_XDS110ET) {
        return ET_ERR_UNSUPPORTED_HW;
    }

    uint32_t cfg = board_read_config_word();
    if (cfg == BOARD_CONFIG_DRAG_DROP) {
        return ET_ERR_INVALID_CONFIG;
    }

    *out_hw_type = ET_HW_TYPE_HDR;
    *out_caps = (cfg == BOARD_CONFIG_CMSIS_DAP_V2) ? ET_HW_CAPS_ALT : ET_HW_CAPS_STD;
    return ET_ERR_OK;
}

int32_t et_hdr_set_range(uint8_t range)
{
    s_et.cfg_range = range;
    return ET_ERR_OK;
}

int32_t et_hdr_set_dig(uint8_t dig_type)
{
    s_et.cfg_dig_type = dig_type;
    return ET_ERR_OK;
}

int32_t et_hdr_setup(uint8_t mode, uint8_t dig_en, uint32_t sample_freq, uint8_t trigger_mode)
{
    (void)dig_en;
    (void)trigger_mode;
    (void)s_et.cfg_range;
    (void)s_et.cfg_dig_type;

    if ((board_get_hw_version() != BOARD_HW_VERSION_LP_XDS110ET) || (mode != ET_MODE_HDR)) {
        return ET_ERR_UNSUPPORTED_MODE;
    }

    if (!s_et.hw_initialized) {
        et_hdr_init();
    }

    et_dma_stop_all();
    et_adc_reset_pulse();
    et_switch_shunt_enable(true);

    s_et.cfg_sample_freq = sample_freq;
    s_et.pkt_seq = 0U;
    s_et.session_setup = true;

    usb_data_tx_flush();
    return ET_ERR_OK;
}

int32_t et_hdr_get_calibration(uint16_t cal_idx, int32_t *out_sum, uint32_t *out_count)
{
    *out_sum = 0;
    *out_count = 0U;

    if (!s_et.session_setup) {
        return ET_ERR_NOT_INITIALIZED;
    }

    int32_t slot = et_cal_idx_to_slot(cal_idx);
    if ((slot < 0) || !s_et.cal_valid) {
        return ET_ERR_OK;
    }

    *out_sum = s_et.cal_cache[slot];
    *out_count = ET_CAL_SAMPLE_CNT;
    return ET_ERR_OK;
}

int32_t et_hdr_start(void)
{
    if (!s_et.session_setup) {
        return ET_ERR_NOT_INITIALIZED;
    }

    /* Send 3-byte EnergyTrace sync header: [0x35, sample_freq_lo, sample_freq_hi] */
    uint8_t sync_hdr[3] = {
        ET_STREAM_SYNC_MAGIC,
        (uint8_t)(s_et.cfg_sample_freq & 0xFFU),
        (uint8_t)((s_et.cfg_sample_freq >> 8U) & 0xFFU),
    };
    usb_data_tx_write(sync_hdr, sizeof(sync_hdr));

    et_init_packet_headers();
    et_dma_start_capture(ET_DMA_STREAMING);
    return ET_ERR_OK;
}

int32_t et_hdr_stop(void)
{
    if (!s_et.session_setup) {
        return ET_ERR_NOT_INITIALIZED;
    }

    et_dma_stop_all();
    return ET_ERR_OK;
}

int32_t et_hdr_cleanup(void)
{
    if (!s_et.session_setup) {
        return ET_ERR_NOT_INITIALIZED;
    }

    et_dma_stop_all();
    et_switch_shunt_enable(false);
    s_et.session_setup = false;
    return ET_ERR_OK;
}

bool et_hdr_is_running(void)
{
    return (s_et.dma_mode == ET_DMA_STREAMING);
}
