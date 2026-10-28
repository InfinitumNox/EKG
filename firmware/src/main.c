// Pico-2-Firmware für das AD8232-EKG.
//
// Ablauf: ADC0 läuft frei mit 32 kHz (DMA), jeder 50-ms-Block wird zu 25 (500 Hz) oder
// 50 (1000 Hz) Rohsummen dezimiert und als SAMPLES-Frame über USB-CDC 0 gesendet.
// Gefiltert wird hier bewusst nichts, die Signalverarbeitung macht der Host.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "calsignal.h"
#include "capture.h"
#include "decimate.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "protocol.h"
#include "testsignal.h"
#include "tusb.h"

#define PIN_LO_PLUS 20u
#define PIN_LO_MINUS 21u
#define PIN_SDN 22u
#define PIN_SMPS_MODE 23u  // nur beim offiziellen Pico 2: High = Schaltregler im PWM-Modus

// Auf 1 setzen, wenn das Board GPIO23 wie das Original an den Schaltregler führt.
#ifndef EKG_SMPS_PWM_MODE
#define EKG_SMPS_PWM_MODE 0
#endif

#define CDC_DATA 0
#define CDC_LOG 1

#define GAIN_NOMINAL 1100u
#define VREF_MV 3300u
#define DEFAULT_RATE_HZ 500u
#define STATUS_INTERVAL_MS 1000u
#define BOOTSEL_BAUD 1200u

static struct {
    uint16_t rate_hz;
    uint32_t factor;
    uint8_t mode;
    bool streaming;
    uint16_t tx_seq;
    uint32_t overruns;
    uint32_t usb_dropped;
    uint8_t lead_off_accum;
    bool gap_pending;
    float test_hr_bpm;
    bool test_mains;
    ekg_testsig_t test;
    uint32_t packets;
    bool bootsel_requested;
} dev;

static void device_log(const char *fmt, ...) {
    if (!tud_cdc_n_connected(CDC_LOG)) {
        return;
    }
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(buf) - 3) {
        n = (int)sizeof(buf) - 3;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (tud_cdc_n_write_available(CDC_LOG) >= (uint32_t)n) {
        tud_cdc_n_write(CDC_LOG, buf, (uint32_t)n);
        tud_cdc_n_write_flush(CDC_LOG);
    }
}

static bool send_payload(const uint8_t *payload, size_t len) {
    if (len == 0 || !tud_cdc_n_connected(CDC_DATA)) {
        return false;
    }
    uint8_t frame[EKG_MAX_FRAME];
    size_t n = ekg_frame(payload, len, frame);
    if (tud_cdc_n_write_available(CDC_DATA) < n) {
        dev.usb_dropped++;
        return false;
    }
    tud_cdc_n_write(CDC_DATA, frame, (uint32_t)n);
    tud_cdc_n_write_flush(CDC_DATA);
    return true;
}

static uint8_t read_lead_off(void) {
    return (uint8_t)((gpio_get(PIN_LO_PLUS) ? EKG_FLAG_LO_PLUS : 0) |
                     (gpio_get(PIN_LO_MINUS) ? EKG_FLAG_LO_MINUS : 0));
}

static uint32_t mv_to_raw(float mv) {
    float v = 1.65f + mv * 1e-3f * (float)GAIN_NOMINAL;
    float counts = v / ((float)VREF_MV * 1e-3f) * 4096.0f;  // 1 LSB = Vref / 4096
    if (counts < 0.0f) {
        counts = 0.0f;
    } else if (counts > 4095.0f) {
        counts = 4095.0f;
    }
    return (uint32_t)(counts * (float)dev.factor + 0.5f);
}

static void reset_test_signal(void) {
    ekg_testsig_init(&dev.test, (float)dev.rate_hz, dev.test_hr_bpm);
    dev.test.mains = dev.test_mains;
}

static void send_info(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    ekg_info_t info = {
        .fw_version = (EKG_FW_VERSION_MAJOR << 16) | (EKG_FW_VERSION_MINOR << 8) |
                      EKG_FW_VERSION_PATCH,
        .sample_rate_hz = dev.rate_hz,
        .oversampling = (uint16_t)dev.factor,
        .adc_bits = 12,
        .vref_mv = VREF_MV,
        .gain_nominal = GAIN_NOMINAL,
        .mode = dev.mode,
        .streaming = dev.streaming,
        .name = "Pico EKG AD8232",
    };
    memcpy(info.board_id, id.id, sizeof(info.board_id));
    uint8_t payload[EKG_MAX_PAYLOAD];
    send_payload(payload, ekg_build_info(payload, dev.tx_seq++, &info));
}

static void send_status(void) {
    ekg_status_t st = {
        .uptime_ms = to_ms_since_boot(get_absolute_time()),
        .overruns = dev.overruns,
        .usb_dropped = dev.usb_dropped,
        .adc_fifo_overflows = capture_fifo_overflows(),
        .mode = dev.mode,
        .streaming = dev.streaming,
        .cal_wave = cal_wave(),
        .lead_off = read_lead_off(),
    };
    uint8_t payload[EKG_MAX_PAYLOAD];
    send_payload(payload, ekg_build_status(payload, dev.tx_seq++, &st));
}

