"""The bank status view: the ladder's predicates, and that its check can fail."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import signoff
import status
from toneclass import (
    CANONICAL_DIMENSIONS,
    PERCUSSION_DIMENSIONS,
    ToneClass,
    canonical_dimensions,
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
# The policy, resolved at print time
# --------------------------------------------------------------------------- #

CORE = {"name": "core", "rank": 1, "programs": [0, 3], "kits": [8]}
COMMON = {"name": "common", "rank": 2, "default": True}
POLICY = {"tiers": [CORE, COMMON]}


def test_a_kit_number_does_not_rank_the_melodic_program_of_that_number():
    """`kits` and `programs` are two number spaces over the same integers.

    A kit is named by the rhythm-part program a file selects it with, so kit 8
    is the Room set and program 8 is the celesta. Resolved as one list, a tier
    naming the Room kit also promotes the celesta to the top of the bank.
    """
    assert status.tier_of(POLICY, 8, 0, kit=True) == (1, "core")
    assert status.tier_of(POLICY, 8, 0, kit=False) == (2, "common")


def test_a_melodic_program_is_not_ranked_by_the_kit_list_either_way():
    assert status.tier_of(POLICY, 0, 0, kit=False) == (1, "core")
    assert status.tier_of(POLICY, 0, 0, kit=True) == (2, "common")


def test_a_variation_is_ranked_with_its_capital():
    """It is the capital copied and narrowed, so it has no priority of its own."""
    assert status.tier_of(POLICY, 3, 8, kit=False) == (1, "core")


def test_no_policy_at_all_ranks_nothing_rather_than_ranking_everything_first():
    """Rank 0 renders as `t-`; a 1 would report the long tail as the most urgent."""
    assert status.tier_of({}, 0, 0, kit=False) == (0, "unranked")


def test_a_documentation_key_inside_goals_is_not_read_as_a_goal():
    """`_` documents a block in every other JSON file here — calibrations.json,
    signoff.json, each capture definition — so a note written into this one is
    what a reader would expect to be able to do. Read as a goal it has no
    `stage`, and the only tool that reads the policy died on it."""
    pol = {"goals": {"_": "which release asks for what",
                     "1.8.0": {"stage": 0.8, "programs": [0]}}}
    rows = [{"program": 0, "bank": 0, "kit": False, "stage": 0.6, "slug": "p000"}]
    assert [g["name"] for g in status.goal_progress(pol, rows)] == ["1.8.0"]


def test_a_goal_names_capital_tones_and_kits_and_never_a_variation():
    """A variation is not something a file is owed, and one listed here would
    block the goal on a capture nobody has a source for."""
    rows = [{"program": 0, "bank": 0, "kit": False, "stage": 0.6, "slug": "capital"},
            {"program": 0, "bank": 8, "kit": False, "stage": 0.2, "slug": "variation"}]
    members = status.goal_members({"programs": [0]}, rows)
    assert [r["slug"] for r in members] == ["capital"]


# --------------------------------------------------------------------------- #
# What the policy adds to a next action, and why it is added at print time
# --------------------------------------------------------------------------- #

def _row(slug, program, bank=0, stage=0.2, nxt="capture an oracle: no reference exists "
         "for this voice", kit=False):
    return {"slug": slug, "program": program, "bank": bank, "kit": kit,
            "stage": stage, "next": nxt}


def test_an_approximated_slot_is_terminal_rather_than_uncaptured():
    """Its oracle is never going to be captured: the bank does not have the
    mechanism and is deliberately answering with the nearest voice it has. The
    generated answer says `capture an oracle`, which is the one instruction
    that will never be carried out for this slot."""
    pol = {"approximated": {"p105-banjo": {"answered_by": "p024-nylon-guitar",
                                           "reason": "no plucked-membrane mechanism"}}}
    rows = [_row("p105-banjo", 105)]
    got = status.resolved_next(rows[0], pol, rows)
    assert "terminal" in got
    assert "p024-nylon-guitar" in got
    assert "no plucked-membrane mechanism" in got
    assert "capture an oracle" not in got


def test_a_slot_the_policy_says_nothing_about_keeps_the_generated_answer():
    rows = [_row("p105-banjo", 105)]
    assert status.resolved_next(rows[0], {}, rows) == rows[0]["next"]


def test_an_approximation_that_is_not_an_object_is_not_read_as_one():
    """An entry shape the policy check refuses must not change an answer here."""
    rows = [_row("p105-banjo", 105)]
    pol = {"approximated": {"p105-banjo": "no mechanism"}}
    assert status.resolved_next(rows[0], pol, rows) == rows[0]["next"]


def test_a_variation_behind_an_unheard_capital_is_told_not_to_start():
    """Starting one first buys a round of rework — `variations_follow_capital`."""
    rows = [_row("p000-piano", 0, stage=0.6), _row("p000b008-piano-w", 0, bank=8)]
    got = status.resolved_next(rows[1], {"variations_follow_capital": True}, rows)
    assert "p000-piano" in got
    assert "nothing here moves until it is heard" in got


def test_a_variation_whose_capital_is_heard_is_told_to_capture_it():
    rows = [_row("p000-piano", 0, stage=0.8), _row("p000b008-piano-w", 0, bank=8)]
    got = status.resolved_next(rows[1], {"variations_follow_capital": True}, rows)
    assert "capture this variation from the module" in got


def test_the_rule_is_read_from_the_policy_rather_than_assumed():
    """A rule read from nowhere is one the policy cannot turn off."""
    rows = [_row("p000-piano", 0, stage=0.6), _row("p000b008-piano-w", 0, bank=8)]
    assert status.resolved_next(rows[1], {}, rows) == rows[1]["next"]
    assert status.capital_of(rows[1], rows, {}) is None
    assert status.capital_of(rows[1], rows, {"variations_follow_capital": True}) is rows[0]


def test_a_capital_is_not_its_own_capital_and_a_kit_has_none():
    pol = {"variations_follow_capital": True}
    rows = [_row("p000-piano", 0), _row("kit000-standard-kit", 0, kit=True)]
    assert status.capital_of(rows[0], rows, pol) is None
    assert status.capital_of(rows[1], rows, pol) is None


def test_the_variation_queue_is_split_by_what_each_half_needs():
    """A flat queue says neither, and the two are different work."""
    pol = {"variations_follow_capital": True}
    rows = [
        _row("p000-piano", 0, stage=0.6), _row("p000b008-a", 0, bank=8),
        _row("p019-organ", 19, stage=0.8), _row("p019b008-b", 19, bank=8),
        _row("p019b016-c", 19, bank=16),
    ]
    assert status.variation_split(rows, pol) == (1, 2)
    assert status.variation_split(rows, {}) == (0, 0)


def test_the_policy_resolved_answer_never_reaches_the_generated_file():
    """`next` is generated and the policy is not. Resolved at print time for the
    reason `tier_of` gives: baking a decision in would make the generated file
    stale every time the policy moved with no voice having changed."""
    for row in _shipped():
        assert "approximated" not in row["next"]
        assert "its capital" not in row["next"]


def test_an_approximated_slot_is_not_counted_among_the_voices_awaiting_an_oracle():
    """Two different states, and adding them together names the wrong task list.

    The policy here is built in full rather than read, so the test keeps its
    sensitivity whatever the shipped block happens to hold. The entry is planted
    over a slot that really has no capture, because an approximated slot with no
    oracle is exactly the overlap being ruled out.
    """
    rows = _shipped()
    uncaptured = next(r for r in rows if not r["capture"])
    pol = {"approximated": {uncaptured["slug"]: {"answered_by": "p000-acoustic-grand-piano",
                                                 "reason": "planted"}}}
    approximated = [r for r in rows if status.approximation(pol, r["slug"])]
    no_oracle = [r for r in rows if not r["capture"] and r not in approximated]
    assert len(approximated) == 1
    assert uncaptured["slug"] not in {r["slug"] for r in no_oracle}
    assert len(no_oracle) == len([r for r in rows if not r["capture"]]) - 1


def test_every_declared_approximation_names_a_real_slot_and_a_real_answer():
    """An entry is only worth as much as the two names in it.

    This used to assert the block was EMPTY — a claim that every slot had a
    patch written for it. That claim expired the day one did not, so what is
    asserted now is the part that does not expire: an entry names a slot that
    exists, is answered by a slot that exists, and carries a reason. Without
    those an entry reads exactly like the oversight the block was written to
    stop, and a reader cannot tell an approximation from an unfinished voice.
    """
    slugs = {row["slug"] for row in _shipped()}
    block = json.loads(status.POLICY.read_text())["approximated"]
    for slug, entry in block.items():
        assert slug in slugs, f"{slug} is not a slot in the bank"
        assert entry["answered_by"] in slugs, f"{slug} is answered by an unknown slot"
        assert entry["answered_by"] != slug, f"{slug} cannot approximate itself"
        assert entry.get("reason", "").strip(), f"{slug} declares no reason"


# --------------------------------------------------------------------------- #
# The two hand-written claims, counted
# --------------------------------------------------------------------------- #

def _claim(state):
    return {"state": state, "date": "2026-09-01"}


def test_a_structural_diagnosis_behind_an_unmade_musical_claim_is_counted_apart():
    """The expensive half of the last step, recorded where nothing can use it.

    The musical claim promotes a voice to `heard` and the structural one carries
    it to `settled`, so a diagnosis taken before anyone listened raises no stage
    at all. The stage column cannot show it: it says where a voice stopped, not
    what was recorded past the step it stopped at.
    """
    rows = [
        {"axes": {"structure": _claim(signoff.CURRENT), "music": None}},
        {"axes": {"structure": _claim(signoff.STALE), "music": None}},
        {"axes": {"structure": _claim(signoff.CURRENT), "music": _claim(signoff.CURRENT)}},
        {"axes": {"structure": None, "music": None}},
    ]
    got = status.signoff_census(rows)
    assert got == {"structure": 3, "structure_current": 2,
                   "music": 1, "music_current": 1, "structure_only": 2}


def test_an_expired_claim_is_counted_as_recorded_and_not_as_current():
    """Both numbers are printed: a record that expired was still work done."""
    got = status.signoff_census([{"axes": {"structure": _claim(signoff.UNVERIFIED),
                                           "music": None}}])
    assert got["structure"] == 1
    assert got["structure_current"] == 0


def test_the_census_counts_nothing_on_a_bank_with_no_claims():
    assert status.signoff_census([{"axes": {}}, {}]) == {
        "structure": 0, "structure_current": 0, "music": 0, "music_current": 0,
        "structure_only": 0}


def test_the_shipped_bank_is_counted_from_the_rows_rather_than_from_signoff_json():
    """The generated file already carries both claims' state per row, so the
    census needs no build and no second read of the source file."""
    got = status.signoff_census(_shipped())
    assert got["structure"] + got["music"] > 0
    assert got["structure_only"] <= got["structure"]


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


def test_one_reference_timbre_is_a_target_and_carries_the_voice_past_voiced():
    """A reference is what the voice aims at, not a sample of a hidden truth.

    Demanding a second one costs a hand-authored plugin per instrument and buys
    a denominator `docs/objective.md` retired; with one, everything above
    `voiced` still has to be reachable or the ladder reports 123 fitted voices
    and 123 untouched ones as the same number.
    """
    assert status.stage_for(_axes(timbres=1)) > 1


def test_no_reference_at_all_still_stops_at_voiced():
    assert status.stage_for(_axes(timbres=0, profile_rows=0)) == 1


def test_a_stale_gate_does_not_count_as_a_gate():
    assert status.stage_for(_axes(gate_state="stale")) == 2


def test_a_gate_that_was_never_written_is_named_as_one_to_write():
    """Two states, one message, and one of them read as a Python repr.

    A voice whose profile has just been measured reaches this for the first
    time, and `re-record it` names an action there is nothing to re-record for.
    """
    never = status.next_action(_axes(gate_state=None), 2, [])
    stale = status.next_action(_axes(gate_state="stale"), 2, [])
    assert "None" not in never
    assert "write the first gate" in never
    assert "re-record" in stale


def test_a_stale_diagnosis_names_what_moved_and_a_kit_has_no_patch_to_name():
    """"the patch has moved" is a sentence a kit's entry cannot say truthfully."""
    stale = {"state": "stale", "date": "", "unreachable": [], "accepted": [],
             "open": []}
    voice = status.next_action(_axes(structure=stale), 4, [])
    kit = status.next_action(_axes(patch=None, structure=stale), 4, [])
    assert "the patch has moved" in voice
    assert "a voice of this kit has moved" in kit


