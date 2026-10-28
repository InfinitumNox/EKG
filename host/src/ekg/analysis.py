"""Offline-Auswertung einer Aufzeichnung."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field

import numpy as np

from .dsp.beats import Delineation, MedianBeat, delineate, median_beat
from .dsp.filters import PRESETS, FilterSettings, zero_phase
from .dsp.hr import instantaneous_hr, smooth_hr
from .dsp.hrv import HrvResult, compute_hrv, rhythm_hints, segment_hrv
from .dsp.qrs import Beat, detect_qrs, refine_r_peaks
from .dsp.respiration import breathing_from_amplitude, qrs_amplitudes
from .protocol import Flag
from .storage.recording import Recording


@dataclass
class Analysis:
    fs: float
    filtered_mv: np.ndarray
    filter_settings: FilterSettings
    beats: list[Beat]
    hr_t: np.ndarray
    hr_bpm: np.ndarray
    hr_smooth: np.ndarray
    hrv: HrvResult | None
    segments: list[HrvResult]
    breathing_hz: float | None
    median: MedianBeat | None
    delineation: Delineation | None
    hints: list[str] = field(default_factory=list)
    lead_off_s: float = 0.0
    gap_s: float = 0.0

    @property
    def r_index(self) -> np.ndarray:
        return np.array([b.index for b in self.beats])

    def hrv_for(self, t0: float, t1: float) -> HrvResult | None:
        r = self.r_index / self.fs
        return compute_hrv(r[(r >= t0) & (r <= t1)])


def analyse(
    rec: Recording,
    settings: FilterSettings | None = None,
    progress: Callable[[str, float], None] | None = None,
) -> Analysis:
    settings = settings or PRESETS["monitor"][1]
    step = progress or (lambda _m, _p: None)
    fs = rec.fs
    mv = rec.mv
    lead_off = (rec.flags & (Flag.LO_PLUS | Flag.LO_MINUS)) != 0
    mv_clean = mv.copy()
    mv_clean[lead_off] = np.nan

    step("Filtern (phasenfrei)", 0.1)
    filtered = zero_phase(mv_clean, fs, settings)

    step("R-Zacken erkennen", 0.3)
    beats = detect_qrs(mv_clean, fs)
    beats = [b for b in beats if 0 <= int(b.index) < len(mv) and not lead_off[int(b.index)]]
    beats = refine_r_peaks(filtered, beats, fs)
    r = np.array([b.index for b in beats])

    step("Herzfrequenz und HRV", 0.7)
    hr_t, hr_bpm = instantaneous_hr(r, fs)
    hrv = compute_hrv(r / fs)
    segments = segment_hrv(r / fs)
    hints = rhythm_hints(r / fs)
    breathing = breathing_from_amplitude(r / fs, qrs_amplitudes(filtered, r, fs))

    step("Median-Schlag vermessen", 0.85)
    mb = median_beat(filtered, r, fs)
    dl = delineate(mb) if mb else None
    step("fertig", 1.0)
    return Analysis(
        fs=fs,
        filtered_mv=filtered,
        filter_settings=settings,
        beats=beats,
        hr_t=hr_t,
        hr_bpm=hr_bpm,
        hr_smooth=smooth_hr(hr_t, hr_bpm),
        hrv=hrv,
        segments=segments,
        breathing_hz=breathing,
        median=mb,
        delineation=dl,
        hints=hints,
        lead_off_s=float(lead_off.sum() / fs),
        gap_s=float((~np.isfinite(rec.raw)).sum() / fs),
    )
