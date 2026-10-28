"""Dialoge: Sicherheitscheck, Bildschirm-Kalibrierung, Verstärkungs-/Frequenzgang-Kalibrierung."""

from __future__ import annotations

from datetime import datetime

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import QPointF, Qt, QTimer
from PySide6.QtGui import QPainter, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QProgressBar,
    QPushButton,
    QRadioButton,
    QVBoxLayout,
    QWidget,
)

from .. import calibration as cal
from .. import protocol as P
from ..config import DeviceCalibration, Settings
from ..power import PowerState, power_status


class SafetyDialog(QDialog):
    """Vor der ersten Live-Messung einer Sitzung."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Sicherheitscheck")
        lay = QVBoxLayout(self)
        st = power_status()
        if st.state == PowerState.EXTERNAL:
            msg = f"<b style='color:#b71c1c'>Der Laptop hängt am Netzteil ({st.detail}).</b> Erst abziehen!"
        elif st.state == PowerState.NO_BATTERY:
            msg = (
                "<b style='color:#b71c1c'>Kein Akku gefunden – vermutlich ein netzbetriebener PC.</b> "
                "Elektroden nur mit USB-Isolator (ADuM3160/4160) verwenden."
            )
        elif st.state == PowerState.UNKNOWN:
            msg = "Stromversorgung unbekannt – bitte selbst prüfen."
        else:
            msg = "<b style='color:#2e7d32'>Akkubetrieb erkannt.</b>"
        info = QLabel(
            "<h3>Elektroden am Körper – bitte bestätigen</h3>"
            f"<p>{msg}</p>"
            "<p>Der AD8232 hat keine galvanische Trennung. Ein netzbetriebenes Gerät am Laptop "
            "verbindet den Brustkorb mit dem Stromnetz.</p>"
        )
        info.setWordWrap(True)
        lay.addWidget(info)
        self.checks = [
            QCheckBox("Das Ladekabel des Laptops ist abgezogen."),
            QCheckBox(
                "Am Laptop hängt nichts mit Netzanschluss (Monitor, Dock, Netzwerkkabel, USB-Hub mit Netzteil)."
            ),
            QCheckBox("Mir ist klar: Das ist kein Medizinprodukt und liefert keine Diagnose."),
        ]
        for c in self.checks:
            lay.addWidget(c)
            c.toggled.connect(self._update)
        self.buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        self.buttons.accepted.connect(self.accept)
        self.buttons.rejected.connect(self.reject)
        lay.addWidget(self.buttons)
        self._update()

    def _update(self) -> None:
        self.buttons.button(QDialogButtonBox.StandardButton.Ok).setEnabled(
            all(c.isChecked() for c in self.checks)
        )


class _Ruler(QWidget):
    def __init__(self, px_per_mm: float):
        super().__init__()
        self.px_per_mm = px_per_mm
        self.setMinimumHeight(60)

    def paintEvent(self, event) -> None:  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        pen = QPen(Qt.GlobalColor.black, 2)
        p.setPen(pen)
        x0, y = 10.0, 30.0
        length = 100 * self.px_per_mm
        p.drawLine(QPointF(x0, y), QPointF(x0 + length, y))
        for k in range(11):
            h = 12 if k % 5 == 0 else 6
            x = x0 + k * 10 * self.px_per_mm
            p.drawLine(QPointF(x, y - h), QPointF(x, y + h))
        p.end()


class ScreenCalibrationDialog(QDialog):
    """Damit 25 mm/s und 10 mm/mV am Bildschirm wirklich stimmen."""

    def __init__(self, px_per_mm: float, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Bildschirm kalibrieren")
        self.px_per_mm = px_per_mm
        lay = QVBoxLayout(self)
        lay.addWidget(QLabel("Miss die Linie mit einem Lineal. Sie soll 100 mm lang sein."))
        self.ruler = _Ruler(px_per_mm)
        self.ruler.setMinimumWidth(int(100 * px_per_mm) + 30)
        lay.addWidget(self.ruler)
        form = QFormLayout()
        self.measured = QDoubleSpinBox()
        self.measured.setRange(20, 300)
        self.measured.setDecimals(1)
        self.measured.setSuffix(" mm")
        self.measured.setValue(100.0)
        form.addRow("Gemessene Länge:", self.measured)
        lay.addLayout(form)
        apply_btn = QPushButton("Anwenden")
        apply_btn.clicked.connect(self._apply)
        lay.addWidget(apply_btn)
        bb = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        bb.accepted.connect(self._apply_and_close)
        bb.rejected.connect(self.reject)
        lay.addWidget(bb)

    def _apply_and_close(self) -> None:
        self._apply()
        self.accept()

    def _apply(self) -> None:
        self.px_per_mm *= 100.0 / self.measured.value()
        self.ruler.px_per_mm = self.px_per_mm
        self.ruler.setMinimumWidth(int(100 * self.px_per_mm) + 30)
        self.measured.setValue(100.0)
        self.ruler.update()
        self.adjustSize()


class GainCalibrationDialog(QDialog):
    """Führt die Loopback-Messung durch (docs/KALIBRIERUNG.md)."""

    def __init__(self, worker, pipeline, settings: Settings, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Verstärkung und Frequenzgang kalibrieren")
        self.worker = worker
        self.pipeline = pipeline
        self.settings = settings
        self.cal_result: cal.CalibrationResult | None = None
        self._amps: dict[float, float] = {}
        self._steps: list[float] = []
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self.setMinimumWidth(620)

        lay = QVBoxLayout(self)
        warn = QLabel(
            "<b style='color:#b71c1c'>Kein Mensch am Gerät!</b> Elektrodenkabel abziehen und die "
            "Loopback-Schaltung nach docs/KALIBRIERUNG.md anschließen: GP15 → 2 × (1 kΩ / 100 nF) → "
            "100 kΩ → LA, 100 Ω zwischen LA und RA, RA und RL an die Mitte von 2 × 1 kΩ (3V3/GND). "
            "Längswiderstand = Summe aller Widerstände im Stromweg außer dem 100-Ω-Shunt."
        )
        warn.setWordWrap(True)
        lay.addWidget(warn)
        form = QFormLayout()
        self.r_series = QDoubleSpinBox(decimals=1, maximum=1e7, suffix=" Ω")
        self.r_series.setValue(102_500)  # 100 k + 2 × 1 k (RC) + 0,5 k (Mittenteiler)
        self.r_shunt = QDoubleSpinBox(decimals=2, maximum=1e5, suffix=" Ω")
        self.r_shunt.setValue(100.0)
        self.v_rail = QDoubleSpinBox(decimals=3, minimum=3.0, maximum=3.6, suffix=" V")
        self.v_rail.setValue(3.300)
        form.addRow("Längswiderstand gesamt:", self.r_series)
        form.addRow("Querwiderstand (gemessen):", self.r_shunt)
        form.addRow("3V3-Schiene (gemessen):", self.v_rail)
        lay.addLayout(form)
        row = QHBoxLayout()
        self.quick = QRadioButton("Schnell (~1 min)")
        self.full = QRadioButton("Voll inkl. 0,05 Hz (~8 min)")
        self.quick.setChecked(True)
        row.addWidget(self.quick)
        row.addWidget(self.full)
        lay.addLayout(row)
        self.progress = QProgressBar()
        self.status = QLabel("")
        lay.addWidget(self.progress)
        lay.addWidget(self.status)
        self.plot = pg.PlotWidget(title="Frequenzgang (relativ zu 10 Hz)")
        self.plot.setLogMode(x=True, y=False)
        self.plot.setLabel("bottom", "Frequenz", units="Hz")
        self.plot.setLabel("left", "dB")
        self.plot.showGrid(x=True, y=True, alpha=0.3)
        self.plot.addLine(y=-3, pen=pg.mkPen("#999", style=Qt.PenStyle.DashLine))
        self.curve = self.plot.plot(
            [], [], pen=pg.mkPen("#1565c0", width=2), symbol="o", symbolSize=6
        )
        self.plot.setMinimumHeight(220)
        lay.addWidget(self.plot)
        self.result_label = QLabel("")
        self.result_label.setWordWrap(True)
        lay.addWidget(self.result_label)
        bb = QDialogButtonBox()
        self.start_btn = bb.addButton("Messung starten", QDialogButtonBox.ButtonRole.ActionRole)
        self.apply_btn = bb.addButton("Übernehmen", QDialogButtonBox.ButtonRole.AcceptRole)
        bb.addButton(QDialogButtonBox.StandardButton.Close)
        self.apply_btn.setEnabled(False)
        self.start_btn.clicked.connect(self.start)
        self.apply_btn.clicked.connect(self._apply)
        bb.rejected.connect(self.reject)
        lay.addWidget(bb)

    @property
    def a_in_mv(self) -> float:
        swing = 0.5 * self.v_rail.value() / 3.3  # PWM-Hub skaliert mit der Schiene
        return cal.input_amplitude_mv(self.r_series.value(), self.r_shunt.value(), swing)

    def start(self) -> None:
        freqs = cal.DEFAULT_FREQS if self.full.isChecked() else cal.QUICK_FREQS
        # Höchste Frequenzen zuerst: kurze Einschwingzeiten, schnelle Rückmeldung.
        self._steps = sorted(freqs, reverse=True)
        self._amps = {}
        self._total = len(self._steps)
        self.start_btn.setEnabled(False)
        self.apply_btn.setEnabled(False)
        self._next_step()
        self._timer.start(200)

    def _next_step(self) -> None:
        if not self._steps:
            self._finish()
            return
        self._f = self._steps.pop(0)
        self.worker.send_command(P.Opcode.SET_CAL, P.cmd_args_set_cal(P.CalWave.SINE, self._f))
        self._phase = "settle"
        self._t0 = self.pipeline.unfiltered.end
        self.status.setText(f"{self._f:g} Hz: einschwingen …")

    def _tick(self) -> None:
        fs = self.pipeline.fs
        now = self.pipeline.unfiltered.end
        if self._phase == "settle" and now - self._t0 >= cal.settle_s(self._f) * fs:
            self._phase = "measure"
            self._t0 = now
            self.status.setText(f"{self._f:g} Hz: messen …")
        elif self._phase == "measure":
            n = int(cal.measure_s(self._f) * fs)
            if now - self._t0 >= n:
                x = self.pipeline.unfiltered.get(self._t0, self._t0 + n)
                self._amps[self._f] = cal.sine_amplitude(x, fs, self._f)
                done = self._total - len(self._steps)
                self.progress.setValue(int(100 * done / self._total))
                self._plot()
                self._next_step()

    def _plot(self) -> None:
        if cal.REFERENCE_HZ in self._amps:
            ref = self._amps[cal.REFERENCE_HZ]
            f = np.array(sorted(self._amps))
            self.curve.setData(f, [20 * np.log10(self._amps[k] / ref) for k in f])

    def _finish(self) -> None:
        self._timer.stop()
        self.worker.send_command(P.Opcode.SET_CAL, P.cmd_args_set_cal(P.CalWave.OFF, 0))
        self.start_btn.setEnabled(True)
        self.progress.setValue(100)
        try:
            self.cal_result = cal.evaluate(self._amps, self.a_in_mv, self.pipeline.conversion.gain)
        except ValueError as exc:
            self.status.setText(str(exc))
            return
        r = self.cal_result
        hp = f"{r.highpass_hz:.2f} Hz" if r.highpass_hz else "unter Messbereich"
        lp = f"{r.lowpass_hz:.1f} Hz" if r.lowpass_hz else "über Messbereich"
        self.status.setText("fertig")
        self.result_label.setText(
            f"<b>Verstärkung: {r.gain:.0f}</b> (nominal 1100, Abweichung {100 * (r.gain / 1100 - 1):+.1f} %)<br>"
            f"−3 dB: Hochpass {hp}, Tiefpass {lp}"
        )
        self.apply_btn.setEnabled(True)

    def _apply(self) -> None:
        info = self.worker.info if self.worker else None
        if self.cal_result is None or info is None:
            return
        r = self.cal_result
        self.settings.devices[info.board_id] = DeviceCalibration(
            gain=r.gain,
            measured_at=datetime.now().isoformat(timespec="seconds"),
            method=f"Loopback {self.r_series.value():.0f}/{self.r_shunt.value():.1f} Ω",
            highpass_hz=r.highpass_hz,
            lowpass_hz=r.lowpass_hz,
            response=r.response,
        )
        self.settings.save()
        self.accept()

    def reject(self) -> None:
        if self._timer.isActive():
            self._timer.stop()
            self.worker.send_command(P.Opcode.SET_CAL, P.cmd_args_set_cal(P.CalWave.OFF, 0))
        super().reject()
