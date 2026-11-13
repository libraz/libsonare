"""A knob range against the engine clamp: refusal, warning, unbounded, stored vs effective."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from catalogue import Catalogue
from knobs import Knob, clamp_check, clamp_warning, knob_bounds, stored_vs_effective

OSC2 = "synth0.va.osc2_interval_cents"
KEYTRACK = "synth0.filter.key_track"
ENGINE = "va.some_constant"


def _catalogue() -> Catalogue:
    return Catalogue(
        defaults={OSC2: 0.0, KEYTRACK: 0.5, ENGINE: 1.0},
        programs={(88, 0): "synth0"},
        # Same dotted shape as a patch field, but an engine constant: never bounded.
        bounds={"va.osc2_interval_cents": (-2400.0, 2400.0), "filter.key_track": (0.0, 1.0)},
        modes={"synth0": "va"},
    )


def _knob(label, lo, hi, start) -> Knob:
    return Knob(label=label, lo=lo, hi=hi, log=False, start_value=start, tunable=label)


def _check(*knobs):
    return clamp_check(list(knobs), knob_bounds(list(knobs), _catalogue()))


def test_range_wholly_outside_the_clamp_is_refused():
    entries, refusals = _check(_knob(OSC2, 3550.0, 3650.0, 3600.0))
    assert entries == []
    assert len(refusals) == 1
    assert OSC2 in refusals[0] and "3550" in refusals[0] and "2400" in refusals[0]


def test_range_partly_outside_warns_and_records_the_inert_part():
    entries, refusals = _check(_knob(KEYTRACK, 0.0, 1.2, 0.5))
    assert refusals == []
    (entry,) = entries
    assert entry["status"] == "partly_outside"
    assert entry["inert"] == [[1.0, 1.2]]
    assert entry["clamp"] == [0.0, 1.0]
    assert not entry["start_outside_clamp"]
    assert "[1, 1.2]" in clamp_warning(entry)


def test_start_outside_the_clamp_is_reported():
    (entry,) = _check(_knob(KEYTRACK, 0.0, 1.2, 1.1))[0]
    assert entry["start_outside_clamp"]
    assert "start 1.1" in clamp_warning(entry)


def test_unbounded_tunable_is_reported_not_refused():
    entries, refusals = _check(_knob(ENGINE, 0.1, 10.0, 1.0))
    assert refusals == []
    assert entries == [{"key": ENGINE, "status": "unbounded", "range": [0.1, 10.0]}]
    assert "unbounded" in clamp_warning(entries[0])


def test_in_range_knob_has_no_entry():
    knobs = [_knob(OSC2, -1200.0, 1200.0, 0.0), _knob(KEYTRACK, 0.0, 1.0, 0.5)]
    bounds = knob_bounds(knobs, _catalogue())
    assert clamp_check(knobs, bounds) == ([], [])
    assert stored_vs_effective(knobs, [1200.0, 1.0], bounds) == []


def test_stored_vs_effective_for_a_value_past_the_clamp():
    knobs = [_knob(OSC2, -4000.0, 4000.0, 0.0)]
    bounds = knob_bounds(knobs, _catalogue())
    (row,) = stored_vs_effective(knobs, [3608.0], bounds)
    assert row == {"key": OSC2, "stored": 3608.0, "effective": 2400.0, "clamp": [-2400.0, 2400.0]}
