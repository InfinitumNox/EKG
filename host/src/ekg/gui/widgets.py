"""Kleinere Anzeige-Bausteine: Herzfrequenz, Signalqualität, Warnbanner."""

from __future__ import annotations

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFont, QPainter
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QSizePolicy, QVBoxLayout, QWidget

from ..dsp.quality import LEVEL_LABELS, Level, QualityReport
from .theme import BANNER_COLORS, LEVEL_COLORS


class HeartRateDisplay(QFrame):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setFrameShape(QFrame.Shape.StyledPanel)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(12, 8, 12, 8)
        title = QLabel("Herzfrequenz")
        title.setStyleSheet("color: palette(mid); font-weight: 600;")
        row = QHBoxLayout()
        self.heart = QLabel("♥")
        self.heart.setStyleSheet("color: #c62828; font-size: 28pt;")
        self.value = QLabel("–")
        f = QFont(self.font())
        f.setPointSize(46)
        f.setBold(True)
        self.value.setFont(f)
        self.value.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        unit = QLabel("/min")
        unit.setStyleSheet("font-size: 13pt;")
        row.addWidget(self.heart)
        row.addStretch(1)
        row.addWidget(self.value)
        row.addWidget(unit, alignment=Qt.AlignmentFlag.AlignBottom)
        self.rr = QLabel("RR –")
        self.rr.setStyleSheet("color: palette(mid);")
        lay.addWidget(title)
        lay.addLayout(row)
        lay.addWidget(self.rr)
        self._beat_timer = QTimer(self)
        self._beat_timer.setSingleShot(True)
        self._beat_timer.timeout.connect(
            lambda: self.heart.setStyleSheet("color: #c62828; font-size: 28pt;")
        )

    def set_values(self, bpm: float | None, rr_ms: float | None) -> None:
        self.value.setText("–" if bpm is None else f"{bpm:.0f}")
        self.rr.setText("RR –" if rr_ms is None else f"RR {rr_ms:.0f} ms")

    def pulse(self) -> None:
        self.heart.setStyleSheet("color: #ff5252; font-size: 34pt;")
        self._beat_timer.start(120)


class QualityLight(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.level = Level.POOR
        self.setFixedSize(22, 22)

    def set_level(self, level: Level) -> None:
        self.level = level
        self.update()

    def paintEvent(self, event) -> None:  # noqa: N802
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setBrush(QColor(LEVEL_COLORS[int(self.level)]))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(2, 2, 18, 18)
        p.end()


class QualityPanel(QFrame):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setFrameShape(QFrame.Shape.StyledPanel)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(12, 8, 12, 8)
        title = QLabel("Signalqualität")
        title.setStyleSheet("color: palette(mid); font-weight: 600;")
        row = QHBoxLayout()
        self.light = QualityLight()
        self.label = QLabel("–")
        self.label.setStyleSheet("font-size: 14pt; font-weight: 600;")
        row.addWidget(self.light)
        row.addWidget(self.label, 1)
        self.reasons = QLabel("")
        self.reasons.setWordWrap(True)
        self.details = QLabel("")
        self.details.setStyleSheet("color: palette(mid);")
        self.details.setWordWrap(True)
        lay.addWidget(title)
        lay.addLayout(row)
        lay.addWidget(self.reasons)
        lay.addWidget(self.details)

    def set_report(self, rep: QualityReport) -> None:
        self.light.set_level(rep.level)
        self.label.setText(LEVEL_LABELS[rep.level])
        self.reasons.setText("\n".join(f"• {r}" for r in dict.fromkeys(rep.reasons)))
        parts = []
        if rep.r_amplitude_mv is not None:
            parts.append(f"R {rep.r_amplitude_mv:.2f} mV")
        if rep.template_corr is not None:
            parts.append(f"Form r={rep.template_corr:.2f}")
        if rep.mains_ratio is not None:
            parts.append(f"50 Hz {100 * rep.mains_ratio:.0f} %")
        self.details.setText(" · ".join(parts))


class InfoPanel(QFrame):
    """Kleine Schlüssel/Wert-Tabelle."""

    def __init__(self, title: str, parent: QWidget | None = None):
        super().__init__(parent)
        self.setFrameShape(QFrame.Shape.StyledPanel)
        self._lay = QVBoxLayout(self)
        self._lay.setContentsMargins(12, 8, 12, 8)
        t = QLabel(title)
        t.setStyleSheet("color: palette(mid); font-weight: 600;")
        self._lay.addWidget(t)
        self.body = QLabel("")
        self.body.setTextFormat(Qt.TextFormat.RichText)
        self.body.setWordWrap(True)
        self._lay.addWidget(self.body)

    def set_rows(self, rows: list[tuple[str, str]]) -> None:
        html = (
            "<table cellspacing=2>"
            + "".join(
                f"<tr><td style='padding-right:10px'>{k}</td><td><b>{v}</b></td></tr>"
                for k, v in rows
            )
            + "</table>"
        )
        self.body.setText(html)


class BannerArea(QWidget):
    """Gestapelte Hinweise; jeder hat einen Schlüssel, damit er gezielt verschwindet."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._lay = QVBoxLayout(self)
        self._lay.setContentsMargins(0, 0, 0, 0)
        self._lay.setSpacing(2)
        self._items: dict[str, QLabel] = {}
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Maximum)

    def show_banner(self, key: str, text: str, kind: str = "warning") -> None:
        bg, fg = BANNER_COLORS[kind]
        lab = self._items.get(key)
        if lab is None:
            lab = QLabel()
            lab.setWordWrap(True)
            self._lay.addWidget(lab)
            self._items[key] = lab
        lab.setText(text)
        lab.setStyleSheet(f"background:{bg}; color:{fg}; padding:6px 10px; font-weight:600;")
        lab.show()

    def hide_banner(self, key: str) -> None:
        lab = self._items.pop(key, None)
        if lab:
            lab.deleteLater()

    def keys(self) -> set[str]:
        return set(self._items)
