"""Erkennt, ob der Laptop am Netzteil hängt (Linux, /sys/class/power_supply).

Sicherheitsregel des Projekts: Elektroden nur am Körper, wenn der Laptop im Akkubetrieb
läuft. Diese Prüfung ist eine Erinnerung, kein Schutz: sie sieht z. B. keinen über HDMI
angeschlossenen Monitor.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path

POWER_SUPPLY = Path("/sys/class/power_supply")


class PowerState(Enum):
    BATTERY = "battery"
    EXTERNAL = "external"  # Netzteil/USB-C-Ladegerät angeschlossen
    NO_BATTERY = "no_battery"  # vermutlich Desktop-PC: immer netzbetrieben
    UNKNOWN = "unknown"


@dataclass(frozen=True, slots=True)
class PowerStatus:
    state: PowerState
    detail: str = ""

    @property
    def safe_for_electrodes(self) -> bool:
        return self.state == PowerState.BATTERY


def _read(p: Path) -> str:
    try:
        return p.read_text().strip()
    except OSError:
        return ""


def power_status(root: Path = POWER_SUPPLY) -> PowerStatus:
    if not root.is_dir():
        return PowerStatus(PowerState.UNKNOWN, "keine Energieinformationen verfügbar")
    has_battery = False
    external: list[str] = []
    for dev in sorted(root.iterdir()):
        typ = _read(dev / "type")
        if typ == "Battery":
            if _read(dev / "scope") == "Device":
                continue  # Akku einer Maus o. ä.
            has_battery = True
            if _read(dev / "status") == "Charging":
                external.append(f"{dev.name} lädt")
        elif typ in ("Mains", "USB") and _read(dev / "online") == "1":
            if _read(dev / "scope") == "Device":
                continue
            external.append(dev.name)
    if external:
        return PowerStatus(PowerState.EXTERNAL, ", ".join(external))
    if not has_battery:
        return PowerStatus(PowerState.NO_BATTERY, "kein Akku gefunden")
    return PowerStatus(PowerState.BATTERY)
