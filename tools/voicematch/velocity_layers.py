"""How many independent recordings a reference's velocity grid actually holds.

A sample player commonly answers several velocities with one sample at another
gain, so a note asked at five velocities may hold two recordings. A velocity-swing
reading over such a note has fewer independent points than its grid says. Two
adjacent velocities are one layer when their spectral ladder (a melodic row's
h1-relative partials, a kit row's band ladder) agrees up to a constant offset: a
gain moves every rung alike, a filter or a different strike does not.

Separately, a capture whose level follows one velocity exponent on every note,
at the sampler's own law, is reading the sampler rather than strikes of different
strength (`sampler_law`), whatever the count of recordings under it.
"""

from __future__ import annotations

import itertools
import math

#: Dimensions read as a soft-to-hard change, whose independent points are recordings.
VELOCITY_SWING_DIMENSIONS = ("vel_range", "attack_hf_swing")
#: The spectral ladder of a melodic row and of a kit row.
LADDER_FIELDS = ("partials_db", "bands_db")
#: Rungs that must be read on both sides before two ladders can be called one.
MIN_RUNGS = 4
#: The profile stores each rung rounded to this step, so differences are compared in steps.
LADDER_STEP_DB = 0.01
#: Two independently rounded rungs each sit within half a step of their true value, so a
#: pure gain's rung-by-rung difference can spread by up to two steps.
SAME_LADDER_STEPS = 2


def _ladder(row: dict) -> list | None:
    return next((row[f] for f in LADDER_FIELDS if row.get(f)), None)


def same_recording(a: dict, b: dict) -> bool:
    """Whether two cells of one note are one recording at different gains."""
    la, lb = _ladder(a), _ladder(b)
    if la is None or lb is None or len(la) != len(lb):
        return False
    diffs = [
        round(y / LADDER_STEP_DB) - round(x / LADDER_STEP_DB)
        for x, y in zip(la, lb, strict=True)
        if x is not None and y is not None and x > -119.0 and y > -119.0
    ]
    return len(diffs) >= MIN_RUNGS and max(diffs) - min(diffs) <= SAME_LADDER_STEPS


def note_layers(rows: list[dict]) -> dict[int, tuple[int, int]]:
    """Per note: (velocities asked, independent recordings among them)."""
    by: dict[int, list[dict]] = {}
    for r in rows:
        by.setdefault(r["note"], []).append(r)
    out = {}
    for note, cells in by.items():
        cells.sort(key=lambda r: r["velocity"])
        layers = 1 + sum(not same_recording(a, b) for a, b in itertools.pairwise(cells))
        out[note] = (len(cells), layers)
    return out


#: The SoundFont default velocity law: amplitude as velocity squared (40 log10 v dB).
SAMPLER_LAW_EXPONENT = 2.0
#: How near the sampler's exponent, and how alike across notes, a capture must be to read it.
SAMPLER_LAW_TOLERANCE = 0.15
#: Velocities a note needs before its exponent is read.
MIN_LAW_VELOCITIES = 3


def note_exponents(rows: list[dict]) -> dict[int, float]:
    """Per note, the amplitude-versus-velocity exponent: the slope of peak dB on 20 log10 v."""
    by: dict[int, list[tuple[float, float]]] = {}
    for r in rows:
        if r.get("peak_dbfs") is not None and r.get("velocity"):
            by.setdefault(r["note"], []).append((20.0 * math.log10(r["velocity"]), r["peak_dbfs"]))
    out = {}
    for note, pts in by.items():
        if len(pts) < MIN_LAW_VELOCITIES:
            continue
        mx = sum(x for x, _ in pts) / len(pts)
        my = sum(y for _, y in pts) / len(pts)
        den = sum((x - mx) ** 2 for x, _ in pts)
        if den > 0.0:
            out[note] = sum((x - mx) * (y - my) for x, y in pts) / den
    return out


def sampler_law(exponents: dict[int, float]) -> bool:
    """One exponent on every note, at the sampler's own value."""
    if len(exponents) < 2:
        return False
    vals = sorted(exponents.values())
    median = vals[len(vals) // 2]
    return (
        vals[-1] - vals[0] <= 2 * SAMPLER_LAW_TOLERANCE
        and abs(median - SAMPLER_LAW_EXPONENT) <= SAMPLER_LAW_TOLERANCE
    )


def layer_summary(rows: list[dict]) -> dict:
    """A capture's velocity grid against the recordings and the law behind it."""
    per = note_layers(rows)
    collapsed = sorted(n for n, (asked, held) in per.items() if held < asked)
    exponents = note_exponents(rows)
    return {
        "velocities": sum(a for a, _ in per.values()),
        "independent": sum(h for _, h in per.values()),
        "notes": len(per),
        "collapsed_notes": collapsed,
        "exponents": {n: round(e, 3) for n, e in sorted(exponents.items())},
        # A constant law at another value is a fixed velocity curve too, just not the default.
        "exponent_spread": round(max(exponents.values()) - min(exponents.values()), 3)
        if exponents
        else None,
        "sampler_law": sampler_law(exponents),
    }
