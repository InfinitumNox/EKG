"""Klassifikation von RR-Intervallen nach Lipponen & Tarvainen (2019).

J. A. Lipponen, M. P. Tarvainen: "A robust algorithm for heart rate variability time
series artefact correction using novel beat classification", J Med Eng Technol 43(3), 2019.
Dasselbe Verfahren nutzen Kubios HRV und neurokit2.

Statt einer festen Schwelle (z. B. „20 % Änderung“) werden die Schwellen aus der
Streuung der letzten ~90 Schläge abgeleitet. Starke, aber gleichmäßige Schwankungen wie
die atemabhängige Sinusarrhythmie gelten damit als normal. Erkannt werden:

- ectopic:   vorzeitiger Schlag mit Pause (Muster kurz–lang in den RR-Differenzen)
- missed:    vermutlich verpasster Schlag (Intervall ≈ 2 × Median)
- extra:     vermutlich zusätzlicher Schlag (zwei kurze Intervalle ≈ 1 Median)
- longshort: einzelnes zu langes oder zu kurzes Intervall ohne eindeutiges Muster

Indizes beziehen sich auf die RR-Reihe: rr[i] ist das Intervall, das mit Schlag i+1 endet.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy.ndimage import median_filter, percentile_filter

C1, C2 = 0.13, 0.17
ALPHA = 5.2
WINDOW = 91
MEDFILT = 11


@dataclass(frozen=True, slots=True)
class RrClassification:
    n: int
    ectopic: list[int] = field(default_factory=list)
    missed: list[int] = field(default_factory=list)
    extra: list[int] = field(default_factory=list)
    longshort: list[int] = field(default_factory=list)

    @property
    def n_artifacts(self) -> int:
        return len(self.ectopic) + len(self.missed) + len(self.extra) + len(self.longshort)

    def valid_mask(self) -> np.ndarray:
        """True für RR-Intervalle, die als normal (NN) gelten.

        Um einen vorzeitigen Schlag herum werden das kurze und das lange Intervall entfernt,
        bei Erkennungsfehlern die betroffenen Intervalle.
        """
        ok = np.ones(self.n, dtype=bool)

        def drop(*idx: int) -> None:
            for i in idx:
                if 0 <= i < self.n:
                    ok[i] = False

        for i in self.ectopic:
            drop(i - 1, i, i + 1)
        for i in self.missed:
            drop(i)
        for i in self.extra:
            drop(i, i + 1)
        for i in self.longshort:
            drop(i)
        return ok


def _threshold(x: np.ndarray) -> np.ndarray:
    a = np.abs(x)
    size = min(WINDOW, len(a) if len(a) % 2 else len(a) - 1) or 1
    q1 = percentile_filter(a, 25, size=size, mode="nearest")
    q3 = percentile_filter(a, 75, size=size, mode="nearest")
    th = ALPHA * (q3 - q1) / 2.0
    # Untergrenze, damit sehr gleichmäßige Reihen nicht jede Mini-Schwankung melden.
    return np.maximum(th, 0.01)


def classify_rr(rr_s: np.ndarray) -> RrClassification:
    """rr_s: RR-Intervalle in Sekunden."""
    rr = np.asarray(rr_s, dtype=np.float64).copy()
    n = len(rr)
    if n < 6:
        return RrClassification(n)

    drrs = np.diff(rr, prepend=rr[0])
    drrs[0] = np.mean(drrs[1:])
    th1 = _threshold(drrs)
    drrs = drrs / th1

    pad = np.pad(drrs, 2, mode="reflect")
    s12 = np.zeros(n)
    s22 = np.zeros(n)
    for j in range(n):
        d = pad[j + 2]
        prev_, next_, next2 = pad[j + 1], pad[j + 3], pad[j + 4]
        if d > 0:
            s12[j] = max(prev_, next_)
        elif d < 0:
            s12[j] = min(prev_, next_)
        s22[j] = min(next_, next2) if d >= 0 else max(next_, next2)

    medrr = median_filter(rr, size=MEDFILT, mode="nearest")
    mrrs = rr - medrr
    mrrs[mrrs < 0] *= 2.0  # kurze Intervalle stärker gewichten
    th2 = _threshold(mrrs)
    mrrs = mrrs / th2

    res = RrClassification(n)
    i = 0
    while i < n - 2:
        if abs(drrs[i]) <= 1:
            i += 1
            continue
        eq1 = drrs[i] > 1 and s12[i] < -C1 * drrs[i] - C2
        eq2 = drrs[i] < -1 and s12[i] > -C1 * drrs[i] + C2
        if eq1 or eq2:
            res.ectopic.append(i)
            i += 1
            continue
        candidates = [i]
        if abs(drrs[i + 1]) < abs(drrs[i + 2]):
            candidates.append(i + 1)
        for j in candidates:
            long_ = drrs[j] > 1 and s22[j] < -1
            longshort = abs(mrrs[j]) > 3
            short = drrs[j] < -1 and s22[j] > 1
            if not (long_ or longshort or short):
                continue
            # Plausibilität zusätzlich zum Original: Bei starker Atemschwankung ist th2 groß,
            # dann würde schon ein langes Atem-Intervall als „verpasster Schlag“ gelten.
            missed = abs(rr[j] / 2 - medrr[j]) < th2[j] and rr[j] > 1.5 * medrr[j]
            extra = (
                j + 1 < n and abs(rr[j] + rr[j + 1] - medrr[j]) < th2[j] and rr[j] < 0.75 * medrr[j]
            )
            if short and extra:
                res.extra.append(j)
            elif long_ and missed:
                res.missed.append(j)
            else:
                res.longshort.append(j)
        i += 1
    _compensatory_pauses(rr, medrr, res)
    # Doppelte Einträge entfernen; „longshort“ direkt neben einem eindeutig zugeordneten
    # Ereignis ist nur dessen zweites Intervall und zählt nicht extra.
    for lst in (res.ectopic, res.missed, res.extra):
        lst[:] = sorted(set(lst))
    taken = {k for i in res.ectopic + res.missed + res.extra for k in (i - 1, i, i + 1)}
    res.longshort[:] = sorted(set(res.longshort) - taken)
    return res


def _compensatory_pauses(rr: np.ndarray, medrr: np.ndarray, res: RrClassification) -> None:
    """Ergänzt vorzeitige Schläge mit Pause, die das adaptive Verfahren übersieht.

    Sind Extraschläge sehr häufig (z. B. jeder 6. Schlag), passt sich die adaptive Schwelle
    an sie an. Das Muster „deutlich zu kurz, direkt danach deutlich zu lang, zusammen etwa
    zwei normale Intervalle“ entsteht bei gleichmäßiger Atemschwankung dagegen nicht.
    """
    known = set(res.ectopic)
    for k in range(1, len(rr) - 1):
        m = medrr[k]
        short, long_ = rr[k], rr[k + 1]
        if (
            short < 0.85 * m
            and long_ > 1.1 * m
            and abs(short + long_ - 2 * m) < 0.2 * m
            and rr[k - 1] > 1.1 * short
            and not ({k, k + 1} & known)
        ):
            res.ectopic.append(k + 1)  # Index wie im adaptiven Verfahren: das lange Intervall
            known.update((k, k + 1))
