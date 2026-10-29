"""Atemfrequenz aus dem EKG (EDR, ECG-derived respiration) – nur wenn sie belastbar ist.

Beim Atmen ändern sich die Lage des Herzens und der Widerstand des Brustkorbs; die
QRS-Amplitude schwankt deshalb im Atemrhythmus. Das ist unabhängig von der
atemabhängigen Herzfrequenz-Schwankung und dient als Gegenprobe: Liegt die stärkste
HF-Schwankung bei ~0,1 Hz, die Amplituden-Schwankung aber bei 0,2 Hz, stammt die
langsame Schwankung eher von der Blutdruckregulation (Mayer-Wellen) als von der Atmung.

Nur eine Schätzung: Bewegung und Elektrodenkontakt verändern die Amplitude ebenfalls.
Bei den ersten realen Messungen sprang die Amplituden-Spitze in 2-min-Fenstern zwischen
0,16 und 0,38 Hz. Deshalb zählt ein Fenster nur, wenn die Amplituden-Spitze mit einer
deutlichen Spitze im RR-Spektrum übereinstimmt (die Atmung moduliert beide); ohne
ausreichend viele übereinstimmende Fenster gibt es keinen Wert.
"""

from __future__ import annotations

import numpy as np
from scipy.signal import find_peaks

from .hrv import Intervals, dominant_frequency, lomb_psd, nn_mask, rr_overlap_mask

BREATHING_BAND = (0.1, 0.5)  # 6–30 Atemzüge/min
WINDOW_S = 120.0
STEP_S = 60.0
MATCH_HZ = 0.02
MIN_RR_PEAK = 0.25  # RR-Spitze mit mindestens 25 % der stärksten im Atembereich


def qrs_amplitudes(x: np.ndarray, r_index: np.ndarray, fs: float) -> np.ndarray:
    """Spitze-Spitze-Amplitude des QRS (±60 ms um R), robust gegen Grundliniendrift."""
    half = max(1, round(0.06 * fs))
    out = np.full(len(r_index), np.nan)
    n = len(x)
    for i, r in enumerate(np.asarray(r_index)):
        c = int(round(r))
        lo, hi = max(0, c - half), min(n, c + half + 1)
        w = x[lo:hi]
        if len(w) > 3 and np.all(np.isfinite(w)):
            out[i] = float(np.max(w) - np.min(w))
    return out


def breathing_from_amplitude(t_s: np.ndarray, amp: np.ndarray) -> float | None:
    """Stärkste Spitze der Amplitudenreihe im Atembereich (ohne Gegenprobe)."""
    t = np.asarray(t_s, dtype=np.float64)
    a = np.asarray(amp, dtype=np.float64)
    ok = np.isfinite(a)
    # Unter 2 min ist die Schätzung instabil (je nach Amplitudenmaß springt die Spitze).
    if ok.sum() < 100 or t[ok][-1] - t[ok][0] < 120:
        return None
    med = float(np.median(a[ok]))
    mad = float(np.median(np.abs(a[ok] - med))) or 1e-9
    ok &= np.abs(a - med) < 5 * 1.4826 * mad  # Artefakte (Bewegung) ausblenden
    f, p = lomb_psd(t[ok], a[ok])
    return dominant_frequency(f, p, BREATHING_BAND)


def _rr_peaks(t: np.ndarray, rr_ms: np.ndarray) -> list[float]:
    f, p = lomb_psd(t, rr_ms)
    m = (f >= BREATHING_BAND[0]) & (f <= BREATHING_BAND[1])
    pk, _ = find_peaks(p[m])
    if not len(pk):
        return []
    top = float(np.max(p[m][pk]))
    return [float(f[m][i]) for i in pk if p[m][i] >= MIN_RR_PEAK * top]


def estimate_breathing(
    r_times_s: np.ndarray, amp: np.ndarray, invalid: Intervals | None = None
) -> float | None:
    """Atemfrequenz in Hz, wenn Amplituden- und RR-Spektrum übereinstimmen, sonst None."""
    t = np.asarray(r_times_s, dtype=np.float64)
    a = np.asarray(amp, dtype=np.float64)
    if len(t) < 100 or t[-1] - t[0] < WINDOW_S:
        return None
    rr = np.diff(t)
    ok_rr, _ = nn_mask(rr, rr_overlap_mask(t, invalid))
    matches, windows = [], 0
    start = t[0]
    while start + WINDOW_S <= t[-1] + 1e-9:
        sel = (t >= start) & (t < start + WINDOW_S)
        sel_rr = sel[1:] & ok_rr
        start += STEP_S
        if sel.sum() < 60 or sel_rr.sum() < 60:
            continue
        windows += 1
        ta, aa = t[sel], a[sel]
        good = np.isfinite(aa)
        med = float(np.median(aa[good]))
        mad = float(np.median(np.abs(aa[good] - med))) or 1e-9
        good &= np.abs(aa - med) < 5 * 1.4826 * mad
        f, p = lomb_psd(ta[good], aa[good])
        edr = dominant_frequency(f, p, BREATHING_BAND)
        if edr is None:
            continue
        near = [
            fr for fr in _rr_peaks(t[1:][sel_rr], rr[sel_rr] * 1000.0) if abs(fr - edr) <= MATCH_HZ
        ]
        if near:
            matches.append(0.5 * (edr + min(near, key=lambda fr: abs(fr - edr))))
    if windows == 0 or len(matches) < max(2, 0.3 * windows):
        return None
    return float(np.median(matches))
