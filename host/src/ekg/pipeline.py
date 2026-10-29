"""Live-Verarbeitung: Pakete -> Aufzeichnung, Filter, QRS, HF, Signalqualität.

Läuft im Erfassungs-Thread, damit Aufzeichnung und Erkennung nicht von der Oberfläche
abhängen. Die Oberfläche liest nur Momentaufnahmen (Ringpuffer, Schlagliste).
"""

from __future__ import annotations

import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from .device.stream import StreamAssembler
from .dsp.filters import FilterSettings, StreamingSos, design_sos
from .dsp.hr import HeartRateTracker
from .dsp.qrs import Beat, QrsDetector
from .dsp.quality import Level, QualityReport, assess
from .protocol import Flag, InfoPacket, SamplesPacket
from .ringbuffer import RingBuffer
from .storage.recording import RecordingWriter
from .units import Conversion

GAP_FLAG = 0x80  # nur im Host: Sample wurde für eine Lücke eingesetzt
MAX_FILL_S = 2.0
QUALITY_WINDOW_S = 4.0
QUALITY_INTERVAL_S = 0.5


@dataclass(frozen=True, slots=True)
class Event:
    index: int
    kind: str  # "lead_off", "lead_on", "gap", "restart", "clip"
    detail: str = ""


class LivePipeline:
    DISPLAY_S = 120
    MAX_BEATS = 200_000  # ~2 Tage

    def __init__(
        self,
        fs: int = 500,
        conversion: Conversion | None = None,
        filter_settings: FilterSettings | None = None,
    ):
        self._lock = threading.RLock()
        self.filter_settings = filter_settings or FilterSettings()
        self.info: InfoPacket | None = None
        self.recorder: RecordingWriter | None = None
        self.on_beat: list = []  # Callbacks (im Erfassungs-Thread!)
        self.configure(fs, conversion or Conversion())

    # --------------------------------------------------------------- Einrichtung
    def configure(self, fs: int, conversion: Conversion) -> None:
        with self._lock:
            self.fs = int(fs)
            self.conversion = conversion
            cap = self.DISPLAY_S * self.fs
            self.display = RingBuffer(cap, np.float32)
            self.unfiltered = RingBuffer(cap, np.float32)  # mV ohne Filter (Kalibrierung)
            self.hp = RingBuffer(int(QUALITY_WINDOW_S * self.fs) * 2, np.float32)
            self.flags = RingBuffer(cap, np.uint8, fill=0)
            self.assembler = StreamAssembler()
            self.detector = QrsDetector(self.fs)
            self.hr = HeartRateTracker(self.fs)
            self.beats: deque[Beat] = deque(maxlen=self.MAX_BEATS)
            self.events: deque[Event] = deque(maxlen=10_000)
            self.quality = QualityReport(Level.POOR, ["warte auf Daten"])
            self._display_filter = StreamingSos(design_sos(self.filter_settings, self.fs))
            self._hp_filter = StreamingSos(design_sos(FilterSettings(0.5, None, None), self.fs))
            self._last_mv = 0.0
            self._lead_off = False
            self._detector_next: int | None = None
            self._last_quality = 0.0
            self.t0_wall: float | None = None  # Wanduhrzeit von Index 0
            self.test_signal = False
            self.cal_active = False

    def set_conversion(self, conversion: Conversion) -> None:
        with self._lock:
            self.conversion = conversion
            self.recorder_event({"type": "conversion", **conversion.to_dict()})

    def set_filter(self, settings: FilterSettings) -> None:
        with self._lock:
            self.filter_settings = settings
            f = StreamingSos(design_sos(settings, self.fs))
            f.process(np.array([self._last_mv]))  # auf den aktuellen Wert einschwingen
            self._display_filter = f

    def set_info(self, info: InfoPacket) -> None:
        with self._lock:
            self.info = info

    # --------------------------------------------------------------- Aufnahme
    def start_recording(
        self, path: Path, extra_header: dict[str, Any] | None = None
    ) -> RecordingWriter:
        with self._lock:
            if self.recorder:
                raise RuntimeError("Aufnahme läuft bereits")
            info = self.info
            header: dict[str, Any] = {
                "sample_rate_hz": self.fs,
                "conversion": self.conversion.to_dict(),
                "device": {
                    "name": info.name if info else "",
                    "board_id": info.board_id if info else "",
                    "firmware": info.fw_version_str if info else "",
                    "mode": int(info.mode) if info else 0,
                },
                "filter_live": self.filter_settings.describe(),
            }
            header.update(extra_header or {})
            self.recorder = RecordingWriter(path, header)
            return self.recorder

    def stop_recording(self) -> Path | None:
        with self._lock:
            rec, self.recorder = self.recorder, None
        if rec:
            rec.close()
            return rec.path
        return None

    def add_marker(self, text: str) -> None:
        with self._lock:
            idx = self.display.end
            self.events.append(Event(idx, "marker", text))
            if self.recorder:
                self.recorder.write_marker(idx, text)

    def recorder_event(self, event: dict[str, Any]) -> None:
        with self._lock:  # auch aus dem GUI-Thread, parallel zu den Samples
            if self.recorder:
                self.recorder.write_event(self.display.end, event)

    # --------------------------------------------------------------- Verarbeitung
    def push(self, pkt: SamplesPacket) -> None:
        with self._lock:
            chunk = self.assembler.push(pkt)
            if self.t0_wall is None:
                self.t0_wall = time.time() - (chunk.start + len(chunk.raw)) / self.fs
            self.test_signal = bool(chunk.flags & Flag.TEST)
            self.cal_active = bool(chunk.flags & Flag.CAL)
            if chunk.restart:
                self.events.append(Event(chunk.start, "restart"))
                self._reset_dsp(chunk.start)
            if chunk.gap_before:
                self._handle_gap(chunk.start - chunk.gap_before, chunk.gap_before)
            if self.recorder:
                self.recorder.write_samples(chunk.start, chunk.raw, int(chunk.flags))
            mv = self.conversion.raw_to_mv(chunk.raw)
            self._process(chunk.start, mv, int(chunk.flags))

    def _handle_gap(self, start: int, n: int) -> None:
        self.events.append(Event(start, "gap", f"{n} Samples"))
        if self.recorder:
            self.recorder.write_gap(start, n)
        if n <= MAX_FILL_S * self.fs:
            # Kurze Lücke: letzten Wert halten, damit Filter und Detektor stetig bleiben.
            self._process(start, np.full(n, self._last_mv), GAP_FLAG)
        else:
            self._reset_dsp(start + n)
            self.hp.clear(start + n)
            if n <= self.display.capacity:
                self.display.append(np.full(n, np.nan, dtype=np.float32))
                self.unfiltered.append(np.full(n, np.nan, dtype=np.float32))
                self.flags.append(np.full(n, GAP_FLAG, dtype=np.uint8))
            else:  # länger als der Puffer: nur den Index fortschreiben
                self.display.clear(start + n)
                self.unfiltered.clear(start + n)
                self.flags.clear(start + n)

    def _reset_dsp(self, next_index: int) -> None:
        self._display_filter.reset()
        self._hp_filter.reset()
        self.detector.reset(next_index)
        self._detector_next = next_index
        self.hr.reset()

    def _process(self, start: int, mv: np.ndarray, flags: int) -> None:
        n = len(mv)
        if n == 0:
            return
        lead_off = bool(flags & (Flag.LO_PLUS | Flag.LO_MINUS))
        if lead_off != self._lead_off:
            self._lead_off = lead_off
            self.events.append(Event(start, "lead_off" if lead_off else "lead_on"))
            if self.recorder:
                self.recorder.write_event(start, {"type": "lead_off" if lead_off else "lead_on"})
        if lead_off:
            mv = np.full(n, self._last_mv)  # Schienenspannung nicht durch die Filter jagen
        self.display.append(self._display_filter.process(mv).astype(np.float32))
        self.unfiltered.append(mv.astype(np.float32))
        self.hp.append(self._hp_filter.process(mv).astype(np.float32))
        self.flags.append(np.full(n, flags & 0xFF, dtype=np.uint8))
        self._last_mv = float(mv[-1])

        # Bei Übersteuerung (Bewegung) erzeugt die Erkennung Phantom-Schläge: wie Lead-off behandeln.
        unusable = lead_off or bool(flags & Flag.CLIP)
        if unusable or self._detector_next is None or self._detector_next != start:
            # Nach Kontaktverlust oder Übersteuerung neu lernen lassen.
            self.detector.reset(start + n)
            self._detector_next = start + n
            if unusable:
                self.hr.reset()
        else:
            for beat in self.detector.process(mv, start):
                self.beats.append(beat)
                self.hr.add(beat.index)
                for cb in self.on_beat:
                    cb(beat)
            self._detector_next = start + n

        now = time.monotonic()
        if now - self._last_quality >= QUALITY_INTERVAL_S:
            self._last_quality = now
            self._update_quality()

    def _update_quality(self) -> None:
        w = int(QUALITY_WINDOW_S * self.fs)
        end = self.display.end
        start = end - w
        beats = [b for b in self.beats if b.index >= start]
        self.quality = assess(
            self.hp.get(start, end),
            self.display.get(start, end),
            self.flags.get(start, end),
            np.array([b.index - start for b in beats]),
            np.array([b.amplitude_mv for b in beats]),
            self.fs,
        )
        if not self.detector.initialized and self.quality.level < Level.NO_CONTACT:
            self.quality.worsen(Level.FAIR, "Erkennung lernt (2 s)")

    # --------------------------------------------------------------- Abfragen (GUI)
    def heart_rate(self) -> float | None:
        with self._lock:
            return self.hr.bpm(self.display.end)

    def last_rr_ms(self) -> float | None:
        with self._lock:
            return self.hr.last_rr_ms()

    def beats_between(self, start: float, stop: float) -> list[Beat]:
        with self._lock:
            out = []
            for b in reversed(self.beats):
                if b.index < start:
                    break
                if b.index < stop:
                    out.append(b)
            return out[::-1]

    def all_beats(self) -> list[Beat]:
        with self._lock:
            return list(self.beats)

    def events_since(self, start: int) -> list[Event]:
        with self._lock:
            return [e for e in self.events if e.index >= start]
