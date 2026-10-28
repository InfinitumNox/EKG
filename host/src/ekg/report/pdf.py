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
    QPageLayout,
    QPageSize,
    QPainter,
    QPainterPath,
    QPdfWriter,
    QPen,
)

from ..analysis import Analysis
from ..dsp.hrv import hrv_rows
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


def _font(p: QPainter, size_mm: float, bold: bool = False) -> None:
    f = QFont("DejaVu Sans")
    f.setPixelSize(max(1, round(size_mm * 10)))
    f.setBold(bold)
    p.setFont(f)


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
    rows = [
        ("Gerät", f"{dev.get('name', '')} FW {dev.get('firmware', '')}"),
        (
            "Verstärkung",
            f"{conv.gain:.0f} ({'gemessen' if conv.gain_calibrated else 'nominal, nicht kalibriert'})",
        ),
        ("Filter (Auswertung)", an.filter_settings.describe() + ", phasenfrei"),
        ("Testsignal", "ja – keine Messung am Menschen" if rec.test_signal else "nein"),
        ("Lücken / ohne Kontakt", f"{an.gap_s:.1f} s / {an.lead_off_s:.1f} s"),
    ]
    rows += hrv_rows(an.hrv)
    d = an.delineation
    if d:
        rows += [
            ("PR (exp.)", _fmt(d.pr_ms, "ms")),
            ("QRS (exp.)", _fmt(d.qrs_ms, "ms")),
            (
                "QT / QTc Bazett / Fridericia (exp.)",
                f"{_fmt(d.qt_ms, 'ms')} / {_fmt(d.qtc_bazett_ms, 'ms')} / {_fmt(d.qtc_fridericia_ms, 'ms')}",
            ),
        ]
    y = 22.0
    p.setPen(TRACE)
    for k, v in rows:
        _text(p, MARGIN, y, k, size=3.0, w=60)
        _text(p, MARGIN + 62, y, v, size=3.0, bold=True, w=80)
        y += 5.2
    if an.hints:
        y += 2
        _text(p, MARGIN, y, "Hinweise (keine Diagnose):", size=3.0, bold=True)
        for h in an.hints:
            y += 5
            _text(p, MARGIN + 2, y, f"• {h}", size=3.0, w=140)

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

    # HF-Trend
    tr = QRectF(160, 120, 125, 60)
    p.setPen(_pen(QColor("#999999"), 0.15))
    p.drawRect(tr)
    p.setPen(TRACE)
    _text(p, tr.left(), tr.bottom() + 1, "Herzfrequenz über die Aufzeichnung", size=2.6, w=120)
    if len(an.hr_bpm) > 1:
        lo = max(30.0, float(np.percentile(an.hr_bpm, 1)) - 10)
        hi = min(220.0, float(np.percentile(an.hr_bpm, 99)) + 10)
        t_max = max(float(an.hr_t[-1]), 1.0)
        path = QPainterPath()
        for i, (t, h) in enumerate(zip(an.hr_t, np.clip(an.hr_bpm, lo, hi), strict=True)):
            pt = QPointF(
                tr.left() + t / t_max * tr.width(), tr.bottom() - (h - lo) / (hi - lo) * tr.height()
            )
            path.lineTo(pt) if i else path.moveTo(pt)
        p.setPen(_pen(QColor("#c62828"), 0.25))
        p.drawPath(path)
        p.setPen(TRACE)
        _text(
            p,
            tr.left() - 12,
            tr.top() - 1,
            f"{hi:.0f}",
            size=2.4,
            w=10,
            align=Qt.AlignmentFlag.AlignRight,
        )
        _text(
            p,
            tr.left() - 12,
            tr.bottom() - 3,
            f"{lo:.0f}",
            size=2.4,
            w=10,
            align=Qt.AlignmentFlag.AlignRight,
        )
    _footer(p)
