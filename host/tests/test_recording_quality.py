"""Regressionstests aus den Rohdaten-Aufnahmen vom 28./29.10.2026.

Die echten Daten liegen nicht im Repository (Gesundheitsdaten). Nachgebildet sind die
Eigenschaften, die Fehler ausgelöst haben:
- Bewegung trieb den AD8232 an den Anschlag; im übersteuerten Abschnitt fand der Detektor
  Phantom-Schläge (HF-Spitze ~210/min, ein „verpasster Schlag“);
- eine abrupte Verlangsamung (718 -> 1090 ms, danach langsame Erholung) ergab in der
  RR-Klassifikation das Muster eines Extraschlags, obwohl kein Schlag zu früh kam;
- sehr starker 50-Hz-Brumm im Rohsignal (so groß wie das EKG) blieb unbemerkt, weil die
  Filter ihn vollständig entfernen;
- QTc nach Bazett lag bei 95/min rund 30 ms über Fridericia.
"""

from collections import Counter
from dataclasses import replace

import numpy as np
import pytest
from scipy import stats

from ekg import protocol as P
from ekg.analysis import analyse, quality_rows
from ekg.device.simulator import Scenario, SimulatedDevice, SyntheticEcg
from ekg.dsp.artifacts import classify_rr
from ekg.dsp.beats import Delineation, interval_rows, median_beat
from ekg.dsp.ectopy import confirm_ectopics
from ekg.dsp.filters import PRESETS, zero_phase
from ekg.dsp.hrv import compute_hrv, hrv_rows, nn_mask, rhythm_hints
from ekg.dsp.qrs import detect_qrs
from ekg.dsp.quality import Level
from ekg.dsp.respiration import estimate_breathing, qrs_amplitudes
from ekg.dsp.signal_quality import assess_recording, invalid_mask, mask_intervals
from ekg.pipeline import LivePipeline
from ekg.storage.recording import Recording
from ekg.units import Conversion

from .conftest import run_simulator

FS = 500
ARTIFACTS_S = (40.0, 80.0)


def clipped_recording(seed: int) -> tuple[Recording, np.ndarray]:
    """Ruhe-EKG mit zwei Bewegungsartefakten, die den Wandler an die Anschläge treiben."""
    g = SyntheticEcg(FS, scenario=Scenario.REST, seed=seed)
    mv = g.generate(FS * 120)
    t = np.arange(len(mv)) / FS
    for t0 in ARTIFACTS_S:
        sel = (t >= t0) & (t < t0 + 1.2)
        tt = t[sel] - t0
        mv[sel] += 2.5 * np.sin(2 * np.pi * 2.5 * tt) + 0.6 * np.sin(2 * np.pi * 9 * tt)
    conv = Conversion()
    raw = conv.mv_to_raw(mv).astype(np.float64)  # begrenzt auf 0 … raw_max wie der ADC
    rec = Recording(None, {}, FS, conv, raw, np.zeros(len(raw), dtype=np.uint8))
    return rec, np.array(g.r_times)


def n_phantom(r_s: np.ndarray, true_s: np.ndarray) -> int:
    return int(sum(np.min(np.abs(true_s - v)) > 0.06 for v in r_s))


@pytest.mark.parametrize("seed", [0, 3])
def test_clipped_motion_is_masked(seed):
    rec, true = clipped_recording(seed)
    # Ohne Ausblendung findet der Detektor im übersteuerten Abschnitt Phantom-Schläge.
    old = np.array([b.index for b in detect_qrs(rec.mv, FS)]) / FS
    assert n_phantom(old, true) >= 3
    assert np.max(60.0 / np.diff(old)) > 150

    an = analyse(rec)
    r = an.r_index / FS
    assert n_phantom(r, true) == 0
    assert len(an.invalid) == 2
    for (a, b), t0 in zip(an.invalid, ARTIFACTS_S, strict=True):
        assert a < t0 < t0 + 1.2 < b < t0 + 2.5
        assert not np.any((r >= a) & (r <= b))
    assert np.max(an.hr_bpm) < 100  # keine HF-Spitze über die Lücke hinweg
    assert 3.5 < an.quality.masked_s < 5.5
    assert any("ausgeblendet" in n for n in an.quality.notes)
    assert dict(quality_rows(an))["Ausgeblendet (Übersteuerung)"].endswith(" s")
    assert an.hrv is not None and an.hrv.n_missed == 0 and an.hrv.n_extra == 0


