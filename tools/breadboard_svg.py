#!/usr/bin/env python3
"""Erzeugt docs/breadboard.svg (Steckplan) aus einer Verbindungsliste.

    python3 tools/breadboard_svg.py

Koordinaten wie auf dem Breadboard: Reihen 1..35 (oben nach unten), Spalten a..j.
Zwischen e und f liegt die Mittelrinne (3 Raster breit, wie beim echten Board).
"""

from __future__ import annotations

from pathlib import Path

P = 22.0  # Rastermaß in px (entspricht 2,54 mm)
OX, OY = 300.0, 110.0
LABEL_X = OX + 11 * P + 210  # Pin-Namen rechts neben den Drahtbögen
LEGEND_X = LABEL_X + 270
COLS = {c: i for c, i in zip("abcde", range(5), strict=True)} | {
    c: i for c, i in zip("fghij", range(7, 12), strict=True)
}
ROWS = 35


def xy(col: str, row: int) -> tuple[float, float]:
    return OX + COLS[col] * P, OY + (row - 1) * P


# Pico 2, Draufsicht, USB oben: linke Reihe Spalte c = Pin 1..20, rechte Reihe Spalte h = Pin 40..21
PICO_RIGHT = {
    40: "VBUS", 39: "VSYS", 38: "GND", 37: "3V3_EN", 36: "3V3(OUT)", 35: "ADC_VREF", 34: "GP28",
    33: "AGND", 32: "GP27", 31: "GP26/ADC0", 30: "RUN", 29: "GP22", 28: "GND", 27: "GP21",
    26: "GP20", 25: "GP19", 24: "GP18", 23: "GND", 22: "GP17", 21: "GP16",
}  # fmt: skip
PICO_LEFT_LABELS = {20: "GP15 (Kalibrier-PWM, später)"}
AD_HEADER = [("SDN", 26), ("LO+", 27), ("LO−", 28), ("OUTPUT", 29), ("3.3V", 30), ("GND", 31)]

# (von, nach, Farbe, Beschriftung)
WIRES = [
    (("g", 26), ("i", 12), "#1e88e5", "SDN → GP22"),
    (("g", 27), ("i", 15), "#43a047", "LO+ → GP20"),
    (("g", 28), ("i", 14), "#8e24aa", "LO− → GP21"),
    (("g", 30), ("i", 5), "#e53935", "3.3V → 3V3(OUT)"),
    (("g", 31), ("i", 8), "#212121", "GND → AGND"),
    (("g", 29), ("i", 10), "#fbc02d", "OUTPUT → GP26/ADC0"),
]
# (Bauteil, von, nach, Beschriftung)
PARTS = [
    ("C", ("h", 30), ("h", 31), "C1 100 nF"),
    ("E", ("j", 30), ("j", 31), "C2 10 µF (+ oben)"),
]


