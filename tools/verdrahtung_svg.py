#!/usr/bin/env python3
"""Erzeugt docs/verdrahtung.svg: Verdrahtung ohne Breadboard, so wie im Gehäuse eingebaut.

    python3 tools/verdrahtung_svg.py

Der Pico liegt mit der Rückseite nach oben (Beschriftung lesbar), USB-C rechts. Jede Leitung
ist ein Buchse–Buchse-Kabel direkt auf einen Stift; deshalb hat der Touch-Controller einen
eigenen SPI-Bus (ohne Breadboard kann ein Stift nur ein Kabel aufnehmen). Farben wie in der
tatsächlichen Verkabelung.
"""

from __future__ import annotations

from pathlib import Path

# Stiftreihen in Ansicht von der Rückseite, USB-C rechts (links → rechts)
TOP = (
    "GP16", "GP17", "GND", "GP18", "GP19", "GP20", "GP21", "GND", "GP22", "RUN", "GP26_A0",
    "GP27_A1", "AGND", "GP28_A2", "ADC_VREF", "3V3", "3V3_EN", "GND", "VSYS", "VBUS",
)  # fmt: skip  # Pin 21 … 40
BOTTOM = (
    "GP15", "GP14", "GND", "GP13", "GP12", "GP11", "GP10", "GND", "GP9", "GP8", "GP7", "GP6",
    "GND", "GP5", "GP4", "GP3", "GP2", "GND", "GP1", "GP0",
)  # fmt: skip  # Pin 20 … 1

COLORS = {
    "türkis": "#14a39a",
    "blau": "#1e63d6",
    "lila": "#8e24aa",
    "grau": "#9e9e9e",
    "weiß": "#fafafa",
    "schwarz": "#212121",
    "braun": "#8d7a3a",
    "rot": "#e53935",
    "orange": "#fb8c00",
    "gelb": "#fdd835",
    "grün": "#2e9d5b",
}

# (Reihe, Pico-Beschriftung, Farbe, Gegenstelle)
WIRES = [
    ("top", "GP20", "weiß", "LO+ · AD8232"),
    ("top", "GP21", "grau", "LO− · AD8232"),
    ("top", "GP22", "schwarz", "SDN · AD8232"),
    ("top", "GP26_A0", "lila", "OUTPUT · AD8232"),
    ("top", "AGND", "türkis", "GND · AD8232"),
    ("top", "3V3", "blau", "3.3V · AD8232"),
    ("top", "VSYS", "lila", "VCC · Display (5 V!)"),
    ("bottom", "GP2", "orange", "SCK · Display"),
    ("bottom", "GP3", "rot", "SDI(MOSI) · Display"),
    ("bottom", "GP4", "gelb", "SDO(MISO) · Display"),
    ("bottom", "GP5", "grün", "LED · Display"),
    ("bottom", "GP6", "braun", "DC · Display"),
    ("bottom", "GP7", "schwarz", "RESET · Display"),
    ("bottom", "GP8", "weiß", "CS · Display"),
    ("bottom", 7, "grau", "GND · Display"),  # Pin 13 (GND zwischen GP9 und GP10)
    ("bottom", "GP10", "blau", "T_CLK · Touch"),
    ("bottom", "GP11", "grau", "T_DIN · Touch"),
    ("bottom", "GP12", "weiß", "T_DO · Touch"),
    ("bottom", "GP13", "lila", "T_CS · Touch"),
    ("bottom", "GP14", "blau", "T_IRQ · Touch (Farbe?)"),
]
WRONG = ("GP27_A1", "GP26_A0")  # lila OUTPUT steckt auf dem Foto einen Stift daneben

AD8232 = [("GND", "türkis"), ("3.3V", "blau"), ("OUTPUT", "lila"), ("LO−", "grau"),
          ("LO+", "weiß"), ("SDN", "schwarz")]  # fmt: skip
# Display-Stiftleiste wie aufgedruckt; Nr. = Lage im Flachkabel, vom äußeren Blau gezählt
DISPLAY = [
    ("VCC", "lila", 2), ("GND", "grau", 3), ("CS", "weiß", 4), ("RESET", "schwarz", 5),
    ("DC", "braun", 6), ("SDI(MOSI)", "rot", 7), ("SCK", "orange", 8), ("LED", "grün", 10),
    ("SDO(MISO)", "gelb", 9), ("T_CLK", "blau", 11), ("T_CS", "lila", 12), ("T_DIN", "grau", 13),
    ("T_DO", "weiß", 14), ("T_IRQ", "blau", 1),
]  # fmt: skip

