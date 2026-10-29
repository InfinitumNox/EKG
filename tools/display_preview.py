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
    srcs = [*sorted((SRC / "ui").glob("*.c")), *sorted((SRC / "ecg").glob("*.c"))]
    subprocess.run(
        [cc, "-O2", "-std=c11", "-Wall", "-Wextra", f"-I{SRC}", f"-I{SRC / 'ui'}",
         f"-I{SRC / 'ecg'}", str(ROOT / "tools" / "display_preview.c"), *map(str, srcs),
         "-o", str(exe), "-lm"],
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
        for t in (300, 1100, 1900, 3200):
            subprocess.run([exe, "splash", str(t), str(tmp / "s.ppm")], check=True)
            to_png(tmp / "s.ppm", out / f"splash_{t:04d}.png")
        # Live: kleine R-Zacke wie in den Aufnahmen vom 29.10., dann Elektrode ab
        g = SyntheticEcg(FS, scenario=Scenario.REST, seed=4, amplitude=0.7, mains_mv=0.4)
        mv = g.generate(FS * 30)
        flags = np.zeros(len(mv), dtype=np.uint8)
        flags[FS * 24 : FS * 27] = FLAG_LO_PLUS
        mv[FS * 24 : FS * 27] = 1.4
        blocks_file(tmp / "b.bin", mv, flags)
        shots = ["1500", "9000", "22000", "25500", "29900"]
        subprocess.run([exe, "monitor", str(tmp / "b.bin"), str(tmp / "m"), *shots], check=True)
        for s in shots:
            to_png(tmp / f"m_{int(s):05d}.ppm", out / f"monitor_{int(s):05d}.png")
    print(out)


if __name__ == "__main__":
    main()
