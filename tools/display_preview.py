#!/usr/bin/env python3
"""Rendert die Display-Bildschirme am Laptop mit dem Firmware-Code (PNG-Vorschau).

    cd host && uv run python ../tools/display_preview.py [Zielordner]

Übersetzt firmware/src/ui und firmware/src/ecg zusammen mit tools/display_preview.c,
speist ein simuliertes EKG ein (wie es der Pico liefert) und speichert Bilder vom
Startbildschirm und von der Live-Ansicht.
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

from ekg.device.simulator import Scenario, SyntheticEcg
from ekg.units import Conversion

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "firmware" / "src"
FS = 500
FLAG_LO_PLUS = 1


def build(tmp: Path) -> Path:
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        sys.exit("kein C-Compiler gefunden")
    exe = tmp / "display_preview"
    srcs = [
        *sorted((SRC / "ui").glob("*.c")),
        *sorted((SRC / "ecg").glob("*.c")),
        *sorted((SRC / "analysis").glob("*.c")),
    ]
    inc = [f"-I{SRC / d}" for d in ("", "ui", "ecg", "analysis", "display")]
    subprocess.run(
        [cc, "-O2", "-std=c11", "-Wall", "-Wextra", *inc,
         str(ROOT / "tools" / "display_preview.c"), *map(str, srcs), "-o", str(exe), "-lm"],
        check=True,
    )  # fmt: skip
    return exe


def blocks_file(path: Path, mv: np.ndarray, flags: np.ndarray) -> None:
    conv = Conversion(oversampling=64)
    raw = conv.mv_to_raw(mv)
    with path.open("wb") as f:
        for b, start in enumerate(range(0, len(raw) - 24, 25)):
            chunk = raw[start : start + 25]
            fl = int(flags[start : start + 25].max())
            f.write(struct.pack("<BBHI", 25, fl, 64, b * 50))
            f.write(struct.pack(f"<{len(chunk)}I", *chunk))


def to_png(ppm: Path, png: Path, scale: int = 2) -> None:
    img = Image.open(ppm)
    img.resize((img.width * scale, img.height * scale), Image.Resampling.NEAREST).save(png)


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "display_preview"
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        exe = build(tmp)
        # Live: kleine R-Zacke wie in den Aufnahmen vom 29.10., dann Elektrode ab
        g = SyntheticEcg(FS, scenario=Scenario.REST, seed=4, amplitude=0.7, mains_mv=0.4)
        mv = g.generate(FS * 30)
        flags = np.zeros(len(mv), dtype=np.uint8)
        flags[FS * 24 : FS * 27] = FLAG_LO_PLUS
        mv[FS * 24 : FS * 27] = 1.4
        blocks_file(tmp / "b.bin", mv, flags)
        shots = ["1500", "9000", "22000", "25500", "29900"]
        # Bedienung: Menü, Messung (2:10), Auswertung mit allen Seiten, Einrichtung, Info
        g = SyntheticEcg(FS, scenario=Scenario.REST, seed=6, amplitude=0.8, mains_mv=0.03)
        mv = g.generate(FS * 150)
        blocks_file(tmp / "a.bin", mv, np.zeros(len(mv), dtype=np.uint8))
        script = [
            "2500 shot live", "3000 tap 160 120", "3300 shot menu", "3500 tap 160 69",
            "45000 shot messung", "134200 shot analyse",
            "135500 shot seite1", "135600 tap 292 222", "135700 shot seite2",
            "135800 tap 292 222", "135900 shot seite3", "136000 tap 292 222",
            "136100 shot seite4", "136200 tap 292 222", "136300 shot seite5",
            "136400 tap 100 222", "137000 tap 160 120", "137200 tap 230 161",
            "137400 shot einrichten", "137500 long", "138000 tap 160 120",
            "138200 tap 90 204", "138400 shot info",
        ]  # fmt: skip
        (tmp / "script.txt").write_text("\n".join(script) + "\n")
        for theme, opt in (("nacht", []), ("tag", ["tag"])):
            subprocess.run(
                [exe, *opt, "app", str(tmp / "a.bin"), str(tmp / "app"), str(tmp / "script.txt")],
                check=True,
            )
            for name in [line.split()[2] for line in script if " shot " in line]:
                to_png(tmp / f"app_{name}.ppm", out / f"{theme}_app_{name}.png")
        for theme, opt in (("nacht", []), ("tag", ["tag"])):
            for t in (300, 1100, 1900, 3200):
                subprocess.run([exe, *opt, "splash", str(t), str(tmp / "s.ppm")], check=True)
                to_png(tmp / "s.ppm", out / f"{theme}_splash_{t:04d}.png")
            cmd = [exe, *opt, "monitor", str(tmp / "b.bin"), str(tmp / "m"), *shots]
            subprocess.run(cmd, check=True)
            for s in shots:
                to_png(tmp / f"m_{int(s):05d}.ppm", out / f"{theme}_monitor_{int(s):05d}.png")
    print(out)


if __name__ == "__main__":
    main()
