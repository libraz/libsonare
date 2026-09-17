"""The liveness guard: how a program is derived, and what each verdict means."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import liveness

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
    assert liveness.derive_program(knobs, catalogue)[:3] == (19, "church_organ", 0)


def test_a_spec_scoped_to_a_variation_is_swept_at_the_variation():
    """The capital tone sounds at bank 0, so an override scoped to the variation
    moves nothing there and every knob in the spec would read dead."""
    knobs = [liveness.Knob("church_organ_full.pipe_organ.level", 0.0, 1.0)]
    catalogue = _catalogue({(19, 0): "church_organ", (19, 16): "church_organ_full"})
    assert liveness.derive_program(knobs, catalogue)[:3] == (19, "church_organ_full", 16)


def test_the_engine_stand_in_prefers_a_capital_tone():
    """An engine constant is shared by every patch on the engine, so the choice
    among them should land where a plain GM file reaches -- even when a variation
    sorts ahead of it by name."""
    knobs = [liveness.Knob("pipe_organ_voice.kFoo", 0.0, 1.0)]
    catalogue = _catalogue(
        {(19, 0): "church_organ", (19, 16): "aaa_organ_full"},
        {"church_organ": "pipe_organ", "aaa_organ_full": "pipe_organ"})
    assert liveness.derive_program(knobs, catalogue)[:3] == (19, "church_organ", 0)


def test_an_engine_with_only_variations_still_answers():
    knobs = [liveness.Knob("pipe_organ_voice.kFoo", 0.0, 1.0)]
    catalogue = _catalogue({(19, 16): "church_organ_full"}, {"church_organ_full": "pipe_organ"})
    assert liveness.derive_program(knobs, catalogue)[:3] == (19, "church_organ_full", 16)


def test_an_engine_only_spec_falls_back_to_the_engine_map(tmp_path):
    """`piano_voice.kFoo` names no patch, so the engine the file stem names does."""
    knobs = [liveness.Knob("piano_voice.kTrebleBrightPerOct", 0.0, 0.4)]
    catalogue = _catalogue({(0, 0): "fam0"}, {"fam0": "piano"})
    assert liveness.derive_program(knobs, catalogue)[:2] == (0, "fam0")


def test_a_spec_reaching_neither_map_is_skipped_with_a_reason():
    knobs = [liveness.Knob("nowhere_voice.kFoo", 0.0, 1.0)]
    program, patch, bank, why = liveness.derive_program(knobs, _catalogue({(0, 0): "fam0"}))
    assert (program, patch, bank) == (None, None, 0)
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


def test_a_knob_moving_at_one_velocity_of_two_is_named():
    """The axis the fitting notes name first: a dynamics control on a fixed probe."""
    r = liveness.SpecReport(spec="s.json", live={"dyn": [60], "both": [60]},
                            velocities={"dyn": {100}, "both": {32, 100}})
    assert r.velocity_gated((32, 100)) == ["dyn"]


def test_one_velocity_cannot_gate_anything():
    """With a single velocity probed every knob trivially moves at one of them."""
    r = liveness.SpecReport(spec="s.json", live={"dyn": [60]}, velocities={"dyn": {100}})
    assert r.velocity_gated((100,)) == []


def test_an_excused_knob_is_not_also_reported_velocity_gated():
    r = liveness.SpecReport(spec="s.json", live={"dyn": []},
                            velocities={"dyn": {100}}, excuses={"dyn": "gated"})
    assert r.velocity_gated((32, 100)) == []


def test_a_patch_report_shares_its_inert_count():
    r = liveness.PatchReport(patch="violin", program=40, channel=0,
                             inert=["violin.a", "violin.b"], total=8)
    assert r.share() == 0.25


def test_a_patch_with_no_fields_shares_nothing_rather_than_dividing_by_zero():
    assert liveness.PatchReport(patch="x", program=0, channel=0).share() == 0.0


def _programs(**addr):
    """A program map keyed the way the catalogue reports it: (program, bank)."""
    return SimpleNamespace(programs={v: k for k, v in addr.items()})


def test_a_patch_with_a_bank_zero_address_is_probed_there():
    """That is the address a plain GM file reaches it at."""
    cat = _programs(church_organ=(19, 0))
    cat.programs[(19, 2)] = "church_organ"
    jobs = liveness.census_jobs(cat, (60,), drums=False)
    assert [(j[0], j[1], j[2]) for j in jobs] == [("church_organ", 19, 0)]


def test_a_patch_no_bank_zero_program_reaches_is_still_probed():
    """The thirty GS variations. Probing one at bank 0 offers it none of its own
    knobs, so it left the run without a line rather than with a wrong answer."""
    cat = _programs(church_organ=(19, 0))
    cat.programs[(19, 2)] = "church_organ_full"
    jobs = {j[0]: (j[1], j[2]) for j in liveness.census_jobs(cat, (60,), drums=False)}
    assert jobs == {"church_organ": (19, 0), "church_organ_full": (19, 2)}


def test_a_drum_note_keeps_its_own_channel_and_a_grid_of_one():
    jobs = liveness.census_jobs(_programs(piano=(0, 0)), (60,), drums=True)
    drums = [j for j in jobs if j[5] is not None]
    assert len(drums) == 128
    assert drums[38][3] == liveness.PERCUSSION_CHANNEL
    assert drums[38][4] == (38,)


def test_a_census_records_the_generation_it_was_taken_against(tmp_path, monkeypatch):
    monkeypatch.setattr(liveness, "bank_generation", lambda: 31)
    out = tmp_path / "field-coverage.json"
    liveness.write_census(out, [liveness.PatchReport(
        patch="violin", program=40, channel=0, total=8,
        inert=["violin.bowed_string.stribeck"])], (48, 60), (32, 100))
    d = json.loads(out.read_text())
    assert d["bank_generation"] == 31
    assert d["patches"]["violin"]["inert"] == ["bowed_string.stribeck"]
    assert d["patches"]["violin"]["fields"] == 8
    # Bank 0 is the common case and stays out of the file; a variation carries
    # the address it was probed at, or nothing says which patch was measured.
    assert "bank" not in d["patches"]["violin"]


def test_a_variation_records_the_address_it_was_probed_at(tmp_path, monkeypatch):
    monkeypatch.setattr(liveness, "bank_generation", lambda: 32)
    out = tmp_path / "field-coverage.json"
    liveness.write_census(out, [liveness.PatchReport(
        patch="church_organ_full", program=19, bank=2, channel=0, total=8)], (60,), (100,))
    assert json.loads(out.read_text())["patches"]["church_organ_full"]["bank"] == 2


def test_a_census_matching_the_bank_passes(tmp_path, monkeypatch):
    monkeypatch.setattr(liveness, "bank_generation", lambda: 31)
    out = tmp_path / "c.json"
    out.write_text(json.dumps({"bank_generation": 31}))
    assert liveness.check_census(out) == 0


def test_a_census_older_than_the_bank_fails(tmp_path, monkeypatch):
    """A voice moved since it was measured, so what it says is about another bank."""
    monkeypatch.setattr(liveness, "bank_generation", lambda: 32)
    out = tmp_path / "c.json"
    out.write_text(json.dumps({"bank_generation": 31}))
    assert liveness.check_census(out) == 1


def test_a_missing_census_fails_rather_than_reading_as_clean(tmp_path):
    assert liveness.check_census(tmp_path / "absent.json") == 1


def test_an_empty_spec_claims_no_silent_note():
    """With nothing probed every note is trivially silent, which is not a finding."""
    assert liveness.SpecReport(spec="s.json").silent_notes(NOTES) == []
