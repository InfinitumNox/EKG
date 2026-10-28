"""Firmware auf den Pico 2 spielen: `ekg-flash [datei.uf2]`.

Ablauf:
1. UF2-Datei prüfen (muss für den RP2350 gebaut sein).
2. Läuft schon die EKG-Firmware, den Pico per 1200-Baud-Signal in den Bootloader schalten.
   Sonst um BOOT-Taste + Einstecken bitten.
3. Das Laufwerk „RP2350“ finden (bei Bedarf mit udisksctl einhängen) und die Datei kopieren.
4. Warten, bis sich der Pico als „Pico EKG“ zurückmeldet.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_FLAG_FAMILY = 0x00002000
FAMILY_RP2040 = 0xE48BFF56
FAMILIES_RP2350 = {
    0xE48BFF57: "absolute",
    0xE48BFF59: "RP2350 ARM (secure)",
    0xE48BFF5A: "RP2350 RISC-V",
    0xE48BFF5B: "RP2350 ARM (non-secure)",
}
DRIVE_LABEL = "RP2350"


class FlashError(RuntimeError):
    pass


def check_uf2(path: Path) -> int:
    """Prüft Aufbau und Zielchip; liefert die Anzahl der Blöcke."""
    data = path.read_bytes()
    if len(data) == 0 or len(data) % 512:
        raise FlashError(f"{path} ist keine UF2-Datei (Größe kein Vielfaches von 512)")
    families = set()
    for off in range(0, len(data), 512):
        m0, m1, flags = struct.unpack_from("<III", data, off)
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1:
            raise FlashError(f"{path}: ungültiger UF2-Block bei Byte {off}")
        if flags & UF2_FLAG_FAMILY:
            families.add(struct.unpack_from("<I", data, off + 28)[0])
    if FAMILY_RP2040 in families:
        raise FlashError(f"{path} ist für den RP2040 (Pico 1) gebaut, nicht für den Pico 2")
    if families and not families & set(FAMILIES_RP2350):
        raise FlashError(f"{path}: unbekannte Chip-Familie {sorted(hex(f) for f in families)}")
    return len(data) // 512


def mount_roots() -> list[Path]:
    user = os.environ.get("USER") or os.environ.get("LOGNAME") or ""
    return [Path("/run/media") / user, Path("/media") / user, Path("/media"), Path("/mnt")]


def find_drive(roots: list[Path] | None = None) -> Path | None:
    for root in roots or mount_roots():
        try:
            entries = list(root.iterdir())
        except OSError:
            continue
        for d in entries:
            info = d / "INFO_UF2.TXT"
            try:
                text = info.read_text(errors="replace")
            except OSError:
                continue
            if "RP2350" in text:
                return d
            if "RP2040" in text:
                raise FlashError(f"{d} ist ein RP2040-Bootloader (Pico 1). Falsches Board?")
    return None


def mount_with_udisks() -> Path | None:
    dev = Path("/dev/disk/by-label") / DRIVE_LABEL
    if not dev.exists() or shutil.which("udisksctl") is None:
        return None
    r = subprocess.run(
        ["udisksctl", "mount", "--no-user-interaction", "-b", str(dev)],
        capture_output=True,
        text=True,
        check=False,
    )
    m = re.search(r" at (.+?)\.?$", r.stdout.strip())
    if m:
        return Path(m.group(1))
    return find_drive()


def trigger_bootsel() -> bool:
    """Öffnet die Datenschnittstelle mit 1200 Baud: die Firmware startet dann den Bootloader."""
    import serial

    from .device.transport import find_ports

    ports = find_ports()
    if not ports:
        return False
    for p in ports:
        try:
            s = serial.Serial(p.device, 1200)
            time.sleep(0.2)
            s.close()
        except (OSError, serial.SerialException):
            pass
    return True


def wait_for(cond: Callable[[], object], timeout: float, step: float = 0.5) -> object:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return None


def _drive_now() -> Path | None:
    return find_drive() or mount_with_udisks()


def flash(uf2: Path, wait_s: float = 120.0, log: Callable[[str], None] = print) -> str | None:
    """Spielt `uf2` auf; liefert den seriellen Port der neuen Firmware (oder None)."""
    from .device.transport import find_ports

    blocks = check_uf2(uf2)
    log(f"Firmware: {uf2} ({blocks * 256 // 1024} KB)")

    drive = _drive_now()
    if drive is None and trigger_bootsel():
        log("Pico EKG gefunden – schalte in den Bootloader …")
        drive = wait_for(_drive_now, 15.0)  # type: ignore[assignment]
    if drive is None:
        log("")
        log("Bitte jetzt den Pico in den Bootloader bringen:")
        log("  1. USB-Kabel abziehen")
        log("  2. BOOT-Taste gedrückt halten")
        log("  3. USB-Kabel einstecken, Taste loslassen")
        log(f"Warte bis zu {wait_s:.0f} s auf das Laufwerk {DRIVE_LABEL} …")
        drive = wait_for(_drive_now, wait_s)  # type: ignore[assignment]
    if drive is None:
        raise FlashError(
            f"Kein Laufwerk {DRIVE_LABEL} gefunden. Wird es im Dateimanager angezeigt? "
            "Dann die Datei von Hand dorthin kopieren."
        )

    log(f"Kopiere nach {drive} …")
    try:
        shutil.copyfile(uf2, drive / uf2.name)
        os.sync()
    except OSError as exc:
        # Der Pico startet nach dem letzten Block sofort neu; das Laufwerk kann
        # verschwinden, bevor das Schließen der Datei bestätigt ist.
        log(f"(Hinweis beim Kopieren: {exc})")

    log("Warte auf den Neustart …")
    ports = wait_for(find_ports, 20.0)
    if ports:
        port = ports[0].device  # type: ignore[index]
        log(f"Fertig: Pico EKG läuft an {port}.")
        return port
    log("Firmware kopiert, aber der Pico meldet sich nicht als „Pico EKG“.")
    log("USB einmal ab- und anstecken; bleibt es dabei, die udev-Regel prüfen (install.sh).")
    return None


def default_uf2() -> Path | None:
    """Sucht die Firmware im Repository: eigener Build vor mitgelieferter Datei."""
    here = Path(__file__).resolve()
    for base in [Path.cwd(), *here.parents]:
        for rel in ("build/firmware/ekg.uf2", "firmware/prebuilt/ekg.uf2"):
            p = base / rel
            if p.exists():
                return p
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ekg-flash", description="Firmware auf den Pico 2 spielen")
    ap.add_argument("uf2", nargs="?", type=Path, help="UF2-Datei (Standard: Build oder prebuilt)")
    ap.add_argument("--wait", type=float, default=120.0, help="Sekunden auf den Bootloader warten")
    args = ap.parse_args(argv)
    uf2 = args.uf2 or default_uf2()
    if uf2 is None:
        print("Keine UF2-Datei gefunden. Pfad angeben: ekg-flash pfad/zu/ekg.uf2", file=sys.stderr)
        return 2
    try:
        flash(uf2, args.wait)
    except FlashError as exc:
        print(f"Fehler: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
