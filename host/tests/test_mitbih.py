"""Optional: QRS-Erkennung an echten MIT-BIH-Daten.

Läuft nur, wenn EKG_MITDB_DIR auf einen Ordner mit den PhysioNet-Dateien zeigt
(z. B. nach `python tools/mitbih_benchmark.py`, Standard ~/.cache/ekg/mitdb).
"""

import os
from pathlib import Path

import numpy as np
import pytest
from scipy.signal import resample_poly

from ekg.dsp.qrs import detect_qrs

MITDB = os.environ.get("EKG_MITDB_DIR")
BEATS = set("NLRBAaJSVrFejnE/fQ?")

pytestmark = pytest.mark.skipif(not MITDB, reason="EKG_MITDB_DIR nicht gesetzt")


@pytest.mark.parametrize("record", ["100", "101", "103", "105", "119", "208"])
def test_mitbih_record(record):
    wfdb = pytest.importorskip("wfdb")
    from wfdb import processing

    base = Path(MITDB) / record
    if not base.with_suffix(".dat").exists():
        pytest.skip(f"{record} nicht vorhanden")
    r = wfdb.rdrecord(str(base), channels=[0])
    ann = wfdb.rdann(str(base), "atr")
    det = np.array([b.index for b in detect_qrs(resample_poly(r.p_signal[:, 0], 25, 18), 500)])
    det = np.round(det * 360 / 500).astype(int)
    ref = np.array([s for s, y in zip(ann.sample, ann.symbol, strict=True) if y in BEATS])
    ref, det = ref[ref > 3 * 360], det[det > 3 * 360 - 27]
    c = processing.compare_annotations(ref, det, 54)
    se = c.tp / (c.tp + c.fn)
    ppv = c.tp / (c.tp + c.fp)
    # Aufnahme 105 und 208 sind bekannt schwierig (starkes Rauschen bzw. viele PVCs).
    limit = 0.97 if record in ("105", "208") else 0.99
    assert se >= limit and ppv >= limit, f"{record}: Se {se:.4f}, +P {ppv:.4f}"
