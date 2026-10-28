"""Signalqualität als Ampel mit Begründung.

Kriterien (jeweils auf den letzten Sekunden):
- Elektrodenkontakt (Lead-off-Flags des AD8232)
- Übersteuerung (ADC am Anschlag)
- R-Zacken vorhanden und ausreichend groß
- Konstanz der Schlagform (Korrelation mit dem Median-Schlag)
- Kurtosis (kSQI): EKG ist spitz verteilt, Rauschen nicht
- Anteil 50-Hz-Brumm (wird zwar weggefiltert, zeigt aber schlechten Kontakt an)
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum

import numpy as np
from scipy import signal, stats

from ..protocol import Flag


class Level(IntEnum):
    GOOD = 0
    FAIR = 1
    POOR = 2
    NO_CONTACT = 3


LEVEL_LABELS = {
    Level.GOOD: "gut",
    Level.FAIR: "eingeschränkt",
    Level.POOR: "schlecht",
    Level.NO_CONTACT: "kein Kontakt",
}


@dataclass(slots=True)
class QualityReport:
    level: Level = Level.POOR
    reasons: list[str] = field(default_factory=list)
    mains_ratio: float | None = None
    kurtosis: float | None = None
    template_corr: float | None = None
    r_amplitude_mv: float | None = None

    def worsen(self, level: Level, reason: str) -> None:
        if level > self.level:
            self.level = level
        self.reasons.append(reason)


def assess(
    hp_mv: np.ndarray,
    filtered_mv: np.ndarray,
    flags: np.ndarray,
    beat_offsets: np.ndarray,
    beat_amplitudes: np.ndarray,
    fs: float,
) -> QualityReport:
    """hp_mv: nur hochpassgefiltert (für Brumm), filtered_mv: Anzeige-Filter;
    beat_offsets: R-Positionen relativ zum Fensteranfang (Samples)."""
    rep = QualityReport(level=Level.GOOD)
    n = len(filtered_mv)
    last_s = int(fs)
    if n == 0:
        rep.worsen(Level.POOR, "keine Daten")
        return rep

    lo = flags[-last_s:] & (Flag.LO_PLUS | Flag.LO_MINUS)
    if lo.any():
        which = []
        if (lo & Flag.LO_PLUS).any():
            which.append("LO+")
        if (lo & Flag.LO_MINUS).any():
            which.append("LO−")
        rep.worsen(Level.NO_CONTACT, f"Elektrode ohne Kontakt ({', '.join(which)})")
        return rep
    if (flags[-2 * last_s :] & Flag.CLIP).any():
        rep.worsen(Level.POOR, "Übersteuerung – Bewegung oder Elektrodenkontakt")

    x = filtered_mv[np.isfinite(filtered_mv)]
    if len(x) < fs:
        rep.worsen(Level.POOR, "zu wenig Daten")
        return rep

    rep.kurtosis = float(stats.kurtosis(x, fisher=False))
    if rep.kurtosis < 3.5:
        rep.worsen(Level.POOR, "kein EKG-typisches Signal")
    elif rep.kurtosis < 5.0:
        rep.worsen(Level.FAIR, "Signal verrauscht")

    hp = hp_mv[np.isfinite(hp_mv)]
    if len(hp) >= fs:
        f, pxx = signal.welch(hp, fs=fs, nperseg=min(len(hp), int(2 * fs)))
        band = (f >= 0.5) & (f <= 40)
        mains = (f >= 48) & (f <= 52)
        p_sig = float(np.trapezoid(pxx[band], f[band])) if band.any() else 0.0
        p_mains = float(np.trapezoid(pxx[mains], f[mains])) if mains.any() else 0.0
        if p_sig > 0:
            rep.mains_ratio = p_mains / p_sig
            if rep.mains_ratio > 0.5:
                rep.worsen(Level.POOR, "starker 50-Hz-Brumm – Kontakt/Kabelführung prüfen")
            elif rep.mains_ratio > 0.1:
                rep.worsen(Level.FAIR, "50-Hz-Brumm (wird gefiltert)")

    if len(beat_offsets) == 0:
        rep.worsen(Level.POOR, "keine R-Zacken erkannt")
        return rep

    amp = float(np.median(np.abs(beat_amplitudes)))
    rep.r_amplitude_mv = amp
    if amp < 0.2:
        rep.worsen(Level.FAIR, "kleine Amplitude – Elektrodenposition prüfen")

    pre, post = int(0.2 * fs), int(0.4 * fs)
    segs = [
        filtered_mv[int(b) - pre : int(b) + post]
        for b in beat_offsets
        if int(b) - pre >= 0 and int(b) + post <= n
    ]
    segs = [s for s in segs if np.all(np.isfinite(s))]
    if len(segs) >= 3:
        m = np.vstack(segs)
        tmpl = np.median(m, axis=0)
        corr = [float(np.corrcoef(s, tmpl)[0, 1]) for s in m]
        rep.template_corr = float(np.mean(corr))
        if rep.template_corr < 0.6:
            rep.worsen(Level.POOR, "Schlagform instabil – Artefakte")
        elif rep.template_corr < 0.85:
            rep.worsen(Level.FAIR, "Schlagform wechselnd")
    return rep
