"""Export, PDF-Bericht und Oberfläche (offscreen)."""

import time

import numpy as np
import pytest

from ekg import protocol as P
from ekg.analysis import analyse
from ekg.device.simulator import Scenario, SimulatedDevice
from ekg.pipeline import LivePipeline
from ekg.storage import export
from ekg.storage.recording import load_recording
from ekg.units import Conversion

from .conftest import run_simulator

SHOTS = __import__("pathlib").Path(__import__("os").environ.get("EKG_SCREENSHOTS", "/tmp"))


@pytest.fixture
def recording(tmp_path):
    dev = SimulatedDevice(Scenario.PVC, realtime=False, hr_bpm=65)
    packets, _ = run_simulator(dev, 90.0)
    pl = LivePipeline(500, Conversion(oversampling=64))
    path = tmp_path / "rec.ekgrec"
    pl.start_recording(path, {"source": "test"})
    for p in packets:
        if isinstance(p, P.SamplesPacket):
            pl.push(p)
    pl.add_marker("Treppe")
    pl.stop_recording()
    return load_recording(path)


def test_analysis(recording):
    an = analyse(recording)
    assert 85 < len(an.beats) < 105
    assert an.hrv is not None
    assert an.median is not None and an.delineation is not None
    assert any("vorzeitige" in h or "unregelmäßig" in h for h in an.hints)


def test_edf_roundtrip(recording, tmp_path):
    pyedflib = pytest.importorskip("pyedflib")  # unabhängige Referenz zum Lesen

    an = analyse(recording)
    path = export.export_edf(recording, tmp_path / "x.edf", an.r_index)
    with pyedflib.EdfReader(str(path)) as f:
        assert f.getSampleFrequency(0) == 500
        sig = f.readSignal(0)
        onsets, _, texts = f.readAnnotations()
    mv = recording.mv
    n = len(mv)
    np.testing.assert_allclose(sig[:n] / 1000.0, mv, atol=0.002)
    assert "Treppe" in texts
    assert list(texts).count("R") == len(an.beats)


def test_csv_and_wfdb(recording, tmp_path):
    export.export_csv(recording, tmp_path / "x.csv")
    lines = (tmp_path / "x.csv").read_text().splitlines()
    assert lines[0] == "zeit_s,roh,mv,flags" and len(lines) == recording.n_samples + 1
    import wfdb

    an = analyse(recording)
    export.export_wfdb(recording, tmp_path / "wfdb", "rec", an.r_index)
    r = wfdb.rdrecord(str(tmp_path / "wfdb" / "rec"))
    np.testing.assert_allclose(r.p_signal[:, 0], recording.mv, atol=2e-4)
    ann = wfdb.rdann(str(tmp_path / "wfdb" / "rec"), "qrs")
    assert len(ann.sample) == len(an.beats)


def test_pdf_report(recording, tmp_path, qapp):
    from ekg.report.pdf import write_report

    path = write_report(recording, analyse(recording), tmp_path / "r.pdf")
    data = path.read_bytes()
    assert data.startswith(b"%PDF") and len(data) > 20_000


def test_cli_export(recording, tmp_path, capsys):
    from ekg.cli import export_main

    assert export_main([str(recording.path), "--edf", str(tmp_path / "a.edf")]) == 0
    out = capsys.readouterr().out
    assert "Schläge" in out and (tmp_path / "a.edf").exists()


def test_main_window_with_simulator(tmp_path, recording, qapp):
    """Oberfläche starten, Simulator in Echtzeit verbinden, aufnehmen, auswerten."""
    import pyqtgraph as pg

    from ekg.config import Settings
    from ekg.gui.main_window import MainWindow

    app = qapp
    pg.setConfigOptions(antialias=True, background="w", foreground="k")
    win = MainWindow(Settings())
    win.resize(1400, 860)
    win.show()
    assert win.connect_device(("sim", "normal"))

    def spin(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            app.processEvents()
            time.sleep(0.01)

    spin(1.0)
    win.toggle_recording()
    spin(5.0)
    assert win.pipeline.heart_rate() is not None
    assert win.hr.value.text() not in ("", "–")
    win.view.grab().save(str(SHOTS / "live.png"))
    win.toggle_recording()
    assert win.last_recording is not None and win.last_recording.exists()
    win.tabs.setCurrentWidget(win.trends)
    win.trends.refresh()
    win.disconnect_device()
    spin(0.5)
    assert win.worker is None

    win.tabs.setCurrentWidget(win.review)
    win.review.load(recording.path)
    win.review.wait_for_analysis()
    spin(0.5)
    assert win.review.an is not None
    win.review.caliper_btn.setChecked(True)
    win.review.paper_btn.setChecked(True)
    spin(0.2)
    win.grab().save(str(SHOTS / "review.png"))
    win.close()
