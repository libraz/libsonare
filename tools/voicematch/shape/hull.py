"""How far a subject sits outside the references' own disagreement, feature by feature.

Every reading a profile row carries is flattened to one scalar per grid cell,
gated or not. For each reference left out in turn, its distance from the mean of
the others is the family's own scale; the subject's distance from that same mean
is read against it. A ratio near 1 is one more instrument of the family, and the
largest ratio is the axis on which the subject is a different instrument.

A ratio says nothing about audibility, so the absolute distance is printed in the
feature's own unit beside it. Time and frequency features are in doublings, which
makes the unit mean the same thing at both ends of the keyboard.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

#: Keyboard thirds a reading is pooled over; a whole-keyboard median hides a register defect.
REGISTERS = (("bass", 0, 41), ("mid", 42, 77), ("treble", 78, 127))

#: Partials whose level is read one at a time; above this the ladder is pooled in bands.
SINGLE_PARTIALS = 8

#: Pooled ladder bands, as inclusive partial numbers.
LADDER_BANDS = ((2, 6), (7, 12), (13, 20))

#: Suffix of a per-note feature holding the softest-to-hardest change of a per-cell one.
SWING = "~vel"

#: Velocity key the per-note swing rows are stored under.
SWING_KEY = -1


def _log2(x) -> float | None:
    return float(np.log2(x)) if x is not None and np.isfinite(x) and x > 0.0 else None


def row_features(row: dict) -> dict[str, float]:
    """Flatten one profile row to scalars; absent or non-finite readings are left out."""
    out: dict[str, float | None] = {}
    f0 = row.get("f0_hz")
    out["cents"] = row.get("cents_vs_et")
    if row.get("inharmonicity_reliable"):
        out["inharm_log2"] = _log2(row.get("inharmonicity_b"))
    ladder = row.get("partials_db") or []
    for k in range(2, min(len(ladder), SINGLE_PARTIALS) + 1):
        out[f"h{k}_db"] = ladder[k - 1] - ladder[0]
    for lo, hi in LADDER_BANDS:
        band = [v - ladder[0] for v in ladder[lo - 1 : hi]]
        if len(band) == hi - lo + 1:
            out[f"h{lo}-{hi}_db"] = float(np.mean(band))
    if len(ladder) >= 7:
        rel = [v - ladder[0] for v in ladder[:7]]
        out["odd_even_db"] = float(np.mean(rel[2::2]) - np.mean(rel[1::2]))
    peak, held, rms = row.get("peak_dbfs"), row.get("held_peak_dbfs"), row.get("rms_dbfs")
    if peak is not None and held is not None:
        out["strike_crest_db"] = peak - held
    if held is not None and rms is not None:
        out["held_over_gate_rms_db"] = held - rms
    out["attack_log2ms"] = _log2(row.get("attack_ms"))
    out["attack_hf_db"] = row.get("attack_hf_db")
    out["onset_overshoot_db"] = row.get("onset_overshoot_db")
    out["onset_settle_log2ms"] = _log2(row.get("onset_settle_ms"))
    out["decay_db_s"] = row.get("decay_db_s")
    out["decay_early_db_s"] = row.get("decay_early_db_s")
    out["decay_late_db_s"] = row.get("decay_late_db_s")
    if row.get("decay_early_db_s") is not None and row.get("decay_late_db_s") is not None:
        out["decay_gap_db_s"] = row["decay_early_db_s"] - row["decay_late_db_s"]
    out["decay_knee_log2s"] = _log2(row.get("decay_knee_s"))
    out["decay_span_log2s"] = _log2(row.get("decay_span_s"))
    for k, v in enumerate(row.get("partial_decay_db_s") or [], start=1):
        out[f"h{k}_decay_db_s"] = v
    out["body_below_f0_db"] = row.get("body_below_f0_db")
    if f0 and row.get("centroid_hz"):
        out["centroid_oct_over_f0"] = _log2(row["centroid_hz"] / f0)
    out["tnr_db"] = row.get("tnr_db")
    out["stereo_width"] = row.get("stereo_width")
    if not row.get("damper_capped"):
        out["damper_log2ms"] = _log2(row.get("damper_release_ms"))
    out["level_db"] = held
    return {k: float(v) for k, v in out.items() if v is not None and np.isfinite(v)}


def feature_table(
    rows: list[dict], extra: dict[str, dict[tuple[int, int], dict[str, float]]] | None = None
) -> dict[str, dict[tuple[int, int], dict[str, float]]]:
    """Per timbre, per (note, velocity), the flattened features, with @p extra folded in per cell.

    `level_db` is re-read against the timbre's own median across the keyboard at
    that velocity, since three captures at three gains share no absolute level.
    Each note also gets one row under @ref SWING_KEY: every feature's change from
    its softest to its hardest velocity.
    """
    table: dict[str, dict[tuple[int, int], dict[str, float]]] = {}
    for r in rows:
        table.setdefault(r["timbre"], {})[(r["note"], r["velocity"])] = row_features(r)
    for timbre, cells in (extra or {}).items():
        for key, feats in cells.items():
            table.setdefault(timbre, {}).setdefault(key, {}).update(feats)
    for cells in table.values():
        for vel in {v for _, v in cells}:
            at = [k for k in cells if k[1] == vel and "level_db" in cells[k]]
            mid = float(np.median([cells[k]["level_db"] for k in at])) if at else 0.0
            for k in at:
                cells[k]["level_db"] -= mid
        for note in {n for n, _ in cells}:
            vels = sorted(v for n, v in cells if n == note)
            if len(vels) < 2:
                continue
            lo, hi = cells[(note, vels[0])], cells[(note, vels[-1])]
            cells[(note, SWING_KEY)] = {f + SWING: hi[f] - lo[f] for f in lo if f in hi}
    return table


@dataclass(frozen=True)
class Reading:
    feature: str
    register: str
    #: Cells every reference holds, and how many of them the subject holds too.
    asked: int
    got: int
    #: Median distance of a left-out reference from the mean of the rest, in the feature's unit.
    scale: float
    #: Median distance of the subject from that same mean, in the feature's unit.
    distance: float
    #: Signed median of subject minus the mean of all references, in the feature's unit.
    bias: float
    #: Median over the cells of the references' mean: where the family sits, so a share that is
    #: 20 dB out at -70 dB can be told from one that is 20 dB out at -10 dB.
    centre: float

    @property
    def ratio(self) -> float:
        """Subject distance over the family's own; NaN where the family does not disagree at all."""
        return self.distance / self.scale if self.scale > 0.0 else float("nan")


