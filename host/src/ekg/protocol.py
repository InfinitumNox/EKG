"""Binärprotokoll zwischen Pico und Host (Spiegel von firmware/src/protocol.h).

Frame: COBS(payload) + 0x00, payload = [type u8][version u8][seq u16][...][crc16 u16],
little endian. Spezifikation: docs/PROTOKOLL.md
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum, IntFlag

import numpy as np

PROTO_VERSION = 1
MAX_SAMPLES_PER_FRAME = 64
MAX_PAYLOAD = 320


class FrameType(IntEnum):
    SAMPLES = 0x01
    INFO = 0x02
    STATUS = 0x03
    COMMAND = 0x10
    ACK = 0x11


class Flag(IntFlag):
    NONE = 0
    LO_PLUS = 1 << 0
    LO_MINUS = 1 << 1
    OVERRUN = 1 << 2
    CLIP = 1 << 3
    TEST = 1 << 4
    CAL = 1 << 5

    @property
    def lead_off(self) -> bool:
        return bool(self & (Flag.LO_PLUS | Flag.LO_MINUS))


class Opcode(IntEnum):
    START = 0x01
    STOP = 0x02
    SET_RATE = 0x03
    SET_MODE = 0x04
    GET_INFO = 0x05
    SET_CAL = 0x06
    BOOTSEL = 0x07
    SET_TEST = 0x08


class Mode(IntEnum):
    LIVE = 0
    TEST = 1


class CalWave(IntEnum):
    OFF = 0
    SQUARE = 1
    SINE = 2


class AckStatus(IntEnum):
    OK = 0
    BAD_ARG = 1
    UNKNOWN = 2
    BAD_STATE = 3


class ProtocolError(ValueError):
    pass


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (Polynom 0x1021, Start 0xFFFF)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    block = bytearray()
    for i, b in enumerate(data):
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
            continue
        block.append(b)
        if len(block) == 254:
            out.append(0xFF)
            out += block
            block.clear()
            if i + 1 == len(data):
                return bytes(out)  # kanonisch: kein leerer Block nach vollem Block am Ende
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        if code == 0:
            raise ProtocolError("COBS: Nullbyte im Frame")
        i += 1
        end = i + code - 1
        if end > n:
            raise ProtocolError("COBS: Block länger als Frame")
        chunk = data[i:end]
        if 0 in chunk:
            raise ProtocolError("COBS: Nullbyte im Block")
        out += chunk
        i = end
        if code != 0xFF and i < n:
            out.append(0)
    return bytes(out)


@dataclass(frozen=True, slots=True)
class SamplesPacket:
    seq: int
    first_index: int
    flags: Flag
    samples: np.ndarray  # uint32 Rohsummen


@dataclass(frozen=True, slots=True)
class InfoPacket:
    seq: int
    fw_version: tuple[int, int, int]
    sample_rate_hz: int
    oversampling: int
    adc_bits: int
    vref_mv: int
    gain_nominal: int
    mode: Mode
    streaming: bool
    board_id: str
    name: str

    @property
    def fw_version_str(self) -> str:
        return ".".join(str(v) for v in self.fw_version)


@dataclass(frozen=True, slots=True)
class StatusPacket:
    seq: int
    uptime_ms: int
    overruns: int
    usb_dropped: int
    adc_fifo_overflows: int
    mode: Mode
    streaming: bool
    cal_wave: CalWave
    lead_off: Flag


@dataclass(frozen=True, slots=True)
class AckPacket:
    seq: int
    opcode: int
    status: AckStatus


@dataclass(frozen=True, slots=True)
class CommandPacket:
    seq: int
    opcode: int
    args: bytes = field(default=b"")


Packet = SamplesPacket | InfoPacket | StatusPacket | AckPacket | CommandPacket

_HEADER = struct.Struct("<BBH")
_INFO = struct.Struct("<IHHBHHBB")


def _finish(body: bytes) -> bytes:
    return body + struct.pack("<H", crc16(body))


def build_samples(seq: int, first_index: int, flags: int, samples: np.ndarray | list[int]) -> bytes:
    arr = np.asarray(samples, dtype="<u4")
    if len(arr) > MAX_SAMPLES_PER_FRAME:
        raise ProtocolError("zu viele Samples für einen Frame")
    body = _HEADER.pack(FrameType.SAMPLES, PROTO_VERSION, seq & 0xFFFF)
    body += struct.pack("<IBB", first_index & 0xFFFFFFFF, len(arr), int(flags))
    body += arr.tobytes()
    return _finish(body)


def build_info(info: InfoPacket) -> bytes:
    major, minor, patch = info.fw_version
    name = info.name.encode()[:64]
    body = _HEADER.pack(FrameType.INFO, PROTO_VERSION, info.seq & 0xFFFF)
    body += _INFO.pack(
        (major << 16) | (minor << 8) | patch,
        info.sample_rate_hz,
        info.oversampling,
        info.adc_bits,
        info.vref_mv,
        info.gain_nominal,
        int(info.mode),
        int(info.streaming),
    )
    body += bytes.fromhex(info.board_id)[:8].ljust(8, b"\0")
    body += bytes([len(name)]) + name
    return _finish(body)


def build_status(st: StatusPacket) -> bytes:
    body = _HEADER.pack(FrameType.STATUS, PROTO_VERSION, st.seq & 0xFFFF)
    body += struct.pack(
        "<IIIIBBBB",
        st.uptime_ms & 0xFFFFFFFF,
        st.overruns,
        st.usb_dropped,
        st.adc_fifo_overflows,
        int(st.mode),
        int(st.streaming),
        int(st.cal_wave),
        int(st.lead_off),
    )
    return _finish(body)


def build_ack(seq: int, opcode: int, status: int) -> bytes:
    body = _HEADER.pack(FrameType.ACK, PROTO_VERSION, seq & 0xFFFF) + bytes([opcode, status])
    return _finish(body)


def build_command(seq: int, opcode: int, args: bytes = b"") -> bytes:
    body = _HEADER.pack(FrameType.COMMAND, PROTO_VERSION, seq & 0xFFFF) + bytes([opcode]) + args
    return _finish(body)


def frame(payload: bytes) -> bytes:
    return cobs_encode(payload) + b"\x00"


def parse_payload(payload: bytes) -> Packet:
    if len(payload) < 6:
        raise ProtocolError("Frame zu kurz")
    if payload[1] != PROTO_VERSION:
        raise ProtocolError(f"Protokollversion {payload[1]} nicht unterstützt")
    (crc,) = struct.unpack_from("<H", payload, len(payload) - 2)
    if crc16(payload[:-2]) != crc:
        raise ProtocolError("CRC-Fehler")
    ftype, _ver, seq = _HEADER.unpack_from(payload, 0)
    body = payload[4:-2]
    try:
        if ftype == FrameType.SAMPLES:
            first_index, count, flags = struct.unpack_from("<IBB", body, 0)
            if len(body) != 6 + 4 * count:
                raise ProtocolError("SAMPLES: Länge passt nicht zur Anzahl")
            samples = np.frombuffer(body, dtype="<u4", count=count, offset=6).astype(np.uint32)
            return SamplesPacket(seq, first_index, Flag(flags), samples)
        if ftype == FrameType.INFO:
            fields = _INFO.unpack_from(body, 0)
            fw, fs, osr, bits, vref, gain, mode, streaming = fields
            o = _INFO.size
            board = body[o : o + 8].hex()
            name_len = body[o + 8]
            name = body[o + 9 : o + 9 + name_len].decode("utf-8", errors="replace")
            return InfoPacket(
                seq,
                ((fw >> 16) & 0xFF, (fw >> 8) & 0xFF, fw & 0xFF),
                fs,
                osr,
                bits,
                vref,
                gain,
                Mode(mode),
                bool(streaming),
                board,
                name,
            )
        if ftype == FrameType.STATUS:
            up, ovr, drop, fifo, mode, streaming, cal, lo = struct.unpack_from("<IIIIBBBB", body)
            return StatusPacket(
                seq, up, ovr, drop, fifo, Mode(mode), bool(streaming), CalWave(cal), Flag(lo)
            )
        if ftype == FrameType.ACK:
            return AckPacket(seq, body[0], AckStatus(body[1]))
        if ftype == FrameType.COMMAND:
            return CommandPacket(seq, body[0], bytes(body[1:]))
    except (struct.error, IndexError, ValueError) as exc:
        if isinstance(exc, ProtocolError):
            raise
        raise ProtocolError(f"Frame-Typ {ftype:#x} fehlerhaft: {exc}") from exc
    raise ProtocolError(f"unbekannter Frame-Typ {ftype:#x}")


class FrameDecoder:
    """Zerlegt einen Bytestrom in Pakete. Kaputte Frames werden gezählt und übersprungen."""

    MAX_FRAME = MAX_PAYLOAD + MAX_PAYLOAD // 254 + 2

    def __init__(self) -> None:
        self._buf = bytearray()
        self._discarding = False
        self.errors = 0
        self.last_error = ""

    def feed(self, data: bytes) -> list[Packet]:
        packets: list[Packet] = []
        start = 0
        while True:
            end = data.find(b"\x00", start)
            if end < 0:
                self._append(data[start:])
                break
            self._append(data[start:end])
            if self._discarding:
                self._discarding = False
            elif self._buf:
                try:
                    packets.append(parse_payload(cobs_decode(bytes(self._buf))))
                except ProtocolError as exc:
                    self.errors += 1
                    self.last_error = str(exc)
            self._buf.clear()
            start = end + 1
        return packets

    def _append(self, chunk: bytes) -> None:
        if self._discarding:
            return
        self._buf += chunk
        if len(self._buf) > self.MAX_FRAME:
            self.errors += 1
            self.last_error = "Frame zu lang"
            self._buf.clear()
            self._discarding = True


def cmd_args_set_rate(rate_hz: int) -> bytes:
    return struct.pack("<H", rate_hz)


def cmd_args_set_cal(wave: CalWave, freq_hz: float) -> bytes:
    return struct.pack("<BI", int(wave), round(freq_hz * 1000))


def cmd_args_set_test(hr_bpm: float, mains: bool) -> bytes:
    return struct.pack("<HB", round(hr_bpm * 10), 1 if mains else 0)
