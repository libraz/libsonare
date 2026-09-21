"""Which dimensions an instrument is judged on, how much of the grid a bound
was set from, and what the floor under one records.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_profile_bounds.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module

import profile_gate

# --------------------------------------------------------------------------
# which dimensions an instrument is judged on


def test_no_declared_dimensions_means_every_measured_one():
    summary = {"decay": {}, "damper": {}}
    assert profile_module.select_dimensions(summary, []) == summary


def test_a_declared_list_narrows_the_summary():
    summary = {"decay": {}, "damper": {}, "tnr": {}}
    assert set(profile_module.select_dimensions(summary, ["decay", "tnr"])) == {"decay", "tnr"}


def test_a_declared_dimension_that_was_not_measured_is_named(capsys):
    """Dropping it silently reads afterwards as a dimension that came out fine."""
    profile_module.select_dimensions({"decay": {}}, ["decay", "damper"])
    assert "damper" in capsys.readouterr().err


def test_every_gate_dimension_has_a_floor_under_its_bound(tmp_path):
    """A bound at zero fails on measurement noise, and then it gets switched off."""
    summary = {k: {"median": 0.0, "abs_median": 0.0, "p90": 0.0, "n": 4}
               for k in profile_module.DELTA_LABELS}
    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(summary, gate, "ref", 1.25)
    bounds = json.loads(gate.read_text())["bounds"]
    assert set(bounds) == set(profile_module.DELTA_LABELS)
    assert all(b["median"] > 0.0 and b["abs_median"] > 0.0 and b["p90"] > 0.0
               for b in bounds.values())


def test_a_tail_neither_median_can_see_is_summarized(tmp_path):
    """Nine good rows and one bad one: the medians read clean and p90 does not.

    This is the shape a per-note defect takes on a kit, where every row is a
    different instrument and a handful of them can be far out without the
    grid's middle moving at all.
    """
    summary = profile_module.summarize_deltas({"centroid_pct": [0.1] * 9 + [600.0]})
    row = summary["centroid_pct"]
    assert row["median"] == pytest.approx(0.1)
    assert row["abs_median"] == pytest.approx(0.1)
    assert row["p90"] > 100 * row["abs_median"]


def test_a_gate_fails_on_the_tail_alone(tmp_path):
    """A bound may hold on both medians and still be exceeded on p90."""
    gate = tmp_path / "gate.json"
    gate.write_text(json.dumps({"timbre": "ref", "bounds": {
        "centroid_pct": {"median": 1.0, "abs_median": 1.0, "p90": 5.0}}}))
    summary = profile_module.summarize_deltas({"centroid_pct": [0.1] * 9 + [600.0]})
    assert profile_module.check_gate(summary, gate, "ref") == 1


def test_a_gate_without_a_tail_bound_says_so(tmp_path, capsys):
    """A gate written before p90 holds nothing on the tail, and reports it.

    The check loop skips a stat with no bound, so silence here would read as a
    tail that was fine -- the same failure as a dimension with no bound at all.
    """
    gate = tmp_path / "gate.json"
    gate.write_text(json.dumps({"timbre": "ref", "bounds": {
        "centroid_pct": {"median": 1e9, "abs_median": 1e9}}}))
    summary = profile_module.summarize_deltas({"centroid_pct": [0.1] * 9 + [600.0]})
    assert profile_module.check_gate(summary, gate, "ref") == 0
    assert "no tail bound" in capsys.readouterr().out


# --------------------------------------------------------------------------- #
# How much of the grid a bound was set from
# --------------------------------------------------------------------------- #
def _gate(tmp_path, **rows):
    """A gate whose bounds are wide enough that only the evidence can fail it."""
    path = tmp_path / "gate.json"
    path.write_text(json.dumps({"timbre": "ref", "bounds": {
        k: {"median": 1e9, "abs_median": 1e9, **({"rows": n} if n else {})}
        for k, n in rows.items()}}))
    return path


def _summary(**rows):
    return {k: {"median": 0.0, "abs_median": 0.0, "p90": 0.0, "n": n} for k, n in rows.items()}


def test_a_bound_measured_on_the_same_evidence_holds(tmp_path):
    assert profile_module.check_gate(
        _summary(decay=50, tnr=50), _gate(tmp_path, decay=50, tnr=50), "ref") == 0


def test_a_bound_whose_evidence_collapsed_fails(tmp_path):
    """The censors drop a row they cannot compare and the rest are averaged, so a
    dimension can hold a bound while most of the keyboard contributed nothing."""
    rc = profile_module.check_gate(
        _summary(decay=8, tnr=50), _gate(tmp_path, decay=50, tnr=50), "ref")
    assert rc == 1


def test_a_bound_whose_evidence_returned_also_fails(tmp_path):
    """Evidence arriving is as much a different population as evidence leaving:
    the median moves to notes the bound was never set from."""
    rc = profile_module.check_gate(
        _summary(decay=50, tnr=50), _gate(tmp_path, decay=8, tnr=50), "ref")
    assert rc == 1


def test_a_gate_with_no_row_counts_says_so_rather_than_passing_quietly(tmp_path, capsys):
    """Every gate written before the counts existed is this one."""
    assert profile_module.check_gate(
        _summary(decay=8, tnr=50), _gate(tmp_path, decay=0, tnr=0), "ref") == 0
    out = capsys.readouterr().out
    assert "records no row counts" in out
    # And the run's own thin dimension is still named, which is all an old gate
    # can offer: 8 against a grid that spoke 50 times elsewhere.
    assert "8/50" in out


def test_a_per_note_dimension_is_not_reported_as_thin(tmp_path, capsys):
    """Its row IS a note, so a count below the grid's is its shape. A line on
    every gate is a line nobody reads by the time one of them means something."""
    profile_module.check_gate(
        _summary(vel_range=10, tnr=50), _gate(tmp_path, vel_range=10, tnr=50), "ref")
    assert "held on part of the grid" not in capsys.readouterr().out


def test_a_bound_recorded_from_a_sliver_of_the_grid_says_so_even_when_it_holds(
        tmp_path, capsys):
    """`--write-gate` imposes no minimum population, so a dimension reaching one
    row of thirty-five is recorded as confidently as one reaching all of them.
    The run agrees with the bound here and nothing fails — which is exactly the
    case the reader cannot otherwise see, and the case that says least."""
    assert profile_module.check_gate(
        _summary(damper=5, tnr=50), _gate(tmp_path, damper=5, tnr=50), "ref") == 0
    out = capsys.readouterr().out
    assert "recorded from part of the grid" in out
    assert "damper release (ms) 5/50" in out


def test_a_bound_recorded_from_the_whole_grid_is_not_named(tmp_path, capsys):
    """The negative control. A line printed on every gate is a line nobody reads
    by the time one of them means something."""
    profile_module.check_gate(
        _summary(damper=50, tnr=50), _gate(tmp_path, damper=50, tnr=50), "ref")
    assert "recorded from part of the grid" not in capsys.readouterr().out


def test_a_per_note_dimension_is_not_named_as_recorded_from_a_sliver(tmp_path, capsys):
    """The same exemption the run's own thin report makes: a per-note dimension's
    unit is not the row, so a count below the grid's is its shape."""
    profile_module.check_gate(
        _summary(vel_range=10, tnr=50), _gate(tmp_path, vel_range=10, tnr=50), "ref")
    assert "recorded from part of the grid" not in capsys.readouterr().out


