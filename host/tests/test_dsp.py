import numpy as np
import pytest
from scipy import signal

from ekg.device.simulator import Scenario, SyntheticEcg
from ekg.dsp.beats import delineate, median_beat
from ekg.dsp.filters import PRESETS, FilterSettings, StreamingSos, design_sos, zero_phase
from ekg.dsp.hr import HeartRateTracker
from ekg.dsp.hrv import compute_hrv, rhythm_hints
from ekg.dsp.qrs import QrsDetector, detect_qrs

FS = 500


def match(true_idx, det_idx, tol):
    """Einfaches Matching wie bei AAMI EC57: (TP, FN, FP, Zeitfehler)."""
    det = np.sort(np.asarray(det_idx))
    used = np.zeros(len(det), dtype=bool)
    errs = []
    for r in true_idx:
        if len(det) == 0:
            break
        k = int(np.argmin(np.abs(det - r)))
        if abs(det[k] - r) <= tol and not used[k]:
            used[k] = True
            errs.append(det[k] - r)
    tp = len(errs)
    return tp, len(true_idx) - tp, int((~used).sum()), np.array(errs)


def test_notch_attenuates_mains_and_keeps_passband():
    sos = design_sos(PRESETS["monitor"][1], FS)
    w, h = signal.sosfreqz(sos, worN=[10.0, 50.0], fs=FS)
    db = 20 * np.log10(np.abs(h))
    assert db[0] > -0.5
    assert db[1] < -30


def test_streaming_equals_batch():
    x = np.random.default_rng(0).normal(size=5000)
    sos = design_sos(FilterSettings(0.5, 40, 50), FS)
    f = StreamingSos(sos)
    chunks = np.concatenate([f.process(c) for c in np.array_split(x, 37)])
    ref, _ = signal.sosfilt(sos, x, zi=signal.sosfilt_zi(sos) * x[0])
    np.testing.assert_allclose(chunks, ref, atol=1e-12)


def test_zero_phase_bridges_gaps():
    x = np.sin(2 * np.pi * 1.0 * np.arange(5000) / FS)
    x[1000:1100] = np.nan
    y = zero_phase(x, FS, PRESETS["monitor"][1])
    assert np.isnan(y[1000:1100]).all()
    assert np.isfinite(y[:1000]).all() and np.isfinite(y[1100:]).all()


@pytest.mark.parametrize("scenario", [s for s in Scenario if s != Scenario.LEAD_OFF])
def test_qrs_detector_synthetic(scenario):
    g = SyntheticEcg(FS, 70, scenario, seed=7, mains_mv=0.05)
    x = g.generate(FS * 120)
    beats = detect_qrs(x, FS)
    lo, hi = 2.6 * FS, 119.5 * FS
    true = np.array(g.r_times) * FS
    true = true[(true > lo) & (true < hi)]
    det = np.array([b.index for b in beats])
    det = det[(det > lo - 0.1 * FS) & (det < hi + 0.1 * FS)]
    tp, fn, fp, errs = match(true, det, 0.15 * FS)
    assert fn == 0, f"{scenario}: {fn} Schläge verpasst"
    assert fp <= 1, f"{scenario}: {fp} Fehldetektionen"
    if scenario not in (Scenario.PVC,):  # PVC-Spitze liegt bewusst nicht auf r_time
        assert np.abs(np.median(errs)) < 0.5, "systematischer Zeitversatz > 1 ms"
        assert np.std(errs) < 1.0


def test_qrs_streaming_chunk_size_independent():
    g = SyntheticEcg(FS, 80, seed=3)
    x = g.generate(FS * 30)
    a = [b.index for b in detect_qrs(x, FS, chunk_s=0.05)]
    b = [b.index for b in detect_qrs(x, FS, chunk_s=2.0)]
    np.testing.assert_allclose(a, b)


def test_qrs_inverted_polarity():
    g = SyntheticEcg(FS, 70, seed=4)
    x = -g.generate(FS * 40)
    beats = detect_qrs(x, FS)
    assert len(beats) > 40
    assert np.median([b.amplitude_mv for b in beats]) < -0.8


