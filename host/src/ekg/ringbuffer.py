"""Ringpuffer mit absolutem Sample-Index (thread-sicher)."""

from __future__ import annotations

import threading

import numpy as np


class RingBuffer:
    def __init__(self, capacity: int, dtype: np.dtype | type = np.float32, fill: float = np.nan):
        self.capacity = int(capacity)
        self._data = np.full(self.capacity, fill, dtype=dtype)
        self._fill = fill
        self._end = 0  # absoluter Index hinter dem letzten Sample
        self._lock = threading.Lock()

    @property
    def end(self) -> int:
        return self._end

    @property
    def start(self) -> int:
        return max(0, self._end - self.capacity)

    def clear(self, end: int = 0) -> None:
        with self._lock:
            self._data[:] = self._fill
            self._end = end

    def append(self, values: np.ndarray) -> None:
        values = np.asarray(values)
        n = len(values)
        if n == 0:
            return
        with self._lock:
            if n >= self.capacity:
                values = values[-self.capacity :]
                self._end += n - self.capacity
                n = self.capacity
            pos = self._end % self.capacity
            first = min(n, self.capacity - pos)
            self._data[pos : pos + first] = values[:first]
            if first < n:
                self._data[: n - first] = values[first:]
            self._end += n

    def get(self, start: int, stop: int) -> np.ndarray:
        """Werte im Bereich [start, stop); nicht (mehr) vorhandene Werte = fill."""
        with self._lock:
            out = np.full(max(0, stop - start), self._fill, dtype=self._data.dtype)
            lo = max(start, self._end - self.capacity, 0)
            hi = min(stop, self._end)
            if hi <= lo:
                return out
            idx = np.arange(lo, hi) % self.capacity
            out[lo - start : hi - start] = self._data[idx]
            return out

    def last(self, n: int) -> tuple[int, np.ndarray]:
        end = self._end
        return end, self.get(end - n, end)