def test_the_written_gate_records_what_each_bound_was_measured_from(tmp_path):
    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(_summary(decay=18, tnr=50), gate, "ref", 1.25)
    bounds = json.loads(gate.read_text())["bounds"]
    assert bounds["decay"]["rows"] == 18
    assert bounds["tnr"]["rows"] == 50


def test_a_re_record_carries_the_hand_written_unbounded_reasons(tmp_path):
    """A fixed payload silently deleted them, and the kit gates are where they live.

    `_unbounded` says why a dimension the capture asks for holds no bound, which
    nothing computes — so losing it on a re-record turns an argued absence back
    into an unexplained one with no diff to notice.
    """
    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(_summary(decay=18), gate, "ref", 1.25)
    held = json.loads(gate.read_text())
    held["_unbounded"] = {"ring": "no second reference reaches it"}
    gate.write_text(json.dumps(held))

    profile_module.write_gate_file(_summary(decay=20), gate, "ref", 1.25)
    assert json.loads(gate.read_text())["_unbounded"] == {
        "ring": "no second reference reaches it"}


def test_a_gate_records_which_library_measured_its_model_side(tmp_path, monkeypatch):
    """A stale dylib bakes the previous generation's numbers into a live bound.

    The gate records where the reference came from and when it was written, and
    nothing about the model side — so a `--write-gate` run against a library
    older than the sources produces bounds indistinguishable from current ones.
    `warn_if_stale` already detects it and says so on stderr, which is gone by
    the time anyone opens the file.
    """
    lib = tmp_path / "libsonare.dylib"
    lib.write_bytes(b"")
    monkeypatch.setenv("SONARE_LIB_PATH", str(lib))
    import render_model
    monkeypatch.setattr(render_model, "_newest_source_mtime",
                        lambda: lib.stat().st_mtime + 3600)

    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(_summary(decay=18), gate, "ref", 1.25)
    built = json.loads(gate.read_text())["model_build"]
    assert built["stale"] is True
    assert built["built_utc"] and built["newest_source_utc"]

    # And a library newer than the sources is not flagged.
    monkeypatch.setattr(render_model, "_newest_source_mtime",
                        lambda: lib.stat().st_mtime - 3600)
    profile_module.write_gate_file(_summary(decay=18), gate, "ref", 1.25)
    assert json.loads(gate.read_text())["model_build"]["stale"] is False


