# Kalibrierung: Verstärkung und Frequenzgang

Die Verstärkung des AD8232-Boards (nominal 1100) und seine Filter hängen von Bauteiltoleranzen ab.
Ohne Kalibrierung sind die mV-Werte Näherungen. Die Software zeigt dann einen Hinweis.

> **Während der Kalibrierung ist kein Mensch angeschlossen.** Elektrodenkabel abziehen.

## Prinzip

Die Firmware erzeugt an **GP15** (Pin 20) ein PWM-Signal mit 73 kHz. Ein RC-Tiefpass glättet es zu einem Sinus
von ±0,5 V um 1,65 V. Ein Spannungsteiler macht daraus ±0,49 mV an den Eingängen des AD8232, also eine typische
EKG-Größe. Aus der gemessenen Amplitude folgt die echte Verstärkung. Über mehrere Frequenzen ergibt sich der
Frequenzgang mit den −3-dB-Ecken.

## Schaltung (auf einem freien Breadboard-Bereich)

```
GP15 ──1k──┬──1k──┬──────100k──────┬─────────── LA (Pad oder Klinke)
           │      │                │
         100n   100n             100 Ω  (Shunt)
           │      │                │
GND ───────┴──────┴──────┐         ├─────────── RA
                         │         │
3V3 ──1k──┬──────────────│─────────┴─────────── RL
          │              │
          1k             │
          │              │
GND ──────┴──────────────┘
```

- Die beiden RC-Glieder (2 × 1 kΩ / 100 nF, Eckfrequenz ~1,6 kHz) dämpfen den PWM-Träger auf wenige µV.
  Bis 150 Hz verändern sie das Signal praktisch nicht.
- Der Mittenteiler (2 × 1 kΩ) legt RA und RL auf 1,65 V. Das ist die Gleichtaktspannung des AD8232.
- **Längswiderstand gesamt** = 100 kΩ + 2 × 1 kΩ (RC) + 0,5 kΩ (Innenwiderstand des Mittenteilers) ≈ 102,5 kΩ.
  Das ist der Wert, den der Kalibrierdialog verlangt.
  Für ±1 % Genauigkeit die Widerstände nachmessen und die Summe eintragen. Ebenso die gemessene 3V3-Spannung:
  Der PWM-Hub skaliert mit ihr.

Amplitude an den Eingängen: `U = 0,5 V · (U_3V3 / 3,3 V) · 100 Ω / (R_längs + 100 Ω)`.

## Ablauf

1. Pico verbinden (Quelle *Pico EKG*, Modus Live).
2. *Gerät → Verstärkung/Frequenzgang kalibrieren …*
3. Widerstände und 3V3 eintragen, dann **Schnell** (8 Frequenzen, ~1 min) oder **Voll** (17 Frequenzen inkl.
   0,05 Hz, ~8 min) wählen.
4. Auf **Messung starten** klicken. Das Ergebnis zeigt die Verstärkung und die Hoch- und Tiefpass-Ecke.
5. Mit **Übernehmen** wird die Kalibrierung pro Board-ID in `~/.config/ekg/config.json` gespeichert.
   Neue Aufnahmen tragen die kalibrierte Verstärkung im Kopf.

## Erwartete Werte

| Größe | typisch (SparkFun-Design) | Bedeutung |
|---|---|---|
| Verstärkung | ~1100 | mV-Skala |
| Hochpass −3 dB | ~0,5 Hz | darunter: keine ST-Beurteilung möglich |
| Tiefpass −3 dB | ~40 Hz | rundet QRS-Spitzen ab |

Liegt der Hochpass deutlich über 0,5 Hz oder der Tiefpass unter 30 Hz, lohnt ein Blick auf die Bauteile des
Klon-Boards. Ein Umbau auf 0,05 Hz ist möglich (Kondensatoren nach den Konfigurationen im AD8232-Datenblatt).
Die Grundlinie erholt sich dann aber langsamer von Bewegungsartefakten.
