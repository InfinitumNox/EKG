"""Auswertung gespeicherter Aufzeichnungen."""

from __future__ import annotations

import logging
from pathlib import Path

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QProgressDialog,
    QPushButton,
    QSplitter,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from ..analysis import Analysis, analyse
from ..config import Settings, recordings_dir
from ..dsp.filters import PRESETS
from ..dsp.hrv import hrv_rows, segment_rows
from ..storage import export
from ..storage.recording import Recording, load_recording
from .grid_item import EcgGridItem
from .theme import PAPER, Theme
from .widgets import InfoPanel

log = logging.getLogger(__name__)


class AnalysisThread(QThread):
    progress = Signal(str, float)
    done = Signal(object)
    failed = Signal(str)

    def __init__(self, rec: Recording, preset: str, parent=None):
        super().__init__(parent)
        self.rec = rec
        self.preset = preset

    def run(self) -> None:
        try:
            an = analyse(self.rec, PRESETS[self.preset][1], lambda m, f: self.progress.emit(m, f))
            self.done.emit(an)
        except Exception as exc:  # noqa: BLE001
            log.exception("Auswertung fehlgeschlagen")
            self.failed.emit(str(exc))


def _fmt(v, unit, digits=0):
    return "–" if v is None or not np.isfinite(v) else f"{v:.{digits}f} {unit}"


