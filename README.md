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

## Schnellstart unter Fedora

### 1. Einmalig: Rechte und ModemManager

```bash
sudo usermod -aG dialout $USER                       # danach ab- und wieder anmelden
sudo cp host/packaging/99-pico-ekg.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Die udev-Regel verhindert, dass ModemManager den Pico als Modem anspricht und dabei Daten stört.

### 2. Firmware bauen und flashen

```bash
sudo dnf install cmake ninja-build gcc-c++ git python3 \
    arm-none-eabi-gcc-cs arm-none-eabi-gcc-cs-c++ arm-none-eabi-newlib
cmake -S firmware -B build/firmware -G Ninja      # holt das Pico SDK 2.2.0 automatisch
cmake --build build/firmware
```

BOOT-Taste am Pico gedrückt halten, USB einstecken, loslassen. Es erscheint ein Laufwerk `RP2350`,
dorthin `build/firmware/ekg.uf2` kopieren. Später geht es ohne Taste: Menü *Gerät → Bootloader starten*.

### 3. Laptop-Software

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh    # falls uv fehlt
cd host
uv venv && uv pip install -e ".[dev]"
uv run ekg                       # Oberfläche
uv run ekg --simulate            # ohne Hardware ausprobieren (auch: pvc, af, noisy, lead_off …)
```

Ohne uv: `python3 -m venv .venv && .venv/bin/pip install -e ".[dev]" && .venv/bin/ekg`.

### 4. Erste Messung

1. Laptop-Ladekabel ziehen. Dann den Pico per USB anschließen.
2. In `ekg` als Quelle **Pico EKG** wählen und **Verbinden** klicken. Es folgt ein Sicherheitscheck.
3. Elektroden anlegen (siehe [docs/AUFBAU.md](docs/AUFBAU.md#elektroden)), ruhig sitzen, 2 s warten.
4. **● Aufnahme** startet die Rohdaten-Aufzeichnung, **Auswerten** öffnet sie danach.
5. Einmalig: *Ansicht → Bildschirm kalibrieren*, damit 25 mm/s und 10 mm/mV am Bildschirm stimmen.

## Was die Software kann

- **Live:** Sweep-Darstellung wie am Monitor mit echtem mm-Raster und Eichzacke.
  Dazu Herzfrequenz mit R-Zacken-Markern, eine Ampel für die Signalqualität (mit Begründung) und
  Warnungen bei Elektrode ab, Laptop am Netzteil, Paketverlust oder unkalibrierter Verstärkung.
- **Trends:** HF-Verlauf, RR-Tachogramm, Poincaré-Plot, HRV der letzten 5 Minuten.
- **Auswertung:** Aufzeichnungen öffnen und mit phasenfreien Filtern ansehen.
  Es gibt eine 1:1-Papieransicht, einen Messschieber (ms, /min, mV) und die HRV für den gewählten Ausschnitt.
  Der Median-Schlag wird mit PR, QRS und QT/QTc vermessen (experimentell, Messpunkte eingezeichnet).
- **Export:** EDF+ (z. B. EDFbrowser), WFDB (PhysioNet), CSV und ein PDF-Bericht im Papierformat.
- **Kalibrierung:** Verstärkung und Frequenzgang des eigenen Boards über eine Loopback-Schaltung messen.
- **Simulator:** spricht das echte Protokoll. Szenarien: Sinusrhythmus, Extrasystolen, Vorhofflimmern,
  Brady-/Tachykardie, Artefakte, abfallende Elektrode.
- **Kommandozeile:** `ekg-record --duration 300` (ohne Oberfläche), `ekg-export aufnahme.ekgrec --pdf b.pdf`.

Aufnahmen liegen in `~/.local/share/ekg/aufnahmen/`, Einstellungen und Kalibrierungen in `~/.config/ekg/`.

## Tests

```bash
cd host && uv run pytest                                   # Host: Protokoll, DSP, Pipeline, Export, GUI
cmake -S firmware/tests -B build/fw-tests && cmake --build build/fw-tests && build/fw-tests/test_firmware
uv run python ../tools/mitbih_benchmark.py                 # QRS-Erkennung gegen MIT-BIH (lädt ~100 MB)
```

## Grenzen (ehrlich)

- **Eine Ableitung.** Drei Elektroden ergeben einen Kanal, RL ist die Bezugselektrode. Ein 12-Kanal-EKG
  ist damit nicht möglich.
- **Bandbreite des Boards typ. 0,5–40 Hz.** Damit ist keine ST-Strecken-Beurteilung möglich, und QRS-Details
  werden gerundet. Gemessen wird das mit der Kalibrierung.
- **Intervalle (PR/QRS/QT) sind experimentell.** Sie sind nicht an echten Referenzdaten validiert.
- **Rhythmus-Hinweise sind Hinweise.** Sie sind kein Befund.