def test_coverage_is_all_or_nothing():
    """One unexcused gap holds the voice below `covered`, whatever the rest are."""
    assert status.stage_for(_axes(coverage={"complete": False, "gaps": ["body"]})) == 2


def test_disagreeing_with_the_reference_spread_does_not_hold_a_voice_back():
    """`agreement` is printed and decides nothing — `docs/objective.md`.

    It was a promotion condition while the reference was read as one draw from
    the distribution of real instruments. Under the objective the reference is
    the target, so sitting outside two presets' mutual disagreement is
    information about the target's own wobble, not a verdict on the voice.
    """
    apart = _axes(agreement={"inside": 1, "total": 3, "outside": {"stretch": 1.8}},
                  music={"state": signoff.CURRENT})
    together = _axes(music={"state": signoff.CURRENT})
    assert status.stage_for(apart) == status.stage_for(together)


def test_an_unheard_voice_stops_below_heard_however_green_it_measures():
    """A green gate is not acceptance: voices have passed every recorded bound
    while sounding wrong, which is why the ear is a step and not a footnote."""
    assert status.stage_for(_axes()) == 3


def test_a_heard_voice_still_needs_its_structural_claim():
    """Unknown is not satisfied: the diagnosis is unrecorded."""
    assert status.stage_for(_axes(music={"state": signoff.CURRENT})) == 4


