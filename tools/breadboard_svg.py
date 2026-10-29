#!/usr/bin/env python3
"""Erzeugt die Steckpläne in docs/ aus Verbindungslisten.

    python3 tools/breadboard_svg.py

- docs/breadboard.svg: AD8232 + Pico 2
- docs/breadboard_display.svg: zusätzlich das 2,8"-Display mit Touch (Phase 2, Vorschlag)

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
AD_USED = {"3V3(OUT)", "AGND", "GP26/ADC0", "GP22", "GP21", "GP20"}
# (Bauteil, von, nach, Beschriftung)
PARTS = []


def _board(
    out: list[str], used: set[str], left_labels: dict[int, str], pin1_label: bool = True
) -> None:
    """Breadboard, Pico 2 und AD8232 samt seinen Drähten."""
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
        is_used = name in used
        _, y = xy("h", r)
        style = (
            "font-size:11px;font-weight:bold;fill:#111" if is_used else "font-size:10px;fill:#999"
        )
        out.append(
            f'<line x1="{jx + 8}" y1="{y}" x2="{LABEL_X - 6}" y2="{y}" stroke="#ddd" stroke-dasharray="2,3"/>'
        )
        out.append(
            f'<text x="{LABEL_X}" y="{y + 4}" style="{style}">Reihe {r}: Pin {pin} {name}</text>'
        )
    for r, text in left_labels.items():
        x, y = xy("a", r)
        out.append(
            f'<text x="{x - P * 2.6}" y="{y + 4}" text-anchor="end" class="small">{text}</text>'
        )
    if pin1_label:
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
    _board(out, AD_USED, PICO_LEFT_LABELS)

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
    if PARTS:
        out.append(
            f'<text x="{lx}" y="{y}" style="font-size:13px;font-weight:bold">Bauteile</text>'
        )
    for i, (_k, a, b, label) in enumerate(PARTS):
        out.append(
            f'<text x="{lx}" y="{y + 18 + i * 18}" class="lbl">{label}: {a[0]}{a[1]} – {b[0]}{b[1]}</text>'
        )
    y += 18 + len(PARTS) * 18 + 10
    out.append(
        f'<text x="{lx}" y="{y}" class="small">Nur Drähte – keine Widerstände, keine Kondensatoren.</text>'
    )
    out.append(
        f'<text x="{lx}" y="{y + 16}" class="small">Stromschienen werden nicht benutzt.</text>'
    )
    out.append("</svg>")
    return "\n".join(out)


# ------------------------------------------------------------- Display (Phase 2, Vorschlag)
# 2,8"-TFT „2.8 TFT SPI 240X320 V1.2“: ILI9341, Touch-Controller XPT2046 (U2), SD-Steckplatz,
# 3,3-V-Regler (U1), Transistor für die Hintergrundbeleuchtung (Q1). Das Display steckt nicht
# im Breadboard, sondern hängt an Buchse–Stecker-Kabeln.
# Stiftleiste von oben nach unten wie aufgedruckt; Bildschirm nach oben liegt sie rechts.
DISPLAY_HEADER = (
    "T_IRQ", "T_DO", "T_DIN", "T_CS", "T_CLK", "SDO(MISO)", "LED", "SCK", "SDI(MOSI)", "DC",
    "RESET", "CS", "GND", "VCC",
)  # fmt: skip
# Display-Pin: (Loch, Farbe, Pico-Pin und Funktion). Touch teilt sich SPI0 mit dem Display.
DISPLAY_WIRES = {
    "VCC": (("j", 2), "#e53935", "Pin 39 VSYS (≈ 5 V)"),
    "GND": (("a", 13), "#212121", "Pin 13 GND"),
    "CS": (("a", 11), "#fb8c00", "GP8"),
    "RESET": (("a", 10), "#795548", "GP7"),
    "DC": (("a", 9), "#8e24aa", "GP6"),
    "SDI(MOSI)": (("a", 5), "#1e88e5", "GP3 · SPI0 TX"),
    "SCK": (("a", 4), "#f9a825", "GP2 · SPI0 SCK"),
    "LED": (("a", 7), "#9e9e9e", "GP5 · PWM"),
    "SDO(MISO)": (("a", 6), "#43a047", "GP4 · SPI0 RX"),
    "T_CLK": (("b", 4), "#f9a825", "GP2 (wie SCK)"),
    "T_CS": (("a", 2), "#00acc1", "GP1"),
    "T_DIN": (("b", 5), "#1e88e5", "GP3 (wie SDI)"),
    "T_DO": (("b", 6), "#43a047", "GP4 (wie SDO)"),
    "T_IRQ": (("a", 1), "#d81b60", "GP0"),
}
# SD-Steckplatz (später, eigener Bus SPI1): eigene 4er-Leiste, Stifte müssen eingelötet werden.
SD_HEADER = ("SD_SCK", "SD_MISO", "SD_MOSI", "SD_CS")
SD_HOLES = {
    "SD_SCK": (("a", 14), "GP10 · SPI1 SCK"),
    "SD_MOSI": (("a", 15), "GP11 · SPI1 TX"),
    "SD_MISO": (("a", 16), "GP12 · SPI1 RX"),
    "SD_CS": (("a", 17), "GP13"),
}
PICO_LEFT = {
    1: "GP0", 2: "GP1", 3: "GND", 4: "GP2", 5: "GP3", 6: "GP4", 7: "GP5", 8: "GND", 9: "GP6",
    10: "GP7", 11: "GP8", 12: "GP9", 13: "GND", 14: "GP10", 15: "GP11", 16: "GP12", 17: "GP13",
    18: "GND", 19: "GP14", 20: "GP15",
}  # fmt: skip
DX, DY = 680.0, 40.0  # Platz links für das Display, oben für das VCC-Kabel
PIN_STEP = 15.0


def svg_display() -> str:
    w = DX + LABEL_X + 250
    h = DY + OY + ROWS * P + 70
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.0f} {h:.0f}" '
        f'width="{w:.0f}" height="{h:.0f}" font-family="DejaVu Sans, Arial, sans-serif">',
        "<style>.lbl{font-size:12px;fill:#222}.small{font-size:10px;fill:#555}"
        ".pin{font-size:10px;fill:#fff}.title{font-size:18px;font-weight:bold;fill:#111}</style>",
        f'<rect width="{w:.0f}" height="{h:.0f}" fill="#ffffff"/>',
        '<text x="20" y="32" class="title">Steckplan mit Display (Vorschlag, Phase 2): '
        "2,8″-TFT ILI9341 + Touch an Pico 2</text>",
        '<text x="20" y="54" class="small">AD8232-Verdrahtung unverändert (rechte Seite). Display, Touch '
        "und SD liegen links, weit weg vom ADC-Eingang. Nur Drähte, keine Bauteile.</text>",
        f'<g transform="translate({DX},{DY})">',
    ]
    _board(out, AD_USED | {"VSYS"}, {}, pin1_label=False)
    display_pins = {h[0][1] for h in DISPLAY_WIRES.values() if h[0][0] in "ab"}
    sd_pins = {v[0][1] for v in SD_HOLES.values()}
    for r, name in PICO_LEFT.items():
        x, y = xy("c", r)
        if r in display_pins:
            style = "font-size:10px;font-weight:bold;fill:#fff"
        elif r in sd_pins:
            name += " (SD)"
            style = "font-size:9px;fill:#ffe0b2"
        else:
            style = "font-size:9px;fill:#ffccbc"
        out.append(f'<text x="{x + 9}" y="{y + 3.5}" style="{style}">{name}</text>')
    out.append("</g>")

    def hole(col: str, row: int) -> tuple[float, float]:
        x, y = xy(col, row)
        return x + DX, y + DY

    # Display, Bildschirm nach oben (nicht maßstäblich)
    mw, mh = 465.0, 270.0
    pin_x = 30.0 + mw - 16
    y_first = DY + OY + 8 * P - PIN_STEP * (len(DISPLAY_HEADER) - 1) / 2
    mx0, my0 = 30.0, y_first - (mh - PIN_STEP * (len(DISPLAY_HEADER) - 1)) / 2
    out.append(
        f'<rect x="{mx0}" y="{my0}" width="{mw}" height="{mh}" rx="6" fill="#e53935" '
        f'fill-opacity="0.85" stroke="#8e1b1b"/>'
    )
    out.append(
        f'<rect x="{mx0 + 110}" y="{my0 + 22}" width="{mw - 210}" height="{mh - 44}" rx="3" fill="#263238"/>'
    )
    out.append(
        f'<text x="{mx0 + 110 + (mw - 210) / 2}" y="{my0 + mh / 2 - 8}" text-anchor="middle" '
        f'style="font-size:15px;font-weight:bold;fill:#fff">2,8″ TFT 240×320</text>'
    )
    out.append(
        f'<text x="{mx0 + 110 + (mw - 210) / 2}" y="{my0 + mh / 2 + 12}" text-anchor="middle" '
        f'class="pin">Bildschirm nach oben · ILI9341 · nicht maßstäblich</text>'
    )
    out.append(
        f'<text x="{mx0 + 110 + (mw - 210) / 2}" y="{my0 + mh / 2 + 28}" text-anchor="middle" '
        f'class="pin">Lötbrücke J1 auf der Rückseite offen lassen</text>'
    )
    pin_y = {name: y_first + i * PIN_STEP for i, name in enumerate(DISPLAY_HEADER)}
    for name, y in pin_y.items():
        out.append(
            f'<rect x="{pin_x - 5}" y="{y - 5}" width="10" height="10" fill="#ffd54f" stroke="#7a5c00"/>'
        )
        out.append(
            f'<text x="{pin_x - 9}" y="{y + 3.5}" text-anchor="end" class="pin">{name}</text>'
        )
    sd_x = mx0 + 16
    sd_y0 = my0 + mh / 2 - 1.5 * PIN_STEP
    for i, name in enumerate(SD_HEADER):
        y = sd_y0 + i * PIN_STEP
        out.append(
            f'<circle cx="{sd_x}" cy="{y}" r="4" fill="none" stroke="#ffd54f" stroke-width="1.5"/>'
        )
        out.append(f'<text x="{sd_x + 9}" y="{y + 3.5}" class="pin">{name}</text>')
    out.append(f'<text x="{sd_x - 6}" y="{sd_y0 - 14}" class="pin">SD, später</text>')

    # Kabel: erst alle Linien, dann die Endpunkte darüber
    ends = []
    for i, (name, ((col, row), color, _pico)) in enumerate(DISPLAY_WIRES.items()):
        x1, y1 = pin_x + 5, pin_y[name]
        x2, y2 = hole(col, row)
        dash = ' stroke-dasharray="7,4"' if name.startswith("T_") else ""
        if name == "VCC":  # über den USB-Stecker hinweg zur rechten Seite
            top = DY + OY - 2.4 * P
            xl = hole("a", 1)[0] - 3.4 * P
            xr = x2 + P
            d = f"M{x1},{y1} L{xl},{y1} L{xl},{top} L{xr},{top} L{xr},{y2} L{x2},{y2}"
        else:
            k = 0.42 * (x2 - x1) + 6 * (i % 4)
            d = f"M{x1},{y1} C{x1 + k},{y1} {x2 - k},{y2} {x2},{y2}"
        out.append(
            f'<path d="{d}" fill="none" stroke="{color}" stroke-width="3.2" stroke-linecap="round" '
            f'stroke-linejoin="round" opacity="0.92"{dash}/>'
        )
        ends += [(x1, y1, color), (x2, y2, color)]
    for x, y, color in ends:
        out.append(
            f'<circle cx="{x}" cy="{y}" r="4" fill="{color}" stroke="#000" stroke-width="0.8"/>'
        )
    for (col, row), _pico in SD_HOLES.values():
        x, y = hole(col, row)
        out.append(
            f'<circle cx="{x}" cy="{y}" r="5" fill="none" stroke="#fb8c00" stroke-width="1.5" '
            'stroke-dasharray="2,2"/>'
        )

    # Legende unter dem Display
    lx, ly = 30.0, my0 + mh + 46
    out.append(
        f'<text x="{lx}" y="{ly}" style="font-size:13px;font-weight:bold">'
        "Kabel Display → Breadboard (Buchse–Stecker, 10–20 cm)</text>"
    )
    y = ly + 8
    for num, name in enumerate(reversed(DISPLAY_HEADER), start=1):
        (col, row), color, pico = DISPLAY_WIRES[name]
        y += 18
        dash = ' stroke-dasharray="7,4"' if name.startswith("T_") else ""
        out.append(
            f'<line x1="{lx}" y1="{y - 4}" x2="{lx + 26}" y2="{y - 4}" stroke="{color}" stroke-width="4"{dash}/>'
        )
        out.append(
            f'<text x="{lx + 34}" y="{y}" class="lbl">{num:>2}  {name} → {col}{row}   {pico}</text>'
        )
    y += 28
    out.append(
        f'<text x="{lx}" y="{y}" style="font-size:13px;font-weight:bold">'
        "Später: SD-Karte auf eigenem Bus (SPI1)</text>"
    )
    for name in SD_HEADER:
        (col, row), pico = SD_HOLES[name]
        y += 18
        out.append(f'<text x="{lx + 34}" y="{y}" class="lbl">{name} → {col}{row}   {pico}</text>')
    y += 28
    for note in (
        "Gestrichelt: Touch (teilt sich SPI0 mit dem Display, eigener CS).",
        "VCC an VSYS, nicht an 3V3: die 3,3-V-Schiene versorgt AD8232 und ADC-Referenz.",
        "Display-GND an Pin 13 (links), nicht an AGND (Pin 33).",
    ):
        out.append(f'<text x="{lx}" y="{y}" class="small">{note}</text>')
        y += 15
    out.append("</svg>")
    return "\n".join(out)


if __name__ == "__main__":
    docs = Path(__file__).resolve().parents[1] / "docs"
    for name, make in (("breadboard.svg", svg), ("breadboard_display.svg", svg_display)):
        (docs / name).write_text(make(), encoding="utf-8")
        print(docs / name)