def test_invalid_mask_flags_and_margin():
    conv = Conversion()
    raw = np.full(FS * 10, conv.raw_max / 2)
    flags = np.zeros(len(raw), dtype=np.uint8)
    flags[2 * FS : 2 * FS + 10] = P.Flag.CLIP
    raw[6 * FS] = conv.raw_max  # einzelnes Sample am Anschlag
    flags[8 * FS :] = P.Flag.LO_PLUS
    m = invalid_mask(raw, flags, conv, FS)
    iv = mask_intervals(m, FS)
    assert iv[0] == pytest.approx((1.5, 2.52), abs=0.01)  # 0,5 s Rand um die Übersteuerung
    assert iv[1] == pytest.approx((5.5, 6.502), abs=0.01)
    assert iv[2] == pytest.approx((8.0, 10.0), abs=0.01)  # Lead-off ohne Rand


def test_live_pipeline_ignores_clipped_packets():
    dev = SimulatedDevice(Scenario.NORMAL, hr_bpm=70, realtime=False, gain_error=1.0)
    packets, _ = run_simulator(dev, 30.0)
    conv = Conversion(oversampling=64)
    pl = LivePipeline(500, conv)
    clip_from, clip_to = 15 * FS, 18 * FS
    for p in packets:
        if not isinstance(p, P.SamplesPacket):
            continue
        if clip_from <= p.first_index < clip_to:
            rail = np.full(len(p.samples), conv.raw_max, dtype=np.uint32)
            p = replace(p, flags=p.flags | P.Flag.CLIP, samples=rail)
        pl.push(p)
    idx = np.array([b.index for b in pl.all_beats()])
    assert not np.any((idx >= clip_from) & (idx < clip_to))
    assert np.any(idx > clip_to + 3 * FS)  # erholt sich danach
    assert pl.heart_rate() == pytest.approx(70, abs=4)


def test_pvcs_confirmed_by_timing_and_shape():
    g = SyntheticEcg(FS, scenario=Scenario.PVC, seed=2)
    x = zero_phase(g.generate(FS * 300), FS, PRESETS["monitor"][1])
    r = np.array([b.index for b in detect_qrs(x, FS)])
    mb = median_beat(x, r, FS)
    assert mb is not None
    _ok, cls = nn_mask(np.diff(r / FS))
    e = confirm_ectopics(x, r, FS, cls.ectopic, mb.template, mb.r_offset)
    n_true = sum(k % 6 == 5 for k, t in enumerate(g.r_times) if 1.0 < t < 299.0)
    assert e.count >= 0.9 * n_true
    assert len(e.other_qrs) >= 0.9 * e.count  # breiter QRS -> veränderte Form
    assert not e.not_premature
    hints = rhythm_hints(r / FS, ectopy=e)
    assert any("mit veränderter QRS-Form" in h for h in hints)


def abrupt_slowing_rr(seed: int) -> np.ndarray:
    """Gleichmäßiger Puls (~86/min) mit vier abrupten Verlangsamungen wie in der Aufnahme."""
    rng = np.random.default_rng(seed)
    rr = 0.70 + 0.008 * rng.normal(size=400)
    for k in (60, 160, 260, 360):
        rr[k : k + 6] = [1.09, 0.90, 0.957, 0.93, 0.836, 0.77]
    return rr


@pytest.mark.parametrize("seed", [0, 1])
def test_abrupt_slowing_is_not_an_ectopic(seed):
    t = np.concatenate([[0.0], np.cumsum(abrupt_slowing_rr(seed))])
    cls = classify_rr(np.diff(t))
    assert any(60 <= i <= 62 for i in cls.ectopic)  # die RR-Klassifikation allein irrt hier
    assert any("vorzeitige" in h for h in rhythm_hints(t))

    r = np.round(t * FS).astype(int)
    e = confirm_ectopics(np.zeros(r[-1] + FS), r, FS, cls.ectopic, None, 0)
    assert e.count == 0
    assert len(e.not_premature) >= 4
    assert not any("vorzeitige" in h for h in rhythm_hints(t, ectopy=e))
    h = compute_hrv(t)
    assert h is not None
    row = dict(hrv_rows(h, ectopy=e))["Schläge"]
    assert "RR-Sprung ohne Extraschlag" in row and "vorzeitig" not in row


