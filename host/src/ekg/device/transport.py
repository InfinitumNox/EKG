"""Serielle Verbindung zum Pico und automatische Erkennung."""

from __future__ import annotations

from dataclasses import dataclass

import serial
from serial.tools import list_ports

PICO_VID = 0x2E8A
PRODUCT = "Pico EKG"


@dataclass(frozen=True, slots=True)
class PortInfo:
    device: str
    description: str
    serial_number: str = ""


def find_ports() -> list[PortInfo]:
    """Datenschnittstellen (CDC 0) angeschlossener Pico-EKGs."""
    out = []
    for p in list_ports.comports():
        if p.vid != PICO_VID:
            continue
        iface = (p.interface or "").lower()
        # Linux liefert den Interface-String ("EKG Data"); sonst Interface 0 über die Location.
        is_data = "data" in iface if iface else (p.location or "").endswith(":1.0")
        if not is_data:
            continue
        if p.product and PRODUCT.lower() not in p.product.lower():
            continue
        out.append(PortInfo(p.device, f"{p.product or 'Pico'} ({p.device})", p.serial_number or ""))
    return out


class SerialTransport:
    def __init__(self, device: str):
        self.device = device
        # CDC ignoriert die Baudrate; 115200 nur, weil 1200 den Bootloader auslösen würde.
        self._s = serial.Serial(device, 115200, timeout=0.05, write_timeout=1.0)
        self._s.dtr = True

    def read(self, size: int = 4096, timeout: float = 0.05) -> bytes:
        self._s.timeout = timeout
        first = self._s.read(1)  # blockiert höchstens `timeout`
        if not first:
            return b""
        n = min(size - 1, self._s.in_waiting)
        return first + (self._s.read(n) if n > 0 else b"")

    def write(self, data: bytes) -> int:
        return self._s.write(data) or 0

    def close(self) -> None:
        try:
            self._s.close()
        except OSError:
            pass

    @property
    def description(self) -> str:
        return self.device
