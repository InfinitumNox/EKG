"""Farben: „Papier“ (klassisches EKG-Raster) und „Monitor“ (dunkel)."""

from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtGui import QColor


@dataclass(frozen=True, slots=True)
class Theme:
    name: str
    background: str
    grid_minor: str
    grid_major: str
    trace: str
    trace_dim: str
    marker: str
    text: str
    cursor: str

    def c(self, attr: str) -> QColor:
        return QColor(getattr(self, attr))


PAPER = Theme(
    name="paper",
    background="#fffaf6",
    grid_minor="#f7d3d3",
    grid_major="#e9a0a0",
    trace="#151515",
    trace_dim="#9e9e9e",
    marker="#1565c0",
    text="#4a2c2c",
    cursor="#1565c0",
)

MONITOR = Theme(
    name="monitor",
    background="#050806",
    grid_minor="#0d1f10",
    grid_major="#1b3d20",
    trace="#3dff77",
    trace_dim="#4f6b55",
    marker="#ffd54f",
    text="#b9e6c3",
    cursor="#ffd54f",
)

THEMES = {t.name: t for t in (PAPER, MONITOR)}

LEVEL_COLORS = {0: "#2e7d32", 1: "#f9a825", 2: "#c62828", 3: "#6a1b9a"}

BANNER_COLORS = {
    "danger": ("#b71c1c", "#ffffff"),
    "warning": ("#f9a825", "#1a1a1a"),
    "info": ("#1565c0", "#ffffff"),
}