def test_a_re_record_carries_every_other_hand_written_note_too(tmp_path):
    """`_unbounded` was rescued one key at a time; the rest of the class was not.

    A note recorded beside a bound — which change spent its margin, and by how
    much — has no computed counterpart either, and the note a re-record deletes
    is found right up until the day someone needs it. `_` is the writer's own
    preamble and is regenerated rather than carried.
    """
    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(_summary(decay=18), gate, "ref", 1.25)
    held = json.loads(gate.read_text())
    held["_margin_spent"] = {"decay": "consumed by the loop-loss law"}
    held["_"] = "clobber me"
    gate.write_text(json.dumps(held))

    profile_module.write_gate_file(_summary(decay=20), gate, "ref", 1.25)
    written = json.loads(gate.read_text())
    assert written["_margin_spent"] == {"decay": "consumed by the loop-loss law"}
    assert written["_"] != "clobber me"


def test_an_unbounded_reason_is_dropped_once_its_dimension_gains_a_bound(tmp_path):
    """Otherwise a stale reason explains away a bound sitting beside it."""
    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(_summary(decay=18), gate, "ref", 1.25)
    held = json.loads(gate.read_text())
    held["_unbounded"] = {"tnr": "nothing measured it"}
    gate.write_text(json.dumps(held))

    profile_module.write_gate_file(_summary(decay=18, tnr=50), gate, "ref", 1.25)
    written = json.loads(gate.read_text())
    assert "tnr" in written["bounds"]
    assert "_unbounded" not in written


def _one_bound(tmp_path, error: float, name: str = "gate.json") -> dict:
    """Write a gate whose single dimension carries `error`, and read it back."""
    gate = tmp_path / name
    summary = {"centroid_pct": {"median": error, "abs_median": error,
                                "p90": error, "n": 12}}
    profile_module.write_gate_file(summary, gate, "ref", 1.25)
    return json.loads(gate.read_text())["bounds"]["centroid_pct"]


def test_a_re_record_may_tighten_a_bound(tmp_path):
    assert _one_bound(tmp_path, 80.0)["p90"] == pytest.approx(100.0)
    assert _one_bound(tmp_path, 40.0)["p90"] == pytest.approx(50.0)


def test_a_re_record_may_not_loosen_one(tmp_path):
    """The rule was kept by hand, and by hand it survives until the run where
    the number rises for a reason that sounds good — a sharpened measurement
    shrinking the spread under a bound until the floor stops winning."""
    assert _one_bound(tmp_path, 40.0)["p90"] == pytest.approx(50.0)
    assert _one_bound(tmp_path, 80.0)["p90"] == pytest.approx(50.0)


def test_declining_to_loosen_a_bound_is_announced(tmp_path, capsys):
    """A ratchet that acts silently is a second way to lose track of the gate."""
    _one_bound(tmp_path, 40.0)
    capsys.readouterr()
    _one_bound(tmp_path, 80.0)
    assert "centroid_pct.p90" in capsys.readouterr().out


def test_the_ratchet_does_not_reach_a_gate_that_is_not_there_yet(tmp_path):
    """A fresh path records what was measured — there is nothing to ratchet against."""
    assert _one_bound(tmp_path, 80.0, "a.json")["p90"] == pytest.approx(100.0)
    assert _one_bound(tmp_path, 40.0, "b.json")["p90"] == pytest.approx(50.0)


def test_the_row_count_is_not_ratcheted(tmp_path):
    """It is a population, so the smaller of two narrows what the gate claims to
    have measured instead of tightening what it holds."""
    gate = tmp_path / "gate.json"
    for rows in (50, 18):
        profile_module.write_gate_file(
            {"decay": {"median": 1.0, "abs_median": 1.0, "p90": 1.0, "n": rows}},
            gate, "ref", 1.25)
    assert json.loads(gate.read_text())["bounds"]["decay"]["rows"] == 18


def test_a_written_gate_records_where_each_floor_came_from(tmp_path):
    """Three sources of very different weight decide a floor and the number alone
    cannot say which it was: a measured spread, a hand-written guess, or an
    argument default nobody chose for that dimension."""
    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(
        _summary(stretch=35, register=35, tnr=35), gate, "ref", 1.25,
        spread={"tnr": 3.74})
    bounds = json.loads(gate.read_text())["bounds"]
    assert bounds["tnr"]["floor_from"] == "measured spread"
    assert bounds["tnr"]["floor"] == 3.74
    assert bounds["stretch"]["floor_from"] == "guess"
    assert bounds["register"]["floor_from"] == "default"
    assert bounds["register"]["floor"] == profile_gate.GENERIC_FLOOR


