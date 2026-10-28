"""Trends der laufenden Messung: HF, RR-Tachogramm, Poincaré, HRV der letzten 5 min."""

from __future__ import annotations

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QGridLayout, QLabel, QWidget

from ..dsp.hr import instantaneous_hr
from ..dsp.hrv import compute_hrv, hrv_rows, rhythm_hints
from ..pipeline import LivePipeline
from .widgets import InfoPanel

HRV_WINDOW_S = 300.0


class TrendsWidget(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.pipeline: LivePipeline | None = None
        lay = QGridLayout(self)
        self.hr_plot = pg.PlotWidget(title="Herzfrequenz")
        self.hr_plot.setLabel("left", "/min")
        self.hr_plot.setLabel("bottom", "Zeit", units="min")
        self.hr_plot.showGrid(x=True, y=True, alpha=0.3)
        self.hr_curve = self.hr_plot.plot([], [], pen=pg.mkPen("#c62828", width=2))
        self.rr_plot = pg.PlotWidget(title="RR-Tachogramm")
        self.rr_plot.setLabel("left", "RR", units="ms")
        self.rr_plot.setLabel("bottom", "Schlag")
        self.rr_plot.showGrid(x=True, y=True, alpha=0.3)
        self.rr_curve = self.rr_plot.plot(
            [],
            [],
            pen=pg.mkPen("#1565c0", width=1.5),
            symbol="o",
            symbolSize=4,
            symbolBrush="#1565c0",
        )
        self.poincare = pg.PlotWidget(title="Poincaré (RRₙ gegen RRₙ₊₁)")
        self.poincare.setLabel("left", "RRₙ₊₁", units="ms")
        self.poincare.setLabel("bottom", "RRₙ", units="ms")
        self.poincare.setAspectLocked(True)
        self.poincare.showGrid(x=True, y=True, alpha=0.3)
        self.poincare_pts = pg.ScatterPlotItem(
            size=5, brush=pg.mkBrush(21, 101, 192, 120), pen=None
        )
        self.poincare.addItem(self.poincare_pts)
        self.hrv = InfoPanel("HRV (letzte 5 min)")
        self.hints = QLabel("")
        self.hints.setWordWrap(True)
        lay.addWidget(self.hr_plot, 0, 0, 1, 2)
        lay.addWidget(self.rr_plot, 1, 0)
        lay.addWidget(self.poincare, 1, 1)
        lay.addWidget(self.hrv, 0, 2)
        lay.addWidget(self.hints, 1, 2)
        lay.setColumnStretch(0, 3)
        lay.setColumnStretch(1, 2)
        lay.setColumnStretch(2, 1)
        self._timer = QTimer(self)
        self._timer.timeout.connect(self.refresh)
        self._timer.start(1000)

    def set_pipeline(self, pipeline: LivePipeline | None) -> None:
        self.pipeline = pipeline
        self.refresh()

    def refresh(self) -> None:
        if not self.isVisible() or self.pipeline is None:
            return
        pl = self.pipeline
        r = np.array([b.index for b in pl.all_beats()])
        t, hr = instantaneous_hr(r, pl.fs)
        self.hr_curve.setData(t / 60.0, hr)
        rr = np.diff(r) / pl.fs * 1000.0 if len(r) > 1 else np.zeros(0)
        last = rr[-300:]
        self.rr_curve.setData(np.arange(len(last)), last)
        ok = (last > 250) & (last < 2500)
        pts = last[ok]
        if len(pts) > 1:
            self.poincare_pts.setData(pts[:-1], pts[1:])
        else:
            self.poincare_pts.clear()
        if len(r):
            r_s = r / pl.fs
            recent = r_s[r_s >= r_s[-1] - HRV_WINDOW_S]
            self.hrv.set_rows(hrv_rows(compute_hrv(recent)))
            hints = rhythm_hints(recent)
            self.hints.setText(
                "<b>Hinweise</b> (keine Diagnose):<br>" + "<br>".join(f"• {h}" for h in hints)
                if hints
                else ""
            )
        else:
            self.hrv.set_rows([("HRV", "noch keine Daten")])
            self.hints.setText("")