static void send_ack(uint8_t opcode, uint8_t status) {
    uint8_t payload[EKG_MAX_PAYLOAD];
    send_payload(payload, ekg_build_ack(payload, dev.tx_seq++, opcode, status));
}

static void start_streaming(void) {
    if (dev.mode == EKG_MODE_TEST) {
        reset_test_signal();
    }
    dev.gap_pending = false;
    dev.lead_off_accum = read_lead_off();
    capture_start();
    dev.streaming = true;
    device_log("start: %u Hz, Modus %u", dev.rate_hz, dev.mode);
}

static void stop_streaming(void) {
    if (!dev.streaming) {
        return;
    }
    capture_stop();
    dev.streaming = false;
    device_log("stop");
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)get_u16(p) | ((uint32_t)get_u16(p + 2) << 16);
}

// p zeigt auf das Opcode-Byte, n = Anzahl Bytes ab dort (ohne CRC).
static void handle_command(const uint8_t *p, size_t n) {
    if (n < 1) {
        return;
    }
    uint8_t op = p[0];
    const uint8_t *arg = p + 1;
    size_t argn = n - 1;
    uint8_t status = EKG_ACK_OK;

    switch (op) {
        case EKG_CMD_START:
            if (!dev.streaming) {
                start_streaming();
            }
            break;
        case EKG_CMD_STOP:
            stop_streaming();
            break;
        case EKG_CMD_SET_RATE:
            if (argn < 2) {
                status = EKG_ACK_BAD_ARG;
            } else if (dev.streaming) {
                status = EKG_ACK_BAD_STATE;
            } else {
                uint16_t rate = get_u16(arg);
                if (rate != 500 && rate != 1000) {
                    status = EKG_ACK_BAD_ARG;
                } else {
                    dev.rate_hz = rate;
                    dev.factor = EKG_ADC_RATE_HZ / rate;
                }
            }
            break;
        case EKG_CMD_SET_MODE:
            if (argn < 1 || arg[0] > EKG_MODE_TEST) {
                status = EKG_ACK_BAD_ARG;
            } else {
                dev.mode = arg[0];
                if (dev.mode == EKG_MODE_TEST) {
                    reset_test_signal();
                }
            }
            break;
        case EKG_CMD_GET_INFO:
            send_info();
            break;
        case EKG_CMD_SET_CAL:
            if (argn < 5 || arg[0] > EKG_CAL_SINE) {
                status = EKG_ACK_BAD_ARG;
            } else {
                uint32_t mhz = get_u32(arg + 1);
                if (arg[0] != EKG_CAL_OFF && (mhz < 10 || mhz > 250000)) {
                    status = EKG_ACK_BAD_ARG;
                } else {
                    cal_set(arg[0], mhz);
                }
            }
            break;
        case EKG_CMD_BOOTSEL:
            dev.bootsel_requested = true;
            break;
        case EKG_CMD_SET_TEST:
            if (argn < 3) {
                status = EKG_ACK_BAD_ARG;
            } else {
                float hr = (float)get_u16(arg) / 10.0f;
                if (hr < 20.0f || hr > 250.0f) {
                    status = EKG_ACK_BAD_ARG;
                } else {
                    dev.test_hr_bpm = hr;
                    dev.test_mains = (arg[2] & 1u) != 0;
                    dev.test.hr_bpm = hr;
                    dev.test.mains = dev.test_mains;
                }
            }
            break;
        default:
            status = EKG_ACK_UNKNOWN;
            break;
    }
    send_ack(op, status);
}

static void poll_commands(void) {
    static uint8_t rx[EKG_MAX_FRAME];
    static size_t rx_len;
    static bool rx_overflow;

    while (tud_cdc_n_available(CDC_DATA)) {
        uint8_t chunk[64];
        uint32_t got = tud_cdc_n_read(CDC_DATA, chunk, sizeof(chunk));
        for (uint32_t i = 0; i < got; i++) {
            uint8_t b = chunk[i];
            if (b != 0) {
                if (rx_len < sizeof(rx)) {
                    rx[rx_len++] = b;
                } else {
                    rx_overflow = true;
                }
                continue;
            }
            uint8_t payload[EKG_MAX_PAYLOAD];
            size_t n = rx_overflow ? 0 : ekg_cobs_decode(rx, rx_len, payload, sizeof(payload));
            rx_len = 0;
            rx_overflow = false;
            if (n >= 7 && ekg_payload_valid(payload, n) && payload[0] == EKG_FRAME_COMMAND) {
                handle_command(payload + 4, n - 4 - 2);
            } else if (n > 0) {
                device_log("ungültiger Frame (%u Bytes)", (unsigned)n);
            }
        }
    }
}

