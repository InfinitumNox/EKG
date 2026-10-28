"""QRS-Erkennung nach Pan & Tompkins (1985), als Streaming-Detektor.

Kette: Bandpass 5–15 Hz -> 5-Punkt-Ableitung -> Quadrieren -> gleitendes Integral
(150 ms). Auf den Spitzen des Integrals arbeiten adaptive Schwellen (Signal- und
Rauschschätzer), eine Refraktärzeit von 200 ms, eine T-Wellen-Unterscheidung über die
Steilheit und ein Search-Back mit halber Schwelle, wenn ein Schlag überfällig ist.

Zusätzlich zum Original: Schwellenabbau, wenn nach einem Artefakt lange nichts erkannt
wird, und die R-Zacke wird im breitbandigen Signal mit Subsample-Genauigkeit lokalisiert.

Derselbe Detektor läuft live und offline (detect_qrs), damit die Validierung an
MIT-BIH genau den Code prüft, der live verwendet wird.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal
from scipy.ndimage import maximum_filter1d

from .filters import StreamingFir, StreamingSos, fill_gaps, group_delay_samples


@dataclass(frozen=True, slots=True)
class Beat:
    index: float  # R-Zacke als absoluter Sample-Index (mit Subsample-Anteil)
    amplitude_mv: float  # Ausschlag gegen die lokale Grundlinie, vorzeichenbehaftet
    searchback: bool = False


@dataclass(slots=True)
class _Candidate:
    index: int
    value: float
    slope: float
    t_wave: bool = False


class QrsDetector:
    LEARN_S = 2.0
    SETTLE_S = 0.3
    REFRACTORY_S = 0.2
    T_WAVE_WINDOW_S = 0.36
    MWI_S = 0.15
    NEIGHBORHOOD_S = 0.1
    HISTORY_S = 8.0
    STALL_S = 3.0
    LOCATE_HALF_WINDOW_S = 0.09
    MIN_R_MV = 0.05

    def __init__(self, fs: float):
        self.fs = float(fs)
        self._bp_sos = signal.butter(2, [5.0, 15.0], "bandpass", fs=fs, output="sos")
        self._ref_sos = signal.butter(2, [0.5, 40.0], "bandpass", fs=fs, output="sos")
        self._w = max(1, round(self.MWI_S * fs))
        self._lookahead = max(1, round(self.NEIGHBORHOOD_S * fs))
        self._refractory = round(self.REFRACTORY_S * fs)
        self._t_window = round(self.T_WAVE_WINDOW_S * fs)
        # Versatz R-Zacke -> Spitze des Integrals und Gruppenlaufzeit des Referenzfilters.
        gd_bp = group_delay_samples(self._bp_sos, 10.0, fs)
        self._mwi_delay = gd_bp + 2.0 + (self._w - 1) / 2.0
        self._ref_delay = self._pulse_delay(self._ref_sos)
        self._floor = self._threshold_floor()
        self.reset()

    def _pulse_delay(self, sos: np.ndarray) -> float:
        """Verzögerung einer QRS-typischen Spitze (Gauß, σ = 10 ms) durch den Filter.

        Genauer als die Gruppenlaufzeit bei einer Einzelfrequenz, weil das Spektrum der
        R-Zacke breit ist.
        """
        n = round(2 * self.fs)
        t = np.arange(n) / self.fs
        y = signal.sosfilt(sos, np.exp(-0.5 * ((t - 1.0) / 0.010) ** 2))
        k = int(np.argmax(y))
        y0, y1, y2 = y[k - 1], y[k], y[k + 1]
        frac = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)
        return k + frac - 1.0 * self.fs

    def _threshold_floor(self) -> float:
        """Untergrenze der Schwelle: halbe Integral-Spitze einer 50-µV-R-Zacke.

        Verhindert, dass nach langem Schwellenabbau reines Rauschen als QRS zählt.
        """
        t = np.arange(round(2 * self.fs)) / self.fs
        pulse = self.MIN_R_MV * np.exp(-0.5 * ((t - 1.0) / 0.010) ** 2)
        y = signal.sosfilt(self._bp_sos, pulse)
        d = signal.lfilter(np.array([2.0, 1.0, 0.0, -1.0, -2.0]) * self.fs / 8.0, [1.0], y)
        mwi = signal.lfilter(np.ones(self._w) / self._w, [1.0], d * d)
        return 0.5 * float(np.max(mwi))

    # ------------------------------------------------------------------ Zustand
    def reset(self, start_index: int | None = None) -> None:
        self._bp = StreamingSos(self._bp_sos)
        self._ref = StreamingSos(self._ref_sos)
        # Ableitung nach Pan-Tompkins: (2x[n] + x[n-1] - x[n-3] - 2x[n-4]) * fs / 8
        self._deriv = StreamingFir(np.array([2.0, 1.0, 0.0, -1.0, -2.0]) * self.fs / 8.0)
        self._mwi = StreamingFir(np.ones(self._w) / self._w)
        self._origin: int | None = start_index
        self._next: int | None = start_index
        self._hist_start = start_index or 0
        self._mwi_h = np.zeros(0)
        self._sq_h = np.zeros(0)
        self._ref_h = np.zeros(0)
        self._initialized = False
        self._scan_from = 0
        self._spki = 0.0
        self._npki = 0.0
        self._last_qrs: int | None = None
        self._last_slope = 0.0
        self._rr_recent: list[int] = []
        self._rr_selected: list[int] = []
        self._cands: list[_Candidate] = []
        self._polarity = 0
        self._last_stall_decay = 0
        self._init_index = 0

    @property
    def initialized(self) -> bool:
        return self._initialized

    @property
    def thresholds(self) -> tuple[float, float]:
        thr1 = max(self._floor, self._npki + 0.25 * (self._spki - self._npki))
        return thr1, max(self._floor, 0.5 * thr1)

    @property
    def rr_average(self) -> float | None:
        """RR-Mittel (in Samples) über die letzten regulären Schläge."""
        src = self._rr_selected or self._rr_recent
        return float(np.mean(src)) if src else None

    # ------------------------------------------------------------------ Verarbeitung
    def process(self, x: np.ndarray, start_index: int | None = None) -> list[Beat]:
        x = np.asarray(x, dtype=np.float64)
        if self._next is None:
            self._next = start_index or 0
            self._origin = self._next
            self._hist_start = self._next
        elif start_index is not None and start_index != self._next:
            raise ValueError(f"Lücke im Eingang: erwartet {self._next}, bekommen {start_index}")
        if len(x) == 0:
            return []

        sq = self._deriv.process(self._bp.process(x)) ** 2
        mwi = self._mwi.process(sq)
        ref = self._ref.process(x)
        self._mwi_h = np.concatenate([self._mwi_h, mwi])
        self._sq_h = np.concatenate([self._sq_h, sq])
        self._ref_h = np.concatenate([self._ref_h, ref])
        self._next += len(x)

        out: list[Beat] = []
        if not self._initialized and not self._try_initialize():
            return out
        hi = self._next - self._lookahead
        if hi > self._scan_from:
            self._scan(self._scan_from, hi, out)
            self._scan_from = hi
        self._search_back(hi, out)
        self._stall_check(hi)
        self._trim()
        return out

    def _try_initialize(self) -> bool:
        assert self._origin is not None and self._next is not None
        settle = round(self.SETTLE_S * self.fs)
        learn_end = self._origin + settle + round(self.LEARN_S * self.fs)
        if self._next < learn_end:
            return False
        seg = self._mwi_h[settle : learn_end - self._hist_start]
        self._spki = float(np.max(seg)) / 3.0
        self._npki = float(np.mean(seg)) / 2.0
        self._initialized = True
        self._scan_from = self._origin + settle + self._lookahead
        self._init_index = self._scan_from
        return True

    def _scan(self, lo: int, hi: int, out: list[Beat]) -> None:
        L = self._lookahead
        a = lo - L - self._hist_start
        seg = self._mwi_h[a : hi + L - self._hist_start]
        roll = maximum_filter1d(seg, size=2 * L + 1, mode="nearest")
        center = seg[L : L + (hi - lo)]
        prev = seg[L - 1 : L - 1 + (hi - lo)]
        peaks = np.flatnonzero((center >= roll[L : L + (hi - lo)]) & (center > prev) & (center > 0))
        for p in peaks:
            self._candidate(lo + int(p), float(center[p]), out)

    def _slope(self, i: int) -> float:
        b = i - self._hist_start
        return float(np.max(self._sq_h[max(0, b - self._w) : b + 1]))

    def _candidate(self, i: int, v: float, out: list[Beat]) -> None:
        if self._last_qrs is not None and i - self._last_qrs < self._refractory:
            return
        self._search_back(i, out)
        if self._last_qrs is not None and i - self._last_qrs < self._refractory:
            return
        slope = self._slope(i)
        thr1, _ = self.thresholds
        is_t_wave = (
            self._last_qrs is not None
            and i - self._last_qrs < self._t_window
            and slope < 0.5 * self._last_slope
        )
        if v > thr1 and not is_t_wave:
            self._accept(_Candidate(i, v, slope), searchback=False, out=out)
        else:
            self._npki = 0.125 * v + 0.875 * self._npki
            self._cands.append(_Candidate(i, v, slope, t_wave=is_t_wave))

    def _search_back(self, now: int, out: list[Beat]) -> None:
        while self._last_qrs is not None:
            rr = self.rr_average
            if rr is None or now - self._last_qrs <= 1.66 * rr:
                return
            _, thr2 = self.thresholds
            eligible = [
                c
                for c in self._cands
                if c.index - self._last_qrs >= self._refractory
                and c.index < now
                and not c.t_wave
                and c.value > thr2
            ]
            if not eligible:
                return
            self._accept(max(eligible, key=lambda c: c.value), searchback=True, out=out)

    def _stall_check(self, now: int) -> None:
        """Nach Artefakten können die Schwellen zu hoch stehen; dann schrittweise abbauen."""
        ref = self._last_qrs if self._last_qrs is not None else self._init_index
        rr = self.rr_average or self.fs
        limit = max(self.STALL_S * self.fs, 3.0 * rr)
        if now - ref > limit and now - self._last_stall_decay > self.fs:
            self._spki *= 0.5
            self._npki *= 0.5
            self._last_stall_decay = now

    def _accept(self, c: _Candidate, searchback: bool, out: list[Beat]) -> None:
        w = 0.25 if searchback else 0.125
        self._spki = w * c.value + (1 - w) * self._spki
        if self._last_qrs is not None:
            self._update_rr(c.index - self._last_qrs)
        self._last_qrs = c.index
        self._last_slope = c.slope
        self._cands = [k for k in self._cands if k.index > c.index]
        out.append(self._locate(c.index, searchback))

    def _update_rr(self, rr: int) -> None:
        self._rr_recent = (self._rr_recent + [rr])[-8:]
        avg2 = float(np.mean(self._rr_selected)) if self._rr_selected else None
        if avg2 is None or 0.92 * avg2 <= rr <= 1.16 * avg2:
            self._rr_selected = (self._rr_selected + [rr])[-8:]
        elif len(self._rr_recent) == 8 and all(
            not (0.92 * avg2 <= r <= 1.16 * avg2) for r in self._rr_recent
        ):
            # Rhythmuswechsel: acht irreguläre Schläge in Folge -> neu synchronisieren.
            self._rr_selected = list(self._rr_recent)

    def _locate(self, mwi_index: int, searchback: bool) -> Beat:
        center = mwi_index - self._mwi_delay + self._ref_delay
        half = round(self.LOCATE_HALF_WINDOW_S * self.fs)
        a = max(0, round(center) - half - self._hist_start)
        b = min(len(self._ref_h), round(center) + half + 1 - self._hist_start)
        w = self._ref_h[a:b]
        if len(w) < 3:
            return Beat(float(mwi_index - self._mwi_delay), 0.0, searchback)
        base = float(np.median(w))
        pos = float(np.max(w)) - base
        neg = base - float(np.min(w))
        sign = (1 if pos >= neg else -1) if self._polarity == 0 else int(np.sign(self._polarity))
        self._polarity = int(np.clip(self._polarity + (1 if pos >= neg else -1), -8, 8))
        k = int(np.argmax(sign * w))
        frac = 0.0
        if 0 < k < len(w) - 1:
            y0, y1, y2 = sign * w[k - 1], sign * w[k], sign * w[k + 1]
            denom = y0 - 2 * y1 + y2
            if denom < 0:
                frac = 0.5 * (y0 - y2) / denom
        index = self._hist_start + a + k + frac - self._ref_delay
        return Beat(float(index), float(w[k] - base), searchback)

    def _trim(self) -> None:
        keep = round(self.HISTORY_S * self.fs)
        excess = len(self._mwi_h) - keep
        if excess > 0:
            self._mwi_h = self._mwi_h[excess:]
            self._sq_h = self._sq_h[excess:]
            self._ref_h = self._ref_h[excess:]
            self._hist_start += excess
        horizon = self._hist_start + self._lookahead
        self._cands = [c for c in self._cands if c.index >= horizon]


def detect_qrs(x: np.ndarray, fs: float, chunk_s: float = 1.0) -> list[Beat]:
    """Offline-Erkennung mit dem Live-Detektor (NaN-Lücken werden überbrückt)."""
    x, _ = fill_gaps(np.asarray(x, dtype=np.float64))
    det = QrsDetector(fs)
    step = max(1, round(chunk_s * fs))
    beats: list[Beat] = []
    for start in range(0, len(x), step):
        beats.extend(det.process(x[start : start + step]))
    # Am Ende fehlen nur die letzten ~100 ms Vorausschau; mit Nullen auffüllen.
    beats.extend(det.process(np.full(det._lookahead + 1, x[-1] if len(x) else 0.0)))
    return [b for b in beats if b.index < len(x)]


def refine_r_peaks(
    x: np.ndarray, beats: list[Beat], fs: float, half_window_s: float = 0.04
) -> list[Beat]:
    """Verfeinert R-Positionen in einem (phasenfrei gefilterten) Signal."""
    out: list[Beat] = []
    half = max(1, round(half_window_s * fs))
    n = len(x)
    for b in beats:
        c = round(b.index)
        lo, hi = max(0, c - half), min(n, c + half + 1)
        w = x[lo:hi]
        if len(w) < 3 or not np.all(np.isfinite(w)):
            out.append(b)
            continue
        sign = 1.0 if b.amplitude_mv >= 0 else -1.0
        k = int(np.argmax(sign * w))
        frac = 0.0
        if 0 < k < len(w) - 1:
            y0, y1, y2 = sign * w[k - 1], sign * w[k], sign * w[k + 1]
            denom = y0 - 2 * y1 + y2
            if denom < 0:
                frac = 0.5 * (y0 - y2) / denom
        base = float(np.median(x[max(0, c - 2 * half) : min(n, c + 2 * half + 1)]))
        out.append(Beat(lo + k + frac, float(w[k] - base), b.searchback))
    return out