def test_an_open_candidate_does_not_demote_a_voice():
    """A candidate nobody adopted means there may be more to gain — not that
    what shipped is worse than it was, so it is a badge and never a predicate.

    What caps this voice is the ear: it is gated over every canonical dimension
    and nobody has signed a take off. Recomputing the stage from the axes alone
    is what says the candidate is not quietly one of them.
    """
    rows = _shipped()
    piano = next(r for r in rows if r["slug"] == "p000-acoustic-grand-piano")
    assert piano["open_candidates"]
    assert piano["stage_name"] == "fitted"
    assert status.stage_for(piano["axes"]) == round(piano["stage"] * 5)


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


def test_a_spread_of_zero_is_unjudgeable_rather_than_infinitely_outside():
    """Two references agreeing to finer than the metric resolves is not a width.

    Measured on the tonewheel organ, whose registrations both arrive inside one
    5 ms envelope hop: the ratio has no denominator, and taking it raised a
    ZeroDivisionError that stopped the whole bank's status from regenerating.
    """
    got = status.gate_agreement({
        "reference_spread": {"attack": 0.0, "stereo": 0.229},
        "bounds": {"attack": {"median": 40.0}, "stereo": {"median": 0.1}},
    })
    assert got["unjudgeable"] == ["attack"]
    assert got["total"] == 1
    assert got["inside"] == 1


