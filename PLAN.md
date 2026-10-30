# EKG-Projekt: Plan

Ein-Kanal-EKG mit **AD8232** und **Raspberry Pi Pico 2** (RP2350, Board-Klon mit USB-C).
Phase 1 überträgt per USB an eine Desktop-Anwendung unter Fedora. Phase 2 läuft eigenständig mit Display.

> **Kein Medizinprodukt.** Das Gerät ist ein Bastel- und Lernprojekt. Es liefert keine Diagnosen.
> Bei Beschwerden zum Arzt.

---

## 0. Was „beste Software“ hier heißt

Gemeint ist nicht maximale Feature-Fülle, sondern ein Messgerät, dem man trauen kann:

1. **Rohdaten werden verlustfrei gespeichert.** Jede Filterung findet nachgelagert statt und lässt sich reproduzieren.
2. **Die Abtastung ist zeitlich exakt.** Der Takt kommt vom Quarz, Lücken werden per Sequenznummer erkannt.
3. **Die Anzeige ist kalibriert.** Werte werden in mV und ms angezeigt, nicht in ADC-Counts, und die Kalibrierung ist gemessen statt angenommen.
4. **Die Algorithmen sind validiert.** QRS-Erkennung und Frequenzgang werden gegen Referenzen gemessen, nicht geschätzt.
5. **Grenzen werden ehrlich angezeigt.** Die UI sagt, was das Gerät *nicht* kann, z. B. keine ST-Analyse bei 0,5-Hz-Hochpass.

---

## 1. Sicherheit (verbindlich)

| Regel | Grund |
|---|---|
| **Elektroden nur am Körper, wenn das Ladekabel des Laptops gezogen ist** | Sonst führt ein Pfad vom Netz über das Netzteil und USB zum Brustkorb |
| Am Laptop hängt währenddessen **nichts anderes**: kein Monitor (HDMI), kein Dock, kein Ethernet, kein USB-Hub mit eigenem Netzteil | Jedes netzbetriebene Gerät hebt die Trennung auf |
| Flashen und Entwickeln mit Ladekabel ist ok, dann aber **ohne Elektroden am Körper** | — |
| Nur Klebeelektroden (Ag/AgCl, Gel), keine selbstgebauten Metallelektroden | Hautkontakt, Signalqualität |

Für Phase 2 (Display, Akku) gilt dasselbe: USB nur einstecken, wenn kein Mensch angeschlossen ist. Alternativ kommt ein USB-Isolator (ADuM3160/4160) dazwischen.

---

## 2. Hardware-Aufbau

### 2.1 Verdrahtung

Den Steckplan mit Lochpositionen gibt es in [docs/AUFBAU.md](docs/AUFBAU.md) und als Grafik in [docs/breadboard.svg](docs/breadboard.svg).

Die Pin-Nummern beziehen sich auf das **offizielle Pico-2-Layout**. Das Board ist ein Klon, deshalb vor dem Verdrahten die Beschriftung auf der Unterseite prüfen.

| AD8232 | Pico 2 | Pin | Hinweis |
|---|---|---|---|
| GND | AGND | 33 | Analoge Masse, Bezug des ADC |
| 3.3V | 3V3(OUT) | 36 | Ratiometrisch: AD8232 und ADC-Referenz hängen an derselben 3,3-V-Schiene |
| OUTPUT | GP26 / ADC0 | 31 | Direkt, ohne RC-Glied; Anti-Aliasing übernehmen der 40-Hz-Tiefpass des Boards und die 64-fach-Mittelung |
| LO+ | GP20 | 26 | Lead-off-Erkennung, digitaler Ausgang des AD8232 |
| LO− | GP21 | 27 | dito |
| SDN | GP22 | 29 | Wird von der Firmware auf High gelegt; Low schaltet den AD8232 ab (später fürs Akku-Sparen) |

Keine weiteren Bauteile: Beide Module haben ihre Abblockkondensatoren auf der Platine.

Später (Phase 2) kommt **GP28 / ADC2** an einen Spannungsteiler für die Akkuspannung.

### 2.2 Board-spezifische Prüfungen (Klon)