static void process_block(void) {
    uint32_t block_no;
    uint32_t lost;
    const uint16_t *block = capture_next_block(&block_no, &lost);
    if (!block) {
        return;
    }
    if (lost) {
        dev.overruns += lost;
        dev.gap_pending = true;
        device_log("Überlauf: %lu Blöcke verloren", (unsigned long)lost);
    }

    uint32_t samples[EKG_MAX_SAMPLES_PER_FRAME];
    bool clip = false;
    size_t n = ekg_decimate(block, EKG_ADC_BLOCK, dev.factor, samples, &clip);

    uint8_t flags = 0;
    if (dev.mode == EKG_MODE_TEST) {
        flags |= EKG_FLAG_TEST;
        clip = false;
        for (size_t i = 0; i < n; i++) {
            samples[i] = mv_to_raw(ekg_testsig_next_mv(&dev.test));
        }
    } else {
        flags |= dev.lead_off_accum;
    }
    if (clip) {
        flags |= EKG_FLAG_CLIP;
    }
    if (dev.gap_pending) {
        flags |= EKG_FLAG_OVERRUN;
    }
    if (cal_wave() != EKG_CAL_OFF) {
        flags |= EKG_FLAG_CAL;
    }
    dev.lead_off_accum = read_lead_off();

    uint8_t payload[EKG_MAX_PAYLOAD];
    size_t len = ekg_build_samples(payload, dev.tx_seq++, block_no * (uint32_t)n, flags, samples,
                                   (uint8_t)n);
    // Ein verworfener Frame zeigt sich beim Host als Lücke im Sample-Index; das Flag im
    // nächsten Frame sagt zusätzlich, dass die Ursache im Gerät lag.
    dev.gap_pending = !send_payload(payload, len);
    dev.packets++;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;
    if (itf == CDC_DATA && !dtr) {
        stop_streaming();
    }
}

void tud_cdc_line_coding_cb(uint8_t itf, const cdc_line_coding_t *coding) {
    // Wie beim Pico SDK: 1200 Baud öffnen und schließen startet den USB-Bootloader.
    if (itf == CDC_DATA && coding->bit_rate == BOOTSEL_BAUD) {
        dev.bootsel_requested = true;
    }
}

static void update_led(void) {
#ifdef PICO_DEFAULT_LED_PIN
    bool on;
    if (dev.streaming) {
        on = (dev.packets / 10u) % 2u == 0;  // 1 Hz Blinken
    } else {
        on = (to_ms_since_boot(get_absolute_time()) % 2000u) < 100u;  // kurzes Blitzen
    }
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#endif
}

int main(void) {
    gpio_init(PIN_LO_PLUS);
    gpio_set_dir(PIN_LO_PLUS, GPIO_IN);
    gpio_disable_pulls(PIN_LO_PLUS);
    gpio_init(PIN_LO_MINUS);
    gpio_set_dir(PIN_LO_MINUS, GPIO_IN);
    gpio_disable_pulls(PIN_LO_MINUS);
    gpio_init(PIN_SDN);
    gpio_set_dir(PIN_SDN, GPIO_OUT);
    gpio_put(PIN_SDN, 1);  // AD8232 aktiv
#if EKG_SMPS_PWM_MODE
    gpio_init(PIN_SMPS_MODE);
    gpio_set_dir(PIN_SMPS_MODE, GPIO_OUT);
    gpio_put(PIN_SMPS_MODE, 1);
#endif
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    dev.rate_hz = DEFAULT_RATE_HZ;
    dev.factor = EKG_ADC_RATE_HZ / DEFAULT_RATE_HZ;
    dev.mode = EKG_MODE_LIVE;
    dev.test_hr_bpm = 70.0f;

    bool watchdog_reboot = watchdog_caused_reboot();
    capture_init();
    tusb_init();
    watchdog_enable(2000, true);

    uint32_t last_status = 0;
    bool logged_boot = false;
    while (true) {
        watchdog_update();
        tud_task();
        poll_commands();
        if (dev.streaming) {
            dev.lead_off_accum |= read_lead_off();
            process_block();
        }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_status >= STATUS_INTERVAL_MS) {
            last_status = now;
            if (tud_cdc_n_connected(CDC_DATA)) {
                send_status();
            }
            if (!logged_boot && tud_cdc_n_connected(CDC_LOG)) {
                device_log("Pico EKG %u.%u.%u%s", EKG_FW_VERSION_MAJOR, EKG_FW_VERSION_MINOR,
                           EKG_FW_VERSION_PATCH, watchdog_reboot ? " (Neustart durch Watchdog)" : "");
                logged_boot = true;
            }
        }
        update_led();
        if (dev.bootsel_requested) {
            stop_streaming();
            uint32_t until = now + 100;
            while (to_ms_since_boot(get_absolute_time()) < until) {
                tud_task();  // ACK noch rausschicken
            }
            reset_usb_boot(0, 0);
        }
    }
}
