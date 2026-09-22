"""The sustain check, and the positive control that says it can fire.

The control has two halves because the check has two ways of going quiet, and
one specimen cannot cover both. *Reach* is whether real renders arrive at a
verdict at all, and only real voices can answer it. *Sensitivity* is whether the
tolerance sits above every defect it is supposed to catch, and that is answered
with audio this file builds, because a specimen named in the bank is retired the
day somebody fixes it. That happened: the control used to name four reed voices
whose bore rang down, and a calibration round moved all four to the other side,
leaving an assertion that read as permanent standing over an empty population.
The bank supplies the reeds here as a regression guard on that round, not as the
proof that the check can fire.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sustain_check
from sustain_check import (
    EARLY_S,
    EXCESS_TOLERANCE_DB,
    HOLD_S,
    LATE_S,
    ONSET_S,
    SR,
    check_one,
    reference_fall,
)

#: The reed engine's two branches, split by `closing_pressure > 0` in
#: `gm_fallback_programs_physical.h` — the same field `kPathSelectors` uses to
#: gate the two sides in the gesture golden. Both branches sustain, and both
#: carry committed references that sustain, so every one of them belongs to the
#: population this check judges.
BEATING_BRANCH = ("alto_sax", "tenor_sax", "baritone_sax", "bassoon")
TABLE_BRANCH = ("soprano_sax", "oboe", "english_horn", "clarinet")

#: A voice whose committed reference is still sounding at the end of the hold,
#: so the synthesized specimens below are compared against a real target rather
#: than against a number this file also chose.
SPECIMEN_ID = "clarinet"


def _held_tone(fall_db: float) -> np.ndarray:
    """Audio the check will read as falling `fall_db`, for injection in place of a render.

    The envelope is written against the two windows the check actually reads
    rather than against the hold, so the number asked for is the number it
    reports and a specimen can be placed at the tolerance on purpose. The
    carrier only has to carry energy — what is read is two short RMS windows.
    """
    n = int((ONSET_S + HOLD_S + 1.0) * SR)
    t = np.arange(n) / SR
    span = np.clip((t - EARLY_S) / (LATE_S - EARLY_S), 0.0, None)
    return 0.2 * 10.0 ** (fall_db * span / 20.0) * np.sin(2.0 * np.pi * 220.0 * t)


def test_the_reference_fall_comes_from_the_note_being_rendered():
    """A decay rate belongs to the register, so the row for that note wins."""
    rows = [
        {"note": 48, "decay_db_s": -10.0},
        {"note": 60, "decay_db_s": -1.0},
        {"note": 72, "decay_db_s": -20.0},
    ]
    assert reference_fall(rows, 60) == pytest.approx(-1.0 * HOLD_S)
    # A note the grid does not carry falls back to the grid, rather than to zero:
    # "no row here" is not "does not decay".
    assert reference_fall(rows, 55) == pytest.approx(-10.0 * HOLD_S)


def test_a_reference_carrying_no_rate_is_unjudgeable_rather_than_flat():
    assert reference_fall([{"note": 60}], 60) is None
    assert reference_fall([], 60) is None


def test_the_excess_is_signed_so_the_check_is_two_sided():
    """Holding where the reference lets go is wrong the same way round.

    A reference easing off 2 dB/s against a model that does not move is 4 dB of
    excess the positive way; the same gap negative is the ring-down case. One
    threshold on the absolute value, so neither direction needs its own.
    """
    rows = [{"note": 60, "decay_db_s": -2.0}]
    ref = reference_fall(rows, 60)
    assert ref == pytest.approx(-4.0, abs=0.01)
    assert 0.0 - ref == pytest.approx(4.0, abs=0.01)
    # And the far side: a model that stops while the reference holds.
    assert abs(-30.0 - reference_fall([{"note": 60, "decay_db_s": 0.0}], 60)) > EXCESS_TOLERANCE_DB


def test_the_check_fires_on_a_bore_that_rings_down_and_not_on_one_that_holds(monkeypatch):
    """Sensitivity: the tolerance has to sit under a defect and over a good voice.

    Without this the check is unfalsifiable — a tolerance above every real
    defect prints the same clean summary as a bank with nothing wrong in it.
    The two specimens are built here rather than named in the bank so that the
    control owns both sides of its own boundary; everything downstream of the
    render is the shipped code, including the reference this is scored against.
    """

    def verdict(fall_db: float) -> dict:
        monkeypatch.setattr(sustain_check, "render_held", lambda *a, **k: _held_tone(fall_db))
        return check_one(SPECIMEN_ID, Path("/nonexistent"))

    holds = verdict(0.0)
    assert holds["status"] == "compared", holds["status"]
    assert not holds["beyond_tolerance"], (
        f"a bore that does not move was flagged at excess {holds['excess_db']:+.1f} dB"
    )

    rings_down = verdict(-(EXCESS_TOLERANCE_DB * 3.0))
    assert rings_down["beyond_tolerance"], (
        f"a bore falling {EXCESS_TOLERANCE_DB * 3.0:.0f} dB under a reference that holds "
        f"was passed at excess {rings_down['excess_db']:+.1f} dB"
    )
    assert rings_down["excess_db"] < 0.0, "a ring-down falls SHORT of its reference"

    # And the boundary is where the constant says it is, in both directions.
    # A tolerance that is really an order of magnitude away from where it reads
    # would pass both assertions above and neither of these.
    assert not verdict(-(EXCESS_TOLERANCE_DB - 2.0))["beyond_tolerance"]
    assert verdict(-(EXCESS_TOLERANCE_DB + 2.0))["beyond_tolerance"]


def test_both_reed_branches_reach_a_verdict_and_hold_their_note():
    """Reach, and the regression guard on the branch that used to ring down.

    Only real renders can say that the check gets as far as a verdict, and the
    reeds are the population to ask because one branch of them is where it last
    failed to: all four beating-bore voices sat outside the band the Bernoulli
    valve oscillates over and fell away under the gate. They are here to notice
    that returning, not to demonstrate that the check can fire — this asserts
    they all PASS, so it goes red if any of them stops sustaining again.
    """
    import tempfile

    from render_model import DEFAULT_DYLIB

    if not DEFAULT_DYLIB.exists():
        pytest.skip("no built library in this checkout, so nothing can be rendered")

    with tempfile.TemporaryDirectory(prefix="sustain-control-") as tmp:
        scratch = Path(tmp)
        seen = {i: check_one(i, scratch) for i in BEATING_BRANCH + TABLE_BRANCH}

    for name, r in seen.items():
        assert r["status"] == "compared", f"{name} did not reach a verdict: {r['status']}"
        assert not r["beyond_tolerance"], (
            f"{name} no longer holds its note: excess {r['excess_db']:+.1f} dB"
        )

    # Passing is not enough — they have to stand CLEAR of the tolerance. A voice
    # sitting half a decibel inside it is one calibration round from crossing
    # without anything having gone wrong with it.
    worst = max(abs(r["excess_db"]) for r in seen.values())
    assert worst * 2.0 < EXCESS_TOLERANCE_DB, (
        f"the reeds reach {worst:.1f} dB against a {EXCESS_TOLERANCE_DB:.0f} dB "
        f"tolerance — too close to call one of them a pass"
    )