- [ ] Pinout auf der Unterseite gegen die Tabelle prüfen.
- [ ] Spannungsregler identifizieren. Ist es ein Schaltregler wie beim Original, reduziert `GPIO23 = High` den Ripple. Das gilt **nur, wenn das Board es so dokumentiert**, sonst GPIO23 nicht anfassen.
- [ ] Stepping des RP2350 von der Chip-Aufschrift ablesen (A2 oder A4). Beim A2 gilt Erratum E9 (Leckstrom an GPIO-Eingängen). Die Firmware deaktiviert deshalb in jedem Fall den digitalen Eingangspuffer am ADC-Pin (`adc_gpio_init`). LO+ und LO− werden aktiv getrieben und sind nicht betroffen.

### 2.3 Elektrodenkabel und Platzierung

- [ ] Mit dem Multimeter (Durchgang) prüfen, welche Kabelfarbe auf RA, LA und RL geht. Die Farben sind bei Klon-Kabeln **nicht** einheitlich.
- Platzierung, angenähert an **Ableitung II** (größte R-Zacke, gut sichtbare P-Welle, gut für Rhythmus):
  - **RA:** rechts unter dem Schlüsselbein
  - **LA:** links am unteren Rippenbogen
  - **RL:** rechts am unteren Rippenbogen
- Haut vorbereiten: mit Alkohol abwischen, trocknen lassen, Elektroden frisch verwenden. Beim Messen ruhig sitzen und Kabel nicht über das Laptop-Netzteil führen (das ohnehin abgesteckt ist).

### 2.4 Bekannte Grenzen der Hardware

- **3 Elektroden heißen 1 Kanal.** Der AD8232 misst genau eine Differenz (LA − RA). RL ist keine Messelektrode, sondern die Right-Leg-Drive-Referenz, die das Gleichtaktsignal (z. B. 50 Hz) ausregelt.
  Die Ableitungen I, II und III bekommt man nur nacheinander, indem man die Elektroden umsetzt.
  Für gleichzeitige Mehrkanal-Messung braucht es ein anderes Frontend: ADS1292R (2 Kanäle, damit I und II gleichzeitig, III = II − I) oder ADS1298 (8 Kanäle, 12-Kanal-EKG).
  Zwei AD8232 parallel funktionieren nicht ohne Weiteres, weil die beiden RLD-Treiber gegeneinander arbeiten.
- Die AD8232-Breakouts haben typischerweise **0,5–40 Hz** und eine Verstärkung von **~1100**. Das sind Werte vom SparkFun-Design, der Klon wird in M4 nachgemessen.
- Damit ist die ST-Strecke nicht zuverlässig beurteilbar, und der 40-Hz-Tiefpass rundet den QRS-Komplex ab.
- Einen Hardware-Umbau (Hochpass auf 0,05 Hz durch Austausch von Bauteilen) entscheiden wir **nach** der Frequenzgang-Messung in M4.

---

## 3. Systemarchitektur

```
 Elektroden ─► AD8232 ─► ADC0 (12 Bit, 32 kHz, freilaufend)
                              │ DMA (Ping-Pong)
                              ▼
                     Dezimation 64:1 ─► 500 Hz, 18-Bit-Rohwerte
                              │  + Lead-off-Flags, Sequenznr., Sample-Index
                              ▼
                     Framing (COBS + CRC16) ─► USB CDC
═══════════════════════════════════════════════════════════  Laptop (Fedora)
                     Reader-Thread: Deframing, CRC, Lückenerkennung
                              │
              ┌───────────────┼───────────────────┐
              ▼               ▼                   ▼
        Rohdaten-Recorder  Echtzeit-DSP        Ringpuffer
        (crash-sicher)     Filter, QRS, HF,    für die Anzeige
              │            Signalqualität
              ▼               ▼
        Export EDF+/WFDB   GUI: Live-Streifen, HF, Trends,
        PDF-Bericht        Review, Messschieber, HRV
```

Leitprinzip: **Die Firmware liefert ehrliche Rohdaten, der Host interpretiert sie.** Die Firmware filtert nur so weit, wie es die Dezimation zwingend erfordert (Anti-Aliasing).

---

## 4. Firmware (Pico 2)

**Stack:** C mit dem **Pico SDK 2.x** und **TinyUSB**.
Begründung: direkte Kontrolle über ADC-Takt und DMA, offizielle RP2350-Unterstützung, später einfache Display-Anbindung. Arduino-Pico verdeckt zu viel, MicroPython ist für exaktes Timing ungeeignet.