def test_short_recording_is_marked():
    rr = 0.66 + 0.01 * np.random.default_rng(0).normal(size=200)
    t = np.concatenate([[0.0], np.cumsum(rr)])
    short = compute_hrv(t[:62])  # ~41 s wie die kurze Aufnahme
    assert short is not None
    assert "Aussagekraft HRV" in dict(hrv_rows(short))
    assert "Aussagekraft HRV" not in dict(hrv_rows(compute_hrv(t)))


@pytest.mark.parametrize(
    ("mains_mv", "note"),
    [(0.02, None), (0.1, "Erhöhter 50-Hz-Brumm"), (0.3, "Sehr starker 50-Hz-Brumm")],
)
def test_mains_hum_is_reported(mains_mv, note):
    mv = SyntheticEcg(FS, seed=3, mains_mv=mains_mv).generate(FS * 60)
    q = assess_recording(mv, np.zeros(len(mv), dtype=np.uint8), np.zeros(len(mv), bool), FS)
    assert q.mains_uv == pytest.approx(mains_mv / np.sqrt(2) * 1000, rel=0.1)
    if note is None:
        assert not q.notes
    else:
        assert len(q.notes) == 1 and q.notes[0].startswith(note)
    if mains_mv > 0.2:
        assert "Netzteil" in q.notes[0]


def power_recording(start: str | None, changes: tuple[str, ...] = ()) -> Recording:
    """60 s Ruhe-EKG mit starkem Brumm; Stromversorgung wie von der Oberfläche gespeichert."""
    conv = Conversion()
    mv = SyntheticEcg(FS, scenario=Scenario.REST, seed=1, mains_mv=0.3).generate(FS * 60)
    raw = conv.mv_to_raw(mv).astype(np.float64)
    header = {"power": {"state": start, "detail": ""}} if start else {}
    events = [(FS * 20 * (k + 1), {"type": "power", "state": s}) for k, s in enumerate(changes)]
    flags = np.zeros(len(raw), dtype=np.uint8)
    return Recording(None, header, FS, conv, raw, flags, events=events)


def signal_notes(an) -> list[str]:
    return [n for n in an.notes if n.startswith("Signal: ")]


def test_power_state_explains_mains_hum():
    an = analyse(power_recording("battery"))
    assert dict(quality_rows(an))["Laptop-Stromversorgung"] == "Akku"
    assert "obwohl der Laptop auf Akku lief" in signal_notes(an)[0]

    an = analyse(power_recording("battery", ("external", "battery")))
    row = dict(quality_rows(an))["Laptop-Stromversorgung"]
    assert row == "Akku → Netzteil angeschlossen → Akku"
    assert signal_notes(an)[0].startswith(
        "Signal: Der Laptop hing während der Aufnahme am Netzteil"
    )

    old = analyse(power_recording(None))  # Aufnahmen vor 0.2.1 enthalten die Angabe nicht
    assert "Laptop-Stromversorgung" not in dict(quality_rows(old))
    assert "nicht am Netzteil hing" in signal_notes(old)[0]


def breathing_for(seed: int, amp_mod: float, extra_amp_hz: float | None = None) -> float | None:
    g = SyntheticEcg(FS, scenario=Scenario.REST, seed=seed)
    g.amp_mod = amp_mod
    v = g.generate(FS * 600)
    if extra_amp_hz is not None:  # Amplitudenschwankung ohne Gegenstück im RR-Spektrum
        v *= 1.0 + 0.1 * np.sin(2 * np.pi * extra_amp_hz * np.arange(len(v)) / FS)
    x = zero_phase(v, FS, PRESETS["monitor"][1])
    r = np.array([b.index for b in detect_qrs(x, FS)])
    return estimate_breathing(r / FS, qrs_amplitudes(x, r, FS))


def test_breathing_needs_agreement_of_amplitude_and_rr():
    assert breathing_for(5, 0.08) == pytest.approx(0.18, abs=0.02)
    assert breathing_for(5, 0.0) is None  # R-Amplitude ohne Atemsignal
    assert breathing_for(5, 0.0, extra_amp_hz=0.33) is None  # Amplitude und RR uneinig


