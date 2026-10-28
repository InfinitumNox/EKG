"""Live-EKG im Sweep-Modus wie am Monitor, mit echtem Millimeter-Raster.

Mehrere Zeilen werden nacheinander beschrieben, vor dem Schreibcursor bleibt eine
Lücke. Bei mehr Samples als Pixeln wird je Pixelspalte Minimum und Maximum gezeichnet,
damit keine R-Zacke durch Unterabtastung verschwindet.
"""

from __future__ import annotations

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import QFont, QPainter, QPainterPath, QPen, QPixmap, QPolygonF
from PySide6.QtWidgets import QWidget

from ..pipeline import GAP_FLAG, LivePipeline
from ..protocol import Flag
from .theme import PAPER, Theme

DIM_FLAGS = int(Flag.LO_PLUS | Flag.LO_MINUS) | GAP_FLAG


class EcgSweepView(QWidget):
    ROW_MM = 40.0
    MARGIN_MM = 10.0
    GAP_S = 0.25

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.pipeline: LivePipeline | None = None
        self.px_per_mm = 96 / 25.4
        self.speed = 25.0
        self.gain = 10.0
        self.theme: Theme = PAPER
        self.frozen_end: int | None = None
        self.placeholder = "Kein Gerät verbunden"
        self._grid: QPixmap | None = None
        self.setMinimumSize(420, 220)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)
        self._timer = QTimer(self)
        self._timer.timeout.connect(self.update)
        self._timer.start(33)

    # ------------------------------------------------------------ Einstellungen
    def set_pipeline(self, pipeline: LivePipeline | None) -> None:
        self.pipeline = pipeline
        self.frozen_end = None
        self.update()

    def set_scale(self, px_per_mm: float, speed: float, gain: float) -> None:
        self.px_per_mm, self.speed, self.gain = px_per_mm, speed, gain
        self._grid = None
        self.update()

    def set_theme(self, theme: Theme) -> None:
        self.theme = theme
        self._grid = None
        self.update()

    def set_frozen(self, frozen: bool) -> None:
        self.frozen_end = self.pipeline.display.end if (frozen and self.pipeline) else None
        self.update()

    @property
    def frozen(self) -> bool:
        return self.frozen_end is not None

    def resizeEvent(self, event) -> None:  # noqa: N802
        self._grid = None
        super().resizeEvent(event)

    # ------------------------------------------------------------ Geometrie
    def _layout(self) -> tuple[int, float, float]:
        mm = self.px_per_mm
        h_mm = self.height() / mm
        row_mm = max(20.0, 10.0 * round(min(self.ROW_MM, h_mm) / 10.0))
        rows = max(1, int(h_mm // row_mm))
        return rows, row_mm * mm, self.MARGIN_MM * mm

    def _build_grid(self) -> QPixmap:
        pm = QPixmap(self.size())
        pm.fill(self.theme.c("background"))
        p = QPainter(pm)
        mm = self.px_per_mm
        w, h = self.width(), self.height()
        nx, ny = int(w / mm) + 1, int(h / mm) + 1
        if mm >= 3.0:
            p.setPen(QPen(self.theme.c("grid_minor"), 1))
            for i in range(nx):
                if i % 5:
                    p.drawLine(QPointF(i * mm, 0), QPointF(i * mm, h))
            for j in range(ny):
                if j % 5:
                    p.drawLine(QPointF(0, j * mm), QPointF(w, j * mm))
        p.setPen(QPen(self.theme.c("grid_major"), 1))
        for i in range(0, nx, 5):
            p.drawLine(QPointF(i * mm, 0), QPointF(i * mm, h))
        for j in range(0, ny, 5):
            p.drawLine(QPointF(0, j * mm), QPointF(w, j * mm))
        p.end()
        return pm

    # ------------------------------------------------------------ Zeichnen
    def paintEvent(self, event) -> None:  # noqa: N802
        if self._grid is None or self._grid.size() != self.size():
            self._grid = self._build_grid()
        p = QPainter(self)
        p.drawPixmap(0, 0, self._grid)
        pl = self.pipeline
        rows, row_h, margin = self._layout()
        self._draw_cal_pulses(p, rows, row_h)
        if pl is None or pl.display.end == 0:
            self._center_text(p, self.placeholder)
            self._draw_legend(p)
            p.end()
            return

        fs = pl.fs
        px_per_s = self.px_per_mm * self.speed
        n_row = int((self.width() - margin - 4) / px_per_s * fs)
        if n_row < 10:
            p.end()
            return
        total = rows * n_row
        visible = total - int(self.GAP_S * fs)
        end = self.frozen_end if self.frozen_end is not None else pl.display.end
        start = end - visible
        y = pl.display.get(start, end).astype(np.float64)
        flags = pl.flags.get(start, end)
        q = np.arange(start, end) % total
        row = q // n_row
        x = margin + (q % n_row) * (px_per_s / fs)
        ypx = row * row_h + row_h / 2 - y * self.gain * self.px_per_mm

        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        dim = (flags & DIM_FLAGS) != 0
        brk = np.zeros(len(q), dtype=bool)
        brk[1:] = (row[1:] != row[:-1]) | (q[1:] < q[:-1]) | (dim[1:] != dim[:-1])
        for mask, color in ((~dim, "trace"), (dim, "trace_dim")):
            path = self._path(x, ypx, brk, mask)
            if path is not None:
                pen = QPen(self.theme.c(color), 1.4 if color == "trace" else 1.0)
                pen.setCosmetic(True)
                p.setPen(pen)
                p.drawPath(path)

        self._draw_beats(p, pl, start, end, total, n_row, row_h, margin, px_per_s, fs)
        # Schreibcursor
        qe = (end - 1) % total
        cx = margin + (qe % n_row) * (px_per_s / fs)
        cy = (qe // n_row) * row_h
        p.setPen(QPen(self.theme.c("cursor"), 2))
        p.drawLine(QPointF(cx, cy + 2), QPointF(cx, cy + row_h - 2))
        self._draw_legend(p)
        if self.frozen_end is not None:
            self._badge(p, "EINGEFROREN")
        p.end()

    def _path(
        self, x: np.ndarray, y: np.ndarray, brk: np.ndarray, mask: np.ndarray
    ) -> QPainterPath | None:
        if not mask.any():
            return None
        idx = np.flatnonzero(mask)
        xs, ys = x[idx], y[idx]
        b = brk[idx].copy()
        b[1:] |= np.diff(idx) > 1
        # Min/Max je Pixelspalte (Gruppen enden an Spaltenwechsel oder Bruchstelle).
        col = np.floor(xs).astype(np.int64)
        new_group = np.ones(len(xs), dtype=bool)
        new_group[1:] = (col[1:] != col[:-1]) | b[1:]
        starts = np.flatnonzero(new_group)
        with np.errstate(invalid="ignore"):
            ymin = np.fmin.reduceat(ys, starts)
            ymax = np.fmax.reduceat(ys, starts)
        gx = xs[starts]
        px = np.repeat(gx, 2)
        py = np.column_stack([ymin, ymax]).ravel()
        # Punktfolge: min0, max0, min1, max1, ...; connect[k] verbindet Punkt k mit k+1.
        # Innerhalb einer Gruppe immer verbinden, zur nächsten nur ohne Bruchstelle.
        connect = np.ones(len(px), dtype=np.int32)
        between = np.zeros(len(starts), dtype=np.int32)
        between[:-1] = ~b[starts][1:]
        connect[1::2] = between
        finite = np.isfinite(py)
        if not finite.any():
            return None
        return pg.arrayToQPath(px, py, connect=connect, finiteCheck=True)

    def _draw_beats(
        self,
        p: QPainter,
        pl: LivePipeline,
        start: int,
        end: int,
        total: int,
        n_row: int,
        row_h: float,
        margin: float,
        px_per_s: float,
        fs: float,
    ) -> None:
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(self.theme.c("marker"))
        s = 1.6 * self.px_per_mm
        for b in pl.beats_between(start, end):
            q = int(round(b.index)) % total
            bx = margin + (q % n_row) * (px_per_s / fs)
            by = (q // n_row) * row_h + 1.5 * self.px_per_mm
            p.drawPolygon(
                QPolygonF([QPointF(bx - s / 2, by), QPointF(bx + s / 2, by), QPointF(bx, by + s)])
            )
        p.setBrush(Qt.BrushStyle.NoBrush)

    def _draw_cal_pulses(self, p: QPainter, rows: int, row_h: float) -> None:
        """Eichzacke 1 mV × 0,2 s am Zeilenanfang (zeigt den aktuellen Maßstab)."""
        mm = self.px_per_mm
        pen = QPen(self.theme.c("trace"), 1.4)
        pen.setCosmetic(True)
        p.setPen(pen)
        width = 0.2 * self.speed * mm
        for r in range(rows):
            base = r * row_h + row_h / 2
            top = base - self.gain * mm
            x0 = 2 * mm
            p.drawPolyline(
                QPolygonF(
                    [
                        QPointF(x0 - mm, base),
                        QPointF(x0, base),
                        QPointF(x0, top),
                        QPointF(x0 + width, top),
                        QPointF(x0 + width, base),
                        QPointF(x0 + width + mm, base),
                    ]
                )
            )

    def _draw_legend(self, p: QPainter) -> None:
        pl = self.pipeline
        filt = pl.filter_settings.describe() if pl else ""
        text = f"{self.speed:g} mm/s   {self.gain:g} mm/mV   {filt}"
        f = QFont(self.font())
        f.setPointSizeF(max(8.0, f.pointSizeF() * 0.9))
        p.setFont(f)
        p.setPen(self.theme.c("text"))
        p.drawText(
            QRectF(8, self.height() - 22, self.width() - 16, 18),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            text,
        )

    def _center_text(self, p: QPainter, text: str) -> None:
        f = QFont(self.font())
        f.setPointSizeF(f.pointSizeF() * 1.3)
        p.setFont(f)
        p.setPen(self.theme.c("text"))
        p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, text)

    def _badge(self, p: QPainter, text: str) -> None:
        f = QFont(self.font())
        f.setBold(True)
        p.setFont(f)
        r = QRectF(self.width() - 150, 8, 140, 24)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(self.theme.c("cursor"))
        p.drawRoundedRect(r, 4, 4)
        p.setPen(self.theme.c("background"))
        p.drawText(r, Qt.AlignmentFlag.AlignCenter, text)
