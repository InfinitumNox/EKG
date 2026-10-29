#!/usr/bin/env python3
"""Erzeugt firmware/src/ecg/ecg_tables.h aus der Host-Software.

    cd host && uv run python ../tools/gen_ecg_tables.py

Die Firmware rechnet für die Anzeige dieselben Filter und denselben QRS-Detektor wie der
Laptop. Statt die Filterentwürfe in C nachzubauen, kommen Koeffizienten und abgeleitete
Konstanten direkt aus ekg.dsp; host/tests/test_firmware_ecg.py prüft, dass die Datei aktuell
ist und der C-Detektor dieselben Schläge findet.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from scipy import signal

from ekg.dsp.filters import PRESETS, design_sos
from ekg.dsp.qrs import QrsDetector

FS = 500  # die Anzeige arbeitet immer mit 500 Hz (1000-Hz-Daten werden paarweise gemittelt)
OUT = Path(__file__).resolve().parents[1] / "firmware" / "src" / "ecg" / "ecg_tables.h"


def _d(v: float) -> str:
    return repr(float(v))


def _sos(name: str, sos: np.ndarray, ctype: str) -> list[str]:
    zi = signal.sosfilt_zi(sos)
    rows = []
    for s, z in zip(sos, zi, strict=True):
        b0, b1, b2, a0, a1, a2 = (float(v) for v in s)
        assert abs(a0 - 1.0) < 1e-15
        vals = (b0, b1, b2, a1, a2, float(z[0]), float(z[1]))
        rows.append("    {" + ", ".join(_d(v) for v in vals) + "},")
    return [
        f"// {name}: je Abschnitt b0, b1, b2, a1, a2 und der Einschwing-Zustand für Eingang 1",
        f"static const {ctype} {name}[{len(sos)}][7] = {{",
        *rows,
        "};",
    ]


def render() -> str:
    det = QrsDetector(FS)
    lines = [
        "// Erzeugt von tools/gen_ecg_tables.py – nicht von Hand ändern.",
        "#pragma once",
        "",
        f"#define ECG_FS {FS}",
        f"#define QRS_W {det._w}  // gleitendes Integral (150 ms)",
        f"#define QRS_LOOKAHEAD {det._lookahead}",
        f"#define QRS_REFRACTORY {det._refractory}",
        f"#define QRS_T_WINDOW {det._t_window}",
        f"#define QRS_SETTLE {round(det.SETTLE_S * FS)}",
        f"#define QRS_LEARN {round(det.LEARN_S * FS)}",
        f"#define QRS_KEEP {round(det.HISTORY_S * FS)}",
        f"#define QRS_LOCATE_HALF {round(det.LOCATE_HALF_WINDOW_S * FS)}",
        f"#define QRS_STALL {_d(det.STALL_S * FS)}",
        f"#define QRS_MWI_DELAY {_d(det._mwi_delay)}",
        f"#define QRS_REF_DELAY {_d(det._ref_delay)}",
        f"#define QRS_FLOOR {_d(det._floor)}",
        f"#define QRS_DERIV_GAIN {_d(FS / 8.0)}  // Ableitung (2, 1, 0, -1, -2) * fs / 8",
        "",
        *_sos("QRS_BP_SOS", det._bp_sos, "double"),
        "",
        *_sos("QRS_REF_SOS", det._ref_sos, "double"),
        "",
        "// Anzeige wie die Live-Ansicht am Laptop (Voreinstellung „Monitor“)",
        *_sos("ECG_DISPLAY_SOS", design_sos(PRESETS["monitor"][1], FS), "float"),
        "",
        "// Auswertung: dieselbe Kette phasenfrei (vorwärts und rückwärts) wie ekg.analysis",
        *_sos("ANA_SOS", design_sos(PRESETS["monitor"][1], FS), "double"),
        "",
    ]
    return "\n".join(lines)


if __name__ == "__main__":
    text = render()
    if "--check" in sys.argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            sys.exit(f"{OUT} ist veraltet: tools/gen_ecg_tables.py ausführen")
        print(f"{OUT} ist aktuell")
    else:
        OUT.parent.mkdir(parents=True, exist_ok=True)
        OUT.write_text(text, encoding="utf-8")
        print(OUT)
