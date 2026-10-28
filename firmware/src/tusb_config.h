#pragma once

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64

// Zwei serielle Schnittstellen: 0 = Binärdaten, 1 = Textlog.
#define CFG_TUD_CDC 2
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#define CFG_TUD_CDC_RX_BUFSIZE 256
// ~1,6 s Puffer bei 2,5 kB/s, falls der Host kurz nicht abholt.
#define CFG_TUD_CDC_TX_BUFSIZE 4096
#define CFG_TUD_CDC_EP_BUFSIZE 64
