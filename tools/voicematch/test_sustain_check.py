"""The sustain check, and the positive control that says it can fire.

The control is not synthetic. Four reed voices drive their bore through a
Bernoulli branch whose loop gain sits under unity, so they ring down instead of
oscillating; the other four of the same engine take the linearised table and
hold. Both sets have committed references that sustain. So the engine supplies
a population with a known answer on both sides, which is what a check of this
shape needs before any number it prints means anything.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sustain_check import EXCESS_TOLERANCE_DB, HOLD_S, check_one, reference_fall

#: Reeds whose bore is driven by the beating branch, and reeds driven by the
#: memoryless table. The split is `closing_pressure > 0` in
#: `gm_fallback_programs_physical.h`, and it is the whole of the difference.
RINGS_DOWN = ("alto_sax", "tenor_sax", "baritone_sax", "bassoon")
HOLDS = ("soprano_sax", "oboe", "english_horn", "clarinet")


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


def test_the_check_separates_the_two_reed_branches():
    """The positive control: it must fire on one half of one engine and not the other.

    Without this the check is unfalsifiable — a run that compares nothing, or
    one whose tolerance sits above every real defect, prints the same clean
    summary as a bank with nothing wrong in it.
    """
    import tempfile

    from render_model import DEFAULT_DYLIB

    if not DEFAULT_DYLIB.exists():
        pytest.skip("no built library in this checkout, so nothing can be rendered")

    with tempfile.TemporaryDirectory(prefix="sustain-control-") as tmp:
        scratch = Path(tmp)
        rang = {i: check_one(i, scratch) for i in RINGS_DOWN}
        held = {i: check_one(i, scratch) for i in HOLDS}

    for name, r in {**rang, **held}.items():
        assert r["status"] == "compared", f"{name} did not reach a verdict: {r['status']}"

    for name, r in rang.items():
        assert r["beyond_tolerance"], (
            f"{name} drives its bore through the beating branch and rings down, "
            f"but the check passed it at excess {r['excess_db']:+.1f} dB")
        assert r["excess_db"] < 0.0, f"{name} should fall SHORT of its reference"

    for name, r in held.items():
        assert not r["beyond_tolerance"], (
            f"{name} holds its note and its reference holds too, but the check "
            f"flagged it at excess {r['excess_db']:+.1f} dB")

    # The two populations have to stand CLEAR of the tolerance on both sides,
    # not merely fall the right side of it. A threshold sitting half a decibel
    # from a real case is fitted to the sample it was read from, and the next
    # calibration round moves the voice across it without moving the defect.
    worst_holder = max(abs(r["excess_db"]) for r in held.values())
    best_ringer = min(abs(r["excess_db"]) for r in rang.values())
    assert worst_holder * 2.0 < EXCESS_TOLERANCE_DB, (
        f"the holders reach {worst_holder:.1f} dB against a {EXCESS_TOLERANCE_DB:.0f} dB "
        f"tolerance — too close to call one of them a pass")
    assert best_ringer > EXCESS_TOLERANCE_DB * 1.5, (
        f"the ring-downs start at {best_ringer:.1f} dB against a {EXCESS_TOLERANCE_DB:.0f} dB "
        f"tolerance — too close to call one of them a failure")
