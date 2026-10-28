"""EKG-Raster für pyqtgraph-Plots (x in s, y in mV): 40 ms / 0,1 mV und 200 ms / 0,5 mV."""

from __future__ import annotations

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import QLineF, QRectF
from PySide6.QtGui import QColor, QPen

from .theme import Theme


class EcgGridItem(pg.GraphicsObject):
    MIN_SPACING_PX = 3.0

    def __init__(self, theme: Theme):
        super().__init__()
        self.theme = theme
        self.setZValue(-100)
        self._rect = QRectF()

    def set_theme(self, theme: Theme) -> None:
        self.theme = theme
        self.update()

    def viewRangeChanged(self) -> None:  # noqa: N802 - pyqtgraph-Hook
        self.prepareGeometryChange()
        vb = self.getViewBox()
        if vb is not None:
            (x0, x1), (y0, y1) = vb.viewRange()
            self._rect = QRectF(x0, y0, x1 - x0, y1 - y0)
        self.update()

    def boundingRect(self) -> QRectF:  # noqa: N802
        return self._rect

    def dataBounds(self, ax, frac=1.0, orthoRange=None):  # noqa: N802, ARG002
        """Das Raster folgt der Ansicht und darf die Autoskalierung nicht beeinflussen."""
        return [None, None]

    def paint(self, p, *args) -> None:
        vb = self.getViewBox()
        if vb is None:
            return
        (x0, x1), (y0, y1) = vb.viewRange()
        px_x, px_y = vb.viewPixelSize()
        for (dx, dy), color in (
            ((0.04, 0.1), self.theme.grid_minor),
            ((0.2, 0.5), self.theme.grid_major),
        ):
            if dx / px_x < self.MIN_SPACING_PX or dy / px_y < self.MIN_SPACING_PX:
                continue
            pen = QPen(QColor(color))
            pen.setCosmetic(True)
            pen.setWidth(1)
            p.setPen(pen)
            xs = np.arange(np.ceil(x0 / dx) * dx, x1, dx)
            ys = np.arange(np.ceil(y0 / dy) * dy, y1, dy)
            p.drawLines([QLineF(x, y0, x, y1) for x in xs] + [QLineF(x0, y, x1, y) for y in ys])
