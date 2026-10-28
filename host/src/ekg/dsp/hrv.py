"""Herzfrequenzvariabilität nach Task Force (1996).

Zeitbereich: SDNN, RMSSD, pNN50; Frequenzbereich per Lomb-Scargle (verträgt die
ungleichmäßige Abtastung der RR-Reihe ohne Interpolation); Poincaré SD1/SD2.
Ektope Schläge und Fehldetektionen werden vorher entfernt (Abweichung > 20 % vom
vorherigen gültigen Intervall).
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal

VLF = (0.0033, 0.04)
LF = (0.04, 0.15)
HF = (0.15, 0.4)


@dataclass(frozen=True, slots=True)
class HrvResult:
    n_beats: int
    n_excluded: int
    duration_s: float
    mean_rr_ms: float
    mean_hr: float
    min_hr: float
    max_hr: float
    sdnn_ms: float
    rmssd_ms: float
    pnn50: float
    sd1_ms: float
    sd2_ms: float
    lf_ms2: float | None
    hf_ms2: float | None
    lf_hf: float | None
    lf_nu: float | None
    hf_nu: float | None

    @property
    def short_window(self) -> bool:
        """Frequenzbereich ist erst ab ~5 min aussagekräftig."""
        return self.duration_s < 300


def clean_rr(rr_ms: np.ndarray, max_change: float = 0.2) -> np.ndarray:
    """Maske gültiger RR-Intervalle (physiologisch plausibel, keine Sprünge > 20 %)."""
    rr = np.asarray(rr_ms, dtype=np.float64)
    ok = (rr >= 300) & (rr <= 2000)
    ref = float(np.median(rr[ok])) if ok.any() else None
    for i in range(len(rr)):
        if not ok[i] or ref is None:
            continue
        if abs(rr[i] - ref) > max_change * ref:
            ok[i] = False
        else:
            ref = rr[i]
    return ok


def compute_hrv(r_times_s: np.ndarray) -> HrvResult | None:
    t = np.asarray(r_times_s, dtype=np.float64)
    if len(t) < 10:
        return None
    rr = np.diff(t) * 1000.0
    t_rr = t[1:]
    ok = clean_rr(rr)
    rr_ok = rr[ok]
    if len(rr_ok) < 8:
        return None
    # Für sukzessive Differenzen nur Paare aus zwei gültigen, direkt aufeinanderfolgenden RR.
    pair = ok[1:] & ok[:-1]
    diffs = np.diff(rr)[pair]
    hr = 60000.0 / rr_ok
    sd1 = float(np.sqrt(0.5 * np.var(diffs, ddof=1))) if len(diffs) > 1 else float("nan")
    sdnn = float(np.std(rr_ok, ddof=1))
    sd2 = float(np.sqrt(max(0.0, 2 * sdnn**2 - sd1**2))) if np.isfinite(sd1) else float("nan")

    lf = hf = lf_hf = lf_nu = hf_nu = None
    duration = float(t[-1] - t[0])
    if duration >= 60:
        freqs, psd = lomb_psd(t_rr[ok], rr_ok)
        lf = _band_power(freqs, psd, LF)
        hf = _band_power(freqs, psd, HF)
        if hf > 0:
            lf_hf = lf / hf
        if lf + hf > 0:
            lf_nu = 100 * lf / (lf + hf)
            hf_nu = 100 * hf / (lf + hf)

    return HrvResult(
        n_beats=len(t),
        n_excluded=int((~ok).sum()),
        duration_s=duration,
        mean_rr_ms=float(np.mean(rr_ok)),
        mean_hr=float(np.mean(hr)),
        min_hr=float(np.min(hr)),
        max_hr=float(np.max(hr)),
        sdnn_ms=sdnn,
        rmssd_ms=float(np.sqrt(np.mean(diffs**2))) if len(diffs) else float("nan"),
        pnn50=float(100 * np.mean(np.abs(diffs) > 50)) if len(diffs) else float("nan"),
        sd1_ms=sd1,
        sd2_ms=sd2,
        lf_ms2=lf,
        hf_ms2=hf,
        lf_hf=lf_hf,
        lf_nu=lf_nu,
        hf_nu=hf_nu,
    )


def lomb_psd(t_s: np.ndarray, rr_ms: np.ndarray, f_max: float = 0.5, df: float = 0.001):
    """Einseitige Leistungsdichte (ms²/Hz), so skaliert, dass ihr Integral der Varianz entspricht."""
    y = signal.detrend(np.asarray(rr_ms, dtype=np.float64), type="linear")
    freqs = np.arange(df, f_max, df)
    p = signal.lombscargle(np.asarray(t_s, dtype=np.float64), y, 2 * np.pi * freqs)
    area = float(np.trapezoid(p, freqs))
    var = float(np.var(y))
    psd = p * (var / area) if area > 0 else p
    return freqs, psd


def _band_power(freqs: np.ndarray, psd: np.ndarray, band: tuple[float, float]) -> float:
    m = (freqs >= band[0]) & (freqs < band[1])
    return float(np.trapezoid(psd[m], freqs[m])) if m.sum() > 1 else 0.0


def rhythm_hints(r_times_s: np.ndarray) -> list[str]:
    """Einfache Hinweise, ausdrücklich keine Diagnose."""
    t = np.asarray(r_times_s, dtype=np.float64)
    if len(t) < 20:
        return []
    rr = np.diff(t)
    ok = (rr >= 0.3) & (rr <= 2.0)
    rr = rr[ok]
    if len(rr) < 15:
        return []
    hints = []
    hr = 60.0 / np.median(rr)
    if hr < 50:
        hints.append(f"Herzfrequenz niedrig ({hr:.0f}/min)")
    elif hr > 100:
        hints.append(f"Herzfrequenz hoch ({hr:.0f}/min)")
    # Unregelmäßigkeit: Anteil sprunghafter Intervalle und RMSSD der übrigen. Einzelne
    # Extrasystolen (vorzeitig + Pause) sollen nicht als „unregelmäßig“ zählen.
    ok = clean_rr(rr * 1000.0)
    frac_bad = 1.0 - float(ok.mean())
    pair = ok[1:] & ok[:-1]
    d = np.diff(rr)[pair]
    nrmssd = float(np.sqrt(np.mean(d**2)) / np.mean(rr[ok])) if len(d) > 5 else 0.0
    if frac_bad > 0.45 or nrmssd > 0.1:
        hints.append("Rhythmus deutlich unregelmäßig")
    else:
        n_ectopic = int((~ok).sum())
        if n_ectopic >= 3 and frac_bad > 0.02:
            hints.append(f"{n_ectopic} vorzeitige/ausgefallene Schläge")
    return hints


def hrv_rows(h: HrvResult | None) -> list[tuple[str, str]]:
    """Zeilen für Tabellen in Oberfläche und Bericht."""
    if h is None:
        return [("HRV", "zu wenige Schläge")]
    rows = [
        ("Schläge", f"{h.n_beats} ({h.n_excluded} verworfen)"),
        ("HF Ø / min / max", f"{h.mean_hr:.0f} / {h.min_hr:.0f} / {h.max_hr:.0f} /min"),
        ("SDNN", f"{h.sdnn_ms:.0f} ms"),
        ("RMSSD", f"{h.rmssd_ms:.0f} ms"),
        ("pNN50", f"{h.pnn50:.1f} %"),
        ("SD1 / SD2", f"{h.sd1_ms:.0f} / {h.sd2_ms:.0f} ms"),
    ]
    if h.lf_hf is not None:
        note = " (Fenster < 5 min)" if h.short_window else ""
        rows.append(("LF / HF", f"{h.lf_ms2:.0f} / {h.hf_ms2:.0f} ms², Verh. {h.lf_hf:.2f}{note}"))
    return rows
