"""Kalibrierung über die Loopback-Schaltung (docs/KALIBRIERUNG.md).

Die Firmware erzeugt an GP15 einen Sinus mit ±0,5 V um 1,65 V. Ein Teiler aus ~102,5 kΩ
Längswiderstand und 100 Ω Shunt macht daraus ±0,49 mV an den Elektrodeneingängen. Aus der gemessenen Amplitude ergeben sich
die tatsächliche Verstärkung des Boards und, über mehrere Frequenzen, der Frequenzgang.

Während der Kalibrierung darf kein Mensch angeschlossen sein.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

DEFAULT_FREQS = (
    0.05,
    0.1,
    0.2,
    0.3,
    0.5,
    0.7,
    1.0,
    2.0,
    5.0,
    10.0,
    20.0,
    30.0,
    40.0,
    50.0,
    70.0,
    100.0,
    150.0,
)
QUICK_FREQS = (0.2, 0.5, 1.0, 5.0, 10.0, 20.0, 40.0, 70.0)
REFERENCE_HZ = 10.0


def input_amplitude_mv(
    r_series: float = 102.5e3, r_shunt: float = 100.0, swing_v: float = 0.5
) -> float:
    """Spitzenamplitude an den Elektrodeneingängen in mV.

    r_series ist der gesamte Widerstand im Stromweg ohne Shunt: 100 kΩ, die beiden
    RC-Widerstände und der Innenwiderstand des Mittenteilers (2 × 1 kΩ -> 500 Ω).
    """
    return swing_v * r_shunt / (r_series + r_shunt) * 1000.0


def settle_s(f: float) -> float:
    return max(2.0, 3.0 / f)


def measure_s(f: float) -> float:
    return max(2.0, 5.0 / f)


def sine_amplitude(x: np.ndarray, fs: float, f: float) -> float:
    """Amplitude bei bekannter Frequenz per Kleinste-Quadrate-Fit (mit Offset und Trend)."""
    x = np.asarray(x, dtype=np.float64)
    x = x[np.isfinite(x)]
    t = np.arange(len(x)) / fs
    a = np.column_stack([np.sin(2 * np.pi * f * t), np.cos(2 * np.pi * f * t), np.ones_like(t), t])
    coef, *_ = np.linalg.lstsq(a, x, rcond=None)
    return float(np.hypot(coef[0], coef[1]))


@dataclass(frozen=True, slots=True)
class CalibrationResult:
    gain: float
    highpass_hz: float | None
    lowpass_hz: float | None
    response: list[tuple[float, float]]  # (Hz, dB relativ zu 10 Hz)


def evaluate(
    amplitudes: dict[float, float], a_in_mv: float, nominal_gain: float
) -> CalibrationResult:
    """amplitudes: gemessene Amplitude in mV (mit nominaler Verstärkung umgerechnet)."""
    if REFERENCE_HZ not in amplitudes:
        raise ValueError("Referenzfrequenz 10 Hz fehlt")
    a_ref = amplitudes[REFERENCE_HZ]
    gain = nominal_gain * a_ref / a_in_mv
    freqs = np.array(sorted(amplitudes))
    db = np.array([20 * np.log10(max(amplitudes[f], 1e-12) / a_ref) for f in freqs])
    return CalibrationResult(
        gain=gain,
        highpass_hz=_corner(freqs, db, low_side=True),
        lowpass_hz=_corner(freqs, db, low_side=False),
        response=[(float(f), float(d)) for f, d in zip(freqs, db, strict=True)],
    )


def _corner(freqs: np.ndarray, db: np.ndarray, low_side: bool) -> float | None:
    """-3-dB-Frequenz durch log-lineare Interpolation; None, wenn außerhalb des Messbereichs."""
    ref = np.argmin(np.abs(freqs - REFERENCE_HZ))
    idx = range(ref, 0, -1) if low_side else range(ref, len(freqs) - 1)
    for i in idx:
        j = i - 1 if low_side else i + 1
        if db[i] >= -3.0 > db[j]:
            lf = np.interp(-3.0, [db[j], db[i]], [np.log10(freqs[j]), np.log10(freqs[i])])
            return float(10**lf)
    return None
