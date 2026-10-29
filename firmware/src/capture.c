#include "capture.h"

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "pico/time.h"

#define VSYS_GPIO 29u  // Pico 2: VSYS über 200 k / 100 k

static uint16_t buffers[2][EKG_ADC_BLOCK] __attribute__((aligned(4)));
static int dma_ch[2];
static volatile uint32_t blocks_done;
static uint32_t blocks_taken;
static uint32_t fifo_overflows;

// Die Kanäle wechseln sich strikt ab (0, 1, 0, ...), Block b liegt also in buffers[b % 2].
static void dma_irq_handler(void) {
    for (int i = 0; i < 2; i++) {
        if (dma_channel_get_irq0_status((uint)dma_ch[i])) {
            dma_channel_acknowledge_irq0((uint)dma_ch[i]);
            dma_channel_set_write_addr((uint)dma_ch[i], buffers[i], false);
            blocks_done++;
        }
    }
}

static void configure_channels(void) {
    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config((uint)dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, DREQ_ADC);
        channel_config_set_chain_to(&c, (uint)dma_ch[1 - i]);
        dma_channel_configure((uint)dma_ch[i], &c, buffers[i], &adc_hw->fifo,
                              dma_encode_transfer_count(EKG_ADC_BLOCK), false);
    }
}

void capture_init(void) {
    adc_init();
    adc_gpio_init(EKG_ADC_GPIO);  // schaltet auch den digitalen Eingangspuffer ab (RP2350-E9)
    adc_gpio_init(VSYS_GPIO);
    adc_select_input(EKG_ADC_GPIO - 26u);
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv((float)clock_get_hz(clk_adc) / (float)EKG_ADC_RATE_HZ - 1.0f);

    dma_ch[0] = dma_claim_unused_channel(true);
    dma_ch[1] = dma_claim_unused_channel(true);
    configure_channels();
    dma_channel_set_irq0_enabled((uint)dma_ch[0], true);
    dma_channel_set_irq0_enabled((uint)dma_ch[1], true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

void capture_start(void) {
    capture_stop();
    configure_channels();
    blocks_done = 0;
    blocks_taken = 0;
    adc_fifo_drain();
    dma_channel_start((uint)dma_ch[0]);
    adc_run(true);
}

void capture_stop(void) {
    adc_run(false);
    // RP2350-E5: vor dem Abbruch die Enable-Bits beider verketteter Kanäle löschen,
    // sonst startet der Partnerkanal erneut. Abbruch kann ein IRQ auslösen (RP2040-E13).
    for (int i = 0; i < 2; i++) {
        dma_channel_set_irq0_enabled((uint)dma_ch[i], false);
        hw_clear_bits(&dma_hw->ch[dma_ch[i]].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    }
    for (int i = 0; i < 2; i++) {
        dma_channel_abort((uint)dma_ch[i]);
        dma_channel_acknowledge_irq0((uint)dma_ch[i]);
        dma_channel_set_irq0_enabled((uint)dma_ch[i], true);
    }
    adc_fifo_drain();
}

const uint16_t *capture_next_block(uint32_t *block_no, uint32_t *lost) {
    if (adc_hw->fcs & ADC_FCS_OVER_BITS) {
        fifo_overflows++;
        hw_set_bits(&adc_hw->fcs, ADC_FCS_OVER_BITS);  // write-1-to-clear
    }
    uint32_t done = blocks_done;
    if (done == blocks_taken) {
        return 0;
    }
    *lost = 0;
    // Liegen zwei oder mehr Blöcke an, wird der älteste bereits überschrieben.
    if (done - blocks_taken > 1) {
        *lost = done - 1 - blocks_taken;
        blocks_taken = done - 1;
    }
    *block_no = blocks_taken;
    return buffers[blocks_taken++ % 2];
}

uint32_t capture_fifo_overflows(void) {
    return fifo_overflows;
}

uint32_t capture_measure_vsys_mv(void) {
    capture_stop();
    adc_select_input(VSYS_GPIO - 26u);
    busy_wait_us(20);
    (void)adc_read();  // erste Wandlung nach dem Umschalten verwerfen
    (void)adc_read();
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += adc_read();
    }
    adc_select_input(EKG_ADC_GPIO - 26u);
    capture_start();
    return sum * 3u * 3300u / (16u * 4096u);
}
