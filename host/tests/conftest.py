import os

import numpy as np
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


@pytest.fixture(autouse=True)
def isolated_config(tmp_path, monkeypatch):
    """Keine Tests schreiben in die echte Konfiguration oder den Aufnahmeordner."""
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))


@pytest.fixture(scope="session")
def qapp():
    """Eine QApplication für alle Tests (ein QGuiApplication-Objekt würde Widgets verhindern)."""
    from PySide6.QtWidgets import QApplication

    return QApplication.instance() or QApplication([])


@pytest.fixture
def rng():
    return np.random.default_rng(1234)


def run_simulator(dev, seconds: float):
    """Treibt einen nicht-echtzeitfähigen Simulator und liefert alle Pakete."""
    from ekg import protocol as P

    dec = P.FrameDecoder()
    seq = 0

    def cmd(op, args=b""):
        nonlocal seq
        seq += 1
        dev.write(P.frame(P.build_command(seq, op, args)))

    cmd(P.Opcode.GET_INFO)
    cmd(P.Opcode.START)
    packets = []
    n_blocks = int(seconds / dev.BLOCK_S)
    for _ in range(n_blocks):
        packets.extend(dec.feed(dev.read()))
    return packets, dec
