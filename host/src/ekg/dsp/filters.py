"""Filter für Anzeige und Auswertung.

Live wird kausal gefiltert (mit Zustand über Chunk-Grenzen), in der Auswertung
phasenfrei (vorwärts/rückwärts), damit die Software Strecken nicht verzerrt.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal


@dataclass(frozen=True, slots=True)
class FilterSettings:
    highpass_hz: float | None = 0.5
    lowpass_hz: float | None = 40.0
    notch_hz: float | None = 50.0
    notch_harmonics: bool = False

    def describe(self) -> str:
        parts = []
        if self.highpass_hz and self.lowpass_hz:
            parts.append(f"{_fmt(self.highpass_hz)}–{_fmt(self.lowpass_hz)} Hz")
        elif self.highpass_hz:
            parts.append(f"ab {_fmt(self.highpass_hz)} Hz")
        elif self.lowpass_hz:
            parts.append(f"bis {_fmt(self.lowpass_hz)} Hz")
        else:
            parts.append("ungefiltert")
        if self.notch_hz:
            parts.append(
                f"Notch {_fmt(self.notch_hz)} Hz" + (" + Oberw." if self.notch_harmonics else "")
            )
        return ", ".join(parts)


def _fmt(v: float) -> str:
    return f"{v:g}".replace(".", ",")


PRESETS: dict[str, tuple[str, FilterSettings]] = {
    "monitor": ("Monitor", FilterSettings(0.5, 40.0, 50.0)),
    "breit": ("Breitband", FilterSettings(0.05, 100.0, 50.0, notch_harmonics=True)),
    "roh": ("Roh", FilterSettings(None, None, None)),
}


def design_sos(settings: FilterSettings, fs: float) -> np.ndarray:
    sections: list[np.ndarray] = []
    nyq = fs / 2
    if settings.highpass_hz:
        sections.append(signal.butter(2, settings.highpass_hz, "highpass", fs=fs, output="sos"))
    if settings.lowpass_hz and settings.lowpass_hz < nyq * 0.95:
        sections.append(signal.butter(4, settings.lowpass_hz, "lowpass", fs=fs, output="sos"))
    if settings.notch_hz:
        freqs = [settings.notch_hz]
        if settings.notch_harmonics:
            freqs += [settings.notch_hz * k for k in (2, 3)]
        for f in freqs:
            if f < nyq * 0.95:
                b, a = signal.iirnotch(f, Q=30.0, fs=fs)
                sections.append(signal.tf2sos(b, a))
    if not sections:
        return np.array([[1.0, 0.0, 0.0, 1.0, 0.0, 0.0]])
    return np.vstack(sections)


class StreamingSos:
    """Kausaler SOS-Filter mit Zustand; der Start wird auf den ersten Wert eingeschwungen."""

    def __init__(self, sos: np.ndarray):
        self.sos = np.asarray(sos, dtype=np.float64)
        self._zi_unit = signal.sosfilt_zi(self.sos)
        self._zi: np.ndarray | None = None

    def reset(self) -> None:
        self._zi = None

    def process(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float64)
        if len(x) == 0:
            return x
        if self._zi is None:
            self._zi = self._zi_unit * x[0]
        y, self._zi = signal.sosfilt(self.sos, x, zi=self._zi)
        return y


class StreamingFir:
    def __init__(self, b: np.ndarray):
        self.b = np.asarray(b, dtype=np.float64)
        self._zi = np.zeros(len(self.b) - 1)

    def reset(self) -> None:
        self._zi[:] = 0

    def process(self, x: np.ndarray) -> np.ndarray:
        y, self._zi = signal.lfilter(self.b, [1.0], np.asarray(x, dtype=np.float64), zi=self._zi)
        return y


def zero_phase(x: np.ndarray, fs: float, settings: FilterSettings) -> np.ndarray:
    """Phasenfreie Filterung für die Auswertung; NaN-Lücken werden vorher überbrückt."""
    x = np.asarray(x, dtype=np.float64)
    if len(x) < 16:
        return x.copy()
    filled, mask = fill_gaps(x)
    sos = design_sos(settings, fs)
    padlen = min(len(filled) - 1, int(3 * fs))
    y = signal.sosfiltfilt(sos, filled, padlen=padlen)
    y[mask] = np.nan
    return y


def fill_gaps(x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Ersetzt NaN durch lineare Interpolation; liefert (Signal, NaN-Maske)."""
    mask = ~np.isfinite(x)
    if not mask.any():
        return x.copy(), mask
    y = x.copy()
    good = np.flatnonzero(~mask)
    if len(good) == 0:
        return np.zeros_like(x), mask
    y[mask] = np.interp(np.flatnonzero(mask), good, x[good])
    return y, mask


def group_delay_samples(sos: np.ndarray, freq_hz: float, fs: float) -> float:
    b, a = signal.sos2tf(sos)
    _, gd = signal.group_delay((b, a), w=[freq_hz], fs=fs)
    return float(gd[0])
