"""The liveness guard: how a program is derived, and what each verdict means."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import liveness  # noqa: E402

NOTES = (36, 60, 108)


def _spec(tmp_path: Path, payload, name: str = "spec.json") -> Path:
    path = tmp_path / name
    path.write_text(json.dumps(payload))
    return path


def _catalogue(programs: dict, modes: dict | None = None):
    """Enough of a Catalogue for the derivation: the two maps it walks."""
    return SimpleNamespace(programs=programs, modes=modes or {})


def test_a_knob_without_two_range_ends_is_not_probed(tmp_path):
    """A range of zero width renders the same twice and would read as dead."""
    path = _spec(tmp_path, {"knobs": [
        {"tunable": "a.b", "min": 1.0, "max": 1.0},
        {"tunable": "a.c", "min": 0.0},
        {"tunable": "a.d", "min": 0.0, "max": 1.0},
    ]})
    assert [k.name for k in liveness.spec_entries(path)] == ["a.d"]


def test_a_patch_prefixed_knob_names_its_program():
    knobs = [liveness.Knob("electric_guitar.ks.exc_brightness", 0.0, 1.0)]
    catalogue = _catalogue({(26, 0): "electric_guitar", (0, 0): "fam0"})
    assert liveness.derive_program(knobs, catalogue)[:2] == (26, "electric_guitar")


def test_the_capital_tone_answers_rather_than_a_variation():
    """A variation is a separate patch; the program map is keyed by both."""
    knobs = [liveness.Knob("church_organ.pipe_organ.level", 0.0, 1.0)]
    catalogue = _catalogue({(19, 8): "church_organ", (19, 0): "church_organ"})
    assert liveness.derive_program(knobs, catalogue)[0] == 19


def test_an_engine_only_spec_falls_back_to_the_engine_map(tmp_path):
    """`piano_voice.kFoo` names no patch, so the engine the file stem names does."""
    knobs = [liveness.Knob("piano_voice.kTrebleBrightPerOct", 0.0, 0.4)]
    catalogue = _catalogue({(0, 0): "fam0"}, {"fam0": "piano"})
    assert liveness.derive_program(knobs, catalogue)[:2] == (0, "fam0")


def test_a_spec_reaching_neither_map_is_skipped_with_a_reason():
    knobs = [liveness.Knob("nowhere_voice.kFoo", 0.0, 1.0)]
    program, patch, why = liveness.derive_program(knobs, _catalogue({(0, 0): "fam0"}))
    assert (program, patch) == (None, None)
    assert why


def test_dead_is_live_nowhere_and_partial_is_live_somewhere():
    r = liveness.SpecReport(spec="s.json", live={
        "everywhere": [36, 60, 108],
        "top only": [108],
        "nowhere": [],
    })
    assert r.dead() == ["nowhere"]
    assert r.partial(NOTES) == ["top only"]


def test_a_note_nothing_moves_is_reported_as_the_positive_control():
    """A note the voice does not sound renders silence at both ends of every range."""
    r = liveness.SpecReport(spec="s.json", live={"a": [36, 60], "b": [60]})
    assert r.silent_notes(NOTES) == [108]


def test_a_dead_knob_carrying_a_reason_is_excused_not_failed():
    r = liveness.SpecReport(spec="s.json", live={"gated": []},
                            excuses={"gated": "its switch ships at zero"})
    assert r.dead() == []
    assert r.excused() == ["gated"]


def test_an_excuse_expires_with_the_divergence_it_covers():
    """A knob excused as dead and since come alive keeps asserting a stale decision."""
    r = liveness.SpecReport(spec="s.json", live={"gated": [60]},
                            excuses={"gated": "its switch ships at zero"})
    assert r.stale() == ["gated"]
    assert r.partial(NOTES) == []


def test_an_empty_reason_does_not_excuse():
    """Same rule as the capture's `dimensions_na`: a blank excuse is refused."""
    r = liveness.SpecReport(spec="s.json", live={"gated": []}, excuses={"gated": ""})
    assert r.dead() == ["gated"]


def test_the_reason_is_read_off_the_knob(tmp_path):
    path = _spec(tmp_path, {"knobs": [
        {"tunable": "a.b", "min": 0.0, "max": 1.0, "dead": "gated by a.switch"},
    ]})
    assert liveness.spec_entries(path)[0].excuse == "gated by a.switch"


def test_an_empty_spec_claims_no_silent_note():
    """With nothing probed every note is trivially silent, which is not a finding."""
    assert liveness.SpecReport(spec="s.json").silent_notes(NOTES) == []
