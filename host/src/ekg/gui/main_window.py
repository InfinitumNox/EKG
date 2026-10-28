"""Hauptfenster."""

from __future__ import annotations

import logging
import subprocess
import time
from collections.abc import Callable
from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QActionGroup, QKeySequence
from PySide6.QtWidgets import (
    QComboBox,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QMainWindow,
    QMessageBox,
    QTabWidget,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from .. import __version__
from .. import protocol as P
from ..config import Settings, recordings_dir
from ..device.simulator import SCENARIO_LABELS, Scenario, SimulatedDevice
from ..device.transport import SerialTransport, find_ports
from ..device.worker import DeviceWorker, Transport
from ..dsp.filters import PRESETS
from ..dsp.quality import Level
from ..pipeline import LivePipeline
from ..power import PowerState, power_status
from ..storage.recording import default_filename
from ..units import Conversion
from .dialogs import GainCalibrationDialog, SafetyDialog, ScreenCalibrationDialog
from .live_view import EcgSweepView
from .review import ReviewWidget
from .theme import THEMES
from .trends import TrendsWidget
from .widgets import BannerArea, HeartRateDisplay, InfoPanel, QualityPanel

log = logging.getLogger(__name__)


class MainWindow(QMainWindow):
    def __init__(self, settings: Settings):
        super().__init__()
        self.settings = settings
        self.setWindowTitle("EKG")
        self.resize(1400, 860)
        self.pipeline = LivePipeline(
            settings.sample_rate_hz, filter_settings=self._preset_settings()
        )
        self.worker: DeviceWorker | None = None
        self.source_desc = ""
        self.is_simulator = False
        self.last_recording: Path | None = None
        self._rec_started = 0.0
        self._last_beat_index = -1.0
        self._device_warning_until = 0.0
        self._power = power_status()
        self._build_ui()
        self._apply_scale()
        self._apply_theme(settings.theme)
        self.refresh_ports()

        self._ui_timer = QTimer(self)
        self._ui_timer.timeout.connect(self._tick)
        self._ui_timer.start(200)
        self._power_timer = QTimer(self)
        self._power_timer.timeout.connect(self._check_power)
        self._power_timer.start(5000)

    # ================================================================ Aufbau
    def _preset_settings(self):
        key = self.settings.filter_preset if self.settings.filter_preset in PRESETS else "monitor"
        return PRESETS[key][1]

    def _build_ui(self) -> None:
        tb = QToolBar("Steuerung")
        tb.setMovable(False)
        self.addToolBar(tb)
        self.source = QComboBox()
        self.source.setMinimumWidth(260)
        tb.addWidget(QLabel(" Quelle: "))
        tb.addWidget(self.source)
        self.act_refresh = tb.addAction("↻", self.refresh_ports)
        self.act_refresh.setToolTip("Geräte neu suchen")
        self.act_connect = tb.addAction("Verbinden", self.toggle_connection)
        self.act_connect.setCheckable(True)
        tb.addSeparator()
        self.act_record = tb.addAction("● Aufnahme", self.toggle_recording)
        self.act_record.setCheckable(True)
        self.act_record.setShortcut(QKeySequence("R"))
        self.act_marker = tb.addAction("Marker", self.add_marker)
        self.act_marker.setShortcut(QKeySequence("M"))
        self.act_freeze = tb.addAction("Einfrieren", self.toggle_freeze)
        self.act_freeze.setCheckable(True)
        self.act_freeze.setShortcut(QKeySequence(Qt.Key.Key_Space))
        self.act_analyse = tb.addAction("Auswerten", self.analyse_last)
        self.act_analyse.setEnabled(False)
        tb.addSeparator()
        tb.addWidget(QLabel(" Filter: "))
        self.preset = QComboBox()
        for key, (label, s) in PRESETS.items():
            self.preset.addItem(f"{label} ({s.describe()})", key)
        self.preset.setCurrentIndex(max(0, self.preset.findData(self.settings.filter_preset)))
        self.preset.currentIndexChanged.connect(self._preset_changed)
        tb.addWidget(self.preset)
        tb.addWidget(QLabel("  "))
        self.speed = QComboBox()
        for v in (12.5, 25.0, 50.0):
            self.speed.addItem(f"{v:g} mm/s", v)
        self.speed.setCurrentIndex(max(0, self.speed.findData(self.settings.speed_mm_s)))
        self.speed.currentIndexChanged.connect(self._apply_scale)
        tb.addWidget(self.speed)
        self.gain = QComboBox()
        for v in (5.0, 10.0, 20.0):
            self.gain.addItem(f"{v:g} mm/mV", v)
        self.gain.setCurrentIndex(max(0, self.gain.findData(self.settings.gain_mm_mv)))
        self.gain.currentIndexChanged.connect(self._apply_scale)
        tb.addWidget(self.gain)

        self.tabs = QTabWidget()
        live = QWidget()
        lv = QVBoxLayout(live)
        lv.setContentsMargins(4, 4, 4, 4)
        self.banners = BannerArea()
        lv.addWidget(self.banners)
        row = QHBoxLayout()
        self.view = EcgSweepView()
        self.view.set_pipeline(None)
        row.addWidget(self.view, 1)
        side = QVBoxLayout()
        self.hr = HeartRateDisplay()
        self.quality = QualityPanel()
        self.device_panel = InfoPanel("Gerät")
        self.rec_panel = InfoPanel("Aufnahme")
        for w in (self.hr, self.quality, self.device_panel, self.rec_panel):
            w.setFixedWidth(280)
            side.addWidget(w)
        side.addStretch(1)
        row.addLayout(side)
        lv.addLayout(row, 1)
        self.tabs.addTab(live, "Live")
        self.trends = TrendsWidget()
        self.tabs.addTab(self.trends, "Trends")
        self.review = ReviewWidget(self.settings)
        self.tabs.addTab(self.review, "Auswertung")
        self.setCentralWidget(self.tabs)

        self.status_label = QLabel("")
        self.statusBar().addPermanentWidget(self.status_label)
        self._build_menu()
        self._update_panels()

    def _build_menu(self) -> None:
        m = self.menuBar().addMenu("&Datei")
        a = m.addAction("Aufzeichnung öffnen …", self._open_recording)
        a.setShortcut(QKeySequence.StandardKey.Open)
        m.addAction("Aufnahmeordner öffnen", self._open_recordings_dir)
        m.addSeparator()
        m.addAction("Beenden", self.close).setShortcut(QKeySequence.StandardKey.Quit)

        d = self.menuBar().addMenu("&Gerät")
        self.act_test = d.addAction("Testsignal der Firmware")
        self.act_test.setCheckable(True)
        self.act_test.toggled.connect(self._toggle_test_mode)
        rate = d.addMenu("Abtastrate")
        grp = QActionGroup(self)
        for hz in (500, 1000):
            act = rate.addAction(f"{hz} Hz")
            act.setCheckable(True)
            act.setChecked(hz == self.settings.sample_rate_hz)
            act.triggered.connect(lambda _=False, h=hz: self._set_rate(h))
            grp.addAction(act)
        d.addSeparator()
        self.act_calibrate = d.addAction(
            "Verstärkung/Frequenzgang kalibrieren …", self.calibrate_gain
        )
        self.act_bootsel = d.addAction("Bootloader starten (Firmware flashen)", self._bootsel)

        v = self.menuBar().addMenu("&Ansicht")
        v.addAction("Bildschirm kalibrieren …", self.calibrate_screen)
        self.act_monitor = v.addAction("Monitor-Farben (dunkel)")
        self.act_monitor.setCheckable(True)
        self.act_monitor.setChecked(self.settings.theme == "monitor")
        self.act_monitor.toggled.connect(lambda on: self._apply_theme("monitor" if on else "paper"))
        full = v.addAction(
            "Vollbild", lambda: self.showNormal() if self.isFullScreen() else self.showFullScreen()
        )
        full.setShortcut(QKeySequence(Qt.Key.Key_F11))

        h = self.menuBar().addMenu("&Hilfe")
        h.addAction("Sicherheitshinweise", self._show_safety)
        h.addAction("Über", self._about)

    # ================================================================ Darstellung
    def _px_per_mm(self) -> float:
        if self.settings.px_per_mm:
            return self.settings.px_per_mm
        screen = self.screen()
        dpi = screen.physicalDotsPerInch() if screen else 96.0
        # Viele Linux-Setups melden Unsinn (z. B. 0 oder >400); dann 96 dpi annehmen.
        if not 50 <= dpi <= 400:
            dpi = 96.0
        return dpi / 25.4

    def _apply_scale(self) -> None:
        self.settings.speed_mm_s = float(self.speed.currentData())
        self.settings.gain_mm_mv = float(self.gain.currentData())
        self.view.set_scale(self._px_per_mm(), self.settings.speed_mm_s, self.settings.gain_mm_mv)

    def _apply_theme(self, name: str) -> None:
        theme = THEMES.get(name, THEMES["paper"])
        self.settings.theme = theme.name
        self.view.set_theme(theme)
        self.review.set_theme(theme)

    def _preset_changed(self) -> None:
        key = self.preset.currentData()
        self.settings.filter_preset = key
        self.pipeline.set_filter(PRESETS[key][1])

    # ================================================================ Gerät
    def refresh_ports(self) -> None:
        current = self.source.currentData()
        self.source.clear()
        for port in find_ports():
            self.source.addItem(port.description, ("serial", port.device))
        for sc in Scenario:
            self.source.addItem(f"Simulator – {SCENARIO_LABELS[sc]}", ("sim", sc.value))
        idx = self.source.findData(current) if current else -1
        if idx < 0 and self.settings.last_port:
            idx = self.source.findData(("serial", self.settings.last_port))
        self.source.setCurrentIndex(max(0, idx))

    def conversion_for(self, info: P.InfoPacket) -> Conversion:
        cal = self.settings.devices.get(info.board_id)
        return Conversion(
            oversampling=info.oversampling,
            adc_bits=info.adc_bits,
            vref_v=info.vref_mv / 1000.0,
            gain=cal.gain if cal else float(info.gain_nominal),
            gain_calibrated=cal is not None,
        )

    def toggle_connection(self) -> None:
        if self.worker:
            self.disconnect_device()
        else:
            self.connect_device()

    def connect_device(self, data: tuple[str, str] | None = None) -> bool:
        kind, value = data or self.source.currentData() or ("sim", "normal")
        mode = P.Mode.TEST if self.act_test.isChecked() else P.Mode.LIVE
        if kind == "serial" and mode == P.Mode.LIVE and not self.settings.safety_ack_session:
            if SafetyDialog(self).exec() != SafetyDialog.DialogCode.Accepted:
                self.act_connect.setChecked(False)
                return False
            self.settings.safety_ack_session = True
        opener: Callable[[], Transport]
        if kind == "serial":
            self.settings.last_port = value
            opener = lambda: SerialTransport(value)  # noqa: E731
        else:
            opener = lambda: SimulatedDevice(Scenario(value))  # noqa: E731
        self.is_simulator = kind == "sim"
        self.source_desc = self.source.currentText()
        self.pipeline = LivePipeline(
            self.settings.sample_rate_hz, filter_settings=self._preset_settings()
        )
        self.view.set_pipeline(self.pipeline)
        self.view.placeholder = "Verbinde …"
        self.trends.set_pipeline(self.pipeline)
        w = DeviceWorker(
            opener, self.pipeline, self.conversion_for, self.settings.sample_rate_hz, mode, self
        )
        w.connected.connect(lambda d: self.statusBar().showMessage(f"Verbunden: {d}", 5000))
        w.disconnected.connect(self._on_disconnected)
        w.info_received.connect(lambda _i: self._update_panels())
        w.warning.connect(self._on_device_warning)
        self.worker = w
        w.start()
        self.act_connect.setText("Trennen")
        self.act_connect.setChecked(True)
        self.source.setEnabled(False)
        return True

    def disconnect_device(self) -> None:
        if self.act_record.isChecked():
            self.toggle_recording()
        if self.worker:
            self.worker.stop()

    def _on_disconnected(self, reason: str) -> None:
        if self.pipeline.recorder:
            self._stop_recording()
        self.worker = None
        self.view.placeholder = "Kein Gerät verbunden"
        self.act_connect.setText("Verbinden")
        self.act_connect.setChecked(False)
        self.source.setEnabled(True)
        self.statusBar().showMessage(reason, 8000)
        if reason.startswith(("Verbindung fehlgeschlagen", "Verbindung verloren")):
            self.banners.show_banner("conn", reason, "danger")
            QTimer.singleShot(10000, lambda: self.banners.hide_banner("conn"))
        self._update_panels()

    def _on_device_warning(self, text: str) -> None:
        self.banners.show_banner("device", text, "warning")
        self._device_warning_until = time.monotonic() + 10

    def _toggle_test_mode(self, on: bool) -> None:
        if self.worker:
            self.worker.send_command(P.Opcode.SET_MODE, bytes([1 if on else 0]))

    def _set_rate(self, hz: int) -> None:
        self.settings.sample_rate_hz = hz
        if self.worker:
            QMessageBox.information(
                self, "Abtastrate", "Die neue Abtastrate gilt ab der nächsten Verbindung."
            )

    def _bootsel(self) -> None:
        if not self.worker or self.is_simulator:
            QMessageBox.information(self, "Bootloader", "Erst mit dem Pico verbinden.")
            return
        if (
            QMessageBox.question(
                self,
                "Bootloader",
                "Pico in den USB-Bootloader schalten?\n"
                "Danach erscheint ein Laufwerk RP2350, dorthin ekg.uf2 kopieren.",
            )
            == QMessageBox.StandardButton.Yes
        ):
            self.worker.send_command(P.Opcode.BOOTSEL)

    # ================================================================ Aufnahme
    def toggle_recording(self) -> None:
        if self.pipeline.recorder:
            self._stop_recording()
        elif not self.worker:
            self.act_record.setChecked(False)
            QMessageBox.information(
                self, "Aufnahme", "Erst mit einem Gerät oder dem Simulator verbinden."
            )
        else:
            path = recordings_dir() / default_filename()
            self.pipeline.start_recording(
                path, {"source": self.source_desc, "simulator": self.is_simulator}
            )
            self._rec_started = time.monotonic()
            self.act_record.setText("■ Stopp")
            self.act_record.setChecked(True)

    def _stop_recording(self) -> None:
        path = self.pipeline.stop_recording()
        self.act_record.setText("● Aufnahme")
        self.act_record.setChecked(False)
        if path:
            self.last_recording = path
            self.act_analyse.setEnabled(True)
            self.banners.show_banner("saved", f"Aufnahme gespeichert: {path}", "info")
            QTimer.singleShot(8000, lambda: self.banners.hide_banner("saved"))

    def analyse_last(self) -> None:
        if self.last_recording:
            self.tabs.setCurrentWidget(self.review)
            self.review.load(self.last_recording)

    def add_marker(self) -> None:
        if not self.worker:
            return
        text, ok = QInputDialog.getText(
            self, "Marker", "Beschreibung (z. B. „Treppe gestiegen“):", text="Marker"
        )
        if ok and text:
            self.pipeline.add_marker(text)

    def toggle_freeze(self) -> None:
        self.view.set_frozen(not self.view.frozen)
        self.act_freeze.setChecked(self.view.frozen)

    # ================================================================ Kalibrierung
    def calibrate_screen(self) -> None:
        dlg = ScreenCalibrationDialog(self._px_per_mm(), self)
        if dlg.exec() == ScreenCalibrationDialog.DialogCode.Accepted:
            self.settings.px_per_mm = dlg.px_per_mm
            self.settings.save()
            self._apply_scale()

    def calibrate_gain(self) -> None:
        if not self.worker or self.worker.info is None:
            QMessageBox.information(
                self, "Kalibrierung", "Erst mit dem Pico verbinden (Modus Live)."
            )
            return
        dlg = GainCalibrationDialog(self.worker, self.pipeline, self.settings, self)
        if dlg.exec() == GainCalibrationDialog.DialogCode.Accepted:
            self.pipeline.set_conversion(self.conversion_for(self.worker.info))
            self._update_panels()

    # ================================================================ Laufende Anzeige
    def _check_power(self) -> None:
        self._power = power_status()

    def _tick(self) -> None:
        pl = self.pipeline
        connected = self.worker is not None
        bpm = pl.heart_rate() if connected else None
        self.hr.set_values(bpm, pl.last_rr_ms() if connected else None)
        if pl.beats:
            last = pl.beats[-1].index
            if last != self._last_beat_index:
                self._last_beat_index = last
                self.hr.pulse()
        if connected:
            self.quality.set_report(pl.quality)
        self._update_banners()
        self._update_panels()

    def _update_banners(self) -> None:
        b = self.banners
        pl = self.pipeline
        connected = self.worker is not None
        live_human = connected and not self.is_simulator and not pl.test_signal
        if live_human and self._power.state == PowerState.EXTERNAL:
            b.show_banner(
                "power",
                "⚠ Laptop hängt am Netzteil – Elektroden abnehmen oder Ladekabel ziehen!",
                "danger",
            )
        elif live_human and self._power.state == PowerState.NO_BATTERY:
            b.show_banner(
                "power",
                "⚠ Kein Akku erkannt (Desktop-PC?) – nur mit USB-Isolator messen!",
                "danger",
            )
        else:
            b.hide_banner("power")
        if connected and pl.quality.level == Level.NO_CONTACT:
            b.show_banner("lead", "Elektrode ohne Kontakt – Sitz und Gel prüfen", "danger")
        else:
            b.hide_banner("lead")
        if connected and self.is_simulator:
            b.show_banner("sim", "Simulator – keine echten Messdaten", "info")
        elif connected and pl.test_signal:
            b.show_banner("sim", "Testsignal der Firmware – kein Mensch angeschlossen", "info")
        else:
            b.hide_banner("sim")
        if connected and not self.is_simulator and not pl.conversion.gain_calibrated and pl.info:
            b.show_banner(
                "gain",
                "Verstärkung nicht kalibriert (nominal 1100) – mV-Werte sind Näherungen. "
                "Gerät → Kalibrieren",
                "warning",
            )
        else:
            b.hide_banner("gain")
        if connected and pl.cal_active:
            b.show_banner("cal", "Kalibriersignal aktiv – kein Mensch am Gerät!", "warning")
        else:
            b.hide_banner("cal")
        lost = pl.assembler.stats.lost_samples
        if connected and lost:
            b.show_banner(
                "lost",
                f"{lost} Samples verloren ({pl.assembler.stats.gaps} Lücken) – USB prüfen",
                "warning",
            )
        else:
            b.hide_banner("lost")
        if "device" in b.keys() and time.monotonic() > self._device_warning_until:
            b.hide_banner("device")

    def _update_panels(self) -> None:
        pl = self.pipeline
        w = self.worker
        info = w.info if w else None
        if info:
            cal = self.settings.devices.get(info.board_id)
            self.device_panel.set_rows(
                [
                    ("Gerät", info.name),
                    ("Firmware", info.fw_version_str),
                    ("Abtastrate", f"{info.sample_rate_hz} Hz ({info.oversampling}× Oversampling)"),
                    (
                        "Verstärkung",
                        f"{pl.conversion.gain:.0f} " + ("(kalibriert)" if cal else "(nominal)"),
                    ),
                    ("Board-ID", info.board_id),
                ]
            )
        else:
            self.device_panel.set_rows([("Status", "nicht verbunden")])
        if pl.recorder:
            dur = time.monotonic() - self._rec_started
            size = pl.recorder.samples_written * 4 / 1e6
            self.rec_panel.set_rows(
                [
                    ("Läuft", f"{int(dur // 60)}:{int(dur % 60):02d}"),
                    ("Daten", f"{size:.1f} MB"),
                    ("Datei", pl.recorder.path.name),
                ]
            )
        else:
            self.rec_panel.set_rows([("Status", "keine Aufnahme")])
        st = pl.assembler.stats
        dec = w.decoder if w else None
        self.status_label.setText(
            f"Pakete {st.packets} · verloren {st.lost_samples} · Frame-Fehler {dec.errors if dec else 0} · "
            f"Filter: {pl.filter_settings.describe()}"
        )

    # ================================================================ Sonstiges
    def _open_recording(self) -> None:
        self.tabs.setCurrentWidget(self.review)
        self.review.open_dialog()

    def _open_recordings_dir(self) -> None:
        d = recordings_dir()
        d.mkdir(parents=True, exist_ok=True)
        subprocess.Popen(["xdg-open", str(d)])  # noqa: S603, S607

    def _show_safety(self) -> None:
        QMessageBox.information(
            self,
            "Sicherheit",
            "• Elektroden nur am Körper, wenn das Ladekabel des Laptops gezogen ist.\n"
            "• Nichts mit Netzanschluss am Laptop (Monitor, Dock, Netzwerkkabel).\n"
            "• Flashen/Entwickeln mit Ladekabel nur ohne Elektroden am Körper.\n"
            "• Kalibriersignal nur ohne Menschen am Gerät.\n"
            "• Kein Medizinprodukt – keine Diagnose. Bei Beschwerden zum Arzt.",
        )

    def _about(self) -> None:
        QMessageBox.about(
            self,
            "Über",
            f"<b>EKG</b> {__version__}<br>1-Kanal-EKG mit AD8232 und Raspberry Pi Pico 2.<br><br>"
            "Kein Medizinprodukt. Die Anzeigen sind nicht für Diagnosen gedacht.",
        )

    def closeEvent(self, event) -> None:  # noqa: N802
        self.disconnect_device()
        self.settings.save()
        super().closeEvent(event)