def test_a_bound_inside_the_spread_agrees():
    got = status.gate_agreement({
        "reference_spread": {"attack": 25.0, "stereo": 0.229},
        "bounds": {"attack": {"median": 25.0}, "stereo": {"median": 0.414}},
    })
    assert got["inside"] == 1
    assert got["total"] == 2
    assert got["outside"]["stereo"] == pytest.approx(1.81, abs=0.01)


def test_the_gate_margin_does_not_decide_whether_a_voice_agrees():
    """A bound is the measurement times the gate's slack, and the slack is there
    so a regression guard survives noise. Counted as if it were the measurement,
    it would put a voice measurably inside the references' own spread on the
    wrong side of them, and would move the stage when a gate is re-recorded at a
    different margin with nothing about the voice having changed."""
    gate = {
        "margin": 1.25,
        "reference_spread": {"attack": 25.0, "stereo": 0.229},
        # 22.5 and 0.4 measured: the first inside the spread, the second not.
        "bounds": {"attack": {"median": 28.125}, "stereo": {"median": 0.5}},
    }
    got = status.gate_agreement(gate)
    assert got["inside"] == 1
    assert got["outside"]["stereo"] == pytest.approx(0.4 / 0.229, abs=0.01)


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
    assert "damper" in status.coverage(V(), [prose_only], [{"bounds": {}}])["gaps"]


