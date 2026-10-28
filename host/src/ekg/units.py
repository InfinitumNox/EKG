"""Umrechnung der Rohwerte in Elektrodenspannung."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import numpy as np


@dataclass(frozen=True, slots=True)
class Conversion:
    """Rohsumme -> mV an den Elektroden.

    raw ist die Summe von `oversampling` ADC-Werten. Der AD8232 legt sein Ausgangssignal
    auf die halbe Versorgungsspannung; da ADC-Referenz und AD8232 an derselben 3,3-V-Schiene
    hängen, ist die Messung ratiometrisch und der Nullpunkt liegt bei Vref/2.
    """

    oversampling: int = 64
    adc_bits: int = 12
    vref_v: float = 3.3
    gain: float = 1100.0
    gain_calibrated: bool = False

    @property
    def lsb_v(self) -> float:
        return self.vref_v / (2**self.adc_bits) / self.oversampling

    @property
    def raw_max(self) -> int:
        return (2**self.adc_bits - 1) * self.oversampling

    @property
    def full_scale_mv(self) -> float:
        """Halber Aussteuerbereich bezogen auf den Eingang (±)."""
        return self.vref_v / 2 / self.gain * 1000.0

    def raw_to_mv(self, raw: np.ndarray) -> np.ndarray:
        v = np.asarray(raw, dtype=np.float64) * self.lsb_v
        return (v - self.vref_v / 2) / self.gain * 1000.0

    def mv_to_raw(self, mv: np.ndarray) -> np.ndarray:
        v = np.asarray(mv, dtype=np.float64) / 1000.0 * self.gain + self.vref_v / 2
        return np.clip(np.round(v / self.lsb_v), 0, self.raw_max).astype(np.uint32)

    def to_dict(self) -> dict[str, object]:
        return {
            "oversampling": self.oversampling,
            "adc_bits": self.adc_bits,
            "vref_v": self.vref_v,
            "gain": self.gain,
            "gain_calibrated": self.gain_calibrated,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> Conversion:
        return cls(
            oversampling=int(d.get("oversampling", 64)),
            adc_bits=int(d.get("adc_bits", 12)),
            vref_v=float(d.get("vref_v", 3.3)),
            gain=float(d.get("gain", 1100.0)),
            gain_calibrated=bool(d.get("gain_calibrated", False)),
        )
