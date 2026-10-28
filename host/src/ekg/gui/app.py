"""Einstiegspunkt der Oberfläche: `ekg`."""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

import pyqtgraph as pg
from PySide6.QtWidgets import QApplication

from .. import __version__
from ..config import Settings, data_dir


def _setup_logging(debug: bool) -> None:
    log_dir = data_dir()
    log_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=logging.DEBUG if debug else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=[logging.FileHandler(log_dir / "ekg.log"), logging.StreamHandler()],
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ekg", description="EKG-Software für AD8232 + Pico 2")
    ap.add_argument(
        "--simulate",
        metavar="SZENARIO",
        nargs="?",
        const="normal",
        help="sofort mit dem Simulator verbinden (normal, pvc, af, brady, tachy, noisy, lead_off)",
    )
    ap.add_argument("--port", help="serielle Schnittstelle, z. B. /dev/ttyACM0")
    ap.add_argument("--open", type=Path, help="Aufzeichnung direkt in der Auswertung öffnen")
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--version", action="version", version=f"ekg {__version__}")
    args = ap.parse_args(argv)
    _setup_logging(args.debug)

    app = QApplication(sys.argv[:1])
    app.setApplicationName("EKG")
    app.setOrganizationName("ekg")
    app.setDesktopFileName("ekg")
    pg.setConfigOptions(antialias=True, background="w", foreground="k")

    from .main_window import MainWindow

    win = MainWindow(Settings.load())
    win.show()
    if args.open:
        win.tabs.setCurrentWidget(win.review)
        win.review.load(args.open)
    elif args.simulate:
        win.connect_device(("sim", args.simulate))
    elif args.port:
        win.connect_device(("serial", args.port))
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