PP = 46.0  # Stiftabstand in der Zeichnung
X0 = 342.0
BX0, BX1, BY0, BY1 = 300.0, 1260.0, 420.0, 640.0
Y_TOP, Y_BOT = 440.0, 620.0


def pin_x(k: int) -> float:
    return X0 + k * PP


def _wire(out: list[str], x1: float, y1: float, x2: float, y2: float, color: str) -> None:
    c = COLORS[color]
    # dunkle Unterlage, damit weiße und gelbe Leitungen sichtbar bleiben
    out.append(
        f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="#333" stroke-width="8" '
        'stroke-linecap="round"/>'
    )
    out.append(
        f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{c}" stroke-width="5.5" '
        'stroke-linecap="round"/>'
    )


def svg() -> str:
    w, h = 1560, 1170
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
        'font-family="DejaVu Sans, Arial, sans-serif">',
        "<style>.lbl{font-size:13px;fill:#222}.small{font-size:11px;fill:#555}"
        ".pin{font-size:11px;fill:#fff}.title{font-size:20px;font-weight:bold;fill:#111}"
        ".warn{font-size:13px;font-weight:bold;fill:#c62828}</style>",
        f'<rect width="{w}" height="{h}" fill="#ffffff"/>',
        '<text x="24" y="36" class="title">Verdrahtung ohne Breadboard (so wie eingebaut)</text>',
        '<text x="24" y="60" class="small">Pico 2 von der Rückseite (Beschriftung lesbar), USB-C '
        "rechts. Jede Leitung steckt direkt auf einem Stift. Farben wie bei deinen Kabeln.</text>",
    ]

    # Platine
    out.append(
        f'<rect x="{BX0}" y="{BY0}" width="{BX1 - BX0}" height="{BY1 - BY0}" rx="10" '
        'fill="#e53935" stroke="#8e1b1b" stroke-width="2"/>'
    )
    out.append(
        f'<rect x="{BX1}" y="{(BY0 + BY1) / 2 - 30}" width="44" height="60" rx="6" fill="#9e9e9e" '
        'stroke="#555"/>'
    )
    out.append(f'<text x="{BX1 + 52}" y="{(BY0 + BY1) / 2 + 5}" class="lbl">USB-C</text>')
    for i in range(3):  # SWD-Pads (SWDIO, GND, SWCLK) zur Orientierung
        y = (BY0 + BY1) / 2 - 40 + i * 40
        out.append(f'<circle cx="{BX0 + 24}" cy="{y}" r="7" fill="#c9a227" stroke="#7a5c00"/>')
    out.append(
        f'<text x="{BX0 + 280}" y="{(BY0 + BY1) / 2 + 6}" text-anchor="middle" '
        'style="font-size:15px;font-weight:bold;fill:#fff">RP2350 Pico2 · Rückseite</text>'
    )

    # Stifte und Beschriftung (wie auf der Platine)
    for row, names, y in (("top", TOP, Y_TOP), ("bottom", BOTTOM, Y_BOT)):
        for k, name in enumerate(names):
            x = pin_x(k)
            out.append(f'<circle cx="{x}" cy="{y}" r="7" fill="#ffd54f" stroke="#7a5c00"/>')
            if row == "top":
                out.append(
                    f'<text x="{x + 4}" y="{y + 16}" transform="rotate(90 {x + 4} {y + 16})" '
                    f'class="pin">{name}</text>'
                )
            else:
                out.append(
                    f'<text x="{x + 4}" y="{y - 16}" transform="rotate(-90 {x + 4} {y - 16})" '
                    f'class="pin">{name}</text>'
                )

    # Leitungen: kurzer Stummel in Kabelfarbe, Beschriftung schräg daneben
    for row, pin, color, label in WIRES:
        names = TOP if row == "top" else BOTTOM
        k = pin if isinstance(pin, int) else names.index(pin)
        x = pin_x(k)
        if row == "top":
            y1, y2 = Y_TOP - 9, Y_TOP - 92
            _wire(out, x, y1, x, y2, color)
            out.append(
                f'<text x="{x + 4}" y="{y2 - 8}" transform="rotate(-50 {x + 4} {y2 - 8})" '
                f'class="lbl"><tspan font-weight="bold">{color}</tspan> → {label}</text>'
            )
        else:
            y1, y2 = Y_BOT + 9, Y_BOT + 92
            _wire(out, x, y1, x, y2, color)
            out.append(
                f'<text x="{x + 4}" y="{y2 + 16}" transform="rotate(50 {x + 4} {y2 + 16})" '
                f'class="lbl"><tspan font-weight="bold">{color}</tspan> → {label}</text>'
            )
        out.append(
            f'<circle cx="{x}" cy="{Y_TOP if row == "top" else Y_BOT}" r="11" fill="none" '
            f'stroke="{COLORS[color] if color != "weiß" else "#333"}" stroke-width="3"/>'
        )

    # Korrektur: lila OUTPUT steckt einen Stift zu weit rechts
    xw, xr = pin_x(TOP.index(WRONG[0])), pin_x(TOP.index(WRONG[1]))
    out.append(
        f'<circle cx="{xw}" cy="{Y_TOP}" r="13" fill="none" stroke="#c62828" stroke-width="3" '
        'stroke-dasharray="4,3"/>'
    )
    ya = Y_TOP + 86  # unter den gedrehten Stiftnamen
    out.append(
        f'<path d="M{xw},{ya} Q{(xw + xr) / 2},{ya + 18} {xr + 4},{ya}" fill="none" '
        'stroke="#fff" stroke-width="6"/>'
    )
    out.append(
        f'<path d="M{xw},{ya} Q{(xw + xr) / 2},{ya + 18} {xr + 4},{ya}" fill="none" '
        'stroke="#b71c1c" stroke-width="3" marker-end="url(#arrow)"/>'
    )
    out.append(
        '<defs><marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" '
        'markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" '
        'fill="#c62828"/></marker></defs>'
    )
    out.append(
        '<text x="24" y="112" class="warn">Korrektur: Das lila Kabel vom AD8232-OUTPUT steckt auf '
        "deinem Foto auf GP27_A1 (rot gestrichelt).</text>"
    )
    out.append(
        '<text x="24" y="132" class="warn">Einen Stift nach links auf GP26_A0 umstecken – die '
        "Firmware misst nur dort.</text>"
    )

    # Tafeln: Farben an AD8232 und Display (zum Gegenprüfen)
    ty = 975.0
    out.append(
        f'<text x="40" y="{ty - 34}" style="font-size:14px;font-weight:bold">AD8232 '
        "(bei dir schon gesteckt)</text>"
    )
    for i, (name, color) in enumerate(AD8232):
        x = 60 + i * 52
        _wire(out, x, ty - 22, x, ty + 4, color)
        out.append(f'<rect x="{x - 7}" y="{ty + 4}" width="14" height="14" fill="#ffd54f" '
                   'stroke="#7a5c00"/>')  # fmt: skip
        out.append(f'<text x="{x}" y="{ty + 34}" text-anchor="middle" class="small">{name}</text>')
    dx0 = 470.0
    out.append(
        f'<text x="{dx0 - 20}" y="{ty - 34}" style="font-size:14px;font-weight:bold">Display-'
        "Stiftleiste (bei dir schon gesteckt), Nr. = Lage im Flachkabel vom äußeren Blau</text>"
    )
    for i, (name, color, pos) in enumerate(DISPLAY):
        x = dx0 + i * 62
        _wire(out, x, ty - 22, x, ty + 4, color)
        out.append(f'<rect x="{x - 7}" y="{ty + 4}" width="14" height="14" fill="#ffd54f" '
                   'stroke="#7a5c00"/>')  # fmt: skip
        out.append(f'<text x="{x}" y="{ty + 34}" text-anchor="middle" class="small">{name}</text>')
        out.append(
            f'<text x="{x}" y="{ty + 50}" text-anchor="middle" class="small">{color} ({pos})</text>'
        )

    ny = ty + 88
    for note in (
        "Doppelte Farben (lila, grau, weiß, blau): VCC ist das Lila direkt neben dem äußeren Blau. "
        "Vor dem Einstecken markieren – VCC (5 V) nie auf einen GPIO.",
        "Lötbrücke J1 auf dem Display offen lassen. Display-Strom aus VSYS, nicht aus 3V3 "
        "(die 3,3-V-Schiene versorgt AD8232 und ADC-Referenz).",
        "Touch hat einen eigenen Bus (SPI1, GP10–GP14), weil ohne Breadboard jeder Stift nur "
        "ein Kabel aufnimmt. SD-Karte kommt später.",
        "T_IRQ: Farbe auf den Fotos verdeckt – vermutlich das äußere Blau (Nr. 1). Bitte "
        "nachsehen, bevor du es auf GP14 steckst.",
    ):
        out.append(f'<text x="40" y="{ny}" class="lbl">{note}</text>')
        ny += 20
    out.append("</svg>")
    return "\n".join(out)


if __name__ == "__main__":
    dst = Path(__file__).resolve().parents[1] / "docs" / "verdrahtung.svg"
    dst.write_text(svg(), encoding="utf-8")
    print(dst)