def _register_of(note: int) -> str:
    return next(name for name, lo, hi in REGISTERS if lo <= note <= hi)


def hull(table: dict, references: list[str], subject: str, group=None) -> list[Reading]:
    """Read @p subject against the leave-one-out disagreement of @p references.

    The subject may be one of the references, in which case it is dropped from
    the family first and the reading is how typical a member it is. @p group
    names the pool a cell key belongs to beside "all"; the default is the
    keyboard third of the key's note.
    """
    group = group or (lambda key: _register_of(key[0]))
    refs = [t for t in references if t != subject]
    if len(refs) < 2:
        raise ValueError("a leave-one-out scale needs at least two references besides the subject")
    pooled: dict[tuple[str, str], dict[str, list[float]]] = {}
    for key in sorted(set.intersection(*(set(table[t]) for t in refs))):
        feats = set.intersection(*(set(table[t][key]) for t in refs))
        sub = table.get(subject, {}).get(key, {})
        for f in feats:
            vals = {t: table[t][key][f] for t in refs}
            for pool in sorted({group(key), "all"}):
                slot = pooled.setdefault((f, pool), {"ref": [], "sub": [], "bias": [], "asked": []})
                slot["asked"].append(float(np.mean(list(vals.values()))))
                for out in refs:
                    centre = float(np.mean([vals[t] for t in refs if t != out]))
                    slot["ref"].append(abs(vals[out] - centre))
                    if f in sub:
                        slot["sub"].append(abs(sub[f] - centre))
                if f in sub:
                    slot["bias"].append(sub[f] - float(np.mean(list(vals.values()))))
    out: list[Reading] = []
    for (f, pool), slot in pooled.items():
        out.append(
            Reading(
                feature=f,
                register=pool,
                asked=len(slot["asked"]),
                got=len(slot["bias"]),
                scale=float(np.median(slot["ref"])),
                distance=float(np.median(slot["sub"])) if slot["sub"] else float("nan"),
                bias=float(np.median(slot["bias"])) if slot["bias"] else float("nan"),
                centre=float(np.median(slot["asked"])),
            )
        )
    return out


def typicality(readings: list[Reading], register: str = "all") -> dict[str, float]:
    """One subject's ratios over every feature of one pool, reduced: the null a ranking is read against."""
    ratios = np.array(
        [
            r.ratio
            for r in readings
            if r.register == register and r.asked >= MIN_CELLS and np.isfinite(r.ratio)
        ]
    )
    if not ratios.size:
        return {}
    return {
        "features": int(ratios.size),
        "median": float(np.median(ratios)),
        "p90": float(np.percentile(ratios, 90)),
        "max": float(ratios.max()),
        "over_2": float(np.mean(ratios > 2.0)),
        "over_4": float(np.mean(ratios > 4.0)),
    }


#: Cells below which a ratio is two small medians divided; measured on three grands, a
#: reference read as the subject reaches 8-12x on 5-cell rows and 3x at most on 15 or more.
MIN_CELLS = 8


def report(readings: list[Reading], top: int = 0, min_cells: int = MIN_CELLS) -> str:
    """One line per feature and register, widest ratio first; unanswered cells stay visible."""
    order = sorted(
        (r for r in readings if r.asked >= min_cells),
        key=lambda r: -(r.ratio if np.isfinite(r.ratio) else 0.0),
    )
    lines = [
        f"{'feature':<26}{'register':<8}{'ratio':>7}{'distance':>10}{'family':>9}{'bias':>9}{'centre':>9}{'cells':>9}"
    ]
    for r in order[: top or None]:
        lines.append(
            f"{r.feature:<26}{r.register:<8}{r.ratio:>7.1f}{r.distance:>10.2f}{r.scale:>9.2f}"
            f"{r.bias:>+9.2f}{r.centre:>9.2f}{f'{r.got}/{r.asked}':>9}"
        )
    return "\n".join(lines)
