"""Kommandozeile: `ekg-record` (Aufnahme ohne Oberfläche) und `ekg-export`."""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

from . import protocol as P
from .config import Settings, recordings_dir
from .pipeline import LivePipeline
from .storage.recording import default_filename, load_recording
from .units import Conversion


def _open(port: str | None, simulate: str | None):
    if simulate:
        from .device.simulator import SimulatedDevice

        return SimulatedDevice(simulate)
    from .device.transport import SerialTransport, find_ports

    if not port:
        ports = find_ports()
        if not ports:
            sys.exit("Kein Pico EKG gefunden. Angeschlossen? (ls /dev/ttyACM*)")
        port = ports[0].device
    return SerialTransport(port)


def record_main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ekg-record", description="EKG ohne Oberfläche aufzeichnen")
    ap.add_argument("--port")
    ap.add_argument("--simulate", metavar="SZENARIO")
    ap.add_argument("--duration", type=float, default=300.0, help="Sekunden (Standard 300)")
    ap.add_argument("--rate", type=int, default=500, choices=(500, 1000))
    ap.add_argument("--test", action="store_true", help="Testsignal der Firmware statt Messung")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)

    settings = Settings.load()
    t = _open(args.port, args.simulate)
    dec = P.FrameDecoder()
    seq = 0

    def cmd(op: P.Opcode, a: bytes = b"") -> None:
        nonlocal seq
        seq += 1
        t.write(P.frame(P.build_command(seq, op, a)))

    cmd(P.Opcode.STOP)
    cmd(P.Opcode.SET_RATE, P.cmd_args_set_rate(args.rate))
    cmd(P.Opcode.SET_MODE, bytes([1 if args.test else 0]))
    cmd(P.Opcode.GET_INFO)
    info = None
    deadline = time.monotonic() + 2
    while info is None and time.monotonic() < deadline:
        for pkt in dec.feed(t.read(4096, 0.05)):
            if isinstance(pkt, P.InfoPacket):
                info = pkt
    if info is None:
        sys.exit("Gerät antwortet nicht.")
    cal = settings.devices.get(info.board_id)
    conv = Conversion(
        info.oversampling,
        info.adc_bits,
        info.vref_mv / 1000,
        cal.gain if cal else float(info.gain_nominal),
        cal is not None,
    )
    pl = LivePipeline(info.sample_rate_hz, conv)
    pl.set_info(info)
    out = args.out or recordings_dir() / default_filename()
    pl.start_recording(out, {"source": "ekg-record"})
    print(f"Aufnahme nach {out} ({args.duration:.0f} s, Strg+C beendet früher)")
    cmd(P.Opcode.START)
    t_end = time.monotonic() + args.duration
    last_print = 0.0
    try:
        while time.monotonic() < t_end:
            for pkt in dec.feed(t.read(4096, 0.05)):
                if isinstance(pkt, P.SamplesPacket):
                    pl.push(pkt)
            if time.monotonic() - last_print > 1:
                last_print = time.monotonic()
                hr = pl.heart_rate()
                q = pl.quality
                print(
                    f"\rHF {hr or 0:5.0f}/min  Qualität {q.level.name:10s}  "
                    f"verloren {pl.assembler.stats.lost_samples}   ",
                    end="",
                    flush=True,
                )
    except KeyboardInterrupt:
        pass
    finally:
        cmd(P.Opcode.STOP)
        t.close()
        pl.stop_recording()
    print(f"\nGespeichert: {out}")
    return 0


def export_main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="ekg-export", description="Aufzeichnung auswerten und exportieren"
    )
    ap.add_argument("recording", type=Path)
    ap.add_argument("--edf", type=Path)
    ap.add_argument("--csv", type=Path)
    ap.add_argument("--wfdb", type=Path, metavar="ORDNER")
    ap.add_argument("--pdf", type=Path)
    ap.add_argument("--filter", default="monitor", choices=("monitor", "breit", "roh"))
    args = ap.parse_args(argv)

    from .analysis import analyse, quality_rows
    from .dsp.beats import interval_rows
    from .dsp.filters import PRESETS
    from .dsp.hrv import hrv_rows, segment_rows
    from .storage import export

    rec = load_recording(args.recording)
    an = analyse(rec, PRESETS[args.filter][1])
    print(f"{args.recording.name}: {rec.duration_s / 60:.1f} min, {len(an.beats)} Schläge")
    rows = quality_rows(an) + hrv_rows(an.hrv, an.breathing_hz, an.ectopy)
    seg = segment_rows(an.segments)
    rows += seg[:1] + [(f"  {k}", v) for k, v in seg[1:]]  # eingerückt unter „5-min-Abschnitte“
    if an.delineation is not None and an.median is not None:
        rows += interval_rows(an.delineation, an.median.mean_rr_s, " (exp.)")
    width = max(len(k) for k, _ in rows)
    for k, v in rows:
        print(f"  {k:{width}s}  {v}")
    for h in an.notes:
        print(f"  Hinweis: {h}")
    if args.edf:
        print("EDF+:", export.export_edf(rec, args.edf, an.r_index))
    if args.csv:
        print("CSV:", export.export_csv(rec, args.csv))
    if args.wfdb:
        print("WFDB:", export.export_wfdb(rec, args.wfdb, args.recording.stem, an.r_index))
    if args.pdf:
        if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
            os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtGui import QGuiApplication

        from .report.pdf import write_report

        _app = QGuiApplication.instance() or QGuiApplication([])
        print("PDF:", write_report(rec, an, args.pdf))
    return 0