class ReviewWidget(QWidget):
    def __init__(self, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self.theme: Theme = PAPER
        self.rec: Recording | None = None
        self.an: Analysis | None = None
        self._thread: AnalysisThread | None = None
        self._calipers: list[pg.InfiniteLine] = []
        self._build()

    # ------------------------------------------------------------ Aufbau
    def _build(self) -> None:
        lay = QVBoxLayout(self)
        bar = QHBoxLayout()
        self.open_btn = QPushButton("Aufzeichnung öffnen …")
        self.open_btn.clicked.connect(self.open_dialog)
        self.file_label = QLabel("Keine Aufzeichnung geladen")
        self.preset = QComboBox()
        for key, (label, s) in PRESETS.items():
            self.preset.addItem(f"{label} ({s.describe()})", key)
        self.preset.currentIndexChanged.connect(self._reanalyse)
        self.paper_btn = QToolButton()
        self.paper_btn.setText("1:1 Papier")
        self.paper_btn.setCheckable(True)
        self.paper_btn.setToolTip("Maßstabsgetreu: 25 mm/s, 10 mm/mV (Bildschirm kalibrieren!)")
        self.paper_btn.toggled.connect(self._apply_paper_scale)
        self.caliper_btn = QToolButton()
        self.caliper_btn.setText("Messschieber")
        self.caliper_btn.setCheckable(True)
        self.caliper_btn.toggled.connect(self._toggle_calipers)
        self.export_btn = QToolButton()
        self.export_btn.setText("Exportieren")
        self.export_btn.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        from PySide6.QtWidgets import QMenu

        m = QMenu(self)
        m.addAction("PDF-Bericht …", self.export_pdf)
        m.addAction("EDF+ …", self.export_edf)
        m.addAction("CSV …", self.export_csv)
        m.addAction("WFDB (PhysioNet) …", self.export_wfdb)
        self.export_btn.setMenu(m)
        bar.addWidget(self.open_btn)
        bar.addWidget(self.file_label)
        bar.addStretch(1)
        for w in (
            QLabel("Filter:"),
            self.preset,
            self.paper_btn,
            self.caliper_btn,
            self.export_btn,
        ):
            bar.addWidget(w)
        lay.addLayout(bar)

        split = QSplitter(Qt.Orientation.Horizontal)
        left = QWidget()
        ll = QVBoxLayout(left)
        ll.setContentsMargins(0, 0, 0, 0)
        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "Zeit", units="s")
        self.plot.setLabel("left", "mV")
        self.plot.setDownsampling(auto=True, mode="peak")
        self.plot.setClipToView(True)
        self.grid = EcgGridItem(self.theme)
        self.plot.addItem(self.grid)
        self.plot.getViewBox().sigRangeChanged.connect(lambda *_: self.grid.viewRangeChanged())
        self.plot.getViewBox().sigRangeChanged.connect(self._on_range)
        self.plot.getViewBox().sigResized.connect(self._apply_paper_scale)
        self.curve = self.plot.plot([], [], pen=pg.mkPen(self.theme.trace, width=1.2))
        self.beat_pts = pg.ScatterPlotItem(
            size=7, symbol="t", brush=pg.mkBrush(self.theme.marker), pen=None
        )
        self.plot.addItem(self.beat_pts)
        self.caliper_text = pg.TextItem(
            "", anchor=(0, 0), color="#1565c0", fill=pg.mkBrush(255, 255, 255, 200)
        )
        self.caliper_text.setZValue(50)
        self.overview = pg.PlotWidget()
        self.overview.setMaximumHeight(140)
        self.overview.setLabel("left", "HF /min")
        self.overview.setLabel("bottom", "Zeit", units="s")
        self.ov_curve = self.overview.plot([], [], pen=pg.mkPen("#ef9a9a", width=1))
        self.ov_smooth = self.overview.plot([], [], pen=pg.mkPen("#b71c1c", width=2))
        self.region = pg.LinearRegionItem([0, 10])
        self.region.setZValue(10)
        self.overview.addItem(self.region)
        self.region.sigRegionChanged.connect(self._on_region)
        ll.addWidget(self.plot, 4)
        ll.addWidget(self.overview, 1)
        split.addWidget(left)

        right = QWidget()
        rl = QVBoxLayout(right)
        rl.setContentsMargins(0, 0, 0, 0)
        self.summary = InfoPanel("Übersicht")
        self.window_panel = InfoPanel("Sichtbarer Ausschnitt")
        self.beat_plot = pg.PlotWidget(title="Median-Schlag")
        self.beat_plot.setLabel("bottom", "relativ zu R", units="s")
        self.beat_plot.setLabel("left", "mV")
        self.beat_plot.setMinimumHeight(220)
        self.intervals = InfoPanel("Intervalle (experimentell)")
        self.hints = QLabel("")
        self.hints.setWordWrap(True)
        for panel in (self.summary, self.window_panel, self.beat_plot, self.intervals, self.hints):
            rl.addWidget(panel)
        rl.addStretch(1)
        split.addWidget(right)
        split.setStretchFactor(0, 3)
        split.setStretchFactor(1, 1)
        lay.addWidget(split, 1)
        self._set_enabled(False)

    def _set_enabled(self, on: bool) -> None:
        for w in (self.preset, self.paper_btn, self.caliper_btn, self.export_btn):
            w.setEnabled(on)

    def set_theme(self, theme: Theme) -> None:
        self.theme = theme
        self.grid.set_theme(theme)
        self.curve.setPen(pg.mkPen(theme.trace, width=1.2))
        self.plot.setBackground(theme.background)

    # ------------------------------------------------------------ Laden & Auswerten
    def open_dialog(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Aufzeichnung öffnen", str(recordings_dir()), "EKG-Aufzeichnungen (*.ekgrec)"
        )
        if path:
            self.load(Path(path))

    def load(self, path: Path) -> None:
        try:
            rec = load_recording(path)
        except (OSError, ValueError) as exc:
            QMessageBox.critical(self, "Fehler", f"Aufzeichnung kann nicht geöffnet werden:\n{exc}")
            return
        if rec.n_samples < rec.fs * 3:
            QMessageBox.warning(self, "Zu kurz", "Die Aufzeichnung ist kürzer als 3 s.")
            return
        self.rec = rec
        note = " (unvollständig, letzter Block fehlt)" if rec.truncated else ""
        self.file_label.setText(f"{path.name} – {rec.duration_s / 60:.1f} min{note}")
        self._reanalyse()

    def _reanalyse(self) -> None:
        if self.rec is None or (self._thread and self._thread.isRunning()):
            return
        dlg = QProgressDialog(self)
        dlg.setLabelText("Auswertung …")
        dlg.setCancelButton(None)  # kein Abbrechen-Knopf
        dlg.setRange(0, 100)
        dlg.setWindowModality(Qt.WindowModality.WindowModal)
        dlg.setMinimumDuration(300)
        t = AnalysisThread(self.rec, self.preset.currentData())

        def on_progress(msg: str, frac: float) -> None:
            dlg.setLabelText(msg)
            dlg.setValue(int(frac * 100))

        def on_done(an: Analysis) -> None:
            dlg.close()
            self._show(an)

        def on_failed(err: str) -> None:
            dlg.close()
            QMessageBox.critical(self, "Fehler", err)

        t.progress.connect(on_progress)
        t.done.connect(on_done)
        t.failed.connect(on_failed)
        self._thread = t
        t.start()

    def wait_for_analysis(self, timeout_ms: int = 60000) -> None:
        if self._thread:
            self._thread.wait(timeout_ms)

    def _show(self, an: Analysis) -> None:
        self.an = an
        rec = self.rec
        assert rec is not None
        fs = an.fs
        t = np.arange(len(an.filtered_mv)) / fs
        self.curve.setData(t, an.filtered_mv, connect="finite")
        r = an.r_index
        if len(r):
            ymax = np.nanmax(an.filtered_mv) if np.isfinite(an.filtered_mv).any() else 1.0
            self.beat_pts.setData(r / fs, np.full(len(r), min(ymax, 3.0) + 0.3))
        else:
            self.beat_pts.clear()
        for item in getattr(self, "_marker_items", []):
            self.plot.removeItem(item)
        self._marker_items = []
        for idx, text in rec.markers:
            line = pg.InfiniteLine(
                idx / fs,
                angle=90,
                pen=pg.mkPen("#6a1b9a", style=Qt.PenStyle.DashLine),
                label=text,
                labelOpts={"position": 0.95, "color": "#6a1b9a"},
            )
            self.plot.addItem(line)
            self._marker_items.append(line)
        self.ov_curve.setData(an.hr_t, an.hr_bpm)
        self.ov_smooth.setData(an.hr_t, an.hr_smooth)
        self.overview.setXRange(0, rec.duration_s, padding=0.01)
        self._set_enabled(True)
        self._fill_summary()
        self._show_median()
        if self.paper_btn.isChecked():
            self._apply_paper_scale()
        else:
            self.plot.setXRange(0, min(10.0, rec.duration_s), padding=0)
            self.plot.setYRange(-1.5, 2.0, padding=0)

    def _fill_summary(self) -> None:
        an, rec = self.an, self.rec
        assert an and rec
        st = rec.start_time
        conv = rec.conversion
        rows = [
            ("Beginn", st.strftime("%d.%m.%Y %H:%M:%S") if st else "–"),
            ("Dauer", f"{rec.duration_s / 60:.1f} min"),
            (
                "Verstärkung",
                f"{conv.gain:.0f} ({'gemessen' if conv.gain_calibrated else 'nominal'})",
            ),
            ("Lücken / ohne Kontakt", f"{an.gap_s:.1f} s / {an.lead_off_s:.1f} s"),
        ]
        if rec.test_signal:
            rows.insert(0, ("Quelle", "<span style='color:#b71c1c'>Testsignal</span>"))
        seg = segment_rows(an.segments)
        if seg:
            seg = [("<i>Kurzzeit-HRV</i>", "<i>Median (Spanne) je 5 min</i>")] + seg
        self.summary.set_rows(rows + hrv_rows(an.hrv, an.breathing_hz) + seg)
        hints = an.hints
        self.hints.setText(
            "<b>Hinweise</b> (keine Diagnose):<br>" + "<br>".join(f"• {h}" for h in hints)
            if hints
            else ""
        )

    def _show_median(self) -> None:
        self.beat_plot.clear()
        an = self.an
        if an is None or an.median is None:
            self.intervals.set_rows([("Median-Schlag", "zu wenige gleichförmige Schläge")])
            return
        mb, d = an.median, an.delineation
        base = d.baseline_mv if d else 0.0
        self.beat_plot.addItem(EcgGridItem(self.theme))
        y = mb.template - base
        self.beat_plot.plot(mb.t_ms / 1000.0, y, pen=pg.mkPen(self.theme.trace, width=2))
        self.beat_plot.setXRange(mb.t_ms[0] / 1000.0, mb.t_ms[-1] / 1000.0, padding=0)
        self.beat_plot.setYRange(
            min(-0.5, float(np.min(y)) - 0.2), float(np.max(y)) + 0.3, padding=0
        )
        if d:
            for k, (idx, name) in enumerate(
                (
                    (d.p_onset, "P"),
                    (d.qrs_onset, "Q"),
                    (d.qrs_offset, "J"),
                    (d.t_end, "T-Ende"),
                )
            ):
                if idx is not None:
                    x = (idx - mb.r_offset) / mb.fs
                    self.beat_plot.addItem(
                        pg.InfiniteLine(
                            x,
                            angle=90,
                            pen=pg.mkPen("#1565c0", width=1),
                            label=name,
                            labelOpts={"position": 0.95 if k % 2 == 0 else 0.82},
                        )
                    )
            self.intervals.set_rows(
                [
                    ("PR", _fmt(d.pr_ms, "ms")),
                    ("QRS", _fmt(d.qrs_ms, "ms")),
                    ("QT", _fmt(d.qt_ms, "ms")),
                    ("QTc Bazett", _fmt(d.qtc_bazett_ms, "ms")),
                    ("QTc Fridericia", _fmt(d.qtc_fridericia_ms, "ms")),
                    ("R / T", f"{d.r_amp_mv:.2f} / {_fmt(d.t_amp_mv, 'mV', 2)}"),
                    ("Schläge im Median", str(mb.n_beats)),
                ]
            )

    # ------------------------------------------------------------ Navigation
    def _on_range(self, *_):
        if self.an is None:
            return
        (x0, x1), _ = self.plot.getViewBox().viewRange()
        self.region.blockSignals(True)
        self.region.setRegion((x0, x1))
        self.region.blockSignals(False)
        h = self.an.hrv_for(x0, x1)
        if h is None:
            self.window_panel.set_rows(
                [("Ausschnitt", f"{x0:.1f}–{x1:.1f} s"), ("HRV", "zu wenige Schläge")]
            )
        else:
            self.window_panel.set_rows(
                [
                    ("Ausschnitt", f"{x0:.1f}–{x1:.1f} s"),
                    ("HF Ø", f"{h.mean_hr:.0f} /min"),
                    ("SDNN / RMSSD", f"{h.sdnn_ms:.0f} / {h.rmssd_ms:.0f} ms"),
                ]
            )
        self._update_calipers()

    def _on_region(self) -> None:
        x0, x1 = self.region.getRegion()
        if self.paper_btn.isChecked():
            (v0, v1), _ = self.plot.getViewBox().viewRange()
            w = v1 - v0
            self.plot.setXRange(x0, x0 + w, padding=0)
        else:
            self.plot.setXRange(x0, x1, padding=0)

    def _apply_paper_scale(self, *_):
        vb = self.plot.getViewBox()
        if not self.paper_btn.isChecked():
            vb.setLimits(minXRange=None, maxXRange=None, minYRange=None, maxYRange=None)
            vb.setMouseEnabled(x=True, y=True)
            return
        px_mm = self.settings.px_per_mm or self.logicalDpiX() / 25.4
        w_px, h_px = vb.width(), vb.height()
        if w_px <= 0:
            return
        w_s = w_px / (px_mm * 25.0)
        h_mv = h_px / (px_mm * 10.0)
        (x0, _x1), _ = vb.viewRange()
        vb.setLimits(minXRange=w_s, maxXRange=w_s, minYRange=h_mv, maxYRange=h_mv)
        vb.setMouseEnabled(x=True, y=False)
        vb.setRange(xRange=(x0, x0 + w_s), yRange=(-h_mv * 0.4, h_mv * 0.6), padding=0)

    # ------------------------------------------------------------ Messschieber
    def _toggle_calipers(self, on: bool) -> None:
        for item in self._calipers:
            self.plot.removeItem(item)
        self._calipers = []
        if on:
            self.plot.addItem(self.caliper_text)
            (x0, x1), (y0, y1) = self.plot.getViewBox().viewRange()
            pen = pg.mkPen("#1565c0", width=1.5)
            xs = (x0 + 0.4 * (x1 - x0), x0 + 0.6 * (x1 - x0))
            ys = (y0 + 0.3 * (y1 - y0), y0 + 0.7 * (y1 - y0))
            for x in xs:
                self._calipers.append(pg.InfiniteLine(x, angle=90, movable=True, pen=pen))
            for y in ys:
                self._calipers.append(pg.InfiniteLine(y, angle=0, movable=True, pen=pen))
            for c in self._calipers:
                c.sigPositionChanged.connect(self._update_calipers)
                self.plot.addItem(c)
            self._update_calipers()
        else:
            self.plot.removeItem(self.caliper_text)

    def _update_calipers(self, *_):
        if len(self._calipers) != 4:
            return
        xa, xb, ya, yb = (c.value() for c in self._calipers)
        dt = abs(xb - xa) * 1000.0
        bpm = 60000.0 / dt if dt > 0 else float("nan")
        dmv = abs(yb - ya)
        self.caliper_text.setText(f"Δt {dt:.0f} ms ({bpm:.0f}/min)   ΔU {dmv:.2f} mV")
        _, (_, y1) = self.plot.getViewBox().viewRange()
        self.caliper_text.setPos(min(xa, xb), y1)

    # ------------------------------------------------------------ Export
    def _save_path(self, title: str, suffix: str, flt: str) -> Path | None:
        assert self.rec and self.rec.path
        default = self.rec.path.with_suffix(suffix)
        path, _ = QFileDialog.getSaveFileName(self, title, str(default), flt)
        return Path(path) if path else None

    def export_pdf(self) -> None:
        from ..report.pdf import write_report

        p = self._save_path("PDF-Bericht speichern", ".pdf", "PDF (*.pdf)")
        if p and self.rec and self.an:
            (x0, _), _ = self.plot.getViewBox().viewRange()
            write_report(self.rec, self.an, p, start_s=max(0.0, x0))
            QMessageBox.information(self, "Gespeichert", f"Bericht gespeichert:\n{p}")

    def export_edf(self) -> None:
        p = self._save_path("EDF+ speichern", ".edf", "EDF+ (*.edf)")
        if p and self.rec and self.an:
            export.export_edf(self.rec, p, self.an.r_index)
            QMessageBox.information(self, "Gespeichert", f"EDF+ gespeichert:\n{p}")

    def export_csv(self) -> None:
        p = self._save_path("CSV speichern", ".csv", "CSV (*.csv)")
        if p and self.rec:
            export.export_csv(self.rec, p)
            QMessageBox.information(self, "Gespeichert", f"CSV gespeichert:\n{p}")

    def export_wfdb(self) -> None:
        if not (self.rec and self.an and self.rec.path):
            return
        d = QFileDialog.getExistingDirectory(self, "Zielordner für WFDB", str(self.rec.path.parent))
        if not d:
            return
        try:
            hea = export.export_wfdb(self.rec, Path(d), self.rec.path.stem, self.an.r_index)
        except ImportError:
            QMessageBox.warning(
                self, "wfdb fehlt", "Bitte das Paket 'wfdb' installieren (pip install wfdb)."
            )
            return
        QMessageBox.information(self, "Gespeichert", f"WFDB gespeichert:\n{hea}")
