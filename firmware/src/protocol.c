#include "protocol.h"

#include <string.h>

uint16_t ekg_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t ekg_cobs_encode(const uint8_t *in, size_t len, uint8_t *out) {
    size_t code_pos = 0;
    size_t out_pos = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (in[i] != 0) {
            out[out_pos++] = in[i];
            code++;
        }
        if (in[i] == 0 || code == 0xFF) {
            out[code_pos] = code;
            code = 1;
            code_pos = out_pos;
            // Nach einem vollen 254er-Block ohne weitere Eingabe kein leerer Block (kanonisch).
            if (in[i] == 0 || i + 1 < len) {
                out_pos++;
            }
        }
    }
    out[code_pos] = code;
    return out_pos;
}

size_t ekg_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < len) {
        uint8_t code = in[in_pos++];
        if (code == 0) {
            return 0;
        }
        for (uint8_t i = 1; i < code; i++) {
            if (in_pos >= len || in[in_pos] == 0 || out_pos >= out_cap) {
                return 0;
            }
            out[out_pos++] = in[in_pos++];
        }
        if (code != 0xFF && in_pos < len) {
            if (out_pos >= out_cap) {
                return 0;
            }
            out[out_pos++] = 0;
        }
    }
    return out_pos;
}

typedef struct {
    uint8_t *buf;
    size_t pos;
    size_t cap;
    bool overflow;
} writer_t;

static void put_u8(writer_t *w, uint8_t v) {
    if (w->pos + 1 > w->cap) {
        w->overflow = true;
        return;
    }
    w->buf[w->pos++] = v;
}

static void put_u16(writer_t *w, uint16_t v) {
    put_u8(w, (uint8_t)(v & 0xFF));
    put_u8(w, (uint8_t)(v >> 8));
}

static void put_u32(writer_t *w, uint32_t v) {
    put_u16(w, (uint16_t)(v & 0xFFFF));
    put_u16(w, (uint16_t)(v >> 16));
}

static void put_header(writer_t *w, uint8_t type, uint16_t seq) {
    put_u8(w, type);
    put_u8(w, EKG_PROTO_VERSION);
    put_u16(w, seq);
}

static size_t finish(writer_t *w) {
    uint16_t crc = ekg_crc16(w->buf, w->pos);
    put_u16(w, crc);
    return w->overflow ? 0 : w->pos;
}

size_t ekg_build_samples(uint8_t *buf, uint16_t seq, uint32_t first_index, uint8_t flags,
                         const uint32_t *samples, uint8_t count) {
    if (count > EKG_MAX_SAMPLES_PER_FRAME) {
        return 0;
    }
    writer_t w = {buf, 0, EKG_MAX_PAYLOAD, false};
    put_header(&w, EKG_FRAME_SAMPLES, seq);
    put_u32(&w, first_index);
    put_u8(&w, count);
    put_u8(&w, flags);
    for (uint8_t i = 0; i < count; i++) {
        put_u32(&w, samples[i]);
    }
    return finish(&w);
}

size_t ekg_build_info(uint8_t *buf, uint16_t seq, const ekg_info_t *info) {
    writer_t w = {buf, 0, EKG_MAX_PAYLOAD, false};
    put_header(&w, EKG_FRAME_INFO, seq);
    put_u32(&w, info->fw_version);
    put_u16(&w, info->sample_rate_hz);
    put_u16(&w, info->oversampling);
    put_u8(&w, info->adc_bits);
    put_u16(&w, info->vref_mv);
    put_u16(&w, info->gain_nominal);
    put_u8(&w, info->mode);
    put_u8(&w, info->streaming);
    for (int i = 0; i < 8; i++) {
        put_u8(&w, info->board_id[i]);
    }
    size_t name_len = info->name ? strlen(info->name) : 0;
    if (name_len > 64) {
        name_len = 64;
    }
    put_u8(&w, (uint8_t)name_len);
    for (size_t i = 0; i < name_len; i++) {
        put_u8(&w, (uint8_t)info->name[i]);
    }
    return finish(&w);
}

size_t ekg_build_status(uint8_t *buf, uint16_t seq, const ekg_status_t *st) {
    writer_t w = {buf, 0, EKG_MAX_PAYLOAD, false};
    put_header(&w, EKG_FRAME_STATUS, seq);
    put_u32(&w, st->uptime_ms);
    put_u32(&w, st->overruns);
    put_u32(&w, st->usb_dropped);
    put_u32(&w, st->adc_fifo_overflows);
    put_u8(&w, st->mode);
    put_u8(&w, st->streaming);
    put_u8(&w, st->cal_wave);
    put_u8(&w, st->lead_off);
    return finish(&w);
}

size_t ekg_build_ack(uint8_t *buf, uint16_t seq, uint8_t opcode, uint8_t status) {
    writer_t w = {buf, 0, EKG_MAX_PAYLOAD, false};
    put_header(&w, EKG_FRAME_ACK, seq);
    put_u8(&w, opcode);
    put_u8(&w, status);
    return finish(&w);
}

size_t ekg_frame(const uint8_t *payload, size_t len, uint8_t *out) {
    size_t n = ekg_cobs_encode(payload, len, out);
    out[n++] = 0;
    return n;
}

bool ekg_payload_valid(const uint8_t *payload, size_t len) {
    if (len < 6 || payload[1] != EKG_PROTO_VERSION) {
        return false;
    }
    uint16_t crc = (uint16_t)(payload[len - 2] | (payload[len - 1] << 8));
    return ekg_crc16(payload, len - 2) == crc;
}
