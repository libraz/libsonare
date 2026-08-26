"""Connectivity before improvement: the sweep and the verdict it prints."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shape import reach as R  # noqa: E402
from shape.render import read_overrides  # noqa: E402

BASE = {"a": 1.0, "b": 2.0, "dead": 4.0}


def sweep(respond):
    """Run the probe with a measurement function instead of a renderer.

    Nothing here needs audio: what is under test is the bookkeeping that turns
    a set of readings into a verdict, and a real render would only make the
    answer depend on a build directory.
    """
    return R.reach(None, BASE, {}, notes=(42,), velocity=100,
                   steps=(0.5, 2.0), workers=2, log=lambda _m: None,
                   measure=lambda _loss, ov, _n, _v: respond(read_overrides(ov)))


def test_a_bucket_no_coordinate_moves_is_the_finding():
    err0, movement, best, mover = sweep(lambda _ov: {"stuck": 9.0})
    assert err0["stuck"] == 9.0
    assert movement["stuck"] == 0.0
    assert "UNREACHABLE" in R.report(err0, movement, best, mover)


def test_a_bucket_something_moves_but_nothing_improves_is_the_opposite_finding():
    """After a write-back this is what a well-modelled voice looks like.

    Reading improvement alone would call it deficient; the movement column is
    what says the mechanism is present and already spent.
    """
    def respond(ov):
        # Any move makes it worse, in both directions.
        return {"spent": 5.0 + 3.0 * len(ov)}

    err0, movement, best, mover = sweep(respond)
    line = [ln for ln in R.report(err0, movement, best, mover).splitlines()
            if ln.startswith("spent")][0]
    assert "moves, no gain" in line
    assert movement["spent"] >= R.DEAD_DB


def test_a_bucket_a_coordinate_reduces_is_reachable_and_names_its_mover():
    def respond(ov):
        return {"fixable": 0.2 if ov.get("b") == 4.0 else 8.0}

    err0, movement, best, mover = sweep(respond)
    assert best["fixable"] == 0.2
    assert mover["fixable"] == "b"
    line = [ln for ln in R.report(err0, movement, best, mover).splitlines()
            if ln.startswith("fixable")][0]
    assert "reachable" in line and "moves, no gain" not in line


def test_movement_under_the_dead_threshold_still_counts_as_unreachable():
    """A coordinate that grazes a bucket has not touched it.

    Without a threshold every bucket reads reachable, because floating-point
    noise in a render is never exactly zero, and the column that was supposed
    to separate a missing mechanism from a spent one separates nothing.
    """
    def respond(ov):
        return {"graze": 1.0 + (R.DEAD_DB / 2 if ov else 0.0)}

    err0, movement, best, mover = sweep(respond)
    assert 0.0 < movement["graze"] < R.DEAD_DB
    assert "UNREACHABLE" in R.report(err0, movement, best, mover)


def test_a_coordinate_at_zero_is_swept_by_the_ladder_rather_than_by_multiples():
    """No multiple of zero is anything else, so a zeroed mechanism reads inert.

    That is the shape of a false negative this harness can produce on its own:
    a switch that is off looks like a switch that does nothing.
    """
    seen: list[float] = []

    def respond(ov):
        seen.append(ov.get("off", 0.0))
        return {"bucket": 1.0}

    R.reach(None, {"off": 0.0}, {}, notes=(42,), velocity=100,
            steps=(0.5, 2.0), zero_ladder=(0.15, 0.4), workers=1,
            log=lambda _m: None,
            measure=lambda _loss, ov, _n, _v: respond(read_overrides(ov)))
    assert sorted(v for v in seen if v) == [0.15, 0.4]


def test_the_namespace_filter_keeps_only_the_piece_being_probed(tmp_path):
    """A kit's coordinates are per note, and sweeping the whole kit for one
    piece spends every render on knobs that cannot reach it."""
    dump = tmp_path / "knobs.tsv"
    dump.write_text("d042.percussion.tone_gain\t4.0\n"
                    "d046.percussion.tone_gain\t1.5\n"
                    "hat_voice.kEdge\t0.25\n")

    class Args:
        knobs = str(dump)

    got = R.coordinates(Args(), {}, None, (42,), True, ("d042.",))
    assert got == {"d042.percussion.tone_gain": 4.0}
