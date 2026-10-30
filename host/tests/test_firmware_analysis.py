"""Die Auswertung im Gerät (firmware/src/analysis) rechnet wie ekg.analysis am Laptop.

Beide bekommen dieselbe 2-min-Aufnahme im Format des Geräts (Rohsumme / 4 als 16 Bit).
Das Gerät speichert das gefilterte Signal mit 0,1 µV Auflösung; deshalb gelten für
Kurvenwerte kleine Toleranzen, Zählungen und Hinweise müssen genau stimmen.
"""

import ctypes
import re
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest

from ekg.analysis import analyse
from ekg.device.simulator import Ad8232Model, Scenario, SyntheticEcg
from ekg.dsp.respiration import estimate_breathing_window, qrs_amplitudes
from ekg.protocol import Flag
from ekg.storage.recording import Recording
from ekg.units import Conversion

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "firmware" / "src"
FS = 500
BLOCK = 25
SCRATCH = 320 * 240 * 2  # im Gerät der Bildspeicher

SHIM = r"""
#include <stdio.h>
#include <string.h>
#include "ana.h"

static ana_recording_t rec;
static ana_result_t res;
static ana_breath_t br;
static qrs_detector_t det;
static unsigned char scratch[SCRATCH];

#define PUT(fmt, ...) do { int w_ = snprintf(text + used, (size_t)(cap - used), fmt "\n", \
    __VA_ARGS__); if (w_ > 0) used += w_; if (used >= cap) used = cap - 1; } while (0)

int t_run(const unsigned short *raw4, const unsigned char *flags, int n_blocks, int power,
          double *r_index, double *tmpl, char *text, int cap) {
    memcpy(rec.raw4, raw4, sizeof(unsigned short) * (size_t)n_blocks * ANA_BLOCK);
    memcpy(rec.flags, flags, (size_t)n_blocks);
    rec.n_blocks = (unsigned)n_blocks;
    int st = ana_run(&rec, (ana_power_t)power, &det, scratch, sizeof scratch, &res);
    int used = 0;
    text[0] = 0;
    PUT("status=%d", st);
    PUT("peak=%u", res.scratch_peak);
    PUT("lead_off_s=%.17g", res.lead_off_s);
    PUT("gap_s=%.17g", res.gap_s);
    PUT("clipped_s=%.17g", res.clipped_s);
    PUT("masked_s=%.17g", res.masked_s);
    PUT("has_mains=%d", res.has_mains);
    PUT("mains_uv=%.17g", res.mains_uv);
    PUT("has_mains_ratio=%d", res.has_mains_ratio);
    PUT("mains_ratio=%.17g", res.mains_ratio);
    PUT("n_beats=%d", res.n_beats);
    PUT("has_hrv=%d", res.has_hrv);
    const ana_hrv_t *h = &res.hrv;
    PUT("hrv.n_beats=%d", h->n_beats);
    PUT("hrv.n_excluded=%d", h->n_excluded);
    PUT("hrv.n_ectopic=%d", h->n_ectopic);
    PUT("hrv.n_missed=%d", h->n_missed);
    PUT("hrv.n_extra=%d", h->n_extra);
    PUT("hrv.duration_s=%.17g", h->duration_s);
    PUT("hrv.mean_rr_ms=%.17g", h->mean_rr_ms);
    PUT("hrv.mean_hr=%.17g", h->mean_hr);
    PUT("hrv.min_hr=%.17g", h->min_hr);
    PUT("hrv.max_hr=%.17g", h->max_hr);
    PUT("hrv.sdnn_ms=%.17g", h->sdnn_ms);
    PUT("hrv.rmssd_ms=%.17g", h->rmssd_ms);
    PUT("hrv.pnn50=%.17g", h->pnn50);
    PUT("hrv.sd1_ms=%.17g", h->sd1_ms);
    PUT("hrv.sd2_ms=%.17g", h->sd2_ms);
    PUT("hrv.has_lf=%d", h->has_lf);
    PUT("hrv.lf_ms2=%.17g", h->lf_ms2);
    PUT("hrv.hf_ms2=%.17g", h->hf_ms2);
    PUT("hrv.has_lf_hf=%d", h->has_lf_hf);
    PUT("hrv.lf_hf=%.17g", h->lf_hf);
    PUT("hrv.has_peak=%d", h->has_peak);
    PUT("hrv.peak_hz=%.17g", h->peak_hz);
    PUT("ect.normal=%d", res.ect_normal_qrs);
    PUT("ect.other=%d", res.ect_other_qrs);
    PUT("ect.not_premature=%d", res.ect_not_premature);
    PUT("has_median=%d", res.has_median);
    PUT("median_beats=%d", res.median_beats);
    PUT("mean_rr_s=%.17g", res.mean_rr_s);
    PUT("has_delineation=%d", res.has_delineation);
    const ana_delineation_t *d = &res.d;
    PUT("d.p_onset=%d", d->p_onset);
    PUT("d.p_peak=%d", d->p_peak);
    PUT("d.qrs_onset=%d", d->qrs_onset);
    PUT("d.qrs_offset=%d", d->qrs_offset);
    PUT("d.t_peak=%d", d->t_peak);
    PUT("d.t_end=%d", d->t_end);
    PUT("d.pr_ms=%.17g", d->pr_ms);
    PUT("d.qrs_ms=%.17g", d->qrs_ms);
    PUT("d.qt_ms=%.17g", d->qt_ms);
    PUT("d.qtc_fridericia_ms=%.17g", d->qtc_fridericia_ms);
    PUT("d.qtc_bazett_ms=%.17g", d->qtc_bazett_ms);
    PUT("d.p_amp_mv=%.17g", d->p_amp_mv);
    PUT("d.r_amp_mv=%.17g", d->r_amp_mv);
    PUT("d.t_amp_mv=%.17g", d->t_amp_mv);
    PUT("rhythm.valid=%d", res.rhythm.valid);
    PUT("rhythm.median_hr=%.17g", res.rhythm.median_hr);
    PUT("rhythm.has_irr=%d", res.rhythm.has_irr);
    PUT("rhythm.windows=%d", res.rhythm.windows);
    PUT("rhythm.irregular_windows=%d", res.rhythm.irregular_windows);
    PUT("rhythm.n_ect=%d", res.rhythm.n_ect);
    PUT("rhythm.n_err=%d", res.rhythm.n_err);
    PUT("rhythm.resp=%d", res.rhythm.resp_variation);
    for (int i = 0; i < res.n_hints; i++) PUT("hint=%s", res.hints[i]);
    for (int i = 0; i < res.n_qnotes; i++) PUT("qnote=%s", res.qnotes[i]);
    for (int i = 0; i < res.n_beats; i++) r_index[i] = res.r_index[i];
    for (int i = 0; i < ANA_TMPL_LEN; i++) tmpl[i] = res.tmpl[i];
    if (st == 0) {
        ana_breath(&rec, &res, &br, scratch, sizeof scratch);
        PUT("br.why=%d", br.why);
        PUT("br.has_edr=%d", br.has_edr);
        PUT("br.edr_hz=%.17g", br.edr_hz);
        PUT("br.rate_hz=%.17g", br.rate_hz);
        PUT("br.has_swing=%d", br.has_swing);
        PUT("br.swing_bpm=%.17g", br.swing_bpm);
        PUT("br.n_cycles=%d", br.n_cycles);
        PUT("br.cycle_cv=%.17g", br.cycle_cv);
        PUT("br.curve_n=%d", br.curve_n);
    }
    return st;
}
"""


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("kein C-Compiler")
    d = tmp_path_factory.mktemp("ana")
    shim = d / "shim.c"
    shim.write_text(SHIM)
    so = d / "libana.so"
    srcs = [*sorted((SRC / "analysis").glob("*.c")), SRC / "ecg" / "qrs.c"]
    subprocess.run(
        [cc, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation",
         "-shared", "-fPIC", f"-DSCRATCH={SCRATCH}", f"-I{SRC}", f"-I{SRC / 'ecg'}", f"-I{SRC / 'analysis'}",
         str(shim), *map(str, srcs), "-o", str(so), "-lm"],
        check=True,
    )  # fmt: skip
    lib = ctypes.CDLL(str(so))
    lib.t_run.restype = ctypes.c_int
    return lib