def test_an_excused_dimension_completes_coverage():
    class V:
        program, kit = 0, False

    canon = canonical_dimensions(0)
    gate = {"bounds": {d: {"median": 1.0} for d in canon if d != "damper"}}
    assert status.coverage(V(), [{}], [gate])["gaps"] == ["damper"]
    excused = {"dimensions_na": {"damper": "the references disagree by more than the model does"}}
    assert status.coverage(V(), [excused], [gate])["complete"]


def test_a_gate_recorded_reason_is_not_a_gap_and_is_not_completion_either():
    """There are two exclusion registers and they say different things.

    `dimensions_na` is the source not carrying the dimension, which is
    permanent. A gate's `_unbounded` is this comparison recording no bound,
    which covers a live model deficiency as readily as a settled limit — so it
    stops the dimension reading as an oversight and must not promote the voice.
    """

    class V:
        program, kit = 0, False

    canon = canonical_dimensions(0)
    gate = {"bounds": {d: {"median": 1.0} for d in canon if d != "balance"}}
    assert status.coverage(V(), [{}], [gate])["gaps"] == ["balance"]

    gate["_unbounded"] = {"balance": "the model has nothing on the grid this reads"}
    got = status.coverage(V(), [{}], [gate])
    assert got["gaps"] == []
    assert got["unbounded"] == ["balance"]
    assert not got["complete"]
    assert "balance" not in got["excused"]


def test_a_bound_outranks_a_reason_recorded_beside_it():
    """A stale `_unbounded` left behind after a bound lands must not shadow it."""

    class V:
        program, kit = 0, False

    canon = canonical_dimensions(0)
    gate = {"bounds": {d: {"median": 1.0} for d in canon},
            "_unbounded": {"balance": "recorded before the bound existed"}}
    got = status.coverage(V(), [{}], [gate])
    assert got["complete"] and "unbounded" not in got


def test_agreement_folds_a_gate_that_has_no_spread_to_adjudicate_against():
    """A one-timbre capture's gate reports no `outside` key at all rather than
    an empty one, and three of the standard kit's four captures are that."""

    judged = {"reference_spread": {"level": 1.0}, "margin": 1.0,
              "bounds": {"level": {"median": 4.0}}}
    spreadless = {"bounds": {"attack": {"median": 1.0}}}
    got = status.merged_agreement([judged, spreadless])
    assert got["outside"] == {"level": 4.0}
    assert got["unjudgeable"] == ["attack"]
    assert status.merged_agreement([spreadless])["outside"] == {}


def test_a_dimension_gated_by_a_second_capture_is_not_a_gap():
    """A voice is answered by as many captures as its axes take: the kit gates
    colour on the module grids and `vel_range` on the sampled kit, and reading
    either alone reports the other's bounds as absent."""

    class V:
        program, kit = 0, False

    canon = canonical_dimensions(0)
    colour = {"bounds": {d: {"median": 1.0} for d in canon if d != "damper"}}
    damper = {"bounds": {"damper": {"median": 1.0}}}
    assert status.coverage(V(), [{}], [colour])["gaps"] == ["damper"]
    assert status.coverage(V(), [{}, {}], [colour, damper])["complete"]


def test_an_excuse_does_not_unseat_a_bound_another_capture_records():
    """An excuse is a statement about the source carrying it, so it settles a
    dimension only where nothing gates it — `drums.json` excuses the colour it
    handed to the module grids, and that colour is gated rather than absent."""

    class V:
        program, kit = 0, False

    handed_over = {"dimensions_na": {"damper": "moved to the grid that can read it"}}
    gated_there = {"bounds": {"damper": {"median": 1.0}}}
    got = status.coverage(V(), [handed_over, {}], [{"bounds": {}}, gated_there])
    assert "damper" not in got["excused"]
    assert got["gated"] == 1


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
