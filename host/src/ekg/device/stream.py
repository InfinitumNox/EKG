"""Setzt SAMPLES-Pakete zu einem lückenlos nummerierten Datenstrom zusammen."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ..protocol import Flag, SamplesPacket


@dataclass(frozen=True, slots=True)
class Chunk:
    start: int  # Sitzungs-Index des ersten Samples (beginnt bei 0)
    raw: np.ndarray
    flags: Flag
    gap_before: int = 0  # fehlende Samples unmittelbar vor diesem Chunk
    restart: bool = False  # Gerät hat den Strom neu gestartet (Index zurückgesetzt)


@dataclass(slots=True)
class StreamStats:
    packets: int = 0
    samples: int = 0
    lost_samples: int = 0
    gaps: int = 0
    restarts: int = 0


class StreamAssembler:
    def __init__(self) -> None:
        self.stats = StreamStats()
        self._offset = 0
        self._wrap = 0
        self._last_dev: int | None = None
        self._expected: int | None = None

    def reset(self) -> None:
        self.__init__()  # type: ignore[misc]

    @property
    def next_index(self) -> int | None:
        return self._expected

    def push(self, pkt: SamplesPacket) -> Chunk:
        dev = pkt.first_index
        if self._last_dev is not None and dev < self._last_dev and self._last_dev - dev > 2**31:
            self._wrap += 2**32  # 32-Bit-Zähler übergelaufen (nach ~99 Tagen bei 500 Hz)
        self._last_dev = dev
        dev_abs = dev + self._wrap
        n = len(pkt.samples)
        gap = 0
        restart = False
        if self._expected is None:
            self._offset = -dev_abs
        else:
            idx = dev_abs + self._offset
            if idx > self._expected:
                gap = idx - self._expected
                self.stats.gaps += 1
                self.stats.lost_samples += gap
            elif idx < self._expected:
                # Index zurückgesprungen: Gerät neu gestartet. Nummerierung fortsetzen.
                self._offset = self._expected - dev_abs
                restart = True
                self.stats.restarts += 1
        start = dev_abs + self._offset
        self._expected = start + n
        self.stats.packets += 1
        self.stats.samples += n
        return Chunk(start, pkt.samples, pkt.flags, gap, restart)
