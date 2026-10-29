"""Bestätigung von Extraschlägen über Zeitpunkt und QRS-Form.

Die RR-Klassifikation (ekg.dsp.artifacts) sieht nur Abstände. Ein plötzlich langes
Intervall – etwa wenn beim Luftanhalten oder Ziehen an einer Zigarette die Herzfrequenz
kurz abfällt – erzeugt dort dasselbe Muster wie ein Extraschlag; bei sehr gleichmäßigem
Puls reichen schon Sprünge von ~30 ms. Gezählt wird ein
Extraschlag deshalb nur, wenn der Schlag tatsächlich vorzeitig kommt; die QRS-Form im
Vergleich zum Median-Schlag unterscheidet dann zwischen „normaler“ und „veränderter“ Form.

Keine Diagnose: Die Form verrät nur grob, woher ein Schlag stammt.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

PREMATURE = 0.85  # Intervall vor dem Schlag < 85 % des lokalen Medians
QRS_CORR_MIN = 0.9  # darunter gilt die QRS-Form als verändert


@dataclass(frozen=True, slots=True)
class Ectopy:
    normal_qrs: list[int] = field(default_factory=list)  # Schlag-Indizes (vorzeitig, QRS wie sonst)
    other_qrs: list[int] = field(default_factory=list)  # vorzeitig, QRS-Form verändert
    not_premature: list[int] = field(default_factory=list)  # RR-Muster, aber nicht vorzeitig

    @property
    def count(self) -> int:
        return len(self.normal_qrs) + len(self.other_qrs)


def _qrs_window(x: np.ndarray, center: float, fs: float) -> np.ndarray | None:
    a, b = round(center - 0.05 * fs), round(center + 0.07 * fs)
    if a < 0 or b > len(x):
        return None
    w = x[a:b]
    return w if np.all(np.isfinite(w)) else None


def confirm_ectopics(
    x: np.ndarray,
    r_index: np.ndarray,
    fs: float,
    candidates: list[int],
    template: np.ndarray | None,
    template_r: int,
) -> Ectopy:
    """candidates: Indizes aus RrClassification.ectopic (RR-Indizes).

    Zu RR-Index i gehören die Schläge i und i+1; geprüft wird der, dem das kürzere
    Intervall vorausgeht.
    """
    r = np.asarray(r_index, dtype=np.float64)
    rr = np.diff(r) / fs
    tmpl = None
    if template is not None:
        a, b = template_r - round(0.05 * fs), template_r + round(0.07 * fs)
        if a >= 0 and b <= len(template):
            tmpl = template[a:b]
    out = Ectopy()
    seen: set[int] = set()
    for i in candidates:
        beats = [k for k in (i, i + 1) if 1 <= k < len(r)]
        if not beats:
            continue
        k = min(beats, key=lambda b: rr[b - 1])
        if k in seen:
            continue
        seen.add(k)
        lo, hi = max(0, k - 11), min(len(rr), k + 10)
        neighbours = np.delete(rr[lo:hi], [j - lo for j in (k - 1, k) if lo <= j < hi])
        if len(neighbours) < 3 or rr[k - 1] >= PREMATURE * float(np.median(neighbours)):
            out.not_premature.append(k)
            continue
        w = _qrs_window(x, r[k], fs)
        if w is None or tmpl is None:
            out.normal_qrs.append(k)
            continue
        corr = float(np.corrcoef(w, tmpl)[0, 1])
        (out.normal_qrs if corr >= QRS_CORR_MIN else out.other_qrs).append(k)
    return out
