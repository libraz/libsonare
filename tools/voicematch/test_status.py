"""The bank status view: the ladder's predicates, and that its check can fail."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import status  # noqa: E402
from toneclass import (  # noqa: E402
    CANONICAL_DIMENSIONS, PERCUSSION_DIMENSIONS, ToneClass, canonical_dimensions,
)


# --------------------------------------------------------------------------- #
# The generated file
# --------------------------------------------------------------------------- #

def _shipped() -> list[dict]:
    if not status.OUT_PATH.is_file():
        pytest.skip("tools/voice-status.json not generated in this tree")
    return json.loads(status.OUT_PATH.read_text())["voices"]


def test_the_shipped_file_covers_the_whole_bank():
    """128 programs plus their voiced-apart variations plus a kit, not four."""
    rows = _shipped()
    assert len({r["program"] for r in rows if not r["kit"]}) == 128
    assert any(r["kit"] for r in rows)


def test_every_row_carries_the_facts_its_stage_was_read_from():
    """A stage with no axes behind it is a number nobody can argue with."""
    for r in _shipped():
        assert set(r["axes"]) >= {"engine", "patch", "timbres", "profile_rows",
                                  "gate_state", "coverage", "agreement"}
        assert r["stage_name"] == status.STAGES[round(r["stage"] * 5)]
        assert r["next"]


def test_a_kit_is_not_voiced_by_its_programs_melodic_patch():
    """Program 0 answers `piano` on channel 1 and a drum kit on channel 10.

    Reading the engine off the program map gives the melodic one, which is how
    the standard kit first reported as a piano.
    """
    kits = [r for r in _shipped() if r["kit"]]
    assert kits
    for r in kits:
        assert r["engine"] != "piano"


# --------------------------------------------------------------------------- #
# The ladder
# --------------------------------------------------------------------------- #

def _axes(**over) -> dict:
    base = {
        "engine": "piano", "patch": "fam0", "timbres": 3, "profile_rows": 180,
        "gate_state": "current",
        "coverage": {"complete": True, "gaps": []},
        "agreement": {"inside": 8, "total": 11, "outside": {}},
        "structure": None, "music": None,
    }
    base.update(over)
    return base


def test_a_family_patch_on_a_physical_engine_is_a_deliberate_voice():
    """`fam0` is the piano family ON the piano engine, not an unvoiced default."""
    assert status.stage_for(_axes()) > 0


def test_a_family_patch_on_the_default_engine_is_untouched():
    assert status.stage_for(_axes(engine="subtractive", patch="fam10")) == 0


def test_a_named_patch_on_the_default_engine_is_not_untouched():
    """A subtractive synth lead was chosen; a subtractive family fallback was not."""
    assert status.stage_for(_axes(engine="subtractive", patch="orchestra_hit")) > 0


def test_one_reference_timbre_cannot_reach_the_measured_step():
    """With one timbre there is no spread, so no dimension can be adjudicated."""
    assert status.stage_for(_axes(timbres=1)) == 1


def test_a_stale_gate_does_not_count_as_a_gate():
    assert status.stage_for(_axes(gate_state="stale")) == 1


def test_coverage_is_all_or_nothing():
    """One unexcused gap holds the voice below `covered`, whatever the rest are."""
    assert status.stage_for(_axes(coverage={"complete": False, "gaps": ["body"]})) == 2


def test_a_minority_inside_the_spread_does_not_agree():
    assert status.stage_for(
        _axes(agreement={"inside": 1, "total": 3, "outside": {"stretch": 1.8}})) == 3


def test_nothing_reaches_settled_while_the_last_two_facts_are_unrecorded():
    """Unknown is not satisfied: neither a diagnose nor a sign-off is recorded."""
    assert status.stage_for(_axes()) == 4


def test_an_open_candidate_does_not_demote_a_voice():
    """A candidate nobody adopted means there may be more to gain — not that
    what shipped is worse than it was."""
    rows = _shipped()
    piano = next(r for r in rows if r["slug"] == "p000-acoustic-grand-piano")
    assert piano["open_candidates"]
    assert piano["stage"] >= 0.8


# --------------------------------------------------------------------------- #
# Agreement
# --------------------------------------------------------------------------- #

def test_an_empty_spread_is_unjudgeable_rather_than_disagreeing():
    """One captured timbre gives no spread. Reporting that as "0 of 8 agree"
    would read as a voice that is wrong everywhere rather than one nothing has
    been able to check."""
    got = status.gate_agreement(
        {"reference_spread": {}, "bounds": {"attack": {"median": 40.0}}})
    assert got["total"] == 0
    assert got["unjudgeable"] == ["attack"]


def test_a_bound_inside_the_spread_agrees():
    got = status.gate_agreement({
        "reference_spread": {"attack": 25.0, "stereo": 0.229},
        "bounds": {"attack": {"median": 25.0}, "stereo": {"median": 0.414}},
    })
    assert got["inside"] == 1
    assert got["total"] == 2
    assert got["outside"]["stereo"] == pytest.approx(1.81, abs=0.01)


# --------------------------------------------------------------------------- #
# Canonical dimensions
# --------------------------------------------------------------------------- #

def test_a_sustained_voice_is_not_judged_on_a_free_decay_it_does_not_have():
    canon = canonical_dimensions(19)
    assert "decay" not in canon and "aftersound" not in canon
    assert "damper" in canon


def test_a_modal_voice_is_not_judged_against_equal_temperament():
    """A bar or a bell has no series equal temperament predicts."""
    assert "stretch" not in canonical_dimensions(9)


def test_a_kit_uses_the_percussion_vocabulary():
    """A band profile and no ladder, whatever the piece."""
    canon = canonical_dimensions(0, percussive=True)
    assert canon == PERCUSSION_DIMENSIONS
    assert "band_tilt" in canon
    assert "stretch" not in canon


def test_every_class_names_at_least_one_dimension():
    for cls in ToneClass:
        assert CANONICAL_DIMENSIONS[cls]


# --------------------------------------------------------------------------- #
# Excusing a dimension
# --------------------------------------------------------------------------- #

def test_an_exclusion_argued_only_in_prose_reads_as_a_gap():
    """`_dimensions` is a comment. Coverage reads `dimensions_na`, so an
    exclusion has to be data before it counts as one."""

    class V:
        program, kit = 0, False

    prose_only = {"_dimensions": "damper is out because the references disagree"}
    assert "damper" in status.coverage(V(), prose_only, {"bounds": {}})["gaps"]


def test_an_excused_dimension_completes_coverage():
    class V:
        program, kit = 0, False

    canon = canonical_dimensions(0)
    gate = {"bounds": {d: {"median": 1.0} for d in canon if d != "damper"}}
    assert status.coverage(V(), {}, gate)["gaps"] == ["damper"]
    excused = {"dimensions_na": {"damper": "the references disagree by more than the model does"}}
    assert status.coverage(V(), excused, gate)["complete"]


# --------------------------------------------------------------------------- #
# The staleness check, which is worth nothing if it cannot go red
# --------------------------------------------------------------------------- #

def test_the_check_fails_on_a_stale_file(tmp_path, monkeypatch, capsys):
    rows = _shipped()
    moved = json.loads(json.dumps(rows))
    moved[0]["stage"] = 0.0
    path = tmp_path / "voice-status.json"
    path.write_text(json.dumps({"voices": moved}, indent=2, ensure_ascii=False) + "\n")
    monkeypatch.setattr(status, "OUT_PATH", path)
    monkeypatch.setattr(status, "build", lambda catalogue: rows)
    monkeypatch.setattr(status.catalogue_mod, "dump_catalogue",
                        lambda *a, **k: None)
    monkeypatch.setattr(sys, "argv", ["status.py", "--check"])
    assert status.main() == 1
    assert "stale" in capsys.readouterr().out


def test_the_check_passes_on_a_current_file(tmp_path, monkeypatch):
    rows = _shipped()
    path = tmp_path / "voice-status.json"
    path.write_text(json.dumps({"voices": rows}, indent=2, ensure_ascii=False) + "\n")
    monkeypatch.setattr(status, "OUT_PATH", path)
    monkeypatch.setattr(status, "build", lambda catalogue: rows)
    monkeypatch.setattr(status.catalogue_mod, "dump_catalogue", lambda *a, **k: None)
    monkeypatch.setattr(sys, "argv", ["status.py", "--check"])
    assert status.main() == 0
