"""Atemfrequenz aus dem EKG (EDR, ECG-derived respiration).

Beim Atmen ändern sich die Lage des Herzens und der Widerstand des Brustkorbs; die
QRS-Amplitude schwankt deshalb im Atemrhythmus. Das ist unabhängig von der
atemabhängigen Herzfrequenz-Schwankung und dient als Gegenprobe: Liegt die stärkste
HF-Schwankung bei ~0,1 Hz, die Amplituden-Schwankung aber bei 0,2 Hz, stammt die
langsame Schwankung eher von der Blutdruckregulation (Mayer-Wellen) als von der Atmung.

Nur eine Schätzung: Bewegung und Elektrodenkontakt verändern die Amplitude ebenfalls.
"""

from __future__ import annotations

import numpy as np

from .hrv import dominant_frequency, lomb_psd

BREATHING_BAND = (0.1, 0.5)  # 6–30 Atemzüge/min


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
    """Atemfrequenz in Hz aus der Amplitudenreihe, oder None ohne klare Spitze."""
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
