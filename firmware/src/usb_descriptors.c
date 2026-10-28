#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

// Raspberry-Pi-VID mit der PID, die das Pico SDK für RP2350-CDC-Geräte verwendet.
#define USB_VID 0x2E8A
#define USB_PID 0x0009

enum {
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_CDC_DATA,
    STR_CDC_LOG,
};

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

enum {
    ITF_CDC_DATA = 0,
    ITF_CDC_DATA_DATA,
    ITF_CDC_LOG,
    ITF_CDC_LOG_DATA,
    ITF_TOTAL,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC_DATA, STR_CDC_DATA, 0x81, 8, 0x02, 0x82, 64),
    TUD_CDC_DESCRIPTOR(ITF_CDC_LOG, STR_CDC_LOG, 0x83, 8, 0x04, 0x84, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static const char *const strings[] = {
    [STR_MANUFACTURER] = "EKG-Projekt",
    [STR_PRODUCT] = "Pico EKG",
    [STR_CDC_DATA] = "EKG Data",
    [STR_CDC_LOG] = "EKG Log",
};

static uint16_t desc_str[33];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t len;
    if (index == STR_LANGID) {
        desc_str[1] = 0x0409;
        len = 1;
    } else {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        const char *s;
        if (index == STR_SERIAL) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            s = serial;
        } else if (index < sizeof(strings) / sizeof(strings[0]) && strings[index]) {
            s = strings[index];
        } else {
            return 0;
        }
        len = strlen(s);
        if (len > 32) {
            len = 32;
        }
        for (size_t i = 0; i < len; i++) {
            desc_str[1 + i] = (uint8_t)s[i];
        }
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