### 4.1 Erfassung

- ADC0 freilaufend: ADC-Takt 48 MHz aus dem 12-MHz-Quarz, `clkdiv = 1499` → **32 000 S/s**.
- Zwei DMA-Kanäle im Ping-Pong-Betrieb, jeweils 64×N Samples, ohne CPU-Last während der Wandlung.
- Dezimation: Summe über je 64 Samples → **500 Hz**, Wert 0…262 080 (18 Bit, Ganzzahl, keine Information geht verloren).
  - Die Boxcar-Mittelung hat Nullstellen bei Vielfachen von 500 Hz. Zusammen mit dem analogen 40-Hz-Tiefpass des AD8232 reicht das als Anti-Aliasing.
  - Konfigurierbar sind 1000 Hz (32:1), z. B. für feinere RR-Auflösung.
- Lead-off: LO+ und LO− werden bei jedem Ausgabesample gelesen und pro Paket als Flags gesendet.
- Überlauferkennung: Kommt der DMA nicht hinterher, wird ein Flag gesetzt und der Zähler erhöht. Lücken werden nie stillschweigend verschluckt.

### 4.2 Weitere Funktionen

- **Testsignal-Modus:** ein synthetisches EKG (Gauß-Summenmodell à la ECGSYN, mit HF-Variabilität und optionalem 50-Hz-Brumm), damit Host und GUI ohne Elektroden getestet werden können.
- **Kalibrier-Modus:** Sinus oder Rechteck per PWM an GP15 für die Loopback-Messung (siehe 8.1).
- Watchdog, eindeutige Board-ID und Firmware-Version in der INFO-Antwort.
- USB-Composite mit zwei CDC-Schnittstellen: **Daten** (binär) und **Log** (Text, zum Debuggen).

### 4.3 Protokoll (Gerät → Host)

COBS-kodierte Frames, getrennt durch `0x00`, gesichert mit CRC-16/CCITT-FALSE.

```
SAMPLES (alle 50 ms):
  u8  type = 0x01
  u8  proto_version
  u16 seq                  // Paketzähler, erkennt Verluste
  u32 first_sample_index   // absoluter Sample-Zähler seit Start
  u8  count = 25
  u8  flags                // bit0 LO+, bit1 LO−, bit2 DMA-Überlauf, bit3 Clipping, bit4 Testsignal
  u32 samples[count]       // Rohsummen
  u16 crc
INFO / STATUS / LOG: eigene Typen; INFO enthält fs, Oversampling, ADC-Bits,
                     nominale Verstärkung, Firmware-Version und Board-ID.
```

**Host → Gerät:** `START`, `STOP`, `SET_RATE`, `SET_MODE` (live/test/kalibrieren), `GET_INFO`.

Die Datenrate liegt bei rund 2,5 kB/s, für USB Full-Speed trivial.

---

## 5. Host-Software (Fedora)

### 5.1 Stack

**Python 3.12+, PySide6 (Qt 6), pyqtgraph, NumPy und SciPy, pyserial, pyedflib, wfdb, neurokit2.** Paketverwaltung mit `uv`.

Begründung:
- pyqtgraph stellt 500 Hz in Echtzeit problemlos dar.
- Mit wfdb (MIT-BIH, AAMI-Auswertung), neurokit2 (Delineation, HRV) und pyedflib (EDF+) gibt es das Ökosystem für die *Validierung* nur hier.
- Die Alternative Rust mit egui wäre schneller und ein einzelnes Binary, bringt aber deutlich mehr Eigenbau für die Analyse.

### 5.2 Module

```
host/src/ekg/
  device/    Serial-Reader (QThread), COBS, CRC, Sequenz/Lücken, Befehle, Testsignal-Simulator
  dsp/       kausale Echtzeitfilter (SOS mit Zustand), QRS-Detektor, HF, Signalqualität
  storage/   Rohdaten-Recorder (Chunks + JSON-Sidecar), EDF+-/WFDB-/CSV-Export
  analysis/  Offline: phasenfreie Filter, Median-Beat, Intervalle, HRV, PDF-Bericht
  gui/       Hauptfenster, Live-Ansicht, Trends, Review, Gerät/Kalibrierung, Einstellungen
```

