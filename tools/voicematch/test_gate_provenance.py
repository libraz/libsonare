"""Per-bound provenance in the gate file, the computed `_unbounded`, and the migration.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_gate_provenance.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import gate_migrate
import profile_gate
import profile_status
import status
from profile_gate import CensorTally, write_gate_file


@pytest.fixture(autouse=True)
def _clock(monkeypatch):
    """Each write gets the next stamp, and a build stamp tied to it."""
    ticks = iter(f"2026-01-{day:02d}T00:00:00+00:00" for day in range(1, 30))

    class _Now:
        @staticmethod
        def now(_tz=None):
            return _Dt(next(ticks))

    class _Dt:
        def __init__(self, s):
            self.s = s

        def isoformat(self, timespec="seconds"):
            return self.s

    monkeypatch.setattr(profile_gate, "datetime", _Now)
    monkeypatch.setattr(profile_gate, "_model_build_state", lambda: {"built_utc": "build-1"})


def _rows(value, n=10):
    return {"decay": {"median": value, "abs_median": value, "p90": value, "n": n}}


def _record(path, value, **kw):
    write_gate_file(_rows(value), path, "ref", 1.0, **kw)
    return json.loads(path.read_text())["bounds"]["decay"]["provenance"]["median"]


def test_a_first_record_is_new_and_held_now(tmp_path):
    p = _record(tmp_path / "g.json", 10.0)
    assert p["outcome"] == "new"
    assert p["set_utc"] == p["held_utc"] == "2026-01-01T00:00:00+00:00"
    assert p["set_build"] == "build-1"
    assert p["measured"] == 10.0


def test_a_tighter_re_record_moves_the_value_and_everything_with_it(tmp_path):
    gate = tmp_path / "g.json"
    _record(gate, 10.0)
    p = _record(gate, 8.0)
    assert p["outcome"] == "tightened"
    assert p["set_utc"] == p["held_utc"] == "2026-01-02T00:00:00+00:00"
    assert json.loads(gate.read_text())["bounds"]["decay"]["median"] == 8.0


def test_the_same_value_is_kept_and_its_set_stamp_carried(tmp_path):
    gate = tmp_path / "g.json"
    _record(gate, 10.0)
    p = _record(gate, 10.0)
    assert p["outcome"] == "kept"
    assert p["set_utc"] == "2026-01-01T00:00:00+00:00"
    assert p["held_utc"] == "2026-01-02T00:00:00+00:00"


def test_a_refused_loosening_prints_one_line_per_statistic(tmp_path, capsys):
    gate = tmp_path / "g.json"
    write_gate_file(_rows(10.0), gate, "ref", 1.0)
    capsys.readouterr()
    write_gate_file(_rows(14.0), gate, "ref", 1.0)
    out = capsys.readouterr().out
    assert out.count("kept the recorded bound:") == 3
    assert "decay.median 14 -> 10 (measured 14, over it; last held" in out


def test_every_statistic_reports_its_outcome(tmp_path, capsys):
    """A status report quotes these, so a tightened bound prints as surely as a kept one."""
    gate = tmp_path / "g.json"
    write_gate_file(_rows(10.0), gate, "ref", 1.0)
    first = capsys.readouterr().out
    assert first.count("new        decay.") == 3
    write_gate_file(_rows(8.0), gate, "ref", 1.0)
    out = capsys.readouterr().out
    assert "tightened  decay.median bound 8 (measured 8, was 10)" in out
    assert out.count("tightened  decay.") == 3 and "kept the recorded bound" not in out


def test_a_kept_bound_with_the_measurement_inside_it_reads_kept(tmp_path):
    gate = tmp_path / "g.json"
    write_gate_file(_rows(10.0), gate, "ref", 1.0)
    # candidate 12 > old 10 so the ratchet keeps 10, and the measured 9.5 is inside it.
    write_gate_file(_rows(9.5), gate, "ref", 1.263)
    b = json.loads(gate.read_text())["bounds"]["decay"]
    assert b["median"] == 10.0
    p = b["provenance"]["median"]
    assert p["outcome"] == "kept"
    assert p["set_utc"] == "2026-01-01T00:00:00+00:00"
    assert p["held_utc"] == "2026-01-02T00:00:00+00:00"
    assert p["measured"] == 9.5


def test_an_exceeded_bound_carries_held_and_set_and_turns_status_red(tmp_path):
    gate = tmp_path / "g.json"
    _record(gate, 10.0)
    write_gate_file(_rows(14.0), gate, "ref", 1.0)
    doc = json.loads(gate.read_text())
    b = doc["bounds"]["decay"]
    assert b["median"] == 10.0
    p = b["provenance"]["median"]
    assert p["outcome"] == "exceeded"
    assert p["held_utc"] == "2026-01-01T00:00:00+00:00"
    assert p["set_utc"] == "2026-01-01T00:00:00+00:00"
    assert p["measured"] == 14.0
    # the file stamp moved on, which is exactly what it must not be read as
    assert doc["recorded_utc"] == "2026-01-02T00:00:00+00:00"

    found = profile_gate.provenance_findings(doc)["exceeded"]
    assert ("decay", "median", "2026-01-01T00:00:00+00:00") in found
    note = profile_gate.provenance_notes(doc)[0]
    assert "re-recorded over a kept bound, last held 2026-01-01T00:00:00+00:00" in note


def test_status_reads_an_exceeded_gate_as_red(tmp_path, monkeypatch):
    gate = tmp_path / "x_gate.json"
    _record(gate, 10.0)
    write_gate_file(_rows(14.0), gate, "ref", 1.0, measured_utc="m1")
    (tmp_path / "x.json").write_text(json.dumps({"measured_utc": "m1", "rows": []}))
    monkeypatch.setattr(status, "REFERENCE_DIR", tmp_path)
    facts = status.profile_facts("x")
    assert facts["gate_state"] == "exceeded"
    assert any("re-recorded over a kept bound" in n for n in facts["gate_notes"])
    assert status.stage_for(_axes(gate_state="exceeded")) == 2
    assert "re-recorded over a kept bound" in status.next_action(
        _axes(gate_state="exceeded", gate_notes=facts["gate_notes"]), 2, []
    )


def test_an_unknown_held_time_is_not_verified_since_the_set_time():
    gate = {
        "bounds": {
            "decay": {
                "median": 1.0,
                "provenance": {
                    "median": {"set_utc": "2026-03-01T00:00:00+00:00", "held_utc": None},
                },
            }
        }
    }
    notes = profile_gate.provenance_notes(gate)
    assert notes == ["not verified since 2026-03-01T00:00:00+00:00: decay.median"]


def test_an_exceeded_bound_with_no_known_hold_says_unknown():
    gate = {
        "bounds": {"decay": {"provenance": {"median": {"outcome": "exceeded", "held_utc": None}}}}
    }
    assert "last held unknown" in profile_gate.provenance_notes(gate)[0]


# --------------------------------------------------------------------------- #
# computed `_unbounded`


def _tally(rows, absent):
    t = CensorTally()
    for i in range(rows):
        t.begin_row()
        t.absent("tnr", **absent)
        t.absent("tnr", **absent)  # asked twice in a row, counted once
    return t


def test_a_dimension_the_model_never_measured_is_unbounded_with_side_and_counts(tmp_path):
    gate = tmp_path / "g.json"
    write_gate_file(
        _rows(10.0),
        gate,
        "ref",
        1.0,
        requested=["decay", "tnr"],
        censors=_tally(7, {"model": True, "reference": False}),
    )
    un = json.loads(gate.read_text())["_unbounded"]
    assert un == {"tnr": "measured: no rows; censored on model, 7 of 7 rows (tnr_at_floor)"}
    assert profile_gate.model_unmeasurable(un) == ["tnr"]


def test_a_named_censor_and_both_sides(tmp_path):
    t = CensorTally()
    for _ in range(4):
        t.begin_row()
        t.absent("damper", True, False)
    t.begin_row()
    t.absent("damper", False, True)
    gate = tmp_path / "g.json"
    write_gate_file(_rows(1.0), gate, "ref", 1.0, requested=["decay", "damper"], censors=t)
    reason = json.loads(gate.read_text())["_unbounded"]["damper"]
    assert reason == "measured: no rows; censored on both, 5 of 5 rows (decayed_before_note_off)"
    assert profile_gate.model_unmeasurable({"damper": reason}) == []


def test_an_excused_dimension_gets_no_computed_reason(tmp_path):
    gate = tmp_path / "g.json"
    write_gate_file(
        _rows(10.0),
        gate,
        "ref",
        1.0,
        requested=["decay", "tnr"],
        dimensions_na={"tnr": "no tone"},
        censors=_tally(3, {"model": True, "reference": False}),
    )
    assert "_unbounded" not in json.loads(gate.read_text())


def test_a_hand_written_reason_is_carried_and_a_computed_one_regenerated(tmp_path):
    gate = tmp_path / "g.json"
    write_gate_file(
        _rows(10.0),
        gate,
        "ref",
        1.0,
        requested=["decay", "tnr"],
        censors=_tally(3, {"model": True, "reference": False}),
    )
    doc = json.loads(gate.read_text())
    doc["_unbounded"]["ring"] = "no second reference reaches it"
    gate.write_text(json.dumps(doc))
    # tnr now measures: its computed reason must go; the hand-written one stays.
    rows = {**_rows(10.0), "tnr": {"median": 1.0, "abs_median": 1.0, "p90": 1.0, "n": 3}}
    write_gate_file(rows, gate, "ref", 1.0, requested=["decay", "tnr"], censors=CensorTally())
    assert json.loads(gate.read_text())["_unbounded"] == {"ring": "no second reference reaches it"}


def test_status_gives_a_model_unmeasurable_dimension_its_own_state():
    un = {"tnr": "measured: no rows; censored on model, 7 of 7 rows (field absent)"}
    ref = {"tnr": "measured: no rows; censored on reference, 7 of 7 rows (field absent)"}
    assert profile_gate.model_unmeasurable(un) == ["tnr"]
    assert profile_gate.model_unmeasurable(ref) == []
    msg = status.next_action(
        _axes(
            coverage={
                "complete": False,
                "gaps": [],
                "unbounded": ["tnr"],
                "model_unmeasurable": ["tnr"],
            }
        ),
        2,
        [],
    )
    assert "model unmeasurable on tnr" in msg
    assert "recorded unbounded" not in msg


def test_profile_status_names_the_states(tmp_path):
    assert profile_status.model_unmeasurable(
        {"a": "measured: no rows; censored on model, 2 of 2 rows (x)"}
    ) == ["a"]


# --------------------------------------------------------------------------- #
# migration


def _old_gate():
    return {
        "recorded_utc": "2026-06-01T00:00:00+00:00",
        "model_build": {"built_utc": "b3"},
        "bounds": {
            "decay": {"floor": 1.0, "median": 3.5, "abs_median": 4.0, "p90": 9.0, "rows": 12},
        },
    }


def _hist(median_by_commit):
    return [
        (
            f"c{i}",
            {
                "recorded_utc": f"2026-0{i + 1}-01T00:00:00+00:00",
                "model_build": {"built_utc": f"b{i}"},
                "bounds": {"decay": {"median": m, "abs_median": 4.0, "p90": 9.0}},
            },
        )
        for i, m in median_by_commit
    ]


def test_migrate_keeps_every_value_and_dates_the_oldest_equal_run():
    # newest first: c3 and c2 hold 3.5, c1 differs, c0 holds 3.5 again but the run is broken.
    gate = _old_gate()
    out = gate_migrate.migrate(gate, _hist([(3, 3.5), (2, 3.5), (1, 9.9), (0, 3.5)]))
    b, was = out["bounds"]["decay"], gate["bounds"]["decay"]
    assert {k: b[k] for k in was} == was
    assert json.dumps({k: b[k] for k in was}) == json.dumps(was)
    p = b["provenance"]["median"]
    assert p["set_utc"] == "2026-03-01T00:00:00+00:00"
    assert p["set_build"] == "b2"
    assert p["held_utc"] is p["measured"] is p["outcome"] is None


def test_migrate_with_no_history_leaves_the_set_stamp_unknown():
    p = gate_migrate.migrate(_old_gate(), [])["bounds"]["decay"]["provenance"]["p90"]
    assert p["set_utc"] is None and p["set_build"] is None


def test_migrate_reports_a_restamped_gate(tmp_path, monkeypatch, capsys):
    src = tmp_path / "in"
    src.mkdir()
    g = src / "a_gate.json"
    g.write_text(json.dumps(_old_gate()))
    monkeypatch.setattr(gate_migrate, "read_history", lambda _p: _hist([(2, 3.5)]))
    out = tmp_path / "out"
    assert gate_migrate.run([g], out, False) == 0
    text = capsys.readouterr().out
    assert "with a set_utc: 3" in text
    assert "a_gate.json" in text  # recorded_utc 2026-06 is newer than the 2026-03 set
    written = json.loads((out / "a_gate.json").read_text())
    assert written["bounds"]["decay"]["median"] == 3.5


def test_migrate_refuses_the_reference_directory_without_in_place(tmp_path, capsys):
    g = tmp_path / "a_gate.json"
    g.write_text(json.dumps(_old_gate()))
    assert gate_migrate.run([g], gate_migrate.REFERENCE_DIR, False) == 2
    assert "refusing" in capsys.readouterr().err


def _axes(**over):
    base = {
        "engine": "piano",
        "patch": "fam0",
        "timbres": 3,
        "profile_rows": 180,
        "gate_state": "current",
        "coverage": {"complete": True, "gaps": []},
        "agreement": {"inside": 8, "total": 11, "outside": {}},
        "structure": None,
        "music": None,
    }
    base.update(over)
    return base


def test_a_first_rerecord_of_an_old_gate_dates_kept_values_from_history(tmp_path, monkeypatch):
    """The writer migrates an old-format gate before ratcheting, so a kept value keeps its date."""
    import gate_migrate

    gate = tmp_path / "x_gate.json"
    gate.write_text(
        json.dumps({"bounds": {"decay": {"median": 1.0, "abs_median": 1.0, "p90": 2.0}}})
    )
    history = [
        (
            "c1",
            {
                "recorded_utc": "2026-09-10T00:00:00+00:00",
                "model_build": {"built_utc": "B1"},
                "bounds": {"decay": {"median": 1.0, "abs_median": 1.0, "p90": 2.0}},
            },
        )
    ]
    monkeypatch.setattr(gate_migrate, "read_history", lambda _p: history)
    bounds = {"decay": {"median": 5.0, "abs_median": 5.0, "p90": 9.0, "floor": 0.1}}
    measured = {"decay": {"median": 4.0, "abs_median": 4.0, "p90": 7.0}}
    profile_gate._keep_the_tighter(bounds, measured, gate, "2026-11-13T00:00:00+00:00", "B2")[0]
    prov = bounds["decay"]["provenance"]["median"]
    assert bounds["decay"]["median"] == 1.0
    assert (prov["set_utc"], prov["set_build"], prov["outcome"]) == (
        "2026-09-10T00:00:00+00:00",
        "B1",
        "exceeded",
    )


def test_an_untouched_old_gate_reads_as_before(tmp_path, capsys):
    """Mixed period: a committed old-format gate and its migrated copy give the same verdict."""
    import gate_migrate

    committed = Path(__file__).resolve().parent / "reference" / "english_horn_gate.json"
    raw = json.loads(committed.read_text())
    for bound in raw["bounds"].values():
        bound.pop("provenance", None)
    old = tmp_path / "old" / "english_horn_gate.json"
    old.parent.mkdir()
    old.write_text(json.dumps(raw))
    summary = {
        k: {
            "median": b["median"] * 0.9,
            "abs_median": b["abs_median"] * 1.2,
            "p90": b["p90"],
            "n": 5,
        }
        for k, b in raw["bounds"].items()
    }
    migrated = tmp_path / "english_horn_gate.json"
    migrated.write_text(json.dumps(gate_migrate.migrate(raw, [])))
    rc_old = profile_gate.check_gate(summary, old, raw["timbre"], "")
    text_old = capsys.readouterr().out
    rc_new = profile_gate.check_gate(summary, migrated, raw["timbre"], "")
    text_new = capsys.readouterr().out
    assert rc_old == rc_new
    assert text_old.replace(str(old), "GATE") == text_new.replace(str(migrated), "GATE")
    assert profile_gate.gate_readings(summary, raw["bounds"]) == profile_gate.gate_readings(
        summary, json.loads(migrated.read_text())["bounds"]
    )


# --------------------------------------------------------------------------- #
# percussion rows


def _kit_gate(tmp_path, rows, requested, **kw):
    import profile_percussion as pp

    t = CensorTally()
    for m, r in rows:
        t.begin_row()
        pp.tally_row_absence(t, pp.percussion_row_deltas(m, r), m, r)
    gate = tmp_path / "kit.json"
    summary = {**_rows(2.0), **kw.pop("summary", {})}
    write_gate_file(summary, gate, "kit", 1.0, requested=requested, censors=t, **kw)
    return json.loads(gate.read_text()).get("_unbounded", {}), gate


def _hit(**over):
    h = {
        "attack_ms": 2.0,
        "crest_db": 10.0,
        "centroid_hz": 1000.0,
        "decay_ms": 100.0,
        "bands_db": [0.0, -3.0, -6.0],
        "band_decay_db_s": [-10.0, -20.0, -30.0],
        "flatness_db": -10.0,
        "tilt_rise_db": 1.0,
        "stereo_width": 0.1,
        "peak_dbfs": -12.0,
    }
    return {**h, **over}


def test_a_kit_dimension_absent_on_the_model_side_names_the_model(tmp_path):
    rows = [(_hit(flatness_db=None), _hit()) for _ in range(3)]
    un, _ = _kit_gate(tmp_path, rows, ["decay", "tonality"])
    assert un == {"tonality": "measured: no rows; censored on model, 3 of 3 rows (field absent)"}


def test_a_kit_dimension_absent_on_the_reference_side_names_the_reference(tmp_path):
    rows = [(_hit(), _hit(tilt_rise_db=None)) for _ in range(2)]
    un, _ = _kit_gate(tmp_path, rows, ["decay", "tilt_rise"])
    assert un == {
        "tilt_rise": "measured: no rows; censored on reference, 2 of 2 rows (field absent)"
    }


def test_a_capped_kit_decay_names_the_side_and_the_censor(tmp_path):
    rows = [(_hit(decay_capped=True), _hit()) for _ in range(2)]
    un, _ = _kit_gate(tmp_path, rows, ["ring"])
    assert un == {"ring": "measured: no rows; censored on model, 2 of 2 rows (decay_capped)"}


def test_a_hand_written_kit_reason_wins_and_a_measured_dimension_gets_none(tmp_path):
    rows = [(_hit(flatness_db=None), _hit()) for _ in range(3)]
    _un, gate = _kit_gate(tmp_path, rows, ["decay", "tonality"])
    doc = json.loads(gate.read_text())
    doc["_unbounded"]["tonality"] = "by hand"
    gate.write_text(json.dumps(doc))
    t = CensorTally()
    t.begin_row()
    t.absent("tonality", True, False)
    write_gate_file(_rows(2.0), gate, "kit", 1.0, requested=["decay", "tonality"], censors=t)
    assert json.loads(gate.read_text())["_unbounded"] == {"tonality": "by hand"}
    # `decay` has rows in `_rows`, so it is bounded and carries no entry.
    assert "decay" not in json.loads(gate.read_text())["_unbounded"]


def test_a_kit_dimension_with_rows_gets_a_bound_and_no_entry(tmp_path):
    rows = [(_hit(flatness_db=None), _hit()) for _ in range(3)]
    un, gate = _kit_gate(
        tmp_path,
        rows,
        ["tonality"],
        summary={"tonality": {"median": 1.0, "abs_median": 1.0, "p90": 1.0, "n": 3}},
    )
    assert un == {}
    assert "tonality" in json.loads(gate.read_text())["bounds"]


def test_vel_range_names_single_velocity_notes():
    """A note with one velocity reads no span; the gate says so instead of dropping vel_range."""
    tally = profile_gate.CensorTally()
    model = {36: {100: -6.0}, 38: {100: -8.0}}
    ref = {36: {100: -5.0}, 38: {100: -7.0}}
    assert profile_gate.vel_range_deltas(model, ref, tally) == []
    assert tally.reason("vel_range") == (
        "measured: no rows; censored on both, 2 of 2 notes (fewer than two velocities at this note)"
    )
    # The model alone short of two velocities names the model side.
    tally = profile_gate.CensorTally()
    profile_gate.vel_range_deltas({36: {100: -6.0}}, {36: {40: -20.0, 100: -5.0}}, tally)
    assert "censored on model, 1 of 1 notes" in tally.reason("vel_range")
    # Two shared velocities read a span and leave nothing tallied.
    tally = profile_gate.CensorTally()
    got = profile_gate.vel_range_deltas(
        {36: {40: -20.0, 100: -6.0}}, {36: {40: -21.0, 100: -5.0}}, tally
    )
    assert got == [pytest.approx(14.0 - 16.0)] and tally.reason("vel_range") is None


def test_a_dirty_tree_is_refused_unless_allowed_and_then_recorded(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(
        profile_gate, "tree_state", lambda: {"head": "abc1234", "dirty": ["src/x.cpp"]}
    )
    gate = tmp_path / "g.json"
    assert write_gate_file(_rows(10.0), gate, "ref", 1.0) == 2
    assert not gate.exists()
    assert "src/x.cpp" in capsys.readouterr().err
    assert write_gate_file(_rows(10.0), gate, "ref", 1.0, allow_dirty=True) == 0
    prov = json.loads(gate.read_text())["bounds"]["decay"]["provenance"]["median"]
    assert prov["set_tree"] == {"head": "abc1234", "dirty": ["src/x.cpp"]}
    notes = profile_gate.provenance_notes(json.loads(gate.read_text()))
    assert any(n.startswith("set on a dirty tree: decay.") for n in notes)


def test_a_kept_bound_keeps_the_tree_it_was_set_on(tmp_path, monkeypatch):
    monkeypatch.setattr(
        profile_gate, "tree_state", lambda: {"head": "abc1234", "dirty": ["src/x.cpp"]}
    )
    gate = tmp_path / "g.json"
    write_gate_file(_rows(10.0), gate, "ref", 1.0, allow_dirty=True)
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "def5678", "dirty": []})
    write_gate_file(_rows(12.0), gate, "ref", 1.0)
    prov = json.loads(gate.read_text())["bounds"]["decay"]["provenance"]["median"]
    assert prov["outcome"] == "exceeded"
    assert prov["set_tree"] == {"head": "abc1234", "dirty": ["src/x.cpp"]}


def _dirty_note(gate):
    return [n for n in profile_gate.provenance_notes(json.loads(gate.read_text())) if "dirty" in n]


def test_a_dirty_set_held_equal_on_a_clean_tree_leaves_the_note(tmp_path, monkeypatch):
    gate = tmp_path / "g.json"
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "a", "dirty": ["src/x.cpp"]})
    _record(gate, 10.0, allow_dirty=True)
    assert _dirty_note(gate)
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "b", "dirty": []})
    p = _record(gate, 10.0)
    assert p["set_tree"] == {"head": "a", "dirty": ["src/x.cpp"]}
    assert p["held_tree"] == {"head": "b", "dirty": []}
    assert p["clean_held_utc"] == p["held_utc"]
    assert _dirty_note(gate) == []
    # A later dirty hold keeps the clean one it already earned.
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "c", "dirty": ["src/y.cpp"]})
    p = _record(gate, 10.0, allow_dirty=True)
    assert p["held_tree"]["dirty"] == ["src/y.cpp"] and "clean_held_utc" in p
    assert _dirty_note(gate) == []


def test_a_dirty_hold_earns_no_clean_stamp(tmp_path, monkeypatch):
    gate = tmp_path / "g.json"
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "a", "dirty": ["src/x.cpp"]})
    _record(gate, 10.0, allow_dirty=True)
    p = _record(gate, 10.0, allow_dirty=True)
    assert p["outcome"] == "kept" and "clean_held_utc" not in p
    assert p["held_tree"] == {"head": "a", "dirty": ["src/x.cpp"]}
    assert _dirty_note(gate) == ["set on a dirty tree: decay.abs_median, decay.median, decay.p90"]


def test_a_value_set_again_on_a_dirty_tree_loses_its_clean_hold(tmp_path, monkeypatch):
    gate = tmp_path / "g.json"
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "a", "dirty": ["src/x.cpp"]})
    _record(gate, 10.0, allow_dirty=True)
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "b", "dirty": []})
    _record(gate, 10.0)
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "c", "dirty": ["src/y.cpp"]})
    p = _record(gate, 8.0, allow_dirty=True)
    assert p["outcome"] == "tightened" and "clean_held_utc" not in p
    assert _dirty_note(gate)


def test_an_exceeded_bound_keeps_the_tree_of_its_last_hold(tmp_path, monkeypatch):
    gate = tmp_path / "g.json"
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "a", "dirty": []})
    _record(gate, 10.0)
    monkeypatch.setattr(profile_gate, "tree_state", lambda: {"head": "b", "dirty": ["src/x.cpp"]})
    p = _record(gate, 12.0, allow_dirty=True)
    assert p["outcome"] == "exceeded"
    assert p["held_tree"] == {"head": "a", "dirty": []}


def test_a_gate_without_hold_trees_reads_as_before():
    gate = {
        "bounds": {
            "decay": {
                "median": 1.0,
                "provenance": {
                    "median": {
                        "set_utc": "t",
                        "held_utc": "u",
                        "outcome": "kept",
                        "set_tree": {"head": "a", "dirty": ["src/x.cpp"]},
                    }
                },
            }
        }
    }
    assert "set on a dirty tree: decay.median" in profile_gate.provenance_notes(gate)


def test_a_verified_hold_names_no_tree_and_earns_no_clean_stamp():
    import gate_verify

    gate = {
        "bounds": {
            "decay": {
                "median": 1.0,
                "provenance": {
                    "median": {
                        "set_utc": "t",
                        "held_utc": "u",
                        "outcome": "kept",
                        "set_tree": {"head": "a", "dirty": ["src/x.cpp"]},
                        "held_tree": {"head": "a", "dirty": ["src/x.cpp"]},
                    }
                },
            }
        }
    }
    out, found = gate_verify.apply(gate, [{"key": "decay", "stat": "median", "value": 0.5}], "v")
    p = out["bounds"]["decay"]["provenance"]["median"]
    assert found["held"] == ["decay.median"] and p["held_utc"] == "v"
    assert "held_tree" not in p and "clean_held_utc" not in p
    assert "set on a dirty tree: decay.median" in profile_gate.provenance_notes(out)
