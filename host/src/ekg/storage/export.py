"""Export in Standardformate.

- EDF+: Europäisches Biosignal-Format, z. B. mit EDFbrowser lesbar. Marker und
  (optional) erkannte R-Zacken werden als Annotationen geschrieben.
- WFDB: PhysioNet-Format (benötigt das Paket `wfdb`), R-Zacken als Annotator "qrs".
- CSV: Zeit, Rohwert, mV, Flags.
"""

from __future__ import annotations

import csv
from collections.abc import Sequence
from datetime import datetime
from pathlib import Path

import numpy as np

from ..dsp.filters import fill_gaps
from .recording import Recording


def export_edf(
    rec: Recording, path: Path, r_peaks: Sequence[float] | np.ndarray | None = None
) -> Path:
    """Schreibt EDF+ (kontinuierlich) mit einem EKG-Kanal und einem Annotationskanal.

    Eigene Implementierung nach der EDF+-Spezifikation (edfplus.info), damit keine
    kompilierte Abhängigkeit nötig ist. Lücken werden linear überbrückt und annotiert.
    """
    path = Path(path)
    fs = int(round(rec.fs))
    if abs(rec.fs - fs) > 1e-9:
        raise ValueError("EDF braucht eine ganzzahlige Abtastrate")
    mv, gap_mask = fill_gaps(rec.mv)
    uv = mv * 1000.0
    n_records = max(1, int(np.ceil(len(uv) / fs)))
    uv = np.concatenate([uv, np.zeros(n_records * fs - len(uv))])
    phys = float(np.ceil(rec.conversion.full_scale_mv * 1000.0 * 1.05))
    dmin, dmax = -32768, 32767
    digital = np.round((np.clip(uv, -phys, phys) + phys) / (2 * phys) * (dmax - dmin) + dmin)
    digital = np.clip(digital, dmin, dmax).astype("<i2")

    # Annotationen je Datensatz (1 s) sammeln: (Beginn s, Dauer s oder None, Text)
    per_record: list[list[tuple[float, float | None, str]]] = [[] for _ in range(n_records)]

    def add(onset: float, duration: float | None, text: str) -> None:
        k = min(n_records - 1, max(0, int(onset)))
        per_record[k].append((onset, duration, _edf_text(text)))

    for idx, text in rec.markers:
        add(idx / rec.fs, None, text)
    for start_idx, n in _runs(gap_mask):
        add(start_idx / rec.fs, n / rec.fs, "Datenluecke")
    if r_peaks is not None:
        for r in r_peaks:
            add(float(r) / rec.fs, None, "R")

    tals = [_tal_record(k, anns) for k, anns in enumerate(per_record)]
    ann_samples = max(30, (max(len(t) for t in tals) + 1) // 2)  # 2 Byte je "Sample"

    start = rec.start_time or datetime.now().astimezone()
    fw = rec.header.get("device", {}).get("firmware", "")
    equipment = _edf_text(f"Pico_EKG_{fw}".rstrip("_")).replace(" ", "_")
    month = ("JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC")
    rec_id = f"Startdate {start.day:02d}-{month[start.month - 1]}-{start.year} X X {equipment}"

    def field(value: object, width: int) -> bytes:
        text = str(value).encode("ascii", errors="replace")
        if len(text) > width:
            raise ValueError(f"EDF-Feld zu lang: {value!r}")
        return text.ljust(width, b" ")

    def num(v: float) -> str:
        return f"{v:.0f}" if float(v).is_integer() else f"{v:.6g}"

    ns = 2
    header = b"".join(
        [
            field("0", 8),
            field("X X X X", 80),
            field(rec_id, 80),
            field(start.strftime("%d.%m.%y"), 8),
            field(start.strftime("%H.%M.%S"), 8),
            field(256 * (ns + 1), 8),
            field("EDF+C", 44),
            field(n_records, 8),
            field(1, 8),
            field(ns, 4),
            field("EKG", 16) + field("EDF Annotations", 16),
            field("AD8232 Ag/AgCl electrodes", 80) + field("", 80),
            field("uV", 8) + field("", 8),
            field(num(-phys), 8) + field(-1, 8),
            field(num(phys), 8) + field(1, 8),
            field(dmin, 8) + field(dmin, 8),
            field(dmax, 8) + field(dmax, 8),
            field(f"HW ~0.5-40Hz G={rec.conversion.gain:.0f}", 80) + field("", 80),
            field(fs, 8) + field(ann_samples, 8),
            field("", 32) + field("", 32),
        ]
    )
    assert len(header) == 256 * (ns + 1)
    with open(path, "wb") as f:
        f.write(header)
        for k in range(n_records):
            f.write(digital[k * fs : (k + 1) * fs].tobytes())
            f.write(tals[k].ljust(2 * ann_samples, b"\0"))
    return path


def _edf_text(text: str) -> str:
    """EDF+-Annotationen: UTF-8 erlaubt, aber keine Steuerzeichen."""
    return "".join(ch for ch in text if ch >= " ")[:40]


def _tal_record(k: int, anns: list[tuple[float, float | None, str]]) -> bytes:
    """Time-stamped Annotation Lists eines Datensatzes; der erste TAL hält die Zeitbasis."""
    out = f"+{k}\x14\x14\x00".encode()
    for onset, duration, text in anns:
        dur = f"\x15{duration:.4f}" if duration is not None else ""
        out += f"+{onset:.4f}{dur}\x14{text}\x14\x00".encode()
    return out


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