def test_a_failure_on_a_bound_nobody_chose_a_floor_for_says_so(tmp_path, capsys):
    """101 of 119 `register` bounds in the tree are the generic floor, and four
    voices fail one by 3 to 11 percent. The reader cannot see that from 1.0."""
    gate = tmp_path / "gate.json"
    gate.write_text(json.dumps({"timbre": "ref", "bounds": {
        "register": {"median": 1.0, "abs_median": 1.0, "rows": 35,
                     "floor": 1.0, "floor_from": "default"},
        "tnr": {"median": 1e9, "abs_median": 1e9, "rows": 35,
                "floor": 3.74, "floor_from": "measured spread"}}}))
    summary = {"register": {"median": 1.08, "abs_median": 1.08, "p90": 1.08, "n": 35},
               "tnr": {"median": 0.0, "abs_median": 0.0, "p90": 0.0, "n": 35}}
    assert profile_module.check_gate(summary, gate, "ref") == 1
    out = capsys.readouterr().out
    assert "rests on the generic 1.0 floor" in out
    assert "register profile" in out
    # The measured-spread bound held, and a gate carries many floors: naming one
    # that did not fail would put this line on every gate in the tree.
    assert "tone-to-noise" not in out.split("rests on the generic")[1].split("\n")[0]


def test_a_bound_that_is_its_floor_is_named_even_where_the_gate_records_none(tmp_path, capsys):
    """108 of the 153 gates in the tree predate the recorded `floor` field, and on
    those a bound sitting at its floor is indistinguishable from a measured one.
    The floor table is reachable from the reader, so it is recomputed."""
    gate = tmp_path / "gate.json"
    gate.write_text(json.dumps({"timbre": "ref", "bounds": {
        "register": {"median": 1.0, "abs_median": 1.0, "p90": 3.1, "rows": 35},
        "attack": {"median": 40.0, "abs_median": 40.0, "p90": 40.0, "rows": 35}}}))
    summary = {"register": {"median": 0.1, "abs_median": 0.1, "p90": 0.1, "n": 35},
               "attack": {"median": 1.0, "abs_median": 1.0, "p90": 1.0, "n": 35}}
    assert profile_module.check_gate(summary, gate, "ref") == 0
    out = capsys.readouterr().out
    named = out.split("resting on the dimension's floor")[1].split("\n")[0]
    assert "(median/abs_median/p90)" in named          # attack, floored on all three
    assert "(median/abs_median)" in named              # register, its p90 is not
    assert "this gate records no floors" in out


def test_a_bound_above_its_floor_is_not_named_as_floored(tmp_path, capsys):
    """The sensitivity specimen for the check above. Reach is not enough: a check
    that named every bound would pass that test and report nothing."""
    gate = tmp_path / "gate.json"
    gate.write_text(json.dumps({"timbre": "ref", "bounds": {
        "register": {"median": 4.2, "abs_median": 4.2, "p90": 9.0, "rows": 35}}}))
    summary = {"register": {"median": 0.1, "abs_median": 0.1, "p90": 0.1, "n": 35}}
    assert profile_module.check_gate(summary, gate, "ref") == 0
    assert "resting on the dimension's floor" not in capsys.readouterr().out


def test_a_gate_read_through_a_stale_library_says_the_pass_is_not_evidence(
        tmp_path, capsys, monkeypatch):
    """The build state was recorded into a written gate and asked by nobody, so a
    comparison against a library older than the change under test returned green
    in silence. Reported rather than failed: the test is on mtimes."""
    gate = tmp_path / "gate.json"
    gate.write_text(json.dumps({"timbre": "ref", "bounds": {
        "tnr": {"median": 1e9, "abs_median": 1e9, "p90": 1e9, "rows": 35,
                "floor": 3.74, "floor_from": "measured spread"}}}))
    summary = {"tnr": {"median": 0.0, "abs_median": 0.0, "p90": 0.0, "n": 35}}
    monkeypatch.setattr(profile_gate, "_model_build_state",
                        lambda: {"built_utc": "2026-09-21T00:05:10Z",
                                 "newest_source_utc": "2026-09-21T10:53:08Z",
                                 "stale": True})
    assert profile_module.check_gate(summary, gate, "ref") == 0
    err = capsys.readouterr().err
    assert "2026-09-21T00:05:10Z" in err
    assert "A pass here is not evidence" in err

    monkeypatch.setattr(profile_gate, "_model_build_state", lambda: {"stale": False})
    assert profile_module.check_gate(summary, gate, "ref") == 0
    assert "not evidence" not in capsys.readouterr().err
