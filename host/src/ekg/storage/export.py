"""Export in Standardformate.

- EDF+: Europäisches Biosignal-Format, z. B. mit EDFbrowser lesbar. Marker und
  (optional) erkannte R-Zacken werden als Annotationen geschrieben.
- WFDB: PhysioNet-Format (benötigt das Paket `wfdb`), R-Zacken als Annotator "qrs".
- CSV: Zeit, Rohwert, mV, Flags.
"""

from __future__ import annotations

import csv
from collections.abc import Sequence
from pathlib import Path

import numpy as np

from ..dsp.filters import fill_gaps
from .recording import Recording


def export_edf(
    rec: Recording, path: Path, r_peaks: Sequence[float] | np.ndarray | None = None
) -> Path:
    import pyedflib

    path = Path(path)
    fs = int(round(rec.fs))
    mv, gap_mask = fill_gaps(rec.mv)
    uv = mv * 1000.0
    n_records = int(np.ceil(len(uv) / fs))
    uv = np.concatenate([uv, np.zeros(n_records * fs - len(uv))])
    phys = float(np.ceil(rec.conversion.full_scale_mv * 1000.0 * 1.05))
    w = pyedflib.EdfWriter(str(path), 1, file_type=pyedflib.FILETYPE_EDFPLUS)
    try:
        # Platz für ~1 Annotation pro Schlag und Sekunde plus Marker.
        w.set_number_of_annotation_signals(4 if r_peaks is not None else 1)
        w.setSignalHeaders(
            [
                {
                    "label": "EKG",
                    "dimension": "uV",
                    "sample_frequency": fs,
                    "physical_max": phys,
                    "physical_min": -phys,
                    "digital_max": 32767,
                    "digital_min": -32768,
                    "transducer": "AD8232 Ag/AgCl",
                    "prefilter": f"HW ~0.5-40Hz G={rec.conversion.gain:.0f}",
                }
            ]
        )
        fw = rec.header.get("device", {}).get("firmware", "")
        w.setEquipment(f"Pico_EKG_{fw}".rstrip("_"))  # EDF-Kopf: nur ASCII ohne Leerzeichen
        w.setPatientName("X")
        w.setPatientCode("X")
        start = rec.start_time
        if start:
            w.setStartdatetime(start.replace(tzinfo=None))
        w.writeSamples([np.clip(uv, -phys, phys)])
        for idx, text in rec.markers:
            w.writeAnnotation(idx / rec.fs, -1, text[:40])
        for start_idx, n in _runs(gap_mask):
            w.writeAnnotation(start_idx / rec.fs, n / rec.fs, "Datenluecke")
        if r_peaks is not None:
            for r in r_peaks:
                w.writeAnnotation(float(r) / rec.fs, -1, "R")
    finally:
        w.close()
    return path


def export_wfdb(
    rec: Recording, directory: Path, name: str, r_peaks: Sequence[float] | np.ndarray | None = None
) -> Path:
    import wfdb

    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    mv, _ = fill_gaps(rec.mv)
    wfdb.wrsamp(
        name,
        fs=rec.fs,
        units=["mV"],
        sig_name=["EKG"],
        p_signal=mv.reshape(-1, 1),
        fmt=["16"],
        adc_gain=[10000.0],
        baseline=[0],
        write_dir=str(directory),
    )
    if r_peaks is not None and len(r_peaks):
        samples = np.round(np.asarray(r_peaks)).astype(int)
        wfdb.wrann(name, "qrs", samples, symbol=["N"] * len(samples), write_dir=str(directory))
    return directory / f"{name}.hea"


def export_csv(rec: Recording, path: Path) -> Path:
    path = Path(path)
    mv = rec.mv
    with open(path, "w", newline="", encoding="utf-8") as f:
        wr = csv.writer(f)
        wr.writerow(["zeit_s", "roh", "mv", "flags"])
        t = np.arange(rec.n_samples) / rec.fs
        for i in range(rec.n_samples):
            raw = "" if not np.isfinite(rec.raw[i]) else int(rec.raw[i])
            v = "" if not np.isfinite(mv[i]) else f"{mv[i]:.5f}"
            wr.writerow([f"{t[i]:.4f}", raw, v, int(rec.flags[i])])
    return path


def _runs(mask: np.ndarray) -> list[tuple[int, int]]:
    if not mask.any():
        return []
    d = np.diff(np.concatenate([[0], mask.astype(np.int8), [0]]))
    starts = np.flatnonzero(d == 1)
    ends = np.flatnonzero(d == -1)
    return [(int(s), int(e - s)) for s, e in zip(starts, ends, strict=True)]
