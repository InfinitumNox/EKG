"""Herzfrequenzvariabilität nach Task Force (1996).

Zeitbereich: SDNN, RMSSD, pNN50; Frequenzbereich per Lomb-Scargle (verträgt die
ungleichmäßige Abtastung der RR-Reihe ohne Interpolation); Poincaré SD1/SD2.

Vorher werden Extraschläge und Erkennungsfehler entfernt (ekg.dsp.artifacts,
Lipponen & Tarvainen 2019). Eine feste „20 %-Regel“ ist dafür ungeeignet: Bei ruhiger,
langsamer Atmung schwankt das RR-Intervall gesunder Menschen um 30 % und mehr.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal

from .artifacts import RrClassification, classify_rr
from .ectopy import Ectopy

VLF = (0.0033, 0.04)
LF = (0.04, 0.15)
HF = (0.15, 0.4)
PEAK_BAND = (0.08, 0.5)  # 5–30 Schwankungen/min
RR_MIN_S, RR_MAX_S = 0.25, 2.5


@dataclass(frozen=True, slots=True)
class HrvResult:
    n_beats: int
    n_excluded: int
    n_ectopic: int
    n_missed: int
    n_extra: int
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
    peak_hz: float | None  # stärkste Schwankung der HF zwischen 0,08 und 0,5 Hz

    @property
    def short_window(self) -> bool:
        """Frequenzbereich ist erst ab ~5 min aussagekräftig."""
        return self.duration_s < 300


Intervals = list[tuple[float, float]]
SHORT_S = 120.0  # darunter sind SDNN, RMSSD & Co. nur grob


def rr_overlap_mask(t: np.ndarray, invalid: Intervals | None) -> np.ndarray:
    """True für RR-Intervalle, die einen ungültigen Abschnitt (Übersteuerung, Lead-off) berühren."""
    t = np.asarray(t, dtype=np.float64)
    bad = np.zeros(max(0, len(t) - 1), dtype=bool)
    if not invalid or len(t) < 2:
        return bad
    a, b = t[:-1], t[1:]
    for s0, e0 in invalid:
        bad |= (a < e0) & (b > s0)
    return bad


def nn_mask(rr_s: np.ndarray, bad: np.ndarray | None = None) -> tuple[np.ndarray, RrClassification]:
    """Gültige NN-Intervalle: physiologisch möglich und nicht als Artefakt klassifiziert.

    bad: zusätzlich auszuschließende Intervalle (z. B. über einen übersteuerten Abschnitt).
    """
    rr = np.asarray(rr_s, dtype=np.float64)
    hard = (rr >= RR_MIN_S) & (rr <= RR_MAX_S)
    if bad is not None:
        hard &= ~bad
    filled = rr.copy()
    if hard.any():
        filled[~hard] = np.median(rr[hard])  # damit Lücken die Schwellen nicht verzerren
    cls = classify_rr(filled)
    return hard & cls.valid_mask(), cls


def compute_hrv(r_times_s: np.ndarray, invalid: Intervals | None = None) -> HrvResult | None:
    t = np.asarray(r_times_s, dtype=np.float64)
    if len(t) < 10:
        return None
    rr_s = np.diff(t)
    ok, cls = nn_mask(rr_s, rr_overlap_mask(t, invalid))
    rr = rr_s * 1000.0
    t_rr = t[1:]
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

    lf = hf = lf_hf = lf_nu = hf_nu = peak = None
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
        peak = dominant_frequency(freqs, psd)

    return HrvResult(
        n_beats=len(t),
        n_excluded=int((~ok).sum()),
        n_ectopic=len(cls.ectopic),
        n_missed=len(cls.missed),
        n_extra=len(cls.extra),
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
        peak_hz=peak,
    )


def segment_hrv(
    r_times_s: np.ndarray,
    seg_s: float = 300.0,
    min_cover: float = 0.8,
    invalid: Intervals | None = None,
) -> list[HrvResult]:
    """Kurzzeit-HRV je 5-min-Abschnitt (Task-Force-Standard für Vergleiche)."""
    t = np.asarray(r_times_s, dtype=np.float64)
    if len(t) < 10:
        return []
    out = []
    start = t[0]
    while start + seg_s <= t[-1] + 1e-9:
        sel = t[(t >= start) & (t < start + seg_s)]
        h = compute_hrv(sel, invalid)
        if (
            h is not None
            and h.mean_rr_ms * (h.n_beats - 1 - h.n_excluded) / 1000.0 >= min_cover * seg_s
        ):
            out.append(h)
        start += seg_s
    return out


def lomb_psd(t_s: np.ndarray, rr_ms: np.ndarray, f_max: float = 0.5, df: float = 0.001):
    """Einseitige Leistungsdichte (ms²/Hz), so skaliert, dass ihr Integral der Varianz entspricht."""
    y = signal.detrend(np.asarray(rr_ms, dtype=np.float64), type="linear")
    freqs = np.arange(df, f_max, df)
    p = signal.lombscargle(np.asarray(t_s, dtype=np.float64), y, 2 * np.pi * freqs)
    area = float(np.trapezoid(p, freqs))
    var = float(np.var(y))
    psd = p * (var / area) if area > 0 else p
    return freqs, psd


def dominant_frequency(
    freqs: np.ndarray, psd: np.ndarray, band: tuple[float, float] = PEAK_BAND
) -> float | None:
    """Frequenz der stärksten Spitze im Band, wenn sie sich deutlich abhebt."""
    m = (freqs >= band[0]) & (freqs <= band[1])
    if m.sum() < 5:
        return None
    p = psd[m]
    k = int(np.argmax(p))
    if p[k] < 3.0 * float(np.median(p)):
        return None
    return float(freqs[m][k])


def _band_power(freqs: np.ndarray, psd: np.ndarray, band: tuple[float, float]) -> float:
    m = (freqs >= band[0]) & (freqs < band[1])
    return float(np.trapezoid(psd[m], freqs[m])) if m.sum() > 1 else 0.0


def turning_point_ratio(x: np.ndarray) -> float:
    """Anteil lokaler Extrema; für eine zufällige Folge ≈ 2/3, für glatte Schwankungen kleiner."""
    x = np.asarray(x, dtype=np.float64)
    if len(x) < 3:
        return float("nan")
    a, b, c = x[:-2], x[1:-1], x[2:]
    tp = ((b > a) & (b > c)) | ((b < a) & (b < c))
    return float(tp.mean())


@dataclass(frozen=True, slots=True)
class Irregularity:
    windows: int
    irregular_windows: int
    nrmssd: float
    sd1_sd2: float
    tpr: float

    @property
    def fraction(self) -> float:
        return self.irregular_windows / self.windows if self.windows else 0.0


def irregularity(
    rr_s: np.ndarray, window: int = 64, bad: np.ndarray | None = None
) -> Irregularity | None:
    """Zufällige Unregelmäßigkeit (Muster wie bei Vorhofflimmern) in Fenstern zu 64 Schlägen.

    Ein Fenster gilt als unregelmäßig, wenn die Schwankung groß (nRMSSD > 0,08), ohne
    Vorzugsrichtung (SD1/SD2 > 0,7) und zufällig ist (Turning-Point-Ratio wie bei
    Zufallsfolgen). Atemabhängige Schwankungen sind glatt: SD1/SD2 und TPR bleiben klein.
    Erkennungsfehler und einzelne Extraschläge (mit Pause) werden vorher entfernt; bei
    zufälliger Unregelmäßigkeit bleibt danach genug Unregelmäßigkeit übrig.
    """
    rr = np.asarray(rr_s, dtype=np.float64)
    keep, _ = nn_mask(rr, bad)
    x = rr[keep]
    if len(x) < window:
        return None
    n_win = n_irr = 0
    stats = []
    for s in range(0, len(x) - window + 1, window // 2):
        w = x[s : s + window]
        d = np.diff(w)
        nrmssd = float(np.sqrt(np.mean(d**2)) / np.mean(w))
        sd1 = float(np.sqrt(0.5 * np.var(d, ddof=1)))
        sd2 = float(np.sqrt(max(1e-12, 2 * np.var(w, ddof=1) - sd1**2)))
        tpr = turning_point_ratio(w)
        stats.append((nrmssd, sd1 / sd2, tpr))
        n_win += 1
        if nrmssd > 0.08 and sd1 / sd2 > 0.7 and 0.55 <= tpr <= 0.8:
            n_irr += 1
    if not n_win:
        return None
    med = np.median(np.array(stats), axis=0)
    return Irregularity(n_win, n_irr, float(med[0]), float(med[1]), float(med[2]))


def rhythm_hints(
    r_times_s: np.ndarray, invalid: Intervals | None = None, ectopy: Ectopy | None = None
) -> list[str]:
    """Einfache Hinweise, ausdrücklich keine Diagnose.

    invalid: unbrauchbare Abschnitte; ectopy: per QRS-Form bestätigte Extraschläge (ohne
    Angabe zählen die RR-Muster allein, z. B. live ohne Kurvenform).
    """
    t = np.asarray(r_times_s, dtype=np.float64)
    if len(t) < 20:
        return []
    rr = np.diff(t)
    bad = rr_overlap_mask(t, invalid)
    hard = (rr >= RR_MIN_S) & (rr <= RR_MAX_S) & ~bad
    if hard.sum() < 15:
        return []
    hints = []
    hr = 60.0 / np.median(rr[hard])
    if hr < 50:
        hints.append(f"Herzfrequenz niedrig ({hr:.0f}/min)")
    elif hr > 100:
        hints.append(f"Herzfrequenz hoch ({hr:.0f}/min)")

    ok, cls = nn_mask(rr, bad)
    irr = irregularity(rr, bad=bad)
    if irr is not None and irr.fraction >= 0.5:
        hints.append(
            "Rhythmus über weite Strecken zufällig unregelmäßig – bei wiederholtem Befund ärztlich abklären"
        )
    elif irr is not None and irr.irregular_windows:
        hints.append("Rhythmus zeitweise zufällig unregelmäßig")
    mostly_irregular = irr is not None and irr.fraction >= 0.5
    # Mindestens 3 und 1 % der Schläge (oder 10): einzelne Treffer sind oft Messrauschen.
    n_ect = ectopy.count if ectopy is not None else len(cls.ectopic)
    if not mostly_irregular and n_ect >= 3 and (n_ect >= 10 or n_ect >= 0.01 * len(rr)):
        text = f"{n_ect} vorzeitige Schläge mit nachfolgender Pause"
        if ectopy is not None and ectopy.other_qrs:
            text += f", davon {len(ectopy.other_qrs)} mit veränderter QRS-Form"
        hints.append(text)
    n_err = len(cls.missed) + len(cls.extra)
    if n_err >= 3:
        hints.append(
            f"{n_err} vermutete Erkennungsfehler (verpasste/zusätzliche Schläge) – Signal prüfen"
        )
    if irr is not None and irr.fraction < 0.5 and ok.sum() > 30:
        nn = rr[ok]
        pair = ok[1:] & ok[:-1]
        d = np.diff(rr)[pair]
        nrmssd = float(np.sqrt(np.mean(d**2)) / np.mean(nn)) if len(d) > 10 else 0.0
        if nrmssd > 0.05 and irr.sd1_sd2 < 0.6:
            hints.append(
                "Herzfrequenz schwankt stark, aber gleichmäßig (Atmung und Kreislaufregulation – in Ruhe normal)"
            )
    return hints


def _per_min(hz: float | None) -> str:
    return "–" if hz is None else f"{hz * 60:.0f} /min ({hz:.2f} Hz)"


def hrv_rows(
    h: HrvResult | None, breathing_hz: float | None = None, ectopy: Ectopy | None = None
) -> list[tuple[str, str]]:
    """Zeilen für Tabellen in Oberfläche und Bericht.

    breathing_hz: Atemfrequenz (ekg.dsp.respiration), falls belastbar bestimmt.
    ectopy: per QRS-Form bestätigte Extraschläge (Auswertung); sonst zählen RR-Muster.
    """
    if h is None:
        return [("HRV", "zu wenige Schläge")]
    detail = []
    if ectopy is not None:
        if ectopy.count:
            detail.append(f"{ectopy.count} vorzeitig")
        if ectopy.not_premature:
            # RR-Muster wie bei einem Extraschlag, aber kein Schlag kam zu früh (z. B. abrupte
            # Verlangsamung oder knapp über der Schwelle).
            detail.append(f"{len(ectopy.not_premature)} RR-Sprung ohne Extraschlag")
    elif h.n_ectopic:
        detail.append(f"{h.n_ectopic} vorzeitig")
    if h.n_missed or h.n_extra:
        detail.append(f"{h.n_missed + h.n_extra} Erkennungsfehler")
    excl = f"{h.n_excluded} verworfen" + (f": {', '.join(detail)}" if detail else "")
    rows = [("Schläge", f"{h.n_beats} ({excl})")]
    if h.duration_s < SHORT_S:
        rows.append(("Aussagekraft HRV", "gering – Aufnahme kürzer als 2 min"))
    rows += [
        ("HF Ø / min / max", f"{h.mean_hr:.0f} / {h.min_hr:.0f} / {h.max_hr:.0f} /min"),
        ("SDNN", f"{h.sdnn_ms:.0f} ms"),
        ("RMSSD", f"{h.rmssd_ms:.0f} ms"),
        ("pNN50", f"{h.pnn50:.1f} %"),
        ("SD1 / SD2", f"{h.sd1_ms:.0f} / {h.sd2_ms:.0f} ms"),
    ]
    if h.peak_hz is not None:
        rows.append(("Stärkste HF-Schwankung", _per_min(h.peak_hz)))
    if breathing_hz is not None:
        rows.append(("Atmung (aus R-Amplitude, Schätzung)", _per_min(breathing_hz)))
    if h.lf_hf is not None:
        if breathing_hz is not None and breathing_hz < HF[0]:
            note = " – nicht deutbar: Atmung im LF-Band"
        elif breathing_hz is None and h.peak_hz is not None and h.peak_hz < HF[0]:
            note = " – Vorsicht: bei langsamer Atmung nicht deutbar"
        elif h.short_window:
            note = " (Fenster < 5 min)"
        else:
            note = ""
        rows.append(("LF / HF", f"{h.lf_ms2:.0f} / {h.hf_ms2:.0f} ms², Verh. {h.lf_hf:.2f}{note}"))
    return rows


def segment_rows(segs: list[HrvResult]) -> list[tuple[str, str]]:
    """Zusammenfassung der 5-min-Abschnitte (Median und Spanne)."""
    if not segs:
        return []

    def stat(values: list[float], unit: str, digits: int = 0) -> str:
        v = np.array(values)
        if len(v) == 1:
            return f"{v[0]:.{digits}f} {unit}"
        return f"{np.median(v):.{digits}f} {unit} ({v.min():.{digits}f}–{v.max():.{digits}f})"

    rows = [
        ("5-min-Abschnitte", str(len(segs))),
        ("HF Ø", stat([s.mean_hr for s in segs], "/min")),
        ("RMSSD", stat([s.rmssd_ms for s in segs], "ms")),
        ("SDNN", stat([s.sdnn_ms for s in segs], "ms")),
    ]
    peaks = [s.peak_hz * 60 for s in segs if s.peak_hz is not None]
    if peaks:
        rows.append(("Stärkste HF-Schwankung", stat(peaks, "/min")))
    return rows