def make_recording(
    scenario: Scenario,
    seconds: float = 120.0,
    seed: int = 1,
    hr: float = 70.0,
    mains_mv: float = 0.04,
    drop_blocks: tuple[int, ...] = (),
    ecg_kw: dict[str, float] | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """Aufnahme wie im Gerät: (raw4 je Sample, Flags je Block)."""
    conv = Conversion(oversampling=64)
    ecg = SyntheticEcg(FS, hr, scenario, seed=seed, mains_mv=mains_mv)
    for key, value in (ecg_kw or {}).items():
        setattr(ecg, key, value)
    model = Ad8232Model(FS, conv, 1100.0 * 1.03, seed=seed + 2)
    n_blocks = int(round(seconds * FS / BLOCK))
    raw4 = np.zeros(n_blocks * BLOCK, dtype=np.uint16)
    flags = np.zeros(n_blocks, dtype=np.uint8)
    for b in range(n_blocks):
        t = b * BLOCK / FS
        lead_off = scenario == Scenario.LEAD_OFF and 20.0 < (t % 40.0) < 25.0
        raw = model.process(ecg.generate(BLOCK), lead_off=lead_off).astype(np.int64)
        fl = Flag.LO_PLUS if lead_off else 0
        if np.any((raw <= 8 * 64) | (raw >= 4087 * 64)):
            fl |= Flag.CLIP
        raw4[b * BLOCK : (b + 1) * BLOCK] = (raw + 2) // 4
        flags[b] = fl
    for b in drop_blocks:
        raw4[b * BLOCK : (b + 1) * BLOCK] = 0xFFFF
        flags[b] = Flag.OVERRUN
    return raw4, flags


def python_analysis(raw4: np.ndarray, flags: np.ndarray):
    raw = raw4.astype(np.float64) * 4.0
    raw[raw4 == 0xFFFF] = np.nan
    per_sample = np.repeat(flags, BLOCK).astype(np.uint8)
    rec = Recording(None, {}, float(FS), Conversion(oversampling=64), raw, per_sample)
    return analyse(rec)


def c_analysis(lib, raw4: np.ndarray, flags: np.ndarray, power: int = 0):
    r_index = np.zeros(1000)
    tmpl = np.zeros(475)
    text = ctypes.create_string_buffer(1 << 16)
    raw = np.ascontiguousarray(raw4, dtype=np.uint16)
    fl = np.ascontiguousarray(flags, dtype=np.uint8)
    st = lib.t_run(
        raw.ctypes.data_as(ctypes.POINTER(ctypes.c_ushort)),
        fl.ctypes.data_as(ctypes.POINTER(ctypes.c_ubyte)),
        ctypes.c_int(len(fl)),
        ctypes.c_int(power),
        r_index.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
        tmpl.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
        text,
        ctypes.c_int(len(text)),
    )
    out: dict[str, object] = {"hint": [], "qnote": []}
    for line in text.value.decode().splitlines():
        key, _, value = line.partition("=")
        if key in ("hint", "qnote"):
            out[key].append(value)  # type: ignore[union-attr]
        else:
            out[key] = float(value) if any(c in value for c in ".en") else int(value)
    out["r_index"] = r_index[: int(out["n_beats"])]
    out["tmpl"] = tmpl
    out["status"] = st
    return out


def close(a: float | None, b: float, rel: float = 1e-3, abs_: float = 1e-6) -> bool:
    if a is None:
        return not np.isfinite(b)
    if not np.isfinite(a):
        return not np.isfinite(b)
    return abs(a - b) <= max(abs_, rel * abs(a))


CASES = [
    ("rest", Scenario.REST, {}),
    ("normal", Scenario.NORMAL, {"seed": 2}),
    ("pvc", Scenario.PVC, {}),
    ("af", Scenario.AF, {"seed": 3}),
    ("brady", Scenario.BRADY, {}),
    ("tachy", Scenario.TACHY, {}),
    ("noisy", Scenario.NOISY, {"seed": 5}),
    ("lead_off", Scenario.LEAD_OFF, {"seconds": 60.0}),
    ("mains", Scenario.REST, {"mains_mv": 0.5, "seed": 7}),
    ("gaps", Scenario.NORMAL, {"drop_blocks": (100, 101, 102, 900, 1500)}),
    ("short", Scenario.NORMAL, {"seconds": 45.0, "seed": 4}),
]


@pytest.mark.parametrize(("name", "scenario", "kw"), CASES, ids=[c[0] for c in CASES])
def test_analysis_matches_python(lib, name, scenario, kw):
    raw4, flags = make_recording(scenario, **kw)
    py = python_analysis(raw4, flags)
    c = c_analysis(lib, raw4, flags)
    assert c["status"] == 0, c
    assert c["peak"] <= SCRATCH

    # Signalqualität
    assert c["lead_off_s"] == pytest.approx(py.lead_off_s)
    assert c["gap_s"] == pytest.approx(py.gap_s)
    assert c["clipped_s"] == pytest.approx(py.quality.clipped_s)
    assert c["masked_s"] == pytest.approx(py.quality.masked_s)
    q = py.quality
    assert bool(c["has_mains"]) == (q.mains_uv is not None)
    if q.mains_uv is not None:
        assert close(q.mains_uv, c["mains_uv"], rel=1e-3)
        assert close(q.mains_ratio, c["mains_ratio"], rel=1e-3)
    assert c["qnote"] == q.notes

    # Schläge
    r_py = py.r_index
    assert c["n_beats"] == len(r_py)
    # Die Parabel durch die Spitze reagiert auf die 0,1-µV-Stufen: bis 0,05 Samples (0,1 ms)
    np.testing.assert_allclose(c["r_index"], r_py, atol=0.05)

    # HRV
    h = py.hrv
    assert bool(c["has_hrv"]) == (h is not None)
    if h is not None:
        for f in ("n_beats", "n_excluded", "n_ectopic", "n_missed", "n_extra"):
            assert c[f"hrv.{f}"] == getattr(h, f), f
        for f in ("duration_s", "mean_rr_ms", "mean_hr", "min_hr", "max_hr", "sdnn_ms",
                  "rmssd_ms", "pnn50", "sd1_ms", "sd2_ms"):  # fmt: skip
            assert close(getattr(h, f), c[f"hrv.{f}"], rel=1e-4, abs_=1e-3), f
        assert bool(c["hrv.has_lf"]) == (h.lf_ms2 is not None)
        if h.lf_ms2 is not None:
            assert close(h.lf_ms2, c["hrv.lf_ms2"], rel=2e-3, abs_=1e-2)
            assert close(h.hf_ms2, c["hrv.hf_ms2"], rel=2e-3, abs_=1e-2)
        assert bool(c["hrv.has_lf_hf"]) == (h.lf_hf is not None)
        if h.lf_hf is not None:
            assert close(h.lf_hf, c["hrv.lf_hf"], rel=2e-3)
        assert bool(c["hrv.has_peak"]) == (h.peak_hz is not None)
        if h.peak_hz is not None:
            assert c["hrv.peak_hz"] == pytest.approx(h.peak_hz)

    # Extraschläge und Hinweise
    e = py.ectopy
    assert (c["ect.normal"], c["ect.other"], c["ect.not_premature"]) == (
        len(e.normal_qrs),
        len(e.other_qrs),
        len(e.not_premature),
    )
    assert c["hint"] == py.hints

    # Rhythmus in Zahlen (Grundlage der Check-Liste des Geräts) passt zu den Hinweistexten
    for hint in py.hints:
        if m := re.match(r"(\d+) vorzeitige Schläge", hint):
            assert c["rhythm.n_ect"] == int(m.group(1))
        if m := re.match(r"Herzfrequenz (?:niedrig|hoch) \((\d+)/min\)", hint):
            assert round(c["rhythm.median_hr"]) == int(m.group(1))
        if "über weite Strecken zufällig" in hint:
            assert c["rhythm.has_irr"] and 2 * c["rhythm.irregular_windows"] >= c["rhythm.windows"]
        elif "zeitweise zufällig" in hint:
            assert (
                0 < c["rhythm.irregular_windows"]
                and 2 * c["rhythm.irregular_windows"] < c["rhythm.windows"]
            )
        if hint.startswith("Herzfrequenz schwankt stark"):
            assert c["rhythm.resp"] == 1
        if "vermutete Erkennungsfehler" in hint:
            assert c["rhythm.n_err"] >= 3
    assert bool(c["rhythm.resp"]) == any(h.startswith("Herzfrequenz schwankt") for h in py.hints)
    if py.hints:
        assert c["rhythm.valid"] == 1

    # Median-Schlag und Vermessung
    mb, dl = py.median, py.delineation
    assert bool(c["has_median"]) == (mb is not None)
    if mb is not None:
        assert c["median_beats"] == mb.n_beats
        assert close(mb.mean_rr_s, c["mean_rr_s"], rel=1e-6)
        # Liegt eine R-Zacke genau zwischen zwei Samples, rundet ein Schlag anders und sein
        # Ausschnitt verschiebt sich um ein Sample: an steilen Stellen bis ~10 µV, sonst < 0,1 µV.
        diff = np.abs(c["tmpl"] - mb.template)
        assert diff.max() < 0.015
        assert np.median(diff) < 1e-4
        assert bool(c["has_delineation"]) == (dl is not None)
        for f in ("qrs_onset", "qrs_offset", "t_peak", "t_end", "p_onset", "p_peak"):
            v = getattr(dl, f)
            got = c[f"d.{f}"]
            if v is None:
                assert got == -1, f
            else:
                assert abs(got - v) <= 1, (f, got, v)
        for f in ("pr_ms", "qrs_ms", "qt_ms", "qtc_fridericia_ms", "qtc_bazett_ms"):
            v = getattr(dl, f)
            got = c[f"d.{f}"]
            if v is None:
                assert not np.isfinite(got), f
            else:
                assert abs(got - v) <= 2.5, (f, got, v)
        for f in ("p_amp_mv", "r_amp_mv", "t_amp_mv"):
            v = getattr(dl, f)
            got = c[f"d.{f}"]
            if v is None:
                assert not np.isfinite(got), f
            else:
                assert abs(got - v) <= 5e-3, (f, got, v)


def test_too_short(lib):
    raw4, flags = make_recording(Scenario.NORMAL, seconds=20.0)
    assert c_analysis(lib, raw4, flags)["status"] == 1


def test_battery_note(lib):
    """Auf Batterie gilt starker Brumm als Kontaktproblem, nicht als Netzteil-Frage."""
    raw4, flags = make_recording(Scenario.REST, mains_mv=1.0, seconds=40.0)
    c = c_analysis(lib, raw4, flags, power=1)
    assert any("obwohl das Gerät auf Batterie lief" in n for n in c["qnote"])


BREATH_CASES = [
    ("0.25 Hz", {"resp_hz": 0.25, "rsa": 0.05, "amp_mod": 0.15}, 1),
    ("0.15 Hz", {"resp_hz": 0.15, "rsa": 0.06, "amp_mod": 0.15}, 2),
    ("0.33 Hz", {"resp_hz": 0.33, "rsa": 0.05, "amp_mod": 0.18}, 3),
    ("ohne Atmung", {"rsa": 0.0, "amp_mod": 0.0}, 4),
    ("ohne Pulsschwankung", {"resp_hz": 0.3, "rsa": 0.0, "amp_mod": 0.2}, 5),
]


@pytest.mark.parametrize(("name", "ecg_kw", "seed"), BREATH_CASES, ids=[c[0] for c in BREATH_CASES])
def test_breathing_matches_python(lib, name, ecg_kw, seed):
    raw4, flags = make_recording(Scenario.NORMAL, seed=seed, ecg_kw=ecg_kw)
    py = python_analysis(raw4, flags)
    amp = qrs_amplitudes(py.filtered_mv, py.r_index, FS)
    rate, edr = estimate_breathing_window(py.r_index / FS, amp, py.invalid)
    c = c_analysis(lib, raw4, flags)
    assert (c["br.why"] == 0) == (rate is not None)
    assert bool(c["br.has_edr"]) == (edr is not None)
    if edr is not None:
        assert c["br.edr_hz"] == pytest.approx(edr, abs=0.003)
    if rate is not None:
        assert c["br.rate_hz"] == pytest.approx(rate, abs=0.003)
        assert c["br.rate_hz"] == pytest.approx(ecg_kw.get("resp_hz", 0.25), abs=0.02)
    if name == "ohne Atmung":
        assert c["br.why"] != 0  # keine Atmung: kein Wert (Randspitze bei 0,48 Hz wird verworfen)
    if c["br.why"] == 0:
        assert c["br.has_swing"] and c["br.swing_bpm"] > 0.5
        assert c["br.curve_n"] > 440
        # Zyklen: ungefähr Atemfrequenz × Dauer
        assert abs(c["br.n_cycles"] - ecg_kw.get("resp_hz", 0.25) * 118) <= 4
        assert 0 <= c["br.cycle_cv"] < 0.25