### 5.3 GUI-Funktionen, priorisiert

**Muss (M2/M3)**
- Live-EKG im Sweep-Modus wie am Monitor, mit **echtem mm-Raster** (1/5 mm), 25 oder 50 mm/s und 5/10/20 mm/mV.
- Bildschirm-Kalibrierung per Lineal-Dialog. Qt meldet unter Wayland oft eine falsche DPI-Zahl, ohne Kalibrierung stimmen „25 mm/s“ also nicht.
- Große HF-Anzeige mit Markierung der R-Zacken.
- Ampel für die Signalqualität und ein deutliches Banner bei Lead-off.
- Filter-Presets: *Monitor* (0,5–40 Hz + 50-Hz-Notch) und *Roh*. Die aktive Hardware-Bandbreite wird immer mit angezeigt.
- Aufnahme mit Zeitanzeige, Marker-Taste („Treppe gestiegen“) und Pause/Einfrieren.
- Gerätestatus: verbunden, Paketverluste, Testsignal-Modus.

**Soll (M5)**
- Review-Modus: Aufnahme öffnen, scrollen, zoomen, **Messschieber** (ms, bpm, mV).
- HF-Trend, RR-Tachogramm, Poincaré-Plot.
- HRV: SDNN, RMSSD, pNN50, LF/HF (Lomb-Scargle) nach Task-Force-Standard 1996, mit Filterung ektoper Schläge.
- Median-Beat mit PR, QRS und QT/QTc (Bazett und Fridericia), **als experimentell gekennzeichnet**.
- Export als EDF+ (z. B. mit EDFbrowser lesbar), WFDB und CSV; PDF-Bericht mit 25-mm/s-Streifen.

**Kann (später)**
- Rhythmus-*Hinweise* (Brady- oder Tachykardie, unregelmäßige RR-Intervalle als Hinweis auf Vorhofflimmern), ausdrücklich als nicht diagnostisch gekennzeichnet.
- Kalibrier-Assistent, der den Loopback-Ablauf aus 8.1 führt.

### 5.4 Fedora-Einrichtung

```bash
sudo usermod -aG dialout $USER          # danach neu anmelden
# ModemManager daran hindern, den Pico als Modem anzusprechen:
echo 'ATTRS{idVendor}=="2e8a", ENV{ID_MM_DEVICE_IGNORE}="1"' | \
  sudo tee /etc/udev/rules.d/99-ekg.rules
sudo udevadm control --reload-rules
# Firmware-Toolchain:
sudo dnf install cmake ninja-build arm-none-eabi-gcc-cs arm-none-eabi-gcc-cs-c++ arm-none-eabi-newlib
```

Das Pico SDK und picotool holt das CMake-Projekt selbst (`PICO_SDK_FETCH_FROM_GIT`).

---

## 6. Signalverarbeitung

### 6.1 Echtzeit (kausal, auf dem Host)

1. Umrechnung Counts → mV: `mV = (raw/64/4095 · 3,3 V − 1,65 V) / G · 1000`, mit dem **gemessenen** G aus M4.
2. Hochpass 0,5 Hz (Butterworth, 2. Ordnung) gegen Restdrift. Abschaltbar.
3. 50-Hz-Notch (IIR, Q ≈ 30), optional auch 100 Hz.
4. Tiefpass 40 Hz (entspricht der Hardware, unterdrückt aber digitales Restrauschen).
5. **QRS-Detektor nach Pan-Tompkins:**
   - Bandpass 5–15 Hz → Ableitung → Quadrierung → gleitendes Integrationsfenster 150 ms.
   - Adaptive Schwellen, Search-Back und 200 ms Refraktärzeit.
   - R-Zacke anschließend im gefilterten Signal verfeinert (parabolische Interpolation, Auflösung unter 1 ms).
6. HF: Median der letzten 8 RR-Intervalle, Ausreißer werden verworfen.
7. Signalqualität: Lead-off, Clipping, 50-Hz-Leistungsanteil, Kurtosis (kSQI) und Korrelation mit dem laufenden Beat-Template. Das Ergebnis wird zur Ampel zusammengefasst.

### 6.2 Offline (Review)

- Phasenfreie Filter (`sosfiltfilt`): kein Phasenverzug, keine verzerrten Strecken durch die Software.
- Delineation (P, QRS, T) per DWT über neurokit2, auf dem Median-Beat.

