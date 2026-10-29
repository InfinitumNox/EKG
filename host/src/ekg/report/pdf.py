"""PDF-Bericht im EKG-Papierformat (25 mm/s, 10 mm/mV), gezeichnet mit QPdfWriter.

Koordinaten in Millimetern; das Raster ist damit maßstabsgetreu ausdruckbar
(beim Drucken „Tatsächliche Größe“ wählen).
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PySide6.QtCore import QMarginsF, QPointF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QFont,
    QFontMetricsF,
    QPageLayout,
    QPageSize,
    QPainter,
    QPainterPath,
    QPdfWriter,
    QPen,
)

from ..analysis import Analysis, quality_rows
from ..dsp.beats import interval_rows
from ..dsp.hrv import hrv_rows, segment_rows
from ..storage.recording import Recording

PAGE_W, PAGE_H = 297.0, 210.0
MARGIN = 10.0
STRIP_S = 10.0
STRIP_H = 30.0
SPEED = 25.0
GAIN = 10.0
GRID_MINOR = QColor("#f4c7c7")
GRID_MAJOR = QColor("#e28a8a")
TRACE = QColor("#111111")


def _pen(color: QColor, width_mm: float) -> QPen:
    pen = QPen(color, width_mm)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    return pen


def _qfont(size_mm: float, bold: bool = False) -> QFont:
    f = QFont("DejaVu Sans")
    f.setPixelSize(max(1, round(size_mm * 10)))
    f.setBold(bold)
    return f


def _font(p: QPainter, size_mm: float, bold: bool = False) -> None:
    p.setFont(_qfont(size_mm, bold))


def _wrap(text: str, size: float, bold: bool, w: float) -> list[str]:
    """Bricht text an Leerzeichen so um, dass jede Zeile in w mm passt."""
    fm = QFontMetricsF(_qfont(size, bold))  # Einheit 1/10 mm wie in _text
    lines: list[str] = []
    for word in text.split(" "):
        if lines and fm.horizontalAdvance(f"{lines[-1]} {word}") <= w * 10:
            lines[-1] += f" {word}"
        else:
            lines.append(word)
    return lines


def _text(
    p: QPainter,
    x: float,
    y: float,
    text: str,
    size: float = 3.2,
    bold: bool = False,
    w: float = 200.0,
    align=Qt.AlignmentFlag.AlignLeft,
) -> None:
    # Schrift in 1/10 mm setzen, damit Größen unabhängig von der Auflösung sind.
    p.save()
    p.translate(x, y)
    p.scale(0.1, 0.1)
    _font(p, size, bold)
    p.drawText(QRectF(0, 0, w * 10, size * 14), int(align | Qt.AlignmentFlag.AlignTop), text)
    p.restore()


def _grid(p: QPainter, r: QRectF) -> None:
    p.save()
    p.setClipRect(r)
    p.setPen(_pen(GRID_MINOR, 0.06))
    x = r.left()
    i = 0
    while x <= r.right() + 1e-6:
        if i % 5:
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
        x += 1.0
        i += 1
    y = r.top()
    j = 0
    while y <= r.bottom() + 1e-6:
        if j % 5:
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y))
        y += 1.0
        j += 1
    p.setPen(_pen(GRID_MAJOR, 0.12))
    x = r.left()
    while x <= r.right() + 1e-6:
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
        x += 5.0
    y = r.top()
    while y <= r.bottom() + 1e-6:
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y))
        y += 5.0
    p.restore()


def _trace(
    p: QPainter,
    x0: float,
    y0: float,
    mv: np.ndarray,
    fs: float,
    speed: float,
    gain: float,
    clip: QRectF,
) -> None:
    path = QPainterPath()
    pen_down = False
    dx = speed / fs
    for i, v in enumerate(mv):
        if not np.isfinite(v):
            pen_down = False
            continue
        pt = QPointF(x0 + i * dx, y0 - float(v) * gain)
        if pen_down:
            path.lineTo(pt)
        else:
            path.moveTo(pt)
            pen_down = True
    p.save()
    p.setClipRect(clip)
    p.setPen(_pen(TRACE, 0.22))
    p.drawPath(path)
    p.restore()


def _cal_pulse(p: QPainter, x: float, base: float, speed: float, gain: float) -> None:
    w = 0.2 * speed
    p.setPen(_pen(TRACE, 0.22))
    p.drawPolyline(
        [
            QPointF(x, base),
            QPointF(x + 1, base),
            QPointF(x + 1, base - gain),
            QPointF(x + 1 + w, base - gain),
            QPointF(x + 1 + w, base),
            QPointF(x + 2 + w, base),
        ]
    )


def _fmt(v: float | None, unit: str, digits: int = 0) -> str:
    return "–" if v is None or not np.isfinite(v) else f"{v:.{digits}f} {unit}"


def write_report(
    rec: Recording, an: Analysis, path: Path, start_s: float = 0.0, strip_pages: int = 2
) -> Path:
    path = Path(path)
    w = QPdfWriter(str(path))
    w.setResolution(600)
    w.setPageSize(QPageSize(QPageSize.PageSizeId.A4))
    w.setPageOrientation(QPageLayout.Orientation.Landscape)
    w.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout.Unit.Millimeter)
    w.setTitle("EKG-Aufzeichnung")
    w.setCreator("ekg")
    p = QPainter(w)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    scale = w.resolution() / 25.4
    p.scale(scale, scale)

    _summary_page(p, rec, an)
    fs = rec.fs
    per_page = 5
    t = start_s
    for _ in range(strip_pages):
        if t >= rec.duration_s:
            break
        w.newPage()
        _header(p, rec, f"Rhythmusstreifen ab {t:.0f} s")
        for k in range(per_page):
            if t >= rec.duration_s:
                break
            top = 22 + k * (STRIP_H + 6)
            r = QRectF(MARGIN, top, PAGE_W - 2 * MARGIN, STRIP_H)
            _grid(p, r)
            base = top + STRIP_H * 0.6
            _cal_pulse(p, MARGIN + 1, base, SPEED, GAIN)
            i0, i1 = int(t * fs), int((t + STRIP_S) * fs)
            _trace(p, MARGIN + 10, base, an.filtered_mv[i0:i1], fs, SPEED, GAIN, r)
            p.setPen(TRACE)
            _text(p, MARGIN, top + STRIP_H + 0.5, f"{_clock(rec, t)}   t = {t:.0f} s", size=2.6)
            for b in an.beats:
                bt = b.index / fs
                if t <= bt < t + STRIP_S:
                    _text(
                        p,
                        MARGIN + 10 + (bt - t) * SPEED - 4,
                        top + 0.5,
                        "▼",
                        size=2.0,
                        w=8,
                        align=Qt.AlignmentFlag.AlignHCenter,
                    )
            t += STRIP_S
        _footer(p)
    p.end()
    return path


def _clock(rec: Recording, t: float) -> str:
    st = rec.start_time
    if st is None:
        return ""
    from datetime import timedelta

    return (st + timedelta(seconds=t)).strftime("%H:%M:%S")


def _header(p: QPainter, rec: Recording, subtitle: str) -> None:
    p.setPen(TRACE)
    _text(p, MARGIN, 7, "EKG-Aufzeichnung · 1 Kanal (AD8232)", size=4.2, bold=True)
    st = rec.start_time
    when = st.strftime("%d.%m.%Y %H:%M:%S") if st else ""
    _text(p, MARGIN, 13, f"{when}   ·   {subtitle}   ·   {SPEED:g} mm/s, {GAIN:g} mm/mV", size=3.0)
    p.setPen(QColor("#b71c1c"))
    _text(
        p,
        PAGE_W - MARGIN - 120,
        7,
        "NICHT DIAGNOSTISCH – kein Medizinprodukt",
        size=3.4,
        bold=True,
        w=120,
        align=Qt.AlignmentFlag.AlignRight,
    )
    p.setPen(TRACE)


def _footer(p: QPainter) -> None:
    p.setPen(QColor("#666666"))
    _text(
        p,
        MARGIN,
        PAGE_H - 8,
        "Bandbreite durch das AD8232-Board begrenzt (typ. 0,5–40 Hz): keine ST-Beurteilung. "
        "Intervalle aus dem Median-Schlag sind experimentell. Beim Drucken „Tatsächliche Größe“ wählen.",
        size=2.4,
        w=PAGE_W - 2 * MARGIN,
    )
    p.setPen(TRACE)


def _summary_page(p: QPainter, rec: Recording, an: Analysis) -> None:
    dur = rec.duration_s
    _header(p, rec, f"Dauer {int(dur // 60)}:{int(dur % 60):02d} min")
    conv = rec.conversion
    dev = rec.header.get("device", {})
    rows: list[tuple[str, str]] = [
        (
            "Gerät",
            f"{dev.get('name') or '–'}"
            + (f", FW {dev['firmware']}" if dev.get("firmware") else ""),
        ),
        ("Verstärkung", f"{conv.gain:.0f} ({'gemessen' if conv.gain_calibrated else 'nominal'})"),
        ("Filter (Auswertung)", an.filter_settings.describe() + ", phasenfrei"),
        ("Testsignal", "ja – keine Messung am Menschen" if rec.test_signal else "nein"),
    ]
    rows += quality_rows(an) + [("", "")]
    rows += hrv_rows(an.hrv, an.breathing_hz, an.ectopy)
    seg = segment_rows(an.segments)
    if seg:
        rows += [("", ""), ("Kurzzeit-HRV", "Median (Spanne) der 5-min-Abschnitte")] + seg
    d = an.delineation
    if d and an.median is not None:
        rows += [("", "")] + interval_rows(d, an.median.mean_rr_s, " (exp.)")
    y = 21.0
    p.setPen(TRACE)
    for k, v in rows:
        if not k:
            y += 1.6
            continue
        _text(p, MARGIN, y, k, size=2.8, w=62)
        for i, line in enumerate(_wrap(v, 2.8, True, 76)):
            y += 3.8 if i else 0.0
            _text(p, MARGIN + 64, y, line, size=2.8, bold=True, w=78)
        y += 4.6
    notes = an.notes
    if notes:
        y += 2
        _text(p, MARGIN, y, "Hinweise (keine Diagnose):", size=2.8, bold=True)
        for h in notes:
            for i, line in enumerate(_wrap(h, 2.8, False, 134)):
                y += 4.6 if i == 0 else 3.8
                _text(
                    p,
                    MARGIN + 2 + (0 if i == 0 else 2.5),
                    y,
                    ("• " if i == 0 else "") + line,
                    size=2.8,
                    w=140,
                )

    # Median-Schlag, 50 mm/s und 20 mm/mV
    mb = an.median
    r = QRectF(160, 22, 125, 80)
    _grid(p, r)
    p.setPen(TRACE)
    _text(p, r.left(), r.bottom() + 1, "Median-Schlag (50 mm/s, 20 mm/mV)", size=2.6, w=120)
    if mb is not None:
        base = r.top() + r.height() * 0.62
        x0 = r.left() + 8
        _cal_pulse(p, r.left() + 1, base, 50, 20)
        _trace(p, x0 + 6, base, mb.template - (d.baseline_mv if d else 0), mb.fs, 50, 20, r)
        if d:
            p.setPen(_pen(QColor("#1565c0"), 0.15))
            for idx in (d.p_onset, d.qrs_onset, d.qrs_offset, d.t_end):
                if idx is not None:
                    x = x0 + 6 + idx / mb.fs * 50
                    p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))

    # HF-Trend: dünn Schlag für Schlag, dick der gleitende Median (30 s)
    tr = QRectF(160, 120, 125, 60)
    p.setPen(TRACE)
    _text(
        p,
        tr.left(),
        tr.bottom() + 5,
        "Herzfrequenz (/min): dünn je Schlag, dick gleitender Median über 30 s",
        size=2.6,
        w=125,
    )
    if len(an.hr_bpm) > 1:
        lo = 10 * np.floor((float(np.percentile(an.hr_bpm, 1)) - 5) / 10)
        hi = 10 * np.ceil((float(np.percentile(an.hr_bpm, 99)) + 5) / 10)
        lo, hi = max(20.0, lo), min(240.0, hi)
        t_max = max(float(an.hr_t[-1]), 1.0)

        def pt(t: float, h: float) -> QPointF:
            h = min(max(h, lo), hi)
            return QPointF(
                tr.left() + t / t_max * tr.width(), tr.bottom() - (h - lo) / (hi - lo) * tr.height()
            )

        p.setPen(_pen(QColor("#dddddd"), 0.12))
        for v in np.arange(lo, hi + 0.1, 10):
            y0 = pt(0, float(v)).y()
            p.drawLine(QPointF(tr.left(), y0), QPointF(tr.right(), y0))
            p.setPen(TRACE)
            _text(
                p,
                tr.left() - 11,
                y0 - 1.3,
                f"{v:.0f}",
                size=2.2,
                w=9,
                align=Qt.AlignmentFlag.AlignRight,
            )
            p.setPen(_pen(QColor("#dddddd"), 0.12))
        step_min = next(s for s in (1, 2, 5, 10, 15, 30, 60, 120, 240) if t_max / 60 / s <= 8)
        for m in np.arange(0, t_max / 60 + 1e-9, step_min):
            x0 = pt(m * 60, lo).x()
            p.setPen(_pen(QColor("#dddddd"), 0.12))
            p.drawLine(QPointF(x0, tr.top()), QPointF(x0, tr.bottom()))
            p.setPen(TRACE)
            if x0 > tr.right() - 7:  # Platz für die Einheit „min“
                continue
            _text(
                p,
                x0 - 5,
                tr.bottom() + 0.8,
                f"{m:.0f}",
                size=2.2,
                w=10,
                align=Qt.AlignmentFlag.AlignHCenter,
            )
        _text(
            p,
            tr.right() - 20,
            tr.bottom() + 0.8,
            "min",
            size=2.2,
            w=20,
            align=Qt.AlignmentFlag.AlignRight,
        )
        p.setPen(_pen(QColor("#999999"), 0.15))
        p.drawRect(tr)
        for series, color, width in (
            (an.hr_bpm, QColor("#ef9a9a"), 0.15),
            (an.hr_smooth, QColor("#b71c1c"), 0.45),
        ):
            path = QPainterPath()
            for i, (t, h) in enumerate(zip(an.hr_t, series, strict=True)):
                path.lineTo(pt(t, h)) if i else path.moveTo(pt(t, h))
            p.setPen(_pen(color, width))
            p.drawPath(path)
        p.setPen(TRACE)
    _footer(p)
