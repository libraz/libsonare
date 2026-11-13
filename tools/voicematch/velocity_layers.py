"""How many independent recordings a reference's velocity grid actually holds.

A sample player commonly answers several velocities with one sample at another
gain, so a note asked at five velocities may hold two recordings. A velocity-swing
reading over such a note has fewer independent points than its grid says. Two
adjacent velocities are one layer when their gain-invariant fields agree exactly:
the h1-relative ladder and the usable decay span. A filter on velocity breaks the
equality, so this counts only layers that are literally the same recording.
"""

from __future__ import annotations

import itertools

#: Dimensions read as a soft-to-hard change, whose independent points are recordings.
VELOCITY_SWING_DIMENSIONS = ("vel_range", "attack_hf_swing")
#: Gain-invariant fields that must agree exactly for two cells to be one recording.
SAME_RECORDING_FIELDS = ("partials_db", "decay_span_s")


def same_recording(a: dict, b: dict) -> bool:
    """Whether two cells of one note are the same sample at different gains."""
    return all(a.get(f) is not None and a.get(f) == b.get(f) for f in SAME_RECORDING_FIELDS)


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


def layer_summary(rows: list[dict]) -> dict:
    """A capture's velocity grid against the recordings behind it."""
    per = note_layers(rows)
    collapsed = sorted(n for n, (asked, held) in per.items() if held < asked)
    return {
        "velocities": sum(a for a, _ in per.values()),
        "independent": sum(h for _, h in per.values()),
        "notes": len(per),
        "collapsed_notes": collapsed,
    }
