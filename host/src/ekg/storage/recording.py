"""Rohdaten-Aufzeichnung im eigenen, absturzsicheren Format (.ekgrec).

Aufbau:
    Magic  b"EKGREC\\x01\\n"
    u32    Länge des JSON-Kopfes, danach JSON (UTF-8)
    Chunks [u8 Typ][u32 Länge][Nutzdaten] ...

Es wird nur angehängt; ein Absturz verliert höchstens den letzten, unvollständigen
Chunk. Gespeichert werden die unveränderten Rohwerte des Geräts, alles andere
(mV, Filter, Erkennung) lässt sich daraus reproduzierbar neu berechnen.
"""

from __future__ import annotations

import json
import os
import struct
import time
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, BinaryIO

import numpy as np

from .. import __version__
from ..units import Conversion

MAGIC = b"EKGREC\x01\n"
SUFFIX = ".ekgrec"


class ChunkType:
    SAMPLES = 0x01
    MARKER = 0x02
    GAP = 0x03
    EVENT = 0x04
    HOSTTIME = 0x05


_CHUNK = struct.Struct("<BI")


class RecordingWriter:
    FLUSH_S = 1.0
    FSYNC_S = 10.0

    def __init__(self, path: Path, header: dict[str, Any]):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._f: BinaryIO = open(self.path, "xb")  # noqa: SIM115 - bleibt bis close() offen
        hdr = dict(header)
        hdr.setdefault("format_version", 1)
        hdr.setdefault("software", f"ekg {__version__}")
        hdr.setdefault("created_utc", datetime.now(UTC).isoformat())
        data = json.dumps(hdr, ensure_ascii=False).encode()
        self._f.write(MAGIC + struct.pack("<I", len(data)) + data)
        self._first_index: int | None = None
        self.samples_written = 0
        self._last_flush = time.monotonic()
        self._last_fsync = self._last_flush
        self._last_hosttime = -1e9
        self._fsync()

    @property
    def first_index(self) -> int | None:
        return self._first_index

    def _rel(self, index: int) -> int:
        if self._first_index is None:
            self._first_index = index
        return index - self._first_index

    def _chunk(self, ctype: int, payload: bytes) -> None:
        self._f.write(_CHUNK.pack(ctype, len(payload)) + payload)

    def write_samples(self, index: int, raw: np.ndarray, flags: int) -> None:
        rel = self._rel(index)
        if rel < 0:
            return
        arr = np.asarray(raw, dtype="<u4")
        now = time.time()
        if now - self._last_hosttime >= 10.0:
            self._chunk(ChunkType.HOSTTIME, struct.pack("<qd", rel, now))
            self._last_hosttime = now
        self._chunk(
            ChunkType.SAMPLES, struct.pack("<qBH", rel, flags & 0xFF, len(arr)) + arr.tobytes()
        )
        self.samples_written += len(arr)
        self._maybe_flush()

    def write_gap(self, index: int, n_missing: int) -> None:
        self._chunk(ChunkType.GAP, struct.pack("<qI", self._rel(index), n_missing))

    def write_marker(self, index: int, text: str) -> None:
        self._chunk(ChunkType.MARKER, struct.pack("<q", self._rel(index)) + text.encode())
        self._f.flush()

    def write_event(self, index: int, event: dict[str, Any]) -> None:
        payload = (
            struct.pack("<q", self._rel(index)) + json.dumps(event, ensure_ascii=False).encode()
        )
        self._chunk(ChunkType.EVENT, payload)

    def _maybe_flush(self) -> None:
        now = time.monotonic()
        if now - self._last_flush >= self.FLUSH_S:
            self._f.flush()
            self._last_flush = now
        if now - self._last_fsync >= self.FSYNC_S:
            self._fsync()

    def _fsync(self) -> None:
        self._f.flush()
        os.fsync(self._f.fileno())
        self._last_fsync = time.monotonic()

    def close(self) -> None:
        if not self._f.closed:
            self._fsync()
            self._f.close()


