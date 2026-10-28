"""Persistente Einstellungen (~/.config/ekg/config.json).

Gerätekalibrierungen werden pro Board-ID gespeichert, weil jedes AD8232-Board andere
Bauteiltoleranzen hat.
"""

from __future__ import annotations

import json
import os
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


def config_dir() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "ekg"


def data_dir() -> Path:
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "ekg"


def recordings_dir() -> Path:
    return data_dir() / "aufnahmen"


@dataclass
class DeviceCalibration:
    gain: float
    measured_at: str
    method: str = ""
    highpass_hz: float | None = None
    lowpass_hz: float | None = None
    notes: str = ""
    response: list[tuple[float, float]] = field(default_factory=list)  # (Hz, dB)


@dataclass
class Settings:
    px_per_mm: float | None = None
    speed_mm_s: float = 25.0
    gain_mm_mv: float = 10.0
    filter_preset: str = "monitor"
    theme: str = "paper"
    sample_rate_hz: int = 500
    safety_ack_session: bool = False  # wird nie gespeichert, siehe save()
    devices: dict[str, DeviceCalibration] = field(default_factory=dict)
    last_port: str = ""

    @classmethod
    def load(cls, path: Path | None = None) -> Settings:
        path = path or config_dir() / "config.json"
        try:
            raw: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return cls()
        devices = {
            k: DeviceCalibration(**{**v, "response": [tuple(p) for p in v.get("response", [])]})
            for k, v in raw.pop("devices", {}).items()
        }
        known = {f for f in cls.__dataclass_fields__ if f not in ("devices", "safety_ack_session")}
        s = cls(**{k: v for k, v in raw.items() if k in known})
        s.devices = devices
        return s

    def save(self, path: Path | None = None) -> None:
        path = path or config_dir() / "config.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        d = asdict(self)
        d.pop("safety_ack_session", None)
        # Atomar schreiben, damit ein Absturz keine halbe Konfiguration hinterlässt.
        fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".config-", suffix=".json")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2, ensure_ascii=False)
        os.replace(tmp, path)
