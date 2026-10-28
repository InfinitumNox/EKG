"""Regressionstests aus der ersten realen Messung (28.10.2026, 30 min, Ruhe).

Die echten Daten liegen nicht im Repository (Gesundheitsdaten). Nachgebildet sind die
Eigenschaften, die damals Fehler ausgelöst haben:
- starke, glatte HF-Schwankung (±13 % bei ~0,1 Hz plus Atmung): die frühere 20-%-Regel
  verwarf 12 % der Schläge und meldete „258 vorzeitige Schläge“;
- PR-Strecke tiefer als die TP-Strecke (Ta-Welle): der P-Beginn landete am Rand des
  Suchfensters und ergab PR = 250 ms statt ~160 ms.
"""

import numpy as np
import pytest

from ekg.device.simulator import Scenario, SyntheticEcg
from ekg.dsp.artifacts import classify_rr
from ekg.dsp.beats import delineate, median_beat
from ekg.dsp.filters import PRESETS, zero_phase
from ekg.dsp.hrv import compute_hrv, hrv_rows, irregularity, rhythm_hints, segment_hrv
from ekg.dsp.qrs import detect_qrs
from ekg.dsp.respiration import breathing_from_amplitude, qrs_amplitudes

FS = 500


@pytest.fixture(scope="module")
def rest():
    g = SyntheticEcg(FS, scenario=Scenario.REST, seed=21)
    x = zero_phase(g.generate(FS * 600), FS, PRESETS["monitor"][1])
    r = np.array([b.index for b in detect_qrs(x, FS)])
    return g, x, r


def test_rest_no_false_ectopics(rest):
    _g, _x, r = rest
    h = compute_hrv(r / FS)
    assert h is not None
    assert h.n_excluded / h.n_beats < 0.02
    hints = rhythm_hints(r / FS)
    assert not any(
        "vorzeitig" in s or "unregelmäßig" in s or "Erkennungsfehler" in s for s in hints
    )
    assert any("Atmung" in s for s in hints)


def test_rest_breathing_and_lf_peak(rest):
    _g, x, r = rest
    breathing = breathing_from_amplitude(r / FS, qrs_amplitudes(x, r, FS))
    assert breathing == pytest.approx(0.18, abs=0.02)
    h = compute_hrv(r / FS)
    assert h.peak_hz == pytest.approx(0.105, abs=0.015)
    rows = dict(hrv_rows(h, breathing))
    assert "nicht deutbar" not in rows["LF / HF"]  # Atmung liegt im HF-Band
    rows_slow = dict(hrv_rows(h, 0.11))
    assert "nicht deutbar" in rows_slow["LF / HF"]


def test_rest_segments(rest):
    _g, _x, r = rest
    segs = segment_hrv(r / FS)
    assert len(segs) == 1 or len(segs) == 2
    assert all(60 < s.mean_hr < 75 for s in segs)


def test_pr_with_lowered_pr_segment(rest):
    _g, x, r = rest
    mb = median_beat(x, r, FS)
    d = delineate(mb)
    assert d.pr_ms is not None
    assert 160 < d.pr_ms < 240, d.pr_ms  # früher am Fensterrand (250/300 ms)
    assert d.p_amp_mv is not None and 0.05 < d.p_amp_mv < 0.15


def test_classifier_detects_errors_and_ectopics():
    rng = np.random.default_rng(3)
    t = np.arange(400) * 0.0
    rr = (
        0.9
        + 0.12 * np.sin(2 * np.pi * 0.105 * np.cumsum(np.full(400, 0.9)))
        + 0.01 * rng.normal(size=400)
    )
    t = np.concatenate([[0], np.cumsum(rr)])
    smooth = classify_rr(np.diff(t))
    assert smooth.n_artifacts <= 4  # starke, aber glatte Schwankung ist kein Artefakt

    missed = np.delete(t, [100, 250])
    c = classify_rr(np.diff(missed))
    assert c.missed == [99, 248]

    extra = np.sort(np.append(t, [t[60] + 0.4 * (t[61] - t[60])]))
    c = classify_rr(np.diff(extra))
    assert c.extra == [60]

    ect = t.copy()
    ect[150] -= 0.3  # vorzeitig, danach kompensatorische Pause
    c = classify_rr(np.diff(ect))
    assert 150 in c.ectopic or 149 in c.ectopic


def test_frequent_pvcs_and_af_are_separated():
    pvc = SyntheticEcg(FS, scenario=Scenario.PVC, seed=4)
    pvc.generate(FS * 300)
    hints = rhythm_hints(np.array(pvc.r_times))
    assert any("vorzeitige" in s for s in hints)
    assert not any("unregelmäßig" in s for s in hints)

    af = SyntheticEcg(FS, scenario=Scenario.AF, seed=4)
    af.generate(FS * 300)
    t = np.array(af.r_times)
    irr = irregularity(np.diff(t))
    assert irr is not None and irr.fraction > 0.8
    hints = rhythm_hints(t)
    assert any("unregelmäßig" in s for s in hints)
    assert not any("vorzeitige" in s for s in hints)
