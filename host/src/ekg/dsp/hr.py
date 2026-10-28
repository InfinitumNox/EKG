"""Herzfrequenz aus R-Zacken."""

from __future__ import annotations

from collections import deque

import numpy as np

RR_MIN_S = 0.25  # 240/min
RR_MAX_S = 2.5  # 24/min


class HeartRateTracker:
    """Median der letzten RR-Intervalle; robust gegen einzelne Fehl- oder Extraschläge."""

    def __init__(self, fs: float, n_intervals: int = 8):
        self.fs = fs
        self._r: deque[float] = deque(maxlen=n_intervals + 1)

    def reset(self) -> None:
        self._r.clear()

    def add(self, r_index: float) -> None:
        self._r.append(r_index)

    @property
    def last_r(self) -> float | None:
        return self._r[-1] if self._r else None

    def rr_intervals_s(self) -> np.ndarray:
        rr = np.diff(np.asarray(self._r)) / self.fs
        return rr[(rr >= RR_MIN_S) & (rr <= RR_MAX_S)]

    def last_rr_ms(self) -> float | None:
        if len(self._r) < 2:
            return None
        rr = (self._r[-1] - self._r[-2]) / self.fs
        return rr * 1000.0 if RR_MIN_S <= rr <= RR_MAX_S else None

    def bpm(self, now_index: float) -> float | None:
        rr = self.rr_intervals_s()
        if len(rr) < 2 or self.last_r is None:
            return None
        med = float(np.median(rr))
        # Veraltet, wenn deutlich länger als erwartet kein Schlag kam.
        if (now_index - self.last_r) / self.fs > max(3.0, 2.5 * med):
            return None
        return 60.0 / med


def instantaneous_hr(r_index: np.ndarray, fs: float) -> tuple[np.ndarray, np.ndarray]:
    """(Zeit in s, HF in /min) je Schlag, unplausible Intervalle entfernt."""
    r = np.asarray(r_index, dtype=np.float64)
    if len(r) < 2:
        return np.zeros(0), np.zeros(0)
    rr = np.diff(r) / fs
    t = r[1:] / fs
    ok = (rr >= RR_MIN_S) & (rr <= RR_MAX_S)
    return t[ok], 60.0 / rr[ok]
