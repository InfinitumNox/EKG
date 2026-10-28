"""Erfassungs-Thread: liest vom Gerät, dekodiert und füttert die Pipeline."""

from __future__ import annotations

import logging
import queue
import time
from collections.abc import Callable
from typing import Protocol

from PySide6.QtCore import QThread, Signal

from .. import protocol as P
from ..pipeline import LivePipeline
from ..units import Conversion

log = logging.getLogger(__name__)


class Transport(Protocol):
    def read(self, size: int = 4096, timeout: float = 0.05) -> bytes: ...
    def write(self, data: bytes) -> int: ...
    def close(self) -> None: ...

    @property
    def description(self) -> str: ...


class DeviceWorker(QThread):
    connected = Signal(str)
    disconnected = Signal(str)
    info_received = Signal(object)
    status_received = Signal(object)
    ack_received = Signal(object)
    warning = Signal(str)

    NO_DATA_TIMEOUT_S = 3.0

    def __init__(
        self,
        open_transport: Callable[[], Transport],
        pipeline: LivePipeline,
        conversion_for: Callable[[P.InfoPacket], Conversion],
        rate_hz: int = 500,
        mode: P.Mode = P.Mode.LIVE,
        parent=None,
    ):
        super().__init__(parent)
        self._open = open_transport
        self.pipeline = pipeline
        self._conversion_for = conversion_for
        self.rate_hz = rate_hz
        self.mode = mode
        self._cmds: queue.Queue[bytes] = queue.Queue()
        self._seq = 0
        self._running = True
        self.decoder = P.FrameDecoder()
        self.info: P.InfoPacket | None = None
        self.last_status: P.StatusPacket | None = None

    # ------------------------------------------------------------ öffentliche API
    def send_command(self, op: P.Opcode, args: bytes = b"") -> None:
        self._cmds.put(P.frame(P.build_command(self._next_seq(), op, args)))

    def stop(self) -> None:
        self._running = False
        self.wait(3000)

    # ------------------------------------------------------------ Thread
    def _next_seq(self) -> int:
        self._seq = (self._seq + 1) & 0xFFFF
        return self._seq

    def _write_now(self, t: Transport, op: P.Opcode, args: bytes = b"") -> None:
        t.write(P.frame(P.build_command(self._next_seq(), op, args)))

    def run(self) -> None:
        try:
            t = self._open()
        except Exception as exc:  # noqa: BLE001 - Fehler sollen in der Oberfläche landen
            self.disconnected.emit(f"Verbindung fehlgeschlagen: {exc}")
            return
        reason = "getrennt"
        try:
            self.connected.emit(t.description)
            self._handshake(t)
            last_data = time.monotonic()
            warned = False
            while self._running:
                while not self._cmds.empty():
                    t.write(self._cmds.get_nowait())
                data = t.read(4096, 0.05)
                if data:
                    for pkt in self.decoder.feed(data):
                        if isinstance(pkt, P.SamplesPacket):
                            last_data = time.monotonic()
                            warned = False
                        self._dispatch(pkt)
                elif time.monotonic() - last_data > self.NO_DATA_TIMEOUT_S and not warned:
                    self.warning.emit("Seit 3 s keine Messdaten vom Gerät")
                    warned = True
        except Exception as exc:  # noqa: BLE001
            log.exception("Erfassung abgebrochen")
            reason = f"Verbindung verloren: {exc}"
        finally:
            try:
                self._write_now(t, P.Opcode.STOP)
            except Exception:  # noqa: BLE001
                pass
            t.close()
            self.disconnected.emit(reason)

    def _handshake(self, t: Transport) -> None:
        self._write_now(t, P.Opcode.STOP)
        self._write_now(t, P.Opcode.SET_RATE, P.cmd_args_set_rate(self.rate_hz))
        self._write_now(t, P.Opcode.SET_MODE, bytes([int(self.mode)]))
        self._write_now(t, P.Opcode.GET_INFO)
        deadline = time.monotonic() + 2.0
        while self.info is None and time.monotonic() < deadline and self._running:
            for pkt in self.decoder.feed(t.read(4096, 0.05)):
                self._dispatch(pkt)
        if self.info is None:
            raise TimeoutError("Gerät antwortet nicht (keine INFO). Richtige Firmware geflasht?")
        self._write_now(t, P.Opcode.START)

    def _dispatch(self, pkt: P.Packet) -> None:
        if isinstance(pkt, P.SamplesPacket):
            if self.info is not None:
                self.pipeline.push(pkt)
        elif isinstance(pkt, P.InfoPacket):
            first = self.info is None
            self.info = pkt
            if first:
                # Im Thread konfigurieren, bevor das erste Sample ankommt.
                self.pipeline.configure(pkt.sample_rate_hz, self._conversion_for(pkt))
                self.pipeline.set_info(pkt)
            self.info_received.emit(pkt)
        elif isinstance(pkt, P.StatusPacket):
            self.last_status = pkt
            self.status_received.emit(pkt)
        elif isinstance(pkt, P.AckPacket):
            if pkt.status != P.AckStatus.OK:
                self.warning.emit(f"Gerät lehnt Befehl {pkt.opcode:#04x} ab: {pkt.status.name}")
            self.ack_received.emit(pkt)
