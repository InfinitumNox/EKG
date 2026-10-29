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
        # Bedienung: Menü, Messung (2 min), Auswertung mit allen Seiten, Einrichtung, Info
        g = SyntheticEcg(FS, scenario=Scenario.REST, seed=6, amplitude=0.8, mains_mv=0.03)
        mv = g.generate(FS * 140)
        blocks_file(tmp / "a.bin", mv, np.zeros(len(mv), dtype=np.uint8))
        # Tippstellen je Ausrichtung: Menü öffnen, Messen, Weiter, Kurve, Einstellungen, Zeilen
        spots = {
            "quer": {"menu": "160 120", "measure": "160 69", "next": "292 222", "live": "100 222",
                     "settings": "160 166", "row_touch": "160 178", "row_info": "160 213"},
            "hochkant": {"menu": "120 160", "measure": "120 100", "next": "220 300",
                         "live": "70 300", "settings": "120 200", "row_touch": "120 214",
                         "row_info": "120 258"},
        }  # fmt: skip
        for orient, sp in spots.items():
            script = [
                "2500 shot live", f"3000 tap {sp['menu']}", "3300 shot menu",
                f"3500 tap {sp['measure']}", "45000 shot messung",
                "125500 shot seite1", f"125600 tap {sp['next']}", "125700 shot seite2",
                f"125800 tap {sp['next']}", "125900 shot seite3", f"126000 tap {sp['next']}",
                "126100 shot seite4", f"126200 tap {sp['next']}", "126300 shot seite5",
                f"126400 tap {sp['live']}", f"127000 tap {sp['menu']}",
                f"127200 tap {sp['settings']}", "127300 shot einstellungen",
                f"127400 tap {sp['row_touch']}", "127500 shot einrichten", "127600 long",
                f"128000 tap {sp['menu']}", f"128200 tap {sp['settings']}",
                f"128300 tap {sp['row_info']}", "128400 shot info",
            ]  # fmt: skip
            (tmp / "script.txt").write_text("\n".join(script) + "\n")
            extra = [] if orient == "quer" else ["hochkant"]
            for theme, opt in (("nacht", []), ("tag", ["tag"])):
                cmd = [exe, *opt, *extra, "app", str(tmp / "a.bin"), str(tmp / "app"),
                       str(tmp / "script.txt")]  # fmt: skip
                subprocess.run(cmd, check=True)
                for name in [line.split()[2] for line in script if " shot " in line]:
                    to_png(tmp / f"app_{name}.ppm", out / f"{theme}_{orient}_{name}.png")
            subprocess.run([exe, *extra, "splash", "3200", str(tmp / "s.ppm")], check=True)
            to_png(tmp / "s.ppm", out / f"nacht_{orient}_splash.png")
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
