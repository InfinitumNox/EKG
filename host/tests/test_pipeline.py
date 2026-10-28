import numpy as np
import pytest

from ekg import calibration as cal
from ekg import protocol as P
from ekg.device.simulator import Scenario, SimulatedDevice
from ekg.dsp.quality import Level
from ekg.pipeline import LivePipeline
from ekg.power import PowerState, power_status
from ekg.storage.recording import load_recording
from ekg.units import Conversion

from .conftest import run_simulator


def feed(pl: LivePipeline, packets):
    for p in packets:
        if isinstance(p, P.SamplesPacket):
            pl.push(p)
            pl._last_quality = (
                0  # Qualität bei jedem Paket neu bewerten (Test läuft schneller als Echtzeit)
            )


def test_conversion_roundtrip():
    c = Conversion(oversampling=64)
    mv = np.array([-1.0, 0.0, 0.5, 1.2])
    np.testing.assert_allclose(c.raw_to_mv(c.mv_to_raw(mv)), mv, atol=1e-4)
    assert c.full_scale_mv == pytest.approx(1.5, abs=0.01)


def test_simulator_info_and_ack():
    dev = SimulatedDevice(realtime=False)
    packets, dec = run_simulator(dev, 1.0)
    info = [p for p in packets if isinstance(p, P.InfoPacket)]
    acks = [p for p in packets if isinstance(p, P.AckPacket)]
    assert info and info[0].sample_rate_hz == 500 and info[0].oversampling == 64
    assert all(a.status == P.AckStatus.OK for a in acks)
    assert dec.errors == 0


def test_pipeline_end_to_end_heart_rate():
    dev = SimulatedDevice(Scenario.NORMAL, hr_bpm=72, realtime=False, gain_error=1.0)
    packets, _ = run_simulator(dev, 40.0)
    pl = LivePipeline(500, Conversion(oversampling=64))
    feed(pl, packets)
    hr = pl.heart_rate()
    assert hr == pytest.approx(72, abs=3)
    assert pl.quality.level <= Level.FAIR, pl.quality.reasons
    assert pl.assembler.stats.lost_samples == 0
    assert len(pl.beats) > 40


def test_pipeline_lead_off_and_recovery():
    dev = SimulatedDevice(Scenario.LEAD_OFF, realtime=False)
    packets, _ = run_simulator(dev, 34.0)
    pl = LivePipeline(500, Conversion(oversampling=64))
    samples = [p for p in packets if isinstance(p, P.SamplesPacket)]
    # Bis 20 s normal, 20–25 s Elektrode ab, danach wieder Kontakt.
    feed(pl, samples[: int(22 / 0.05)])
    assert pl.quality.level == Level.NO_CONTACT
    assert pl.heart_rate() is None
    feed(pl, samples[int(22 / 0.05) :])
    assert pl.quality.level < Level.NO_CONTACT
    assert pl.heart_rate() is not None
    kinds = [e.kind for e in pl.events]
    assert "lead_off" in kinds and "lead_on" in kinds


def test_pipeline_fills_short_gaps_and_records(tmp_path):
    dev = SimulatedDevice(realtime=False, drop_probability=0.05, seed=3)
    packets, _ = run_simulator(dev, 20.0)
    pl = LivePipeline(500, Conversion(oversampling=64))
    path = tmp_path / "x.ekgrec"
    pl.start_recording(path)
    feed(pl, packets)
    pl.add_marker("Test")
    pl.stop_recording()
    assert pl.assembler.stats.gaps > 0
    assert pl.heart_rate() is not None  # Lücken stören die HF nicht
    rec = load_recording(path)
    assert rec.n_samples == pl.display.end
    assert np.isnan(rec.raw).sum() == pl.assembler.stats.lost_samples
    assert rec.markers and rec.markers[0][1] == "Test"
    assert len(rec.gaps) == pl.assembler.stats.gaps


def test_recording_survives_truncation(tmp_path):
    dev = SimulatedDevice(realtime=False)
    packets, _ = run_simulator(dev, 5.0)
    pl = LivePipeline(500, Conversion(oversampling=64))
    path = tmp_path / "crash.ekgrec"
    pl.start_recording(path)
    feed(pl, packets)
    pl.stop_recording()
    data = path.read_bytes()
    path.write_bytes(data[:-37])  # Absturz mitten im letzten Chunk
    rec = load_recording(path)
    assert rec.truncated
    assert rec.n_samples >= 5 * 500 - 50


def test_calibration_with_simulated_loopback():
    """Simulator mit 4 % zu hoher Verstärkung: die Kalibrierung muss das finden."""
    dev = SimulatedDevice(realtime=False, gain_error=1.04)
    dec = P.FrameDecoder()
    pl = LivePipeline(500, Conversion(oversampling=64))
    seq = 0

    def cmd(op, args=b""):
        nonlocal seq
        seq += 1
        dev.write(P.frame(P.build_command(seq, op, args)))

    cmd(P.Opcode.START)
    amps = {}
    for f in (0.2, 0.5, 1.0, 10.0, 40.0, 70.0):
        cmd(P.Opcode.SET_CAL, P.cmd_args_set_cal(P.CalWave.SINE, f))
        n_settle = int(cal.settle_s(f) / dev.BLOCK_S)
        n_meas = int(cal.measure_s(f) / dev.BLOCK_S)
        for _ in range(n_settle):
            feed(pl, dec.feed(dev.read()))
        t0 = pl.unfiltered.end
        for _ in range(n_meas):
            feed(pl, dec.feed(dev.read()))
        amps[f] = cal.sine_amplitude(pl.unfiltered.get(t0, pl.unfiltered.end), 500, f)
    res = cal.evaluate(amps, cal.input_amplitude_mv(), 1100.0)
    assert res.gain == pytest.approx(1144, rel=0.01)
    assert res.highpass_hz == pytest.approx(0.5, rel=0.15)
    assert res.lowpass_hz == pytest.approx(40, rel=0.15)


def test_power_status(tmp_path):
    def mk(name, **files):
        d = tmp_path / name
        d.mkdir()
        for k, v in files.items():
            (d / k).write_text(v)

    mk("BAT0", type="Battery", status="Discharging")
    assert power_status(tmp_path).state == PowerState.BATTERY
    mk("AC", type="Mains", online="1")
    assert power_status(tmp_path).state == PowerState.EXTERNAL
    (tmp_path / "AC" / "online").write_text("0")
    mk("hidpp_battery_0", type="Battery", scope="Device", status="Charging")
    assert power_status(tmp_path).state == PowerState.BATTERY
    empty = tmp_path / "empty"
    empty.mkdir()
    assert power_status(empty).state == PowerState.NO_BATTERY