### 6.3 Vorbereitung für Phase 2

Die Echtzeit-Kette ist bewusst einfach gehalten (IIR plus Pan-Tompkins), damit sie sich nach C für den Pico portieren lässt. Gegen die Python-Implementierung wird mit **Golden Vectors** getestet: gleiche Eingabe, identische Detektionen. Umgesetzt in `host/tests/test_firmware_ecg.py`.

---

## 7. Qualitätssicherung

- `pytest` für DSP (synthetische Signale, MIT-BIH-Ausschnitte), Protokoll (Fuzzing mit kaputten Bytes: darf nicht abstürzen, Fehler werden erkannt) und Recorder (Absturz während der Aufnahme, danach sind die Daten lesbar).
- Firmware-Logik (Dezimation, Framing) wird zusätzlich für den Host kompiliert und dort getestet.
- `ruff` und `mypy` für Python.
- CI über GitHub Actions: Firmware bauen (UF2 als Artefakt), Tests und Lint.

---

## 8. Kalibrierung und Validierung

### 8.1 Loopback: Verstärkung und Frequenzgang des Klons

Die Pico-PWM an GP15 wird über zwei RC-Glieder (2 × 1 kΩ / 100 nF, ≈ 1,6 kHz) geglättet und dann über einen **~1000:1-Teiler** an die AD8232-Eingänge gelegt: 100 kΩ zur LA, 100 Ω zwischen LA und RA, RA und RL an 1,65 V (Teiler 2 × 1 kΩ).
Die Schaltung steht in [docs/KALIBRIERUNG.md](docs/KALIBRIERUNG.md). **Währenddessen ist kein Mensch angeschlossen.**

- **Verstärkung:** 1-mV-Rechteck einspeisen. Ziel ist eine Unsicherheit von ±2–3 %, begrenzt durch Widerstandstoleranz und 3,3-V-Genauigkeit.
- **Frequenzgang:** Sinus-Sweep von 0,05 bis 150 Hz ergibt die tatsächlichen Eckfrequenzen. Sie werden in der GUI als Gerätedaten hinterlegt.
- **Rauschen:** Eingänge kurzgeschlossen, daraus das Eingangsrauschen in µV(eff).

### 8.2 Algorithmen

- **MIT-BIH Arrhythmia Database** (48 Aufnahmen, PhysioNet), resampelt auf 500 Hz.
- Auswertung nach ANSI/AAMI EC57 mit `wfdb.processing.compare_annotations`. **Ziel: Sensitivität und +P ≥ 99 %** (Pan-Tompkins erreicht in der Literatur etwa 99,3 %).

### 8.3 Realer Vergleich (optional, empfohlen)

- Gleichzeitig mit einem Brustgurt messen, der RR-Intervalle liefert (z. B. Polar H10), und die Übereinstimmung von RR und HRV bestimmen.
- Falls möglich, einmal ein Klinik-EKG daneben legen, um die Morphologie zu vergleichen.

---

## 9. Meilensteine

| # | Inhalt | Abnahmekriterium |
|---|---|---|
| **M0** | Toolchain, udev, Blink-Firmware, Kabel durchmessen, Board-Prüfungen aus 2.2 | Blink-UF2 läuft, Kabelbelegung dokumentiert |
| **M1** | Firmware: ADC + DMA + Dezimation, Protokoll, Testsignal | 1 h Testsignal ohne verlorenes Paket; Rate 500 Hz ± 0,01 % gegen Host-Uhr |
| **M2** | Host: Reader, Live-Ansicht, Rohaufnahme | Erstes echtes EKG sichtbar; Latenz < 100 ms; Aufnahme übersteht einen Absturz |
| **M3** | DSP: Filter, QRS, HF, Signalqualität | MIT-BIH Se/+P ≥ 99 %; HF stabil bei ruhigem Sitzen |
| **M4** | Kalibrierung: Loopback, Verstärkung, Frequenzgang, Rauschen | Messprotokoll in `docs/`; danach Entscheidung über den HW-Filter-Umbau |
| **M5** | Review, Messschieber, HRV, Export, PDF | EDF+-Export öffnet in EDFbrowser; HRV gegen neurokit2 plausibel |
| **M6** | Phase 2: Display, Akku, SD-Karte, DSP in C | Golden Vectors identisch; 8 h Akkubetrieb |