def test_qrs_no_beats_in_noise():
    x = 0.01 * np.random.default_rng(5).normal(size=FS * 30)
    assert len(detect_qrs(x, FS)) < 5


def test_qrs_recovers_after_artifact():
    g = SyntheticEcg(FS, 70, seed=9)
    x = g.generate(FS * 60)
    x[10 * FS : 12 * FS] += 8 * np.sin(2 * np.pi * 8 * np.arange(2 * FS) / FS)  # riesiges Artefakt
    beats = np.array([b.index for b in detect_qrs(x, FS)])
    late = beats[beats > 20 * FS]
    true = np.array(g.r_times) * FS
    true = true[(true > 20 * FS) & (true < 59.5 * FS)]
    tp, fn, _, _ = match(true, late, 0.15 * FS)
    assert fn <= 1


def test_detector_rejects_index_jump():
    d = QrsDetector(FS)
    d.process(np.zeros(100), 0)
    with pytest.raises(ValueError):
        d.process(np.zeros(100), 500)


def test_heart_rate_tracker():
    hr = HeartRateTracker(FS)
    for k in range(10):
        hr.add(k * 0.8 * FS)
    assert hr.bpm(9 * 0.8 * FS) == pytest.approx(75.0)
    assert hr.bpm(9 * 0.8 * FS + 5 * FS) is None  # veraltet


def test_hrv_known_values():
    rng = np.random.default_rng(1)
    t = np.arange(400) * 0.0 + 0.0
    rr = 0.8 + 0.03 * np.sin(2 * np.pi * 0.25 * np.arange(400) * 0.8) + 0.005 * rng.normal(size=400)
    t = np.concatenate([[0.0], np.cumsum(rr)])
    h = compute_hrv(t)
    assert h is not None
    assert h.mean_hr == pytest.approx(75, abs=1)
    assert 15 < h.sdnn_ms < 30
    assert h.hf_ms2 > 5 * h.lf_ms2  # Atmung bei 0,25 Hz liegt im HF-Band
    ectopic = t.copy()
    ectopic[200] -= 0.3
    h2 = compute_hrv(ectopic)
    assert h2.n_excluded >= 2
    assert h2.rmssd_ms == pytest.approx(h.rmssd_ms, rel=0.15)


def test_rhythm_hints():
    reg = np.cumsum(np.full(100, 0.8))
    assert rhythm_hints(reg) == []
    rng = np.random.default_rng(2)
    irr = np.cumsum(0.8 * rng.uniform(0.6, 1.4, 100))
    assert any("unregelmäßig" in h for h in rhythm_hints(irr))
    assert any("niedrig" in h for h in rhythm_hints(np.cumsum(np.full(100, 1.4))))


def test_median_beat_and_delineation():
    g = SyntheticEcg(FS, 60, seed=11)
    x = zero_phase(g.generate(FS * 60), FS, PRESETS["monitor"][1])
    r = np.array([b.index for b in detect_qrs(x, FS)])
    mb = median_beat(x, r, FS)
    assert mb is not None and mb.n_beats > 30
    d = delineate(mb)
    assert 50 < d.qrs_ms < 130
    assert d.pr_ms is not None and 100 < d.pr_ms < 260
    assert d.qt_ms is not None and 300 < d.qt_ms < 480
    assert d.r_amp_mv > 0.8


def test_af_has_no_p_wave():
    g = SyntheticEcg(FS, 70, Scenario.AF, seed=12)
    x = zero_phase(g.generate(FS * 90), FS, PRESETS["monitor"][1])
    r = np.array([b.index for b in detect_qrs(x, FS)])
    mb = median_beat(x, r, FS)
    if mb is not None:  # bei starker Irregularität kann es zu wenige typische Schläge geben
        d = delineate(mb)
        assert d.p_amp_mv is None or abs(d.p_amp_mv) < 0.08