def test_interval_rows_prefer_fridericia():
    d = Delineation(
        baseline_mv=0.0,
        p_onset=None,
        p_peak=None,
        qrs_onset=0,
        qrs_offset=48,
        t_peak=None,
        t_end=None,
        pr_ms=178.0,
        qrs_ms=96.0,
        qt_ms=370.0,
        qtc_bazett_ms=370.0 / np.sqrt(0.64),
        qtc_fridericia_ms=370.0 / np.cbrt(0.64),
        p_amp_mv=None,
        r_amp_mv=0.6,
        t_amp_mv=None,
    )
    rows = interval_rows(d, 0.64)  # 94/min
    labels = [k for k, _ in rows]
    assert labels.index("QTc Fridericia") < labels.index("QTc Bazett")
    values = dict(rows)
    assert values["QTc Fridericia"] == "429 ms"
    assert values["QTc Bazett"] == "462 ms (überschätzt, HF > 80/min)"
    slow = dict(interval_rows(d, 1.0, " (exp.)"))
    assert slow["QTc Bazett (exp.)"] == "462 ms"  # Hinweis nur bei hoher Herzfrequenz


# Schlagform wie in der Aufnahme vom 29.10. 07:28: kleine R-Zacke, tiefe S-Zacke, große P- und
# T-Welle. Kurtosis und Formvergleich auf dem Anzeigesignal meldeten dort in bis zu 2/3 der
# Fenster „Signal verrauscht“ bzw. „Schlagform wechselnd“, obwohl das Signal sauber war.
LOW_R_WAVES = (
    (-0.180, 0.20, 0.025, False),  # P
    (-0.090, -0.05, 0.030, False),  # Ta
    (-0.020, -0.04, 0.008, False),  # Q
    (0.000, 0.55, 0.010, False),  # R
    (0.025, -0.38, 0.011, False),  # S
    (0.270, 0.27, 0.060, True),  # T
)


def live_quality(mv: np.ndarray) -> tuple[Counter, Counter, list[float]]:
    """Signal wie vom Gerät durch die Live-Pipeline; Ampel alle 0,5 s (nach 6 s Anlauf)."""
    conv = Conversion(oversampling=64)
    raw = conv.mv_to_raw(mv)
    pl = LivePipeline(FS, conv)
    levels, reasons, old_kurtosis = Counter(), Counter(), []
    for k, i in enumerate(range(0, len(raw) - 25, 25)):
        pl.push(P.SamplesPacket(k, i, P.Flag(0), raw[i : i + 25]))
        if i % (FS // 2) == 0 and i > 6 * FS:
            pl._update_quality()
            levels[pl.quality.level] += 1
            reasons.update(pl.quality.reasons)
            x = pl.display.get(pl.display.end - 4 * FS, pl.display.end)
            old_kurtosis.append(float(stats.kurtosis(x, fisher=False)))
    return levels, reasons, old_kurtosis


def test_live_quality_accepts_small_r_wave():
    g = SyntheticEcg(FS, hr_bpm=80, seed=2, amp_mod=0.1, waves=LOW_R_WAVES)
    levels, reasons, old_k = live_quality(g.generate(FS * 60))
    assert np.mean(np.array(old_k) < 5) > 0.3  # frühere Kennzahl: oft „verrauscht“
    assert levels[Level.GOOD] >= 0.9 * sum(levels.values()), reasons
    assert not reasons["Signal verrauscht"] and not reasons["kein EKG-typisches Signal"]


def test_live_quality_names_mains_hum_as_the_only_problem():
    g = SyntheticEcg(FS, hr_bpm=80, seed=2, amp_mod=0.1, mains_mv=0.57, waves=LOW_R_WAVES)
    levels, reasons, _ = live_quality(g.generate(FS * 60))  # wie 07:28: 406 µV eff.
    n = sum(levels.values())
    assert levels[Level.POOR] == n
    assert reasons["starker 50-Hz-Brumm – Kontakt/Kabelführung prüfen"] == n
    assert sum(reasons.values()) <= 1.1 * n  # kaum andere Gründe


def test_live_quality_still_flags_noise_and_motion():
    rng = np.random.default_rng(0)
    levels, reasons, _ = live_quality(0.05 * rng.normal(size=FS * 30))
    assert levels[Level.POOR] == sum(levels.values())
    assert reasons["kein EKG-typisches Signal"] >= 0.9 * sum(levels.values())

    mv = SyntheticEcg(FS, hr_bpm=70, seed=6).generate(FS * 40)
    t = np.arange(len(mv)) / FS
    sel = (t >= 20) & (t < 23)  # Bewegung: langsame Schwingung plus Zittern
    mv[sel] += 0.6 * np.sin(2 * np.pi * 2.5 * (t[sel] - 20)) + 0.15 * rng.normal(size=sel.sum())
    levels, reasons, _ = live_quality(mv)
    assert reasons["Schlagform wechselnd"] + reasons["Schlagform instabil – Artefakte"] >= 5