### Phase 2 im Überblick (M6)

- Display (umgesetzt in 0.3.0): 2,8″-TFT 240×320 mit ILI9341, Touch (XPT2046) und SD-Steckplatz. Anschluss in
  [docs/AUFBAU.md](docs/AUFBAU.md) und [docs/verdrahtung.svg](docs/verdrahtung.svg). Kern 1 zeichnet in einen
  vollen Framebuffer (150 KB von 520 KB RAM) und überträgt nur geänderte Bereiche. Kern 0 erfasst weiter und
  bedient USB; die Erfassung läuft jetzt ab dem Einschalten, USB-Pakete gibt es erst nach START.
- DSP in C (umgesetzt): Pan-Tompkins und HF-Median als zeilengetreuer Port von `ekg.dsp`, Koeffizienten aus
  `tools/gen_ecg_tables.py`. `host/tests/test_firmware_ecg.py` vergleicht C und Python blockweise
  (Schlagindex auf 1e-6, Amplitude auf 1e-9 genau).
- Auswertung auf dem Gerät (0.4.0): ekg.analysis nach C portiert (`firmware/src/analysis`), 2 min Rohdaten
  im RAM (120 KB), gerechnet im Bildspeicher als Arbeitsspeicher. `host/tests/test_firmware_analysis.py`
  vergleicht beide an simulierten Aufnahmen; nur die Atemschätzung fehlt (braucht > 3 min).
- Touch: Koordinaten vom XPT2046, Einrichtung über 5 Punkte (Ausgleichsrechnung), Gesten.
- Batteriestand über den VSYS-Teiler an GPIO29 (wie Pico 2), ohne Umbau; am USB geprüft, sonst aus.
- Stromversorgung: vorerst 3×AA an VSYS (AUFBAU.md). Später LiPo mit Laderegler (z. B. MCP73831) und einem
  **rauscharmen LDO für den Analogteil**.
- SD-Karte (SPI) speichert im selben Rohformat wie der Host-Recorder, damit die Review-Software es direkt öffnen kann.
- Gehäuse und Zugentlastung fürs Kabel.

---

## 10. Repository-Struktur

```
firmware/   CMake-Projekt (Pico SDK), src/, tests/ (Host-kompiliert)
host/       pyproject.toml, src/ekg/…, tests/
tools/      mitbih_benchmark.py, freqresp.py, calibrate.py, gen_ecg_tables.py, gen_display_assets.py, display_preview.py
docs/       Aufbau, Sicherheit, Protokoll, Messprotokolle (M4)
PLAN.md     dieses Dokument
```

---

## 11. Risiken und offene Punkte

| Risiko | Gegenmaßnahme |
|---|---|
| Pinout oder Regler des Klon-Boards weichen ab | Prüfung in M0 (2.2) |
| Bauteilwerte des AD8232-Klons sind unbekannt | Messung in M4 statt Annahmen |
| Der 0,5-Hz-Hochpass verhindert eine ST-Beurteilung | In der UI kennzeichnen; HW-Umbau nach M4 entscheiden |
| Bewegungsartefakte, schlechter Elektrodenkontakt | Signalqualitäts-Ampel, Hautvorbereitung, ruhig sitzen |
| Falsche Interpretation der Ergebnisse | Rhythmus nur als „Hinweis“, deutlicher Hinweis „nicht diagnostisch“ |
| Laptop-Ladekabel bleibt beim Messen versehentlich stecken | Option: Software warnt, wenn `/sys/class/power_supply/*/online` = 1 |

## 12. Entscheidungen (getroffen)

| Frage | Entscheidung | Begründung |
|---|---|---|
| Host-Stack | **Python 3.11+, PySide6, pyqtgraph, NumPy/SciPy** | Für die Validierung (MIT-BIH, EDF+, WFDB) gibt es nur hier fertige Werkzeuge; die Live-Darstellung zeichnet mit QPainter selbst und ist schnell genug. |
| DSP-Ort in Phase 1 | **Host** | Schneller iterierbar und gegen Referenzdaten testbar. Die Echtzeit-Kette (IIR + Pan-Tompkins) ist bewusst einfach, damit sie sich in Phase 2 nach C portieren lässt. |
| Abtastrate | **500 Hz** (1000 Hz wählbar) | 500 Hz ist der übliche Mindestwert für EKG-Morphologie; 1000 Hz bringt bei 40 Hz Bandbreite nur feinere RR-Auflösung. |
| Prioritäten | Alle Muss- und Soll-Funktionen aus 5.3 umgesetzt, dazu der Kalibrier-Assistent und die Rhythmus-Hinweise | – |

