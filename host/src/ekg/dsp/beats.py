"""Median-Schlag und Vermessung (P, QRS, T) – experimentell.

Bei einer Ableitung und 0,5–40 Hz Bandbreite sind die Werte Näherungen. Die Fiducials
werden in der Oberfläche eingezeichnet, damit man sie visuell prüfen kann.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.ndimage import uniform_filter1d

PRE_S = 0.35
POST_S = 0.60


@dataclass(frozen=True, slots=True)
class MedianBeat:
    template: np.ndarray  # mV
    fs: float
    r_offset: int  # Index der R-Zacke im Template
    n_beats: int
    mean_rr_s: float

    @property
    def t_ms(self) -> np.ndarray:
        return (np.arange(len(self.template)) - self.r_offset) / self.fs * 1000.0


@dataclass(frozen=True, slots=True)
class Delineation:
    baseline_mv: float
    p_onset: int | None
    p_peak: int | None
    qrs_onset: int
    qrs_offset: int
    t_peak: int | None
    t_end: int | None
    pr_ms: float | None
    qrs_ms: float
    qt_ms: float | None
    qtc_bazett_ms: float | None
    qtc_fridericia_ms: float | None
    p_amp_mv: float | None
    r_amp_mv: float
    t_amp_mv: float | None


def median_beat(
    x: np.ndarray, r_index: np.ndarray, fs: float, max_beats: int = 300
) -> MedianBeat | None:
    """Median über die Schläge mit typischem RR (Extrasystolen verzerren sonst die T-Welle)."""
    r = np.asarray(r_index, dtype=np.float64)
    if len(r) < 5:
        return None
    rr = np.diff(r) / fs
    med_rr = float(np.median(rr))
    pre, post = round(PRE_S * fs), round(POST_S * fs)
    segs = []
    for k in range(1, len(r) - 1):
        rr_before, rr_after = rr[k - 1], rr[k]
        if abs(rr_before - med_rr) > 0.2 * med_rr or abs(rr_after - med_rr) > 0.2 * med_rr:
            continue
        c = round(r[k])
        if c - pre < 0 or c + post > len(x):
            continue
        s = x[c - pre : c + post]
        if np.all(np.isfinite(s)):
            segs.append(s)
    if len(segs) < 3:
        return None
    if len(segs) > max_beats:
        idx = np.linspace(0, len(segs) - 1, max_beats).astype(int)
        segs = [segs[i] for i in idx]
    m = np.vstack(segs)
    tmpl = np.median(m, axis=0)
    # Ausreißer (Artefakte) entfernen und erneut mitteln.
    corr = np.array([np.corrcoef(s, tmpl)[0, 1] for s in m])
    good = corr > 0.8
    if good.sum() >= 3:
        tmpl = np.median(m[good], axis=0)
    return MedianBeat(tmpl, fs, pre, int(good.sum()), med_rr)


def delineate(mb: MedianBeat) -> Delineation:
    x = mb.template
    fs = mb.fs
    r = mb.r_offset
    ms = lambda v: round(v * fs / 1000.0)  # noqa: E731

    sign = 1.0 if x[r] >= np.median(x) else -1.0
    d = np.abs(np.gradient(x)) * fs  # mV/s
    d = uniform_filter1d(d, size=max(1, ms(8)))
    q_lo = max(0, r - ms(120))
    q_hi = min(len(x), r + ms(140))
    dmax = float(np.max(d[q_lo:q_hi]))
    # Der Median-Schlag ist über viele Schläge gemittelt, Rauschen spielt hier kaum eine Rolle.
    thr = 0.04 * dmax
    # QRS-Grenzen: erste Stelle (von R aus), ab der die Steigung über 12 ms ruhig bleibt.
    # Ein einzelner Nulldurchgang der Steigung (Q- oder S-Spitze) reicht dafür nicht.
    w = max(2, ms(12))
    on = r
    while on - w > q_lo and np.max(d[on - w : on + 1]) >= thr:
        on -= 1
    off = r
    while off + w < q_hi and np.max(d[off : off + w + 1]) >= thr:
        off += 1
    baseline = float(x[on])

    # T-Welle: größter Ausschlag nach dem QRS bis 60 % RR nach R.
    t_peak = t_end = None
    t_amp = None
    t_lo = off + ms(60)
    t_hi = min(len(x) - 1, r + round(0.62 * mb.mean_rr_s * fs))
    if t_hi - t_lo > ms(60):
        seg = x[t_lo:t_hi] - baseline
        k = int(np.argmax(np.abs(seg)))
        t_peak = t_lo + k
        t_amp = float(seg[k])
        if abs(t_amp) > 0.03:
            # Tangentenmethode: steilste Stelle im abfallenden Schenkel, Schnitt mit Grundlinie.
            tail = x[t_peak:t_hi] - baseline
            if len(tail) > 3:
                slope = np.gradient(tail)
                j = int(np.argmin(slope * np.sign(t_amp)))
                s = slope[j]
                if abs(s) > 1e-9:
                    cross = j - tail[j] / s
                    t_end = int(round(t_peak + cross))
                    # Plausibilität: T-Ende hinter 60 % RR heißt meist Überlappung mit der
                    # nächsten P-Welle (hohe Frequenz) – dann lieber kein Wert als ein falscher.
                    if not (t_peak < t_end <= min(len(x) - 1, r + 0.6 * mb.mean_rr_s * fs)):
                        t_end = None
        else:
            t_peak = None
            t_amp = None

    # P-Welle: bis 250 ms vor QRS-Beginn, aber nicht vor dem Ende der vorherigen T-Welle
    # (bei hoher Frequenz überlappen sie sonst).
    p_peak = p_onset = None
    p_amp = None
    rr_n = round(mb.mean_rr_s * fs)
    prev_t_end = (t_end if t_end is not None else r + round(0.6 * rr_n)) - rr_n
    p_lo = max(0, on - ms(250), prev_t_end + ms(10))
    p_hi = on - ms(40)
    if p_hi - p_lo > ms(40):
        seg = x[p_lo:p_hi] - baseline
        k = int(np.argmax(seg * sign))
        amp = float(seg[k])
        noise = float(np.std(np.diff(x[: max(ms(60), 4)]))) * 3
        if abs(amp) > max(0.04, noise):
            p_peak = p_lo + k
            p_amp = amp
            j = p_peak
            while j > p_lo and abs(x[j] - baseline) > 0.15 * abs(amp):
                j -= 1
            p_onset = j

    qrs_ms = (off - on) / fs * 1000.0
    qt_ms = (t_end - on) / fs * 1000.0 if t_end is not None else None
    pr_ms = (on - p_onset) / fs * 1000.0 if p_onset is not None else None
    rr = mb.mean_rr_s
    return Delineation(
        baseline_mv=baseline,
        p_onset=p_onset,
        p_peak=p_peak,
        qrs_onset=on,
        qrs_offset=off,
        t_peak=t_peak,
        t_end=t_end,
        pr_ms=pr_ms,
        qrs_ms=qrs_ms,
        qt_ms=qt_ms,
        qtc_bazett_ms=qt_ms / np.sqrt(rr) if qt_ms else None,
        qtc_fridericia_ms=qt_ms / rr ** (1 / 3) if qt_ms else None,
        p_amp_mv=p_amp,
        r_amp_mv=float(x[r] - baseline),
        t_amp_mv=t_amp,
    )
