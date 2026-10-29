"""Signalqualität einer Aufzeichnung: unbrauchbare Abschnitte und Netzbrumm.

Bei Bewegung läuft der AD8232-Ausgang an den Anschlag (Übersteuerung). In solchen
Abschnitten erzeugt jeder Detektor Phantom-Schläge; sie werden deshalb samt Rand
ausgeblendet. Der Anteil des 50-Hz-Brumms zeigt schlechten Elektrodenkontakt oder eine
Verbindung zum Stromnetz (Ladekabel!) an.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy import signal
from scipy.ndimage import binary_dilation

from ..protocol import Flag
from ..units import Conversion

MARGIN_S = 0.5  # Rand um übersteuerte Stellen (Filter-Einschwingen, Erholung des AD8232)
RAIL_FRACTION = 0.01  # Rohwert innerhalb 1 % vom Anschlag gilt als übersteuert


@dataclass(slots=True)
class RecordingQuality:
    mains_uv: float | None = None  # Effektivwert des 50-Hz-Anteils im Rohsignal
    mains_ratio: float | None = None  # Leistung 49–51 Hz relativ zu 0,5–40 Hz
    clipped_s: float = 0.0
    masked_s: float = 0.0
    notes: list[str] = field(default_factory=list)


def invalid_mask(raw: np.ndarray, flags: np.ndarray, conv: Conversion, fs: float) -> np.ndarray:
    """True für Samples ohne verwertbares EKG (Lead-off, Übersteuerung, Lücke) samt Rand."""
    raw = np.asarray(raw, dtype=np.float64)
    lead_off = (flags & (Flag.LO_PLUS | Flag.LO_MINUS)) != 0
    rail = RAIL_FRACTION * conv.raw_max
    sat = np.isfinite(raw) & ((raw <= rail) | (raw >= conv.raw_max - rail))
    bad = sat | ((flags & Flag.CLIP) != 0)
    margin = max(1, round(MARGIN_S * fs))
    if bad.any():
        bad = binary_dilation(bad, structure=np.ones(2 * margin + 1, dtype=bool))
    return bad | lead_off | ~np.isfinite(raw)


def mask_intervals(mask: np.ndarray, fs: float) -> list[tuple[float, float]]:
    """Zusammenhängende True-Bereiche als (Beginn, Ende) in Sekunden."""
    if not mask.any():
        return []
    d = np.diff(np.concatenate([[0], mask.astype(np.int8), [0]]))
    starts, ends = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    return [(float(s) / fs, float(e) / fs) for s, e in zip(starts, ends, strict=True)]


def mains_level(mv: np.ndarray, fs: float) -> tuple[float | None, float | None]:
    x = np.asarray(mv, dtype=np.float64)
    x = x[np.isfinite(x)]
    if len(x) < 4 * fs:
        return None, None
    f, pxx = signal.welch(x - np.mean(x), fs=fs, nperseg=int(min(len(x), 8 * fs)))
    m50 = (f >= 49) & (f <= 51)
    band = (f >= 0.5) & (f <= 40)
    p50 = float(np.trapezoid(pxx[m50], f[m50]))
    pecg = float(np.trapezoid(pxx[band], f[band]))
    return float(np.sqrt(p50) * 1000.0), (p50 / pecg if pecg > 0 else None)


def assess_recording(
    mv: np.ndarray, flags: np.ndarray, mask: np.ndarray, fs: float
) -> RecordingQuality:
    q = RecordingQuality()
    clip = (flags & Flag.CLIP) != 0
    q.clipped_s = float(clip.sum() / fs)
    q.masked_s = float(mask.sum() / fs)
    lead_off = (flags & (Flag.LO_PLUS | Flag.LO_MINUS)) != 0
    q.mains_uv, q.mains_ratio = mains_level(np.where(lead_off, np.nan, mv), fs)
    if q.mains_ratio is not None:
        if q.mains_ratio > 0.5:
            q.notes.append(
                "Sehr starker 50-Hz-Brumm (vor dem Filtern so groß wie das EKG): Elektrodenkontakt "
                "prüfen – und sicherstellen, dass der Laptop nicht am Netzteil hing."
            )
        elif q.mains_ratio > 0.1:
            q.notes.append("Erhöhter 50-Hz-Brumm (wird herausgefiltert): Elektrodenkontakt prüfen.")
    if q.masked_s > 1.0:
        q.notes.append(
            f"{q.masked_s:.0f} s ausgeblendet (Übersteuerung durch Bewegung oder fehlender Kontakt)."
        )
    return q
