// Binärprotokoll Gerät <-> Host. Spezifikation: docs/PROTOKOLL.md
//
// Jeder Frame: payload = [type u8][version u8][seq u16][...][crc16 u16], little endian,
// COBS-kodiert und mit 0x00 abgeschlossen. Die CRC (CRC-16/CCITT-FALSE) deckt alle
// Bytes vor ihr ab.
//
// Diese Datei ist hardwareunabhängig und wird auch auf dem Host getestet.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EKG_PROTO_VERSION 1u

#define EKG_FW_VERSION_MAJOR 0u
#define EKG_FW_VERSION_MINOR 7u
#define EKG_FW_VERSION_PATCH 4u

enum ekg_frame_type {
    EKG_FRAME_SAMPLES = 0x01,
    EKG_FRAME_INFO = 0x02,
    EKG_FRAME_STATUS = 0x03,
    EKG_FRAME_COMMAND = 0x10,
    EKG_FRAME_ACK = 0x11,
};

enum ekg_flag {
    EKG_FLAG_LO_PLUS = 1u << 0,   // LO+ aktiv: Elektrode LA ohne Kontakt
    EKG_FLAG_LO_MINUS = 1u << 1,  // LO- aktiv: Elektrode RA ohne Kontakt
    EKG_FLAG_OVERRUN = 1u << 2,   // Daten vor diesem Paket gingen verloren
    EKG_FLAG_CLIP = 1u << 3,      // ADC am Anschlag (Übersteuerung)
    EKG_FLAG_TEST = 1u << 4,      // synthetisches Testsignal, kein Mensch
    EKG_FLAG_CAL = 1u << 5,       // Kalibriersignal an GP15 aktiv
};

enum ekg_opcode {
    EKG_CMD_START = 0x01,
    EKG_CMD_STOP = 0x02,
    EKG_CMD_SET_RATE = 0x03,  // u16 Hz
    EKG_CMD_SET_MODE = 0x04,  // u8 ekg_mode
    EKG_CMD_GET_INFO = 0x05,
    EKG_CMD_SET_CAL = 0x06,   // u8 ekg_cal_wave, u32 Frequenz in mHz
    EKG_CMD_BOOTSEL = 0x07,
    EKG_CMD_SET_TEST = 0x08,  // u16 Herzfrequenz in 0.1 bpm, u8 Optionen (bit0: 50-Hz-Brumm)
};

enum ekg_mode {
    EKG_MODE_LIVE = 0,
    EKG_MODE_TEST = 1,
};

enum ekg_cal_wave {
    EKG_CAL_OFF = 0,
    EKG_CAL_SQUARE = 1,
    EKG_CAL_SINE = 2,
};

enum ekg_ack_status {
    EKG_ACK_OK = 0,
    EKG_ACK_BAD_ARG = 1,
    EKG_ACK_UNKNOWN = 2,
    EKG_ACK_BAD_STATE = 3,
};

#define EKG_MAX_SAMPLES_PER_FRAME 64u
#define EKG_MAX_PAYLOAD 320u
// COBS: höchstens ein Overhead-Byte je 254 Bytes, plus Startbyte und Trenner.
#define EKG_MAX_FRAME (EKG_MAX_PAYLOAD + EKG_MAX_PAYLOAD / 254u + 2u)

typedef struct {
    uint32_t fw_version;  // major << 16 | minor << 8 | patch
    uint16_t sample_rate_hz;
    uint16_t oversampling;
    uint8_t adc_bits;
    uint16_t vref_mv;
    uint16_t gain_nominal;
    uint8_t mode;
    uint8_t streaming;
    uint8_t board_id[8];
    const char *name;
} ekg_info_t;

typedef struct {
    uint32_t uptime_ms;
    uint32_t overruns;
    uint32_t usb_dropped;
    uint32_t adc_fifo_overflows;
    uint8_t mode;
    uint8_t streaming;
    uint8_t cal_wave;
    uint8_t lead_off;
} ekg_status_t;

uint16_t ekg_crc16(const uint8_t *data, size_t len);

// Liefert die kodierte Länge ohne abschließendes 0x00. out muss len + len/254 + 1 Bytes fassen.
size_t ekg_cobs_encode(const uint8_t *in, size_t len, uint8_t *out);
// Liefert die dekodierte Länge, 0 bei ungültiger Eingabe oder zu kleinem Puffer.
size_t ekg_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

// Payload-Builder: schreiben inklusive CRC und liefern die Länge (0 bei Fehler).
size_t ekg_build_samples(uint8_t *buf, uint16_t seq, uint32_t first_index, uint8_t flags,
                         const uint32_t *samples, uint8_t count);
size_t ekg_build_info(uint8_t *buf, uint16_t seq, const ekg_info_t *info);
size_t ekg_build_status(uint8_t *buf, uint16_t seq, const ekg_status_t *st);
size_t ekg_build_ack(uint8_t *buf, uint16_t seq, uint8_t opcode, uint8_t status);

// COBS-kodiert payload und hängt 0x00 an. Liefert die Gesamtlänge.
size_t ekg_frame(const uint8_t *payload, size_t len, uint8_t *out);

// Prüft Mindestlänge, Version und CRC eines dekodierten Payloads.
bool ekg_payload_valid(const uint8_t *payload, size_t len);
