"""Die EKG-Verarbeitung der Firmware (firmware/src/ecg) rechnet wie die Host-Software.

Der C-Code wird für den Test als Bibliothek übersetzt und blockweise (25 Samples wie im
Gerät) mit denselben Daten gefüttert wie die Python-Implementierung.
"""

import ctypes
import importlib.util
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest

from ekg.device.simulator import Scenario, SyntheticEcg
from ekg.dsp.filters import PRESETS, StreamingSos, design_sos
from ekg.dsp.hr import HeartRateTracker
from ekg.dsp.qrs import QrsDetector

ROOT = Path(__file__).resolve().parents[2]
ECG_SRC = ROOT / "firmware" / "src" / "ecg"
FS = 500
BLOCK = 25

SHIM = r"""
#include <stdlib.h>
#include "qrs.h"
#include "hr.h"
#include "sosf.h"

void *t_qrs_new(void) { qrs_detector_t *d = malloc(sizeof *d); qrs_init(d); return d; }
void t_qrs_free(void *d) { free(d); }
void t_qrs_reset(void *d, long long start) { qrs_reset(d, start); }
int t_qrs_init_done(void *d) { return qrs_initialized(d); }
int t_qrs_process(void *d, const double *x, int n, double *idx, double *amp, int *sb, int max) {
    qrs_beat_t out[16];
    size_t k = qrs_process(d, x, (size_t)n, out, 16);
    for (size_t i = 0; i < k && (int)i < max; i++) {
        idx[i] = out[i].index; amp[i] = out[i].amplitude_mv; sb[i] = out[i].searchback;
    }
    return (int)k;
}
void *t_hr_new(double fs) { hr_tracker_t *h = malloc(sizeof *h); hr_init(h, fs); return h; }
void t_hr_add(void *h, double r) { hr_add(h, r); }
int t_hr_bpm(void *h, double now, double *bpm) { return hr_bpm(h, now, bpm); }
void t_sosf(const float *x, float *y, int n) {
    sosf_t f; sosf_reset(&f);
    for (int i = 0; i < n; i++) y[i] = sosf_step(&f, x[i]);
}
"""


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("kein C-Compiler")
    d = tmp_path_factory.mktemp("ecg")
    shim = d / "shim.c"
    shim.write_text(SHIM)
    so = d / "libecg.so"
    srcs = [str(ECG_SRC / f) for f in ("qrs.c", "hr.c", "sosf.c")]
    subprocess.run(
        [cc, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
         f"-I{ECG_SRC}", str(shim), *srcs, "-o", str(so), "-lm"],
        check=True,
    )  # fmt: skip
    lib = ctypes.CDLL(str(so))
    vp, dp, ip = ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_int)
    lib.t_qrs_new.restype = vp
    lib.t_qrs_free.argtypes = [vp]
    lib.t_qrs_reset.argtypes = [vp, ctypes.c_longlong]
    lib.t_qrs_init_done.argtypes = [vp]
    lib.t_qrs_process.argtypes = [vp, dp, ctypes.c_int, dp, dp, ip, ctypes.c_int]
    lib.t_hr_new.restype = vp
    lib.t_hr_new.argtypes = [ctypes.c_double]
    lib.t_hr_add.argtypes = [vp, ctypes.c_double]
    lib.t_hr_bpm.argtypes = [vp, ctypes.c_double, dp]
    fp = ctypes.POINTER(ctypes.c_float)
    lib.t_sosf.argtypes = [fp, fp, ctypes.c_int]
    return lib


def test_tables_are_current():
    spec = importlib.util.spec_from_file_location("gen", ROOT / "tools" / "gen_ecg_tables.py")
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    assert (ECG_SRC / "ecg_tables.h").read_text(encoding="utf-8") == gen.render()


