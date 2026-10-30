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
        # Vorhofflimmern: Check-Liste mit rotem und gelbem Punkt
        g = SyntheticEcg(FS, scenario=Scenario.AF, seed=8, amplitude=0.8, mains_mv=0.03)
        mv = g.generate(FS * 140)
        blocks_file(tmp / "af.bin", mv, np.zeros(len(mv), dtype=np.uint8))
        # Elektroden ab: erst LA, dann beide; die Anleitung öffnet sich nach 4 s von selbst
        g = SyntheticEcg(FS, scenario=Scenario.REST, seed=5, amplitude=0.8, mains_mv=0.03)
        mv = g.generate(FS * 20)
        flags = np.zeros(len(mv), dtype=np.uint8)
        flags[FS * 1 : FS * 10] = 1
        flags[FS * 10 : FS * 15] = 3
        blocks_file(tmp / "lo.bin", mv, flags)
        lo_script = ["6000 shot ab_la", "12000 shot ab_beide", "17000 shot ab_wieder"]
        # Tippstellen je Ausrichtung (Menü, Person, Profil anlegen, Messen, Seiten, Einstellungen)
        spots = {
            "quer": {"menu": "160 120", "person": "160 206", "prof0": "100 58",
                     "age": "100 73", "sex": "100 103", "height": "100 133",
                     "weight": "100 163", "done": "240 218", "close": "310 12",
                     "measure": "160 69", "next": "292 222", "live": "100 222",
                     "check_row": "160 53", "rhythm_row": "160 75", "back": "160 120",
                     "settings": "160 166", "row_avatar": "160 163", "row_touch": "160 193",
                     "row_info": "160 223", "leads": "230 118", "leads_done": "240 222",
                     "leads_colors": "80 222"},
            "hochkant": {"menu": "120 160", "person": "120 252", "prof0": "100 65",
                         "age": "100 88", "sex": "100 128", "height": "100 168",
                         "weight": "100 208", "done": "180 290", "close": "220 12",
                         "measure": "120 100", "next": "220 300", "live": "70 300",
                         "check_row": "120 84", "rhythm_row": "120 107", "back": "120 160",
                         "settings": "120 200", "row_avatar": "120 208", "row_touch": "120 248",
                         "row_info": "120 288", "leads": "171 156", "leads_done": "180 298",
                         "leads_colors": "60 298"},
        }  # fmt: skip
        for orient, sp in spots.items():
            script = [
                "2500 shot live", f"3000 tap {sp['menu']}", "3300 shot menu",
                f"3500 tap {sp['person']}", "3800 shot personen", f"4000 tap {sp['prof0']}",
                "4300 shot tastatur_leer", "4400 type Sophie", "4600 shot tastatur_name",
                "4700 key OK", f"4900 tap {sp['age']}", "5100 shot zahlen", "5200 type 41",
                "5300 key OK", f"5400 tap {sp['sex']}", f"5500 tap {sp['sex']}",
                f"5600 tap {sp['height']}", "5700 type 168", "5800 key OK",
                f"5900 tap {sp['weight']}", "6000 type 62", "6100 key OK", "6300 shot profil",
                f"6400 tap {sp['done']}", "6500 shot personen2", f"6600 tap {sp['close']}",
                f"6700 tap {sp['leads']}", "6750 shot elektroden", f"6760 tap {sp['leads_colors']}",
                "6770 shot elektroden_farben", f"6780 tap {sp['leads_done']}",
                f"6790 tap {sp['menu']}", f"6800 tap {sp['measure']}", "45000 shot messung",
                "127500 shot seite1", f"127600 tap {sp['next']}", "127700 shot seite2",
                f"127800 tap {sp['check_row']}", "127900 shot detail",
                f"128000 tap {sp['back']}", f"128100 tap {sp['next']}", "128200 shot seite3",
                f"128300 tap {sp['next']}", "128400 shot seite4", f"128500 tap {sp['next']}",
                "128600 shot seite5", f"128700 tap {sp['next']}", "128800 shot seite6",
                f"128900 tap {sp['live']}", f"129500 tap {sp['menu']}",
                f"129700 tap {sp['settings']}", "129800 shot einstellungen",
                f"129900 tap {sp['row_touch']}", "130000 shot einrichten", "130100 long",
                f"130500 tap {sp['menu']}", f"130700 tap {sp['settings']}",
                f"130800 tap {sp['row_info']}", "130900 shot info",
            ]  # fmt: skip
            af_script = [
                f"1000 tap {sp['menu']}", f"1100 tap {sp['leads']}", "1200 shot elektroden_gast",
                f"1300 tap {sp['leads_done']}",
                f"3000 tap {sp['menu']}", f"3500 tap {sp['measure']}", "123900 shot af_uebersicht",
                f"124000 tap {sp['next']}", "124100 shot af_check",
                f"124200 tap {sp['rhythm_row']}", "124300 shot af_detail",
            ]  # fmt: skip

            def profile_script(sp, name, age, sex_taps, height, weight, tag):
                return [
                    f"1000 tap {sp['menu']}", f"1100 tap {sp['person']}", f"1200 tap {sp['prof0']}",
                    f"1300 type {name}", "1400 key OK", f"1500 tap {sp['age']}",
                    f"1600 type {age}", "1700 key OK",
                    *[f"{1800 + 20 * i} tap {sp['sex']}" for i in range(sex_taps)],
                    f"2000 tap {sp['height']}", f"2100 type {height}", "2200 key OK",
                    f"2300 tap {sp['weight']}", f"2400 type {weight}", "2500 key OK",
                    f"2600 tap {sp['done']}", f"2700 tap {sp['close']}",
                    f"2800 tap {sp['leads']}", f"2900 shot {tag}_profil",
                    f"3000 tap {sp['leads_done']}", f"3100 tap {sp['menu']}",
                    f"3200 tap {sp['settings']}", f"3300 tap {sp['row_avatar']}",
                    f"3400 tap {sp['close']}", f"3500 tap {sp['menu']}",
                    f"3600 tap {sp['leads']}", f"3700 shot {tag}_standard",
                ]  # fmt: skip

            men_script = profile_script(sp, "Max", 55, 1, 190, 125, "mann_schwer")
            kid_script = profile_script(sp, "Mia", 9, 2, 135, 28, "kind")
            lean_script = profile_script(sp, "Lea", 30, 2, 178, 52, "frau_schlank")
            extra = [] if orient == "quer" else ["hochkant"]
            for theme, opt in (("nacht", []), ("tag", ["tag"])):
                for src, lines in (
                    ("a.bin", script),
                    ("af.bin", af_script),
                    ("af.bin", men_script),
                    ("af.bin", kid_script),
                    ("af.bin", lean_script),
                    ("lo.bin", lo_script),
                ):
                    (tmp / "script.txt").write_text("\n".join(lines) + "\n")
                    cmd = [exe, *opt, *extra, "app", str(tmp / src), str(tmp / "app"),
                           str(tmp / "script.txt")]  # fmt: skip
                    subprocess.run(cmd, check=True)
                    for name in [line.split()[2] for line in lines if " shot " in line]:
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
