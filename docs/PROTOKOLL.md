# USB-Protokoll (Version 1)

Der Pico meldet sich als USB-Verbundgerät (VID `2E8A`, PID `0009`, Produkt „Pico EKG“) mit zwei seriellen
Schnittstellen:

| Schnittstelle | Name | Inhalt |
|---|---|---|
| CDC 0 (`/dev/ttyACM0`) | EKG Data | Binärprotokoll (unten) |
| CDC 1 (`/dev/ttyACM1`) | EKG Log | Textmeldungen der Firmware, z. B. `cat /dev/ttyACM1` |

Die Baudrate ist bedeutungslos. Ausnahme: Wird CDC 0 mit **1200 Baud** geöffnet, startet der Pico in den
USB-Bootloader (wie beim Pico SDK). Schließt der Host die Datenschnittstelle (DTR fällt), stoppt die Messung.

## Rahmen

```
Frame   = COBS(payload) 0x00
payload = type:u8  version:u8  seq:u16  body…  crc:u16
```

Alle Zahlen sind little endian. Die CRC ist CRC-16/CCITT-FALSE (Polynom 0x1021, Start 0xFFFF, Prüfwert für
`"123456789"` = `0x29B1`) über alle Bytes vor ihr. `seq` zählt je Richtung hoch. Ein Golden-Frame steht
identisch in `firmware/tests/test_firmware.c` und `host/tests/test_protocol.py`.

## Gerät → Host

### SAMPLES (0x01), alle 50 ms

| Feld | Typ | |
|---|---|---|
| first_index | u32 | Index des ersten Samples seit START (läuft nach 2³² über) |
| count | u8 | 25 bei 500 Hz, 50 bei 1000 Hz |
| flags | u8 | bit0 LO+, bit1 LO−, bit2 Daten davor verloren, bit3 ADC am Anschlag, bit4 Testsignal, bit5 Kalibriersignal aktiv |
| samples | u32 × count | Summe von 32000/fs ADC-Werten (12 Bit) |

Umrechnung: `U_adc = raw · 3,3 V / 4096 / oversampling`, `mV = (U_adc − 1,65 V) / Verstärkung · 1000`.
Fehlende Pakete erkennt der Host an Sprüngen in `first_index`. Das Flag bit2 sagt zusätzlich, dass die Ursache
im Gerät lag (Überlauf oder voller USB-Puffer).

### INFO (0x02), Antwort auf GET_INFO

`fw_version:u32 (maj<<16|min<<8|patch)`, `sample_rate_hz:u16`, `oversampling:u16`, `adc_bits:u8`, `vref_mv:u16`,
`gain_nominal:u16`, `mode:u8`, `streaming:u8`, `board_id:u8[8]`, `name_len:u8`, `name:utf8`

### STATUS (0x03), jede Sekunde

`uptime_ms:u32`, `overruns:u32`, `usb_dropped:u32`, `adc_fifo_overflows:u32`, `mode:u8`, `streaming:u8`,
`cal_wave:u8`, `lead_off:u8`

### ACK (0x11), auf jeden Befehl

`opcode:u8`, `status:u8` (0 OK, 1 ungültiges Argument, 2 unbekannt, 3 falscher Zustand)

## Host → Gerät: COMMAND (0x10)

`opcode:u8`, danach die Argumente:

| Opcode | Name | Argumente |
|---|---|---|
| 0x01 | START | – |
| 0x02 | STOP | – |
| 0x03 | SET_RATE | u16 Hz (500 oder 1000), nur im Stopp |
| 0x04 | SET_MODE | u8 (0 Live, 1 Testsignal) |
| 0x05 | GET_INFO | – |
| 0x06 | SET_CAL | u8 (0 aus, 1 Rechteck, 2 Sinus), u32 Frequenz in mHz (10 … 250 000) |
| 0x07 | BOOTSEL | – (Neustart in den USB-Bootloader) |
| 0x08 | SET_TEST | u16 HF in 0,1/min, u8 Optionen (bit0: 50-Hz-Brumm) |

## Abtastung

ADC0 läuft frei mit 32 kHz (ADC-Takt 48 MHz aus dem Quarz, `clkdiv = 1499`). Zwei DMA-Kanäle füllen
abwechselnd Blöcke mit 1600 Werten (50 ms). Je Block entstehen 25 bzw. 50 Ausgabewerte als Summe von 64 bzw.
32 ADC-Werten. Die Firmware filtert darüber hinaus nichts, alles Weitere macht der Host.