def run_c(lib, x, resets=()):
    d = lib.t_qrs_new()
    idx = (ctypes.c_double * 16)()
    amp = (ctypes.c_double * 16)()
    sb = (ctypes.c_int * 16)()
    beats = []
    try:
        for start in range(0, len(x), BLOCK):
            if start in resets:
                lib.t_qrs_reset(d, start)
            chunk = np.ascontiguousarray(x[start : start + BLOCK], dtype=np.float64)
            k = lib.t_qrs_process(
                d, chunk.ctypes.data_as(ctypes.POINTER(ctypes.c_double)), len(chunk),
                idx, amp, sb, 16,
            )  # fmt: skip
            beats += [(idx[i], amp[i], bool(sb[i])) for i in range(k)]
    finally:
        lib.t_qrs_free(d)
    return beats


def run_py(x, resets=()):
    det = QrsDetector(FS)
    beats = []
    for start in range(0, len(x), BLOCK):
        if start in resets:
            det.reset(start)
        beats += [
            (b.index, b.amplitude_mv, b.searchback) for b in det.process(x[start : start + BLOCK])
        ]
    return beats


SIGNALS = {
    "normal": dict(scenario=Scenario.NORMAL, hr_bpm=72, mains_mv=0.05),
    "rest": dict(scenario=Scenario.REST),
    "pvc": dict(scenario=Scenario.PVC, hr_bpm=65),
    "af": dict(scenario=Scenario.AF),
    "tachy": dict(scenario=Scenario.TACHY),
    "brady": dict(scenario=Scenario.BRADY),
    "noisy": dict(scenario=Scenario.NOISY, mains_mv=0.1),
}


@pytest.mark.parametrize("name", sorted(SIGNALS))
def test_qrs_matches_python(lib, name):
    x = SyntheticEcg(FS, seed=11, **SIGNALS[name]).generate(FS * 90)
    py = run_py(x)
    c = run_c(lib, x)
    assert len(py) > 40
    assert len(c) == len(py)
    for (ic, ac, sc), (ip_, ap, sp) in zip(c, py, strict=True):
        assert ic == pytest.approx(ip_, abs=1e-6)
        assert ac == pytest.approx(ap, abs=1e-9)
        assert sc == sp


def test_qrs_reset_and_real_scale(lib):
    # kleine R-Zacke wie in den Aufnahmen vom 29.10. (0,5 mV) und Neustart mitten im Signal
    g = SyntheticEcg(FS, scenario=Scenario.REST, seed=3, amplitude=0.7)
    x = g.generate(FS * 60)
    x[FS * 30 : FS * 31] += 2.0  # Artefakt, danach Neustart wie nach Lead-off
    resets = {FS * 31}
    py, c = run_py(x, resets), run_c(lib, x, resets)
    assert len(c) == len(py) > 40
    np.testing.assert_allclose([b[0] for b in c], [b[0] for b in py], atol=1e-6)


def test_heart_rate_matches_python(lib):
    g = SyntheticEcg(FS, scenario=Scenario.REST, seed=5)
    x = g.generate(FS * 40)
    beats = run_py(x)
    h_py = HeartRateTracker(FS)
    h_c = lib.t_hr_new(FS)
    bpm = ctypes.c_double()
    for i, (r, _a, _s) in enumerate(beats):
        h_py.add(r)
        lib.t_hr_add(h_c, r)
        for now in (r + 10, r + FS * 5):  # frisch und veraltet
            expected = h_py.bpm(now)
            ok = lib.t_hr_bpm(h_c, now, ctypes.byref(bpm))
            assert bool(ok) == (expected is not None), (i, now)
            if expected is not None:
                assert bpm.value == pytest.approx(expected, rel=1e-12)


def test_display_filter_matches_live_view(lib):
    x = SyntheticEcg(FS, scenario=Scenario.REST, seed=2, mains_mv=0.2).generate(FS * 20) + 0.8
    ref = StreamingSos(design_sos(PRESETS["monitor"][1], FS)).process(x)
    xf = np.ascontiguousarray(x, dtype=np.float32)
    y = np.zeros_like(xf)
    fp = ctypes.POINTER(ctypes.c_float)
    lib.t_sosf(xf.ctypes.data_as(fp), y.ctypes.data_as(fp), len(xf))
    np.testing.assert_allclose(y, ref, atol=2e-3)  # float32 im Gerät
