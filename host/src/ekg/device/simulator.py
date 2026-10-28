"""Simuliertes Gerät: spricht das echte Protokoll, ohne Hardware.

Damit lassen sich Oberfläche, Aufzeichnung und Auswertung vollständig testen. Im Modus
LIVE simuliert es einen Menschen am AD8232 (Szenarien wie Extrasystolen, Vorhofflimmern,
Artefakte, abfallende Elektroden), im Modus TEST verhält es sich wie das Testsignal der
Firmware. Ist das Kalibriersignal aktiv, wird die Loopback-Schaltung nachgebildet.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import StrEnum

import numpy as np
from scipy import signal

from .. import protocol as P
from ..units import Conversion

# (Versatz zur R-Zacke s, Amplitude mV, Breite s, wandert mit RR)
Wave = tuple[float, float, float, bool]
_NORMAL_WAVES: tuple[Wave, ...] = (
    (-0.200, 0.15, 0.025, False),
    (-0.035, -0.10, 0.010, False),
    (0.000, 1.20, 0.010, False),
    (0.035, -0.25, 0.012, False),
    (0.300, 0.30, 0.060, True),
)
_AF_WAVES: tuple[Wave, ...] = tuple(w for w in _NORMAL_WAVES if w[0] != -0.200)  # keine P-Welle
# Nachgebildet einer realen Ruhemessung (Brustwand, angenähert Ableitung II): kleine P-
# und T-Welle, R ~0,75 mV und eine flache negative Ta-Welle (Vorhof-Repolarisation), die
# die PR-Strecke etwas unter die TP-Strecke drückt.
_REST_WAVES: tuple[Wave, ...] = (
    (-0.200, 0.10, 0.022, False),  # P
    (-0.110, -0.03, 0.035, False),  # Ta
    (-0.030, -0.06, 0.008, False),  # Q
    (0.000, 0.75, 0.009, False),  # R
    (0.030, -0.12, 0.010, False),  # S
    (0.300, 0.13, 0.055, True),  # T
)
_PVC_WAVES: tuple[Wave, ...] = (
    (-0.020, -0.25, 0.025, False),
    (0.030, 1.50, 0.030, False),
    (0.090, -0.35, 0.030, False),
    (0.330, -0.40, 0.070, True),
)


class Scenario(StrEnum):
    NORMAL = "normal"
    REST = "rest"
    PVC = "pvc"
    AF = "af"
    BRADY = "brady"
    TACHY = "tachy"
    NOISY = "noisy"
    LEAD_OFF = "lead_off"


SCENARIO_LABELS = {
    Scenario.NORMAL: "Sinusrhythmus",
    Scenario.REST: "Ruhe, starke HF-Schwankung (wie reale Messung)",
    Scenario.PVC: "Ventrikuläre Extrasystolen",
    Scenario.AF: "Vorhofflimmern (irregulär)",
    Scenario.BRADY: "Bradykardie (45/min)",
    Scenario.TACHY: "Tachykardie (130/min)",
    Scenario.NOISY: "Bewegungsartefakte",
    Scenario.LEAD_OFF: "Elektrode fällt ab",
}


@dataclass(slots=True)
class _ScheduledBeat:
    r: float
    rr_before: float
    waves: tuple[Wave, ...]


@dataclass
class SyntheticEcg:
    fs: float
    hr_bpm: float = 70.0
    scenario: Scenario = Scenario.NORMAL
    seed: int = 1
    mains_mv: float = 0.0
    noise_mv: float = 0.008
    amplitude: float = 1.0
    resp_hz: float = 0.25  # Atemfrequenz
    rsa: float = 0.04  # relative RR-Schwankung mit der Atmung
    mayer: float = 0.0  # relative RR-Schwankung bei ~0,1 Hz (Blutdruckregulation)
    amp_mod: float = 0.0  # relative QRS-Amplitudenschwankung mit der Atmung
    jitter: float = 0.015
    r_times: list[float] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.scenario = Scenario(self.scenario)
        self._waves = _NORMAL_WAVES
        if self.scenario == Scenario.BRADY:
            self.hr_bpm = 45.0
        elif self.scenario == Scenario.TACHY:
            self.hr_bpm = 130.0
        elif self.scenario == Scenario.REST:
            self.hr_bpm, self.resp_hz, self.rsa, self.mayer = 66.0, 0.18, 0.07, 0.13
            self.amp_mod, self.jitter = 0.08, 0.01
            self._waves = _REST_WAVES
        self._rng = np.random.default_rng(self.seed)
        self._n = 0
        rr0 = 60.0 / self.hr_bpm
        self._beats: list[_ScheduledBeat] = []
        self._next_r = 0.5
        self._last_rr = rr0
        self._count = 0
        self._artifact_until = -1.0
        self._af_phase = self._rng.uniform(0, 2 * np.pi, 3)
        self._mayer_phase = self._rng.uniform(0, 2 * np.pi)

    @property
    def t(self) -> float:
        return self._n / self.fs

    def _schedule_until(self, t_end: float) -> None:
        rr0 = 60.0 / self.hr_bpm
        while self._next_r < t_end:
            r = self._next_r
            waves = _AF_WAVES if self.scenario == Scenario.AF else self._waves
            if self.scenario == Scenario.AF:
                rr = rr0 * self._rng.uniform(0.6, 1.4)
            elif self.scenario == Scenario.PVC and self._count % 6 == 5:
                waves = _PVC_WAVES
                rr = rr0 * 1.45  # kompensatorische Pause nach der Extrasystole
            elif self.scenario == Scenario.PVC and self._count % 6 == 4:
                rr = rr0 * 0.6  # die nächste Erregung kommt vorzeitig
            else:
                mod = self.rsa * np.sin(2 * np.pi * self.resp_hz * r)
                mod += self.mayer * np.sin(2 * np.pi * 0.105 * r + self._mayer_phase)
                rr = rr0 * (1.0 + mod + self.jitter * self._rng.normal())
            self._beats.append(_ScheduledBeat(r, self._last_rr, waves))
            self.r_times.append(r)
            self._last_rr = rr
            self._next_r = r + rr
            self._count += 1
        horizon = self.t - 2.0
        self._beats = [b for b in self._beats if b.r > horizon]

    def generate(self, n: int) -> np.ndarray:
        t = (self._n + np.arange(n)) / self.fs
        self._schedule_until(t[-1] + 1.0)
        v = np.zeros(n)
        for b in self._beats:
            if b.r < t[0] - 1.0 or b.r > t[-1] + 0.5:
                continue
            for off, amp, width, scales in b.waves:
                o = off * np.sqrt(b.rr_before) if scales else off
                x = (t - b.r - o) / width
                m = np.abs(x) < 6
                v[m] += amp * np.exp(-0.5 * x[m] ** 2)
        v *= self.amplitude * (1.0 + self.amp_mod * np.sin(2 * np.pi * self.resp_hz * t))
        v += 0.03 * np.sin(2 * np.pi * self.resp_hz * t)
        v += self.noise_mv * self._rng.normal(size=n)
        if self.mains_mv:
            v += self.mains_mv * np.sin(2 * np.pi * 50.0 * t)
        if self.scenario == Scenario.AF:
            for k, f in enumerate((5.3, 6.1, 7.4)):
                v += 0.03 * np.sin(2 * np.pi * f * t + self._af_phase[k])
        if self.scenario == Scenario.NOISY:
            v += self._artifacts(t)
        self._n += n
        return v

    def _artifacts(self, t: np.ndarray) -> np.ndarray:
        out = 0.03 * self._rng.normal(size=len(t))  # Muskelzittern
        if t[0] > self._artifact_until and self._rng.random() < 0.02:
            self._artifact_until = t[0] + self._rng.uniform(0.5, 2.0)
            self._artifact_amp = self._rng.uniform(0.3, 1.0)
            self._artifact_f = self._rng.uniform(1.0, 4.0)
        if t[0] <= self._artifact_until:
            out += self._artifact_amp * np.sin(2 * np.pi * self._artifact_f * t)
        return out


class Ad8232Model:
    """Grobes Modell des Breakout-Boards: 0,5-Hz-Hochpass und 40-Hz-Tiefpass (je 2. Ordnung),
    Verstärkung, Aussteuerungsgrenzen und ADC-Quantisierung mit Oversampling."""

    def __init__(self, fs: float, conversion: Conversion, true_gain: float, seed: int = 2):
        self.conversion = conversion
        self.true_gain = true_gain
        sos = np.vstack(
            [
                signal.butter(2, 0.5, "highpass", fs=fs, output="sos"),
                signal.butter(2, 40.0, "lowpass", fs=fs, output="sos"),
            ]
        )
        self._sos = sos
        self._zi = np.zeros((sos.shape[0], 2))
        self._rng = np.random.default_rng(seed)

    def process(self, mv: np.ndarray, lead_off: bool = False) -> np.ndarray:
        c = self.conversion
        y, self._zi = signal.sosfilt(self._sos, mv, zi=self._zi)
        v = c.vref_v / 2 + y / 1000.0 * self.true_gain
        if lead_off:
            v = np.full_like(v, c.vref_v * 0.97) + 0.01 * self._rng.normal(size=len(v))
        v = np.clip(v, 0.02, c.vref_v - 0.02)  # Rail-to-Rail mit etwas Abstand
        code = v / c.vref_v * 2**c.adc_bits
        raw = code * c.oversampling + self._rng.normal(
            scale=np.sqrt(c.oversampling) * 0.8, size=len(v)
        )
        return np.clip(np.round(raw), 0, c.raw_max).astype(np.uint32)


class SimulatedDevice:
    """Verhält sich wie die serielle Schnittstelle des Pico (read/write/close)."""

    BLOCK_S = 0.05
    BOARD_ID = "53494d554c000001"  # "SIMUL" + 0x000001

    def __init__(
        self,
        scenario: Scenario | str = Scenario.NORMAL,
        hr_bpm: float = 70.0,
        realtime: bool = True,
        mains_mv: float = 0.04,
        gain_error: float = 1.03,
        drop_probability: float = 0.0,
        seed: int = 1,
    ):
        self.scenario = Scenario(scenario)
        self.hr_bpm = hr_bpm
        self.realtime = realtime
        self.mains_mv = mains_mv
        self.drop_probability = drop_probability
        self.seed = seed
        self.gain_error = gain_error
        self.rate = 500
        self.mode = P.Mode.LIVE
        self.streaming = False
        self.cal_wave = P.CalWave.OFF
        self.cal_freq = 1.0
        self.test_hr = 70.0
        self.test_mains = False
        self._rng = np.random.default_rng(seed + 100)
        self._decoder = P.FrameDecoder()
        self._out = bytearray()
        self._seq = 0
        self._block = 0
        self._t_start = 0.0
        self._last_status = 0.0
        self._closed = False
        self.overruns = 0
        self._setup_signal()

    # ---------------------------------------------------------------- Signalquellen
    def _setup_signal(self) -> None:
        conv = Conversion(oversampling=32000 // self.rate)
        self.conversion = conv
        self.ecg = SyntheticEcg(
            self.rate, self.hr_bpm, self.scenario, seed=self.seed, mains_mv=self.mains_mv
        )
        self.test_ecg = SyntheticEcg(
            self.rate, self.test_hr, seed=self.seed + 1, mains_mv=0.05 if self.test_mains else 0.0
        )
        self.model = Ad8232Model(self.rate, conv, 1100.0 * self.gain_error, seed=self.seed + 2)
        self._cal_phase = 0.0

    def _lead_off_now(self, t: float) -> bool:
        return self.scenario == Scenario.LEAD_OFF and (t % 40.0) > 20.0 and (t % 40.0) < 25.0

    def _cal_input(self, n: int) -> np.ndarray:
        t = self._cal_phase + np.arange(n) / self.rate
        self._cal_phase = t[-1] + 1.0 / self.rate
        amp = 0.5 * 100.0 / (102_500 + 100.0) * 1000.0  # Standard-Loopback, siehe calibration.py
        if self.cal_wave == P.CalWave.SQUARE:
            v = np.where((t * self.cal_freq) % 1.0 < 0.5, amp, -amp)
        else:
            v = amp * np.sin(2 * np.pi * self.cal_freq * t)
        return v + 0.003 * self._rng.normal(size=n)

    def _make_block(self) -> bytes:
        n = round(self.rate * self.BLOCK_S)
        t = self._block * self.BLOCK_S
        flags = P.Flag.NONE
        if self.mode == P.Mode.TEST:
            raw = self.conversion.mv_to_raw(self.test_ecg.generate(n))
            flags |= P.Flag.TEST
        else:
            lead_off = self._lead_off_now(t)
            mv = self._cal_input(n) if self.cal_wave != P.CalWave.OFF else self.ecg.generate(n)
            if self.cal_wave != P.CalWave.OFF:
                self.ecg.generate(n)  # Zeitbasis weiterlaufen lassen
            raw = self.model.process(mv, lead_off=lead_off)
            if lead_off:
                flags |= P.Flag.LO_PLUS
            lo_c = 8 * self.conversion.oversampling
            hi_c = 4087 * self.conversion.oversampling
            if np.any((raw <= lo_c) | (raw >= hi_c)):
                flags |= P.Flag.CLIP
        if self.cal_wave != P.CalWave.OFF:
            flags |= P.Flag.CAL
        payload = P.build_samples(self._next_seq(), self._block * n, flags, raw)
        self._block += 1
        if self.drop_probability and self._rng.random() < self.drop_probability:
            return b""
        return P.frame(payload)

    # ---------------------------------------------------------------- Protokoll
    def _next_seq(self) -> int:
        s = self._seq
        self._seq = (self._seq + 1) & 0xFFFF
        return s

    def _send(self, payload: bytes) -> None:
        self._out += P.frame(payload)

    def _info(self) -> P.InfoPacket:
        return P.InfoPacket(
            seq=self._next_seq(),
            fw_version=(0, 1, 0),
            sample_rate_hz=self.rate,
            oversampling=32000 // self.rate,
            adc_bits=12,
            vref_mv=3300,
            gain_nominal=1100,
            mode=self.mode,
            streaming=self.streaming,
            board_id=self.BOARD_ID,
            name="Simulator",
        )

    def write(self, data: bytes) -> int:
        for pkt in self._decoder.feed(data):
            if isinstance(pkt, P.CommandPacket):
                self._handle(pkt)
        return len(data)

    def _handle(self, cmd: P.CommandPacket) -> None:
        status = P.AckStatus.OK
        op, args = cmd.opcode, cmd.args
        if op == P.Opcode.START:
            if not self.streaming:
                self.streaming = True
                self._block = 0
                self._setup_signal()
                self._t_start = time.monotonic()
        elif op == P.Opcode.STOP:
            self.streaming = False
        elif op == P.Opcode.SET_RATE:
            rate = int.from_bytes(args[:2], "little") if len(args) >= 2 else 0
            if self.streaming:
                status = P.AckStatus.BAD_STATE
            elif rate not in (500, 1000):
                status = P.AckStatus.BAD_ARG
            else:
                self.rate = rate
                self._setup_signal()
        elif op == P.Opcode.SET_MODE:
            if not args or args[0] > 1:
                status = P.AckStatus.BAD_ARG
            else:
                self.mode = P.Mode(args[0])
        elif op == P.Opcode.GET_INFO:
            self._send(P.build_info(self._info()))
        elif op == P.Opcode.SET_CAL:
            if len(args) < 5 or args[0] > 2:
                status = P.AckStatus.BAD_ARG
            else:
                self.cal_wave = P.CalWave(args[0])
                self.cal_freq = int.from_bytes(args[1:5], "little") / 1000.0
                self._cal_phase = 0.0
        elif op == P.Opcode.SET_TEST:
            if len(args) < 3:
                status = P.AckStatus.BAD_ARG
            else:
                self.test_hr = int.from_bytes(args[:2], "little") / 10.0
                self.test_mains = bool(args[2] & 1)
                self.test_ecg = SyntheticEcg(
                    self.rate,
                    self.test_hr,
                    seed=self.seed + 1,
                    mains_mv=0.05 if self.test_mains else 0.0,
                )
        elif op == P.Opcode.BOOTSEL:
            pass
        else:
            status = P.AckStatus.UNKNOWN
        self._send(P.build_ack(self._next_seq(), op, status))

    def _status(self) -> bytes:
        st = P.StatusPacket(
            seq=self._next_seq(),
            uptime_ms=int(self._block * self.BLOCK_S * 1000),
            overruns=self.overruns,
            usb_dropped=0,
            adc_fifo_overflows=0,
            mode=self.mode,
            streaming=self.streaming,
            cal_wave=self.cal_wave,
            lead_off=P.Flag.LO_PLUS
            if self._lead_off_now(self._block * self.BLOCK_S)
            else P.Flag.NONE,
        )
        return P.frame(P.build_status(st))

    def read(self, size: int = 4096, timeout: float = 0.05) -> bytes:
        if self._closed:
            raise OSError("Simulator geschlossen")
        if self.streaming:
            if self.realtime:
                due = int((time.monotonic() - self._t_start) / self.BLOCK_S)
                if due <= self._block and timeout:
                    wait = (self._block + 1) * self.BLOCK_S - (time.monotonic() - self._t_start)
                    time.sleep(max(0.0, min(timeout, wait)))
                    due = int((time.monotonic() - self._t_start) / self.BLOCK_S)
                while self._block < due:
                    self._emit_block()
            else:
                self._emit_block()
        elif self.realtime and not self._out:
            time.sleep(timeout)
        data = bytes(self._out[:size])
        del self._out[:size]
        return data

    def _emit_block(self) -> None:
        self._out += self._make_block()
        if self._block % round(1.0 / self.BLOCK_S) == 0:
            self._out += self._status()

    def close(self) -> None:
        self._closed = True

    @property
    def description(self) -> str:
        return f"Simulator – {SCENARIO_LABELS[self.scenario]}"