def svg() -> str:
    w = LEGEND_X + 360
    h = OY + ROWS * P + 70
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.0f} {h:.0f}" '
        f'width="{w:.0f}" height="{h:.0f}" font-family="DejaVu Sans, Arial, sans-serif">',
        "<style>.lbl{font-size:12px;fill:#222}.small{font-size:10px;fill:#555}"
        ".pin{font-size:10px;fill:#fff}.title{font-size:18px;font-weight:bold;fill:#111}</style>",
        f'<rect width="{w:.0f}" height="{h:.0f}" fill="#ffffff"/>',
        '<text x="20" y="32" class="title">Steckplan: AD8232 + Pico 2 (Draufsicht, USB oben)</text>',
        '<text x="20" y="54" class="small">Reihen/Spalten wie auf dem Breadboard. Stromschienen werden nicht benutzt '
        "(dein Board hat geteilte Schienen).</text>",
    ]
    # Breadboard
    bx0, by0 = OX - P, OY - P
    out.append(
        f'<rect x="{bx0}" y="{by0}" width="{13 * P}" height="{(ROWS + 1) * P}" rx="8" '
        f'fill="#f4f1ea" stroke="#c9c2b0"/>'
    )
    gx = OX + 4.5 * P + P
    out.append(
        f'<rect x="{gx - P}" y="{by0}" width="{P * 1.0}" height="{(ROWS + 1) * P}" fill="#e6e0d2"/>'
    )
    for c in COLS:
        x, _ = xy(c, 1)
        out.append(
            f'<text x="{x}" y="{OY - P * 0.55}" text-anchor="middle" class="small">{c}</text>'
        )
    for r in range(1, ROWS + 1):
        _, y = xy("a", r)
        if r == 1 or r % 5 == 0:
            out.append(
                f'<text x="{OX - P * 1.6}" y="{y + 4}" text-anchor="end" class="small">{r}</text>'
            )
        for c in COLS:
            x, _ = xy(c, r)
            out.append(f'<rect x="{x - 3}" y="{y - 3}" width="6" height="6" fill="#8d8676"/>')

    # Pico
    px0, py0 = xy("c", 1)
    px1, py1 = xy("h", 20)
    out.append(
        f'<rect x="{px0 - 14}" y="{py0 - 22}" width="{px1 - px0 + 28}" height="{py1 - py0 + 36}" rx="6" '
        f'fill="#d84315" fill-opacity="0.88" stroke="#8c2a0b"/>'
    )
    out.append(
        f'<rect x="{(px0 + px1) / 2 - 18}" y="{py0 - 40}" width="36" height="26" rx="4" fill="#9e9e9e" stroke="#555"/>'
    )
    out.append(
        f'<text x="{(px0 + px1) / 2 + 26}" y="{py0 - 24}" class="small">USB-C (zu Reihe 1)</text>'
    )
    out.append(
        f'<text x="{(px0 + px1) / 2}" y="{(py0 + py1) / 2}" text-anchor="middle" '
        f'style="font-size:20px;font-weight:bold;fill:#fff">Pico 2</text>'
    )
    for r in range(1, 21):
        for c in ("c", "h"):
            x, y = xy(c, r)
            out.append(f'<circle cx="{x}" cy="{y}" r="5" fill="#ffd54f" stroke="#7a5c00"/>')
    jx, _ = xy("j", 1)
    for r in range(1, 21):
        pin = 41 - r
        name = PICO_RIGHT[pin]
        used = name in ("3V3(OUT)", "AGND", "GP26/ADC0", "GP22", "GP21", "GP20")
        _, y = xy("h", r)
        style = "font-size:11px;font-weight:bold;fill:#111" if used else "font-size:10px;fill:#999"
        out.append(
            f'<line x1="{jx + 8}" y1="{y}" x2="{LABEL_X - 6}" y2="{y}" stroke="#ddd" stroke-dasharray="2,3"/>'
        )
        out.append(
            f'<text x="{LABEL_X}" y="{y + 4}" style="{style}">Reihe {r}: Pin {pin} {name}</text>'
        )
    for r, text in PICO_LEFT_LABELS.items():
        x, y = xy("a", r)
        out.append(
            f'<text x="{x - P * 2.6}" y="{y + 4}" text-anchor="end" class="small">{text}</text>'
        )
    x, y = xy("c", 1)
    out.append(f'<text x="{x - 8}" y="{y + 4}" text-anchor="end" class="small">Pin 1</text>')

    # AD8232: Stiftleiste in Spalte f, Platine liegt links darüber, Klinkenbuchse nach links
    hx, hy0 = xy("f", AD_HEADER[0][1])
    _, hy1 = xy("f", AD_HEADER[-1][1])
    ax0 = xy("a", 1)[0] - P * 1.7
    out.append(
        f'<rect x="{ax0}" y="{hy0 - P * 4}" width="{hx - ax0 + P * 0.7}" height="{hy1 - hy0 + P * 8.5}" rx="6" '
        f'fill="#e53935" fill-opacity="0.80" stroke="#8e1b1b"/>'
    )
    out.append(
        f'<rect x="{ax0 - P * 1.6}" y="{hy0 + P * 2.4}" width="{P * 2.4}" height="{P * 2.2}" rx="3" fill="#333"/>'
    )
    out.append(
        f'<text x="{ax0 - P * 0.4}" y="{hy0 + P * 5.6}" text-anchor="middle" class="small">Klinke</text>'
    )
    out.append(
        f'<text x="{(ax0 + hx) / 2}" y="{hy0 - P * 2.2}" text-anchor="middle" '
        f'style="font-size:16px;font-weight:bold;fill:#fff">AD8232</text>'
    )
    out.append(
        f'<text x="{(ax0 + hx) / 2}" y="{hy0 - P * 1.2}" text-anchor="middle" class="pin">liegt über a–e</text>'
    )
    for name, r in AD_HEADER:
        x, y = xy("f", r)
        out.append(
            f'<rect x="{x - 6}" y="{y - 6}" width="12" height="12" fill="#ffd54f" stroke="#7a5c00"/>'
        )
        out.append(f'<text x="{x - 12}" y="{y + 4}" text-anchor="end" class="pin">{name}</text>')

    # Bauteile
    for kind, a, b, _label in PARTS:
        (x1, y1), (x2, y2) = xy(*a), xy(*b)
        out.append(
            f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="#777" stroke-width="2"/>'
        )
        mx, my = (x1 + x2) / 2, (y1 + y2) / 2
        if kind == "R":
            out.append(
                f'<rect x="{mx - 6}" y="{my - 18}" width="12" height="36" rx="5" fill="#d7b98e" stroke="#6d4c2a"/>'
            )
        elif kind == "C":
            out.append(
                f'<ellipse cx="{mx}" cy="{my}" rx="8" ry="10" fill="#f9a825" stroke="#8a5a00"/>'
            )
        else:
            out.append(f'<circle cx="{mx}" cy="{my}" r="11" fill="#1a237e" stroke="#000"/>')
            out.append(f'<text x="{mx}" y="{my - 2}" text-anchor="middle" class="pin">+</text>')
        for x, y in ((x1, y1), (x2, y2)):
            out.append(f'<circle cx="{x}" cy="{y}" r="3.5" fill="#555"/>')

    # Drähte (als Bogen, damit sie sich unterscheiden lassen)
    for i, (a, b, color, _label) in enumerate(WIRES):
        (x1, y1), (x2, y2) = xy(*a), xy(*b)
        bend = 60 + 22 * i
        cx = max(x1, x2) + bend
        out.append(
            f'<path d="M{x1},{y1} C{cx},{y1} {cx},{y2} {x2},{y2}" fill="none" stroke="{color}" '
            f'stroke-width="4" stroke-linecap="round" opacity="0.9"/>'
        )
        for x, y in ((x1, y1), (x2, y2)):
            out.append(
                f'<circle cx="{x}" cy="{y}" r="4.5" fill="{color}" stroke="#000" stroke-width="0.8"/>'
            )

    # Legende
    lx = LEGEND_X
    ly = OY + 2 * P
    out.append(
        f'<text x="{lx}" y="{ly - 10}" style="font-size:13px;font-weight:bold">Drähte</text>'
    )
    for i, (a, b, color, label) in enumerate(WIRES):
        y = ly + 8 + i * 18
        out.append(
            f'<line x1="{lx}" y1="{y}" x2="{lx + 26}" y2="{y}" stroke="{color}" stroke-width="4"/>'
        )
        out.append(
            f'<text x="{lx + 34}" y="{y + 4}" class="lbl">{a[0]}{a[1]} → {b[0]}{b[1]}  {label}</text>'
        )
    y = ly + 8 + len(WIRES) * 18 + 14
    out.append(f'<text x="{lx}" y="{y}" style="font-size:13px;font-weight:bold">Bauteile</text>')
    for i, (_k, a, b, label) in enumerate(PARTS):
        out.append(
            f'<text x="{lx}" y="{y + 18 + i * 18}" class="lbl">{label}: {a[0]}{a[1]} – {b[0]}{b[1]}</text>'
        )
    y += 18 + len(PARTS) * 18 + 10
    out.append(
        f'<text x="{lx}" y="{y}" class="small">Keinen Kondensator direkt an OUTPUT/GP26 hängen.</text>'
    )
    out.append(
        f'<text x="{lx}" y="{y + 16}" class="small">Stromschienen werden nicht benutzt.</text>'
    )
    out.append("</svg>")
    return "\n".join(out)


if __name__ == "__main__":
    dst = Path(__file__).resolve().parents[1] / "docs" / "breadboard.svg"
    dst.write_text(svg(), encoding="utf-8")
    print(dst)
