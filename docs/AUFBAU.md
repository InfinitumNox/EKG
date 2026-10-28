# Aufbau

![Steckplan](breadboard.svg)

## Sicherheit zuerst

| Regel | Warum |
|---|---|
| Elektroden nur am Körper, wenn das **Laptop-Ladekabel gezogen** ist | Der AD8232 hat keine galvanische Trennung. Über Netzteil und USB entsteht sonst ein Pfad vom Stromnetz zum Brustkorb. |
| Am Laptop hängt **nichts mit Netzanschluss** (Monitor/HDMI, Dock, Netzwerkkabel, USB-Hub mit Netzteil) | Jedes davon hebt die Trennung ebenfalls auf. |
| Flashen und Entwickeln mit Ladekabel nur **ohne Elektroden am Körper** | – |
| Kalibriersignal (GP15) nur **ohne Menschen** am Gerät | – |
| Desktop-PC: nur mit **USB-Isolator** (ADuM3160/4160) | Ein Desktop-PC ist immer netzbetrieben. |

Die Software warnt, wenn sie ein angeschlossenes Netzteil erkennt (`/sys/class/power_supply`).
Einen HDMI-Monitor oder ein Dock sieht sie aber **nicht**. Die Warnung ist eine Erinnerung, kein Schutz.

## Steckplan

Das Pico-Board wird so gesteckt, dass der **USB-Stecker zur Reihe 1** zeigt. Die Pins sitzen dann in
**Spalte c** (Pin 1–20) und **Spalte h** (Pin 40–21) und überbrücken die Mittelrinne.
Die Stiftleiste des AD8232 kommt in **Spalte f, Reihen 26–31**. Die Platine liegt dabei über den Spalten a–e,
die Klinkenbuchse zeigt nach links.

Die Stromschienen werden **nicht** verwendet. Dein Breadboard hat in der Mitte geteilte Schienen
(Lücke in der Farblinie), und direkte Verbindungen sind ohnehin kürzer.

### Verbindungen

| Von (AD8232) | Loch | Nach (Pico) | Loch | Pin |
|---|---|---|---|---|
| SDN | g26 | GP22 | i12 | 29 |
| LO+ | g27 | GP20 | i15 | 26 |
| LO− | g28 | GP21 | i14 | 27 |
| OUTPUT | über R1 (i29 → i25), dann Draht j25 | GP26 / ADC0 | i10 | 31 |
| 3.3V | g30 | 3V3(OUT) | i5 | 36 |
| GND | g31 | AGND | i8 | 33 |

### Bauteile

| Bauteil | Wert | Löcher | Zweck |
|---|---|---|---|
| R1 | 1 kΩ | i29 – i25 | entkoppelt den AD8232-Ausgang vom Kondensator am ADC |
| C1 | 100 nF Keramik | j10 – j8 | Tiefpass ~1,6 kHz direkt am ADC-Pin, puffert die Abtastung |
| C2 | 100 nF Keramik | h30 – h31 | Abblockkondensator am AD8232 |
| C3 | 10 µF (Elko: + an j30) | j30 – j31 | Abblockkondensator am AD8232 |

R1 und C1 sind optional. Ohne sie einfach einen Draht von **i29 nach i10** stecken und C1 weglassen.
Den Kondensator nie ohne R1 direkt an den AD8232-Ausgang hängen, das kann den Ausgangsverstärker
zum Schwingen bringen.

### Prüfen vor dem ersten Einschalten

- [ ] Pinbelegung des Klon-Boards auf der Unterseite mit der Tabelle vergleichen (Pin 1 = GP0 neben USB links).
- [ ] Mit dem Multimeter (Durchgang): GND des AD8232 ↔ Pin 33, 3.3V ↔ Pin 36, kein Kurzschluss zwischen 3V3 und GND.
- [ ] Nichts an VBUS (Reihe 1) oder VSYS (Reihe 2).
- [ ] Elektrodenkabel durchmessen: welche Farbe geht auf RA, LA, RL? Die Lötpads RA/LA/RL am Board sind dafür
  die Referenz. Klon-Kabel haben keine einheitlichen Farben.

### Board-Details (Klon)

- Ist das Pico-Board ein Original-Nachbau mit Schaltregler, kann GPIO23 = High (PWM-Modus) den Ripple senken.
  Das ist in der Firmware abgeschaltet (`EKG_SMPS_PWM_MODE=0`). Aktiviere es nur, wenn der Schaltplan des Boards
  das bestätigt: `cmake -DCMAKE_C_FLAGS=-DEKG_SMPS_PWM_MODE=1 …`
- Die Firmware schaltet den digitalen Eingangspuffer am ADC-Pin ab (RP2350-Erratum E9 bei Stepping A2).
- Die Board-LED blinkt im Sekundentakt, solange gemessen wird, und blitzt kurz, solange nicht gemessen wird.
  Bei Klonen kann die LED an einem anderen Pin hängen, dann bleibt sie dunkel. Auf die Messung hat das keinen Einfluss.

## Elektroden

Klebeelektroden aus Ag/AgCl mit Gel verwenden, frisch aus der Packung. Die Haut mit Alkohol abwischen und
trocknen lassen. Bei starker Behaarung die Stelle rasieren.

Die Platzierung entspricht ungefähr **Ableitung II**. Das gibt die größte R-Zacke und eine gut sichtbare
P-Welle, was für die Rhythmusbeurteilung gut ist:

| Elektrode | Position |
|---|---|
| RA | rechts unter dem Schlüsselbein |
| LA | links am unteren Rippenbogen |
| RL | rechts am unteren Rippenbogen |

Ruhig sitzen oder liegen, die Kabel nicht unter Zug. Zeigt die Software eine **negative** R-Zacke, sind RA und LA
vertauscht. Die Erkennung funktioniert trotzdem, für vergleichbare Bilder sollte man sie aber tauschen.

## Phase 2 (später): eigenständig mit Display

- Display ST7789 (320×240, SPI) an SPI0. Pins werden dann in einer eigenen Firmware-Variante festgelegt.
- LiPo-Akku mit Laderegler (z. B. MCP73831) an VSYS und ein eigener rauscharmer LDO für den AD8232.
- SD-Karte (SPI) im selben Rohformat wie die Laptop-Aufnahmen.
- Die Signalverarbeitung wird nach C portiert und gegen die Python-Implementierung mit identischen
  Eingabedaten geprüft.
