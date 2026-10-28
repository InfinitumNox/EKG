#include "calsignal.h"

#include <math.h>

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"
#include "protocol.h"

#define CAL_WRAP 2047u
#define CAL_UPDATE_HZ 5000u
// 1,65 V Mitte, ±0,5 V Hub (bei 3,3 V und 1001:1-Teiler ±0,5 mV an den Elektroden).
#define CAL_MID ((CAL_WRAP + 1u) / 2u)
#define CAL_AMP ((float)(CAL_WRAP + 1u) * 0.5f / 3.3f)

static bool initialized;
static volatile uint8_t wave = EKG_CAL_OFF;
static volatile uint32_t phase;
static volatile uint32_t phase_inc;
static uint slice;
static uint channel;
static repeating_timer_t timer;

static bool on_timer(repeating_timer_t *t) {
    (void)t;
    phase += phase_inc;
    float level = (float)CAL_MID;
    if (wave == EKG_CAL_SQUARE) {
        level += (phase < 0x80000000u) ? CAL_AMP : -CAL_AMP;
    } else if (wave == EKG_CAL_SINE) {
        level += CAL_AMP * sinf(6.28318530718f * (float)phase * (1.0f / 4294967296.0f));
    }
    pwm_set_chan_level(slice, channel, (uint16_t)(level + 0.5f));
    return true;
}

void cal_set(uint8_t new_wave, uint32_t freq_mhz) {
    if (!initialized) {
        gpio_set_function(EKG_CAL_GPIO, GPIO_FUNC_PWM);
        slice = pwm_gpio_to_slice_num(EKG_CAL_GPIO);
        channel = pwm_gpio_to_channel(EKG_CAL_GPIO);
        pwm_set_wrap(slice, CAL_WRAP);
        pwm_set_chan_level(slice, channel, CAL_MID);
        pwm_set_enabled(slice, true);
        add_repeating_timer_us(-(int64_t)(1000000 / CAL_UPDATE_HZ), on_timer, 0, &timer);
        initialized = true;
    }
    phase_inc = (uint32_t)(((uint64_t)freq_mhz << 32) / (1000ull * CAL_UPDATE_HZ));
    phase = 0;
    wave = new_wave;
}

uint8_t cal_wave(void) {
    return wave;
}