@dataclass
class Recording:
    path: Path | None
    header: dict[str, Any]
    fs: float
    conversion: Conversion
    raw: np.ndarray  # float64, NaN in Lücken
    flags: np.ndarray  # uint8 je Sample
    markers: list[tuple[int, str]] = field(default_factory=list)
    events: list[tuple[int, dict[str, Any]]] = field(default_factory=list)
    gaps: list[tuple[int, int]] = field(default_factory=list)
    host_times: list[tuple[int, float]] = field(default_factory=list)
    truncated: bool = False

    @property
    def n_samples(self) -> int:
        return len(self.raw)

    @property
    def duration_s(self) -> float:
        return len(self.raw) / self.fs

    @property
    def mv(self) -> np.ndarray:
        return self.conversion.raw_to_mv(self.raw)

    @property
    def start_time(self) -> datetime | None:
        if self.host_times:
            idx, t = self.host_times[0]
            return datetime.fromtimestamp(t - idx / self.fs).astimezone()
        created = self.header.get("created_utc")
        return datetime.fromisoformat(created).astimezone() if created else None

    @property
    def test_signal(self) -> bool:
        from ..protocol import Flag

        return bool(len(self.flags) and (self.flags & Flag.TEST).mean() > 0.5)


def read_header(f: BinaryIO) -> dict[str, Any]:
    if f.read(len(MAGIC)) != MAGIC:
        raise ValueError("keine EKG-Aufzeichnung (falsche Kennung)")
    (n,) = struct.unpack("<I", f.read(4))
    return json.loads(f.read(n).decode())


def load_recording(path: Path | str) -> Recording:
    path = Path(path)
    sample_chunks: list[tuple[int, int, np.ndarray]] = []
    markers: list[tuple[int, str]] = []
    events: list[tuple[int, dict[str, Any]]] = []
    gaps: list[tuple[int, int]] = []
    host_times: list[tuple[int, float]] = []
    truncated = False
    with open(path, "rb") as f:
        header = read_header(f)
        data = f.read()
    pos = 0
    while pos < len(data):
        if pos + _CHUNK.size > len(data):
            truncated = True
            break
        ctype, length = _CHUNK.unpack_from(data, pos)
        pos += _CHUNK.size
        if pos + length > len(data):
            truncated = True
            break
        body = data[pos : pos + length]
        pos += length
        if ctype == ChunkType.SAMPLES:
            idx, flags, count = struct.unpack_from("<qBH", body)
            arr = np.frombuffer(body, dtype="<u4", count=count, offset=11)
            sample_chunks.append((idx, flags, arr))
        elif ctype == ChunkType.MARKER:
            (idx,) = struct.unpack_from("<q", body)
            markers.append((idx, body[8:].decode(errors="replace")))
        elif ctype == ChunkType.GAP:
            gaps.append(struct.unpack_from("<qI", body))
        elif ctype == ChunkType.EVENT:
            (idx,) = struct.unpack_from("<q", body)
            events.append((idx, json.loads(body[8:].decode())))
        elif ctype == ChunkType.HOSTTIME:
            host_times.append(struct.unpack_from("<qd", body))

    n = max((i + len(a) for i, _, a in sample_chunks), default=0)
    raw = np.full(n, np.nan)
    flags = np.zeros(n, dtype=np.uint8)
    for idx, fl, arr in sample_chunks:
        raw[idx : idx + len(arr)] = arr
        flags[idx : idx + len(arr)] = fl
    fs = float(header.get("sample_rate_hz", 500))
    conv = Conversion.from_dict(header.get("conversion", {}))
    return Recording(
        path, header, fs, conv, raw, flags, markers, events, gaps, host_times, truncated
    )


def default_filename(now: datetime | None = None) -> str:
    now = now or datetime.now()
    return now.strftime("ekg_%Y-%m-%d_%H-%M-%S") + SUFFIX
