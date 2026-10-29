#!/usr/bin/env python3
"""Erzeugt firmware/src/ui/assets.c/.h: Schriften, Schriftzug und Herz fürs Display.

    cd host && uv run python ../tools/gen_display_assets.py

Schrift: Inter (SIL Open Font License 1.1, siehe firmware/src/ui/OFL.txt), gerendert mit
Pillow. Schriften als 4-Bit-Alpha (Kantenglättung), Schriftzug und Herz als 8-Bit-Alpha.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

INTER = Path("/usr/share/fonts/opentype/inter")
OUT = Path(__file__).resolve().parents[1] / "firmware" / "src" / "ui"

TEXT_CHARS = "".join(chr(c) for c in range(32, 127)) + "äöüÄÖÜß–·°µ…×→²Ø‹›±≈„“"
FONTS = [
    ("FONT_SMALL", "Inter-Medium.otf", 13, TEXT_CHARS),
    ("FONT_LABEL", "Inter-SemiBold.otf", 16, TEXT_CHARS),
    ("FONT_VALUE", "InterDisplay-SemiBold.otf", 24, TEXT_CHARS),
    ("FONT_HR", "InterDisplay-Bold.otf", 52, "0123456789–- "),
]
LOGO = ("InterDisplay-Bold.otf", 54, "Sophi", "EKG")
HEARTS = [14, 26, 30, 58, 66]  # Pixelhöhe; je Paar Ruhe- und Schlag-Größe


def glyph_bitmap(font: ImageFont.FreeTypeFont, ch: str):
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    adv = round(font.getlength(ch))
    w, h = max(0, x1 - x0), max(0, y1 - y0)
    if w == 0 or h == 0:
        return adv, 0, 0, 0, 0, np.zeros(0, dtype=np.uint8)
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
    return adv, w, h, x0, y0, np.asarray(img, dtype=np.uint8).ravel()


def pack4(a: np.ndarray) -> list[int]:
    q = np.clip(np.round(a.astype(np.float64) / 17.0), 0, 15).astype(np.uint8)
    if len(q) % 2:
        q = np.append(q, 0)
    return [int(hi) << 4 | int(lo) for hi, lo in zip(q[0::2], q[1::2], strict=True)]


def heart(size: int) -> np.ndarray:
    """Herzkurve x = 16 sin³t, y = 13 cos t − 5 cos 2t − 2 cos 3t − cos 4t, geglättet."""
    ss = 8
    t = np.linspace(0, 2 * math.pi, 720, endpoint=False)
    x = 16 * np.sin(t) ** 3
    y = -(13 * np.cos(t) - 5 * np.cos(2 * t) - 2 * np.cos(3 * t) - np.cos(4 * t))
    scale = (size * ss - 2 * ss) / (y.max() - y.min())
    w = math.ceil((x.max() - x.min()) * scale / ss) + 2
    big = Image.new("L", (w * ss, size * ss), 0)
    pts = [
        ((xi - x.min()) * scale + ss, (yi - y.min()) * scale + ss)
        for xi, yi in zip(x, y, strict=True)
    ]
    ImageDraw.Draw(big).polygon(pts, fill=255)
    return np.asarray(big.resize((w, size), Image.Resampling.LANCZOS), dtype=np.uint8)


def c_bytes(name: str, data: list[int]) -> list[str]:
    rows = [", ".join(f"0x{b:02x}" for b in data[i : i + 20]) for i in range(0, len(data), 20)]
    return [f"static const uint8_t {name}[{len(data)}] = {{", *[f"    {r}," for r in rows], "};"]


def render() -> tuple[str, str]:
    h_lines = [
        "// Erzeugt von tools/gen_display_assets.py – nicht von Hand ändern.",
        "// Schrift: Inter, SIL Open Font License 1.1 (siehe OFL.txt).",
        "#pragma once",
        "",
        '#include "gfx.h"',
        "",
    ]
    c_lines = [
        "// Erzeugt von tools/gen_display_assets.py – nicht von Hand ändern.",
        "// Schrift: Inter (c) The Inter Project Authors, SIL Open Font License 1.1 (OFL.txt).",
        '#include "assets.h"',
        "",
    ]
    for name, file, size, chars in FONTS:
        font = ImageFont.truetype(str(INTER / file), size)
        ascent, descent = font.getmetrics()
        glyphs, bits = [], []
        for ch in sorted(set(chars)):
            adv, w, h, x0, y0, a = glyph_bitmap(font, ch)
            glyphs.append((ord(ch), w, h, x0, y0, adv, len(bits)))
            bits += pack4(a)
        c_lines += c_bytes(f"{name.lower()}_bits", bits)
        c_lines.append(f"static const glyph_t {name.lower()}_glyphs[{len(glyphs)}] = {{")
        c_lines += [
            f"    {{{cp}, {w}, {h}, {x0}, {y0}, {adv}, {off}}},"
            for cp, w, h, x0, y0, adv, off in glyphs
        ]
        c_lines.append("};")
        c_lines.append(
            f"const font_t {name} = {{{name.lower()}_glyphs, {len(glyphs)}, {name.lower()}_bits, "
            f"{ascent}, {descent}}};"
        )
        c_lines.append("")
        h_lines.append(f"extern const font_t {name};  // Inter {size} px")

    file, size, left, right = LOGO
    font = ImageFont.truetype(str(INTER / file), size)
    word = left + right
    x0, y0, x1, y1 = font.getbbox(word, anchor="ls")
    img = Image.new("L", (x1 - x0, y1 - y0), 0)
    ImageDraw.Draw(img).text((-x0, -y0), word, font=font, fill=255, anchor="ls")
    a = np.asarray(img, dtype=np.uint8)
    # Trennspalte zwischen „Sophi“ und „EKG“: leere Spalte nahe der Breite von „Sophi“
    guess = round(font.getlength(left)) - x0
    empty = [c for c in range(a.shape[1]) if a[:, c].max() == 0]
    split = min(empty, key=lambda c: abs(c - guess)) if empty else guess
    c_lines += c_bytes("logo_alpha", a.ravel().tolist())
    c_lines.append(f"const alpha_img_t IMG_LOGO = {{{a.shape[1]}, {a.shape[0]}, logo_alpha}};")
    c_lines.append(f"const int LOGO_SPLIT_X = {split};")
    c_lines.append(f"const int LOGO_BASELINE = {-y0};")
    c_lines.append("")
    h_lines += [
        "",
        f"extern const alpha_img_t IMG_LOGO;  // „{word}“, Inter Display {size} px",
        "extern const int LOGO_SPLIT_X;  // erste Spalte von „EKG“",
        "extern const int LOGO_BASELINE;  // Grundlinie ab Oberkante",
        "",
    ]
    for s in HEARTS:
        a = heart(s)
        c_lines += c_bytes(f"heart_{s}_alpha", a.ravel().tolist())
        c_lines.append(
            f"const alpha_img_t IMG_HEART_{s} = {{{a.shape[1]}, {a.shape[0]}, heart_{s}_alpha}};"
        )
        h_lines.append(f"extern const alpha_img_t IMG_HEART_{s};")
    c_lines.append("")
    h_lines.append("")
    return "\n".join(h_lines), "\n".join(c_lines)


def ofl_text() -> str:
    src = Path("/usr/share/doc/fonts-inter/copyright").read_text(encoding="utf-8").splitlines()
    start = src.index("License: OFL-1.1") + 1
    body = []
    for line in src[start:]:
        if line.startswith("License:"):
            break
        body.append("" if line.strip() == "." else line[1:])
    head = [
        "Die Schriften in assets.c sind aus Inter gerendert.",
        "Copyright 2016-2023 The Inter Project Authors (https://github.com/rsms/inter)",
        "",
        "SIL OPEN FONT LICENSE Version 1.1",
        "",
    ]
    return "\n".join(head + body).rstrip() + "\n"


if __name__ == "__main__":
    header, source = render()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "assets.h").write_text(header, encoding="utf-8")
    (OUT / "assets.c").write_text(source, encoding="utf-8")
    (OUT / "OFL.txt").write_text(ofl_text(), encoding="utf-8")
    print(OUT / "assets.c")