## 13. Stand

| # | Status | Anmerkung |
|---|---|---|
| M0 | offen (Hardware) | Steckplan und Prüfliste in docs/AUFBAU.md |
| M1 | Code fertig, auf Hardware ungetestet | Firmware baut für RP2350; Protokoll, Dezimation und Testsignal sind auf dem Host getestet (gleicher Golden-Frame in C und Python) |
| M2 | fertig (mit Simulator getestet) | Live-Ansicht, absturzsichere Rohaufnahme, Lückenerkennung |
| M3 | fertig (synthetisch getestet) | MIT-BIH-Benchmark muss noch lokal laufen: `tools/mitbih_benchmark.py` |
| M4 | Werkzeug fertig, Messung offen | Kalibrier-Assistent gegen den simulierten Loopback getestet (Verstärkung ±1 %) |
| M5 | fertig | EDF+/WFDB/CSV/PDF, Messschieber, HRV, Median-Schlag |
| M6 | weitgehend | Display, Touch und Auswertung laufen auf dem Gerät (C = Python im Test); 0.5.0: hochkant, Maßstab in mm, Einstellungen; 0.6.0: Check-Liste, zwei Profile, Bildschirmtastatur; 0.7.0: Elektroden-Anleitung mit Figur; 0.8.0: Atem-Modus mit Coach; SD-Karte offen |

## 14. Erkenntnisse aus der ersten realen Messung (28.10.2026, 30 min)

Ausgewertet wurde der PDF-Bericht. Die Kurven liegen darin als Vektordaten vor: 100 s EKG mit 500 Hz und
die HF aller 1976 Schläge.

| Befund | Ursache | Änderung |
|---|---|---|
| Alle 110 Schläge der 100 s erkannt, keine falschen | – | QRS-Erkennung unverändert |
| „258 vorzeitige Schläge“, 12 % der RR verworfen | Die 20-%-Regel hielt starke, glatte HF-Schwankungen für Extraschläge und führte den Vergleichswert nicht nach | Klassifikation nach Lipponen & Tarvainen (2019) plus Regel für die kompensatorische Pause; jetzt 0,6 % verworfen |
| RMSSD 74 ms | durch die falsch verworfenen Schläge unterschätzt | 92 ms (5-min-Abschnitte 78–108 ms) |
| LF/HF 1,71 ohne Einordnung | Hauptschwankung bei 0,11 Hz, Atmung vermutlich bei ~0,18 Hz | Atemschätzung aus der R-Amplitude; LF/HF wird als nicht deutbar markiert, wenn die Atmung im LF-Band liegt |
| PR 250 ms (Fenstergrenze) | PR-Strecke ~0,03 mV tiefer als die TP-Strecke, fester Bezugspegel | P-Beginn über die Steigung, Plausibilitätsgrenzen; jetzt 160 ms |
| HF-Verlauf schwer lesbar | Schlag-für-Schlag-Werte schwanken mit der Atmung | zusätzlich gleitender Median über 30 s |

Als Regressionstest ist ein Simulator-Szenario „Ruhe“ mit diesen Eigenschaften hinzugekommen
(`host/tests/test_realistic.py`). Die echten Daten liegen nicht im Repository.

## 15. Erkenntnisse aus den ersten Rohaufnahmen (28./29.10.2026)

Ausgewertet wurden drei `.ekgrec`-Dateien (30 min, 7 min, 41 s), diesmal die Rohwerte statt des Berichts.
Alle drei sind lückenlos und ohne Lead-off. In den beiden längeren Aufnahmen weicht die Abtastrate höchstens
60 ppm von der Laptop-Uhr ab.

