"""Offline-Auswertung einer Aufzeichnung."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field

import numpy as np

from .dsp.beats import Delineation, MedianBeat, delineate, median_beat
from .dsp.ectopy import Ectopy, confirm_ectopics
from .dsp.filters import PRESETS, FilterSettings, zero_phase
from .dsp.hr import instantaneous_hr, smooth_hr
from .dsp.hrv import (
    HrvResult,
    Intervals,
    compute_hrv,
    nn_mask,
    rhythm_hints,
    rr_overlap_mask,
    segment_hrv,
)
from .dsp.qrs import Beat, detect_qrs, refine_r_peaks
from .dsp.respiration import estimate_breathing, qrs_amplitudes
from .dsp.signal_quality import RecordingQuality, assess_recording, invalid_mask, mask_intervals
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
    quality: RecordingQuality = field(default_factory=RecordingQuality)
    ectopy: Ectopy = field(default_factory=Ectopy)
    invalid: Intervals = field(default_factory=list)
    hints: list[str] = field(default_factory=list)
    lead_off_s: float = 0.0
    gap_s: float = 0.0

    @property
    def r_index(self) -> np.ndarray:
        return np.array([b.index for b in self.beats])

    @property
    def notes(self) -> list[str]:
        """Rhythmus-Hinweise und Hinweise zur Signalqualität (keine Diagnose)."""
        return self.hints + [f"Signal: {n}" for n in self.quality.notes]

    def hrv_for(self, t0: float, t1: float) -> HrvResult | None:
        r = self.r_index / self.fs
        return compute_hrv(r[(r >= t0) & (r <= t1)], self.invalid)


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
    # Lead-off, Übersteuerung und Lücken samt Rand ausblenden: dort entstehen sonst Phantom-Schläge.
    mask = invalid_mask(rec.raw, rec.flags, rec.conversion, fs)
    invalid = mask_intervals(mask, fs)
    mv_clean = mv.copy()
    mv_clean[mask] = np.nan
    quality = assess_recording(mv, rec.flags, mask, fs)

    step("Filtern (phasenfrei)", 0.1)
    filtered = zero_phase(mv_clean, fs, settings)

    step("R-Zacken erkennen", 0.3)
    beats = detect_qrs(mv_clean, fs)
    beats = [b for b in beats if 0 <= int(b.index) < len(mv) and not mask[int(b.index)]]
    beats = refine_r_peaks(filtered, beats, fs)
    r = np.array([b.index for b in beats])
    t = r / fs

    step("Herzfrequenz und HRV", 0.6)
    hr_t, hr_bpm = instantaneous_hr(r, fs)
    if len(hr_t):
        # HF-Werte über ausgeblendete Abschnitte hinweg sind keine echten Intervalle.
        keep = ~rr_overlap_mask(t, invalid)[np.searchsorted(t, hr_t) - 1]
        hr_t, hr_bpm = hr_t[keep], hr_bpm[keep]
    hrv = compute_hrv(t, invalid)
    segments = segment_hrv(t, invalid=invalid)
    breathing = estimate_breathing(t, qrs_amplitudes(filtered, r, fs), invalid)

    step("Median-Schlag vermessen", 0.8)
    mb = median_beat(filtered, r, fs)
    dl = delineate(mb) if mb else None
    _ok, cls = nn_mask(np.diff(t), rr_overlap_mask(t, invalid)) if len(t) > 1 else (None, None)
    ectopy = (
        confirm_ectopics(
            filtered, r, fs, cls.ectopic, mb.template if mb else None, mb.r_offset if mb else 0
        )
        if cls is not None
        else Ectopy()
    )
    hints = rhythm_hints(t, invalid, ectopy)
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
        quality=quality,
        ectopy=ectopy,
        invalid=invalid,
        hints=hints,
        lead_off_s=float(lead_off.sum() / fs),
        gap_s=float((~np.isfinite(rec.raw)).sum() / fs),
    )


def quality_rows(an: Analysis) -> list[tuple[str, str]]:
    q = an.quality
    rows = []
    if q.mains_uv is not None and q.mains_ratio is not None:
        level = (
            "sehr stark" if q.mains_ratio > 0.5 else "erhöht" if q.mains_ratio > 0.1 else "gering"
        )
        rows.append(
            (
                "Netzbrumm 50 Hz (vor Filter)",
                f"{q.mains_uv:.0f} µV eff., {level} ({100 * q.mains_ratio:.0f} %)",
            )
        )
    rows.append(("Lücken / ohne Kontakt", f"{an.gap_s:.1f} s / {an.lead_off_s:.1f} s"))
    if q.masked_s:
        rows.append(("Ausgeblendet (Übersteuerung)", f"{q.masked_s:.1f} s"))
    return rows
