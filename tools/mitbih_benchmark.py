#!/usr/bin/env python3
"""QRS-Detektor gegen die MIT-BIH Arrhythmia Database (PhysioNet).

Bewertung wie in ANSI/AAMI EC57: Ein Treffer liegt vor, wenn eine Detektion höchstens
75 ms neben einer annotierten Herzaktion liegt (Fenster 150 ms). Die Signale (360 Hz)
werden vorher auf 500 Hz umgetastet, also auf die Rate, mit der das Gerät arbeitet.
Geprüft wird exakt der Detektor, der auch live läuft (ekg.dsp.qrs).

    cd host && uv run python ../tools/mitbih_benchmark.py              # alle 48 Aufnahmen
    uv run python ../tools/mitbih_benchmark.py --records 100 105 203   # Auswahl
    uv run python ../tools/mitbih_benchmark.py --no-paced              # ohne 102/104/107/217

Fehlende Daten werden beim ersten Lauf von physionet.org geladen (~100 MB) und unter
--dir abgelegt. Benötigt das Paket wfdb (`uv pip install wfdb`).
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly

ALL_RECORDS = [
    "100", "101", "102", "103", "104", "105", "106", "107", "108", "109", "111", "112",
    "113", "114", "115", "116", "117", "118", "119", "121", "122", "123", "124", "200",
    "201", "202", "203", "205", "207", "208", "209", "210", "212", "213", "214", "215",
    "217", "219", "220", "221", "222", "223", "228", "230", "231", "232", "233", "234",
]  # fmt: skip
PACED = {"102", "104", "107", "217"}
BEAT_SYMBOLS = set("NLRBAaJSVrFejnE/fQ?")
FS_DEVICE = 500
LEARN_S = 3.0  # Lernphase des Detektors (Einschwingen + 2 s), wird nicht bewertet


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--dir", type=Path, default=Path.home() / ".cache" / "ekg" / "mitdb")
    ap.add_argument("--records", nargs="*", default=None)
    ap.add_argument(
        "--no-paced", action="store_true", help="Schrittmacher-Aufnahmen auslassen (AAMI)"
    )
    args = ap.parse_args()

    try:
        import wfdb
        from wfdb import processing
    except ImportError:
        sys.exit("Bitte zuerst wfdb installieren: uv pip install wfdb")

    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host" / "src"))
    from ekg.dsp.qrs import detect_qrs

    records = args.records or ALL_RECORDS
    if args.no_paced:
        records = [r for r in records if r not in PACED]
    args.dir.mkdir(parents=True, exist_ok=True)
    missing = [r for r in records if not (args.dir / f"{r}.dat").exists()]
    if missing:
        print(f"Lade {len(missing)} Aufnahmen von PhysioNet nach {args.dir} …")
        wfdb.dl_database("mitdb", str(args.dir), records=missing)

    print(
        f"{'Aufn.':>5} {'Schläge':>7} {'TP':>6} {'FN':>5} {'FP':>5} {'Se %':>7} {'+P %':>7} {'Δt ms':>7}"
    )
    tot_tp = tot_fn = tot_fp = 0
    errors_all: list[float] = []
    t_start = time.monotonic()
    for rec in records:
        r = wfdb.rdrecord(str(args.dir / rec), channels=[0])
        ann = wfdb.rdann(str(args.dir / rec), "atr")
        fs = r.fs
        x = r.p_signal[:, 0]
        if fs != 360:
            sys.exit(f"{rec}: unerwartete Abtastrate {fs} Hz")
        x500 = resample_poly(x, 25, 18)  # 360 Hz -> 500 Hz
        beats = detect_qrs(x500, FS_DEVICE)
        det = np.array([b.index for b in beats]) * fs / FS_DEVICE
        ref = np.array(
            [s for s, sym in zip(ann.sample, ann.symbol, strict=True) if sym in BEAT_SYMBOLS]
        )
        lo = LEARN_S * fs
        ref = ref[ref >= lo]
        det = det[det >= lo - 0.075 * fs]
        cmp = processing.compare_annotations(ref, np.round(det).astype(int), int(0.15 * fs))
        tp, fn, fp = cmp.tp, cmp.fn, cmp.fp
        matched = cmp.matching_sample_nums
        ok = matched >= 0
        err = (det[matched[ok]] - ref[ok]) / fs * 1000.0 if ok.any() else np.zeros(0)
        errors_all.extend(err.tolist())
        se = 100.0 * tp / max(1, tp + fn)
        ppv = 100.0 * tp / max(1, tp + fp)
        tot_tp, tot_fn, tot_fp = tot_tp + tp, tot_fn + fn, tot_fp + fp
        med = float(np.median(err)) if len(err) else float("nan")
        print(f"{rec:>5} {len(ref):>7} {tp:>6} {fn:>5} {fp:>5} {se:>7.2f} {ppv:>7.2f} {med:>7.1f}")

    se = 100.0 * tot_tp / max(1, tot_tp + tot_fn)
    ppv = 100.0 * tot_tp / max(1, tot_tp + tot_fp)
    print("-" * 56)
    print(
        f"{'Summe':>5} {tot_tp + tot_fn:>7} {tot_tp:>6} {tot_fn:>5} {tot_fp:>5} {se:>7.2f} {ppv:>7.2f}"
    )
    e = np.array(errors_all)
    if len(e):
        print(
            f"Zeitversatz zur Annotation: Median {np.median(e):.1f} ms, "
            f"95 % innerhalb ±{np.percentile(np.abs(e - np.median(e)), 95):.1f} ms"
        )
    print(f"Laufzeit {time.monotonic() - t_start:.0f} s")
    print(
        "Ziel laut Plan: Se und +P jeweils ≥ 99 %."
        + ("  ✓" if se >= 99 and ppv >= 99 else "  ✗ verfehlt")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
