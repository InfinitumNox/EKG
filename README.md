# EKG – AD8232 + Raspberry Pi Pico 2

Ein-Kanal-EKG (eine Ableitung) mit Desktop-Software für Linux (getestet für Fedora).

> **Kein Medizinprodukt.** Bastel- und Lernprojekt, keine Diagnose. Bei Beschwerden zum Arzt.
>
> **Sicherheit:** Elektroden nur am Körper, wenn der Laptop **im Akkubetrieb** läuft und nichts
> mit Netzanschluss daran hängt (Monitor, Dock, Netzwerkkabel). Details: [docs/AUFBAU.md](docs/AUFBAU.md).

| Teil | Inhalt |
|---|---|
| [PLAN.md](PLAN.md) | Plan, Entscheidungen, Meilensteine |
| [docs/AUFBAU.md](docs/AUFBAU.md) | Steckplan, Elektroden, Sicherheit |
| [docs/breadboard.svg](docs/breadboard.svg) | Steckplan als Grafik |
| [docs/KALIBRIERUNG.md](docs/KALIBRIERUNG.md) | Verstärkung und Frequenzgang messen |
| [docs/PROTOKOLL.md](docs/PROTOKOLL.md) | USB-Protokoll Gerät ↔ Laptop |
| `firmware/` | Pico-2-Firmware (C, Pico SDK 2.2) |
| `host/` | Laptop-Software (Python, PySide6, pyqtgraph) |
| `tools/` | MIT-BIH-Benchmark, Steckplan-Generator |

## Installation unter Fedora

```bash
git clone https://github.com/InfinitumNox/EKG.git
cd EKG
./install.sh
```

Das Skript fragt bei Bedarf nach dem sudo-Passwort und erledigt:

1. **Laptop:**
   - Es installiert die Software in eine eigene Umgebung (über `uv`, das System-Python bleibt unberührt).
     Danach gibt es die Befehle `ekg`, `ekg-record`, `ekg-export` und `ekg-flash` sowie den Startmenü-Eintrag „EKG“.
   - Es richtet die udev-Regel ein: Zugriff auf den Pico ohne Root und ohne Neuanmelden, und ModemManager
     lässt die Schnittstelle in Ruhe.
2. **Pico:** Es spielt die mitgelieferte Firmware (`firmware/prebuilt/ekg.uf2`) auf.
   - **Beim allerersten Mal:** USB abziehen, BOOT-Taste halten, einstecken, loslassen. Das Skript wartet darauf.
   - **Danach geht es automatisch,** die Firmware schaltet sich selbst in den Bootloader.

| Befehl | Wirkung |
|---|---|
| `./install.sh` | alles (Laptop, dann Frage nach dem Pico) |
| `./install.sh laptop` | nur die Laptop-Software (auch zum Aktualisieren) |
| `./install.sh pico` | nur die Firmware aufspielen |
| `./install.sh pico --build` | Firmware selbst bauen (installiert die ARM-Toolchain) und aufspielen |
| `./install.sh uninstall` | alles entfernen; Aufnahmen und Einstellungen bleiben |

Passt die mitgelieferte Firmware nicht mehr zum Quellcode (geprüft über `firmware/prebuilt/SOURCE_HASH`),
baut das Skript sie automatisch neu.

### Erste Messung

1. `ekg --simulate` zeigt die Oberfläche ohne Hardware.
2. Laptop-Ladekabel ziehen. Dann den Pico per USB anschließen.
3. In `ekg` als Quelle **Pico EKG** wählen und **Verbinden** klicken. Es folgt ein Sicherheitscheck.
   Zum Test erst *Gerät → Testsignal der Firmware* einschalten. Kommt das Test-EKG an, funktioniert die Kette
   vom Pico bis zum Laptop.