| Befund | Ursache | Änderung |
|---|---|---|
| HF-Spitze ~210/min und ein „verpasster Schlag“ in der 30-min-Aufnahme | Bewegung trieb den AD8232 an den Anschlag; im übersteuerten Abschnitt fand der Detektor Phantom-Schläge | Übersteuerte Samples (≤ 1 % vom Anschlag oder CLIP-Flag) werden samt 0,5 s Rand ausgeblendet, ebenso Lead-off und Lücken. RR-Intervalle über ausgeblendete Stellen zählen nicht. Live setzt CLIP die Erkennung zurück wie Lead-off. |
| 2 „vorzeitige“ Schläge in der 7-min-Aufnahme | Abrupte Verlangsamung (718 → 1090 ms, danach langsame Erholung) bzw. ein Sprung knapp über der Schwelle. Die RR-Klassifikation sieht nur Abstände. In der T-Welle vor der Pause ist keine versteckte P-Welle zu sehen. | Extraschläge zählen nur, wenn der Schlag tatsächlich vorzeitig kommt (< 85 % des lokalen Medians); die QRS-Korrelation mit dem Median-Schlag trennt normale und veränderte Form (`ekg/dsp/ectopy.py`). Der Rest heißt „RR-Sprung ohne Extraschlag“. |
| Atemschätzung aus der R-Amplitude unsicher: stärkstes Maximum je 2-min-Fenster zwischen 0,16 und 0,38 Hz | Die R-Amplitude hängt nicht nur von der Atmung ab (u. a. Kontakt, Körperlage); welche Spitze die Atmung ist, lässt sich aus ihr allein nicht sagen | Atemfrequenz wird nur angegeben, wenn Amplituden- und RR-Spektrum in 120-s-Fenstern übereinstimmen (±0,02 Hz) |
| QTc Bazett 460–482 ms bei 91–94/min (Fridericia 428–450 ms) | Bazett überkorrigiert oberhalb ~80/min deutlich | Fridericia steht vorn; Bazett trägt bei HF > 80/min den Hinweis „überschätzt“ |
| Netzbrumm vor dem Filter 147–212 µV eff., mindestens so stark wie das EKG (kurze Aufnahmen) bzw. 25 µV (30 min) | Elektrodenkontakt oder Verbindung zum Stromnetz; die Filter entfernen ihn vollständig, daher war er unsichtbar | Bericht und Auswertung nennen den Brumm-Anteil und geben ab 10 % bzw. 50 % der EKG-Leistung einen Hinweis |
| PR = 250 ms in allen drei Aufnahmen mit der alten Version | Fehler aus Abschnitt 14 | mit der aktuellen Version 162–180 ms |
| Kopfzeile „ekg 0.1.0“ trotz geänderter Auswertung | Versionsnummer nicht erhöht | 0.2.0 |
| Brumm stieg von Aufnahme zu Aufnahme (147 → 212 → 406 µV eff.), Ursache im Nachhinein nicht klärbar | Die Aufnahme enthielt nicht, ob der Laptop am Netzteil hing; die Warnung gab es nur live | Stromversorgung im Kopf der Aufnahme, jeder Wechsel als Ereignis; Bericht zeigt sie an und passt den Brumm-Hinweis an (0.2.1) |
| Live-Ampel meldete bei sauberem Signal in 20–68 % der Fenster „Signal verrauscht“ / „Schlagform wechselnd“ (eine Person) | Kleine R-Zacke, große P- und T-Welle senken die Kurtosis des Anzeigesignals; Atmung und Grundlinie senken die Formkorrelation | Kurtosis im QRS-Band 5–15 Hz, Formvergleich nach Entfernen der Neigung je Schlag; Schwellen unverändert. Fehlalarme jetzt 0–8 %, reines Rauschen weiter „kein EKG-typisches Signal“; langsame Bewegungsartefakte werden in etwas weniger Fenstern erkannt (54–69 % statt ~70 %) |
| PR-Strecke 0,10–0,14 mV unter der TP-Strecke (eine Person, alle Aufnahmen) | Kein Filtereffekt: gleicher Wert mit 0,5 Hz, 0,05 Hz und ohne digitalen Hochpass; wächst mit der P-Welle (Vorhof-Repolarisation) | P-Amplitude wird ab P-Beginn gemessen (siehe oben); Strecken werden weiterhin nicht bewertet (Bandbreite) |

Regressionstests: `host/tests/test_recording_quality.py`. Die echten Daten liegen nicht im Repository.