4. Testsignal aus, Elektroden anlegen (siehe [docs/AUFBAU.md](docs/AUFBAU.md#elektroden)), ruhig sitzen, 2 s warten.
5. **● Aufnahme** startet die Rohdaten-Aufzeichnung, **Auswerten** öffnet sie danach.
6. Einmalig: *Ansicht → Bildschirm kalibrieren*, damit 25 mm/s und 10 mm/mV am Bildschirm stimmen.

### Von Hand (ohne Skript)

```bash
sudo cp host/packaging/70-pico-ekg.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules
uv tool install --python 3.13 "ekg[bench] @ file://$PWD/host"
ekg-flash firmware/prebuilt/ekg.uf2       # oder die Datei aufs Laufwerk RP2350 kopieren
```

Firmware selbst bauen: `sudo dnf install cmake ninja-build gcc-c++ git arm-none-eabi-gcc-cs
arm-none-eabi-gcc-cs-c++ arm-none-eabi-newlib`, dann `cmake -S firmware -B build/firmware -G Ninja &&
cmake --build build/firmware`. Das Pico SDK 2.2.0 wird dabei automatisch geholt.

## Was die Software kann

- **Live:** Sweep-Darstellung wie am Monitor mit echtem mm-Raster und Eichzacke.
  Dazu Herzfrequenz mit R-Zacken-Markern, eine Ampel für die Signalqualität (mit Begründung) und
  Warnungen bei Elektrode ab, Übersteuerung, Laptop am Netzteil oder Paketverlust.
- **Trends:** HF-Verlauf, RR-Tachogramm, Poincaré-Plot, HRV der letzten 5 Minuten.
- **Auswertung:** Aufzeichnungen öffnen und mit phasenfreien Filtern ansehen.
  Es gibt eine 1:1-Papieransicht, einen Messschieber (ms, /min, mV) und die HRV für den gewählten Ausschnitt.
  Der Median-Schlag wird mit PR, QRS und QT/QTc vermessen (experimentell, Messpunkte eingezeichnet).
  QTc steht nach Fridericia; Bazett überschätzt bei höherer Herzfrequenz und ist nur ergänzend angegeben.
- **Export:** EDF+ (z. B. EDFbrowser), WFDB (PhysioNet), CSV und ein PDF-Bericht im Papierformat.
- **Artefakt-Erkennung:** Extraschläge und Erkennungsfehler werden nach Lipponen & Tarvainen (2019) erkannt.
  Starke, aber gleichmäßige HF-Schwankungen (Atmung, Kreislauf) gelten dabei als normal. Als Extraschlag zählt
  ein Schlag erst, wenn er tatsächlich vorzeitig kommt; die QRS-Form im Vergleich zum Median-Schlag wird mit
  angegeben. Übersteuerte Abschnitte (Bewegung) werden samt Rand ausgeblendet.
- **Atmung:** Die Software schätzt die Atemfrequenz aus der R-Amplitude und gibt sie nur an, wenn das RR-Spektrum
  dieselbe Frequenz zeigt. Liegt die Atmung im LF-Band, wird LF/HF als nicht deutbar markiert.
- **Signalqualität der Aufnahme:** Der Bericht nennt den 50-Hz-Brumm im ungefilterten Signal. Viel Brumm heißt
  schlechter Elektrodenkontakt oder eine Verbindung zum Stromnetz. Ob der Laptop am Netzteil hing, steht in der
  Aufnahme (Beginn und jeder Wechsel) und im Bericht.
- **Kalibrierung (optional):** Wer die mV-Werte genau braucht, misst die Verstärkung über eine Zusatzschaltung.
  Für HF, HRV und Intervalle ist das nicht nötig.
- **Simulator:** spricht das echte Protokoll. Szenarien: Sinusrhythmus, Extrasystolen, Vorhofflimmern,
  Brady-/Tachykardie, Artefakte, abfallende Elektrode.
- **Kommandozeile:** `ekg-record --duration 300` (ohne Oberfläche), `ekg-export aufnahme.ekgrec --pdf b.pdf`.

Aufnahmen liegen in `~/.local/share/ekg/aufnahmen/`, Einstellungen und Kalibrierungen in `~/.config/ekg/`.

## Entwicklung und Tests

```bash
cd host && uv venv && uv pip install -e ".[dev]"
uv run pytest                                   # Host: Protokoll, DSP, Pipeline, Export, GUI
cmake -S firmware/tests -B build/fw-tests && cmake --build build/fw-tests && build/fw-tests/test_firmware
uv run python ../tools/mitbih_benchmark.py                 # QRS-Erkennung gegen MIT-BIH (lädt ~100 MB)
```

## Grenzen (ehrlich)

- **Eine Ableitung.** Drei Elektroden ergeben einen Kanal, RL ist die Bezugselektrode. Ein 12-Kanal-EKG
  ist damit nicht möglich.
- **Bandbreite des Boards typ. 0,5–40 Hz.** Damit ist keine ST-Strecken-Beurteilung möglich, und QRS-Details
  werden gerundet. Gemessen wird das mit der Kalibrierung.
- **Intervalle (PR/QRS/QT) sind experimentell.** Geprüft ist nur der Median-Schlag einer realen Messung gegen
  eine Bildablesung. Es gibt keinen Vergleich mit einem Klinik-EKG.
- **Ohne Kalibrierung sind mV-Werte Näherungen.** Grundlage ist die Nennverstärkung 1100. Zeiten, HF und HRV
  hängen davon nicht ab.
- **Rhythmus-Hinweise sind Hinweise.** Sie sind kein Befund.
