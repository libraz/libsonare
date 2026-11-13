"""The knob-class table: its reader, its use by `--spec auto` and a hand spec, and the
declared ranges the clamp check reads beside the engine's own clamp."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from catalogue import Catalogue, parse_dump
from knobs import (
    DECLARED_FILE,
    DECLARED_MACRO,
    KNOB_CLASSES_PATH,
    Knob,
    KnobClass,
    KnobClasses,
    KnobReach,
    absent_summary,
    auto_spec_report,
    clamp_check,
    clamp_warning,
    declared_ranges,
    hand_spec_warnings,
    load_declared_bounds,
    load_knob_classes,
    pinned_lines,
    refuse_absent_knobs,
    shared_engine,
    shared_lines,
    switch_candidates,
)


def _entry(key, cls, **kw):
    return KnobClass(key=key, cls=cls, lane="test", **kw)


def _classes(*entries):
    return KnobClasses({e.key: e for e in entries})


def _catalogue(defaults, bounds=None, **kw):
    return Catalogue(defaults, {(0, 0): "p0"}, bounds or {}, {"p0": "subtractive"}, **kw)


def _write(tmp_path, entries, **blocks):
    path = tmp_path / "knob-classes.json"
    path.write_text(json.dumps({"entries": entries, **blocks}))
    return path


# --- the resolve check -------------------------------------------------------


def test_strict_load_of_an_entry_the_catalogue_no_longer_reports_fails_naming_it(tmp_path):
    cat = _catalogue({"p0.brass.mute": 0.0, "p0.waveform": 1.0})
    path = _write(
        tmp_path,
        {
            "brass.mute": {"class": "identity", "lane": "sustained"},
            "brass.mute_renamed": {"class": "identity", "lane": "sustained"},
        },
    )
    with pytest.raises(ValueError, match="brass.mute_renamed") as exc:
        load_knob_classes(path, cat, strict=True)
    assert "'brass.mute'" not in str(exc.value)


def test_index_and_glob_patterns_resolve_against_catalogue_keys(tmp_path):
    cat = _catalogue({"p0.fm.ops0.ratio": 1.0, "p0.fm.ops1.ratio": 2.0, "piano_voice.kFooMax": 3.0})
    path = _write(
        tmp_path,
        {
            "fm.ops[*].ratio": {"class": "identity"},
            "piano_voice.k*Max": {"class": "identity"},
        },
    )
    classes = load_knob_classes(path, cat)
    assert classes.class_of("p0.fm.ops1.ratio") == "identity"
    assert classes.class_of("piano_voice.kFooMax") == "identity"
    assert classes.class_of("p0.fm.ops1.feedback") is None


def test_a_pattern_that_matches_nothing_fails_even_when_a_glob(tmp_path):
    cat = _catalogue({"p0.waveform": 1.0})
    path = _write(tmp_path, {"piano_voice.k*Max": {"class": "identity"}})
    with pytest.raises(ValueError, match=r"piano_voice\.k\*Max"):
        load_knob_classes(path, cat, strict=True)


def test_pending_landing_and_notes_are_exempt_from_the_resolve_check(tmp_path):
    cat = _catalogue({"p0.waveform": 1.0})
    path = _write(
        tmp_path,
        {"waveform": {"class": "identity"}},
        notes={"mode": "not reachable"},
        pending_landing={"piano_voice.kNotYet": {"class": "set_by_measurement"}},
    )
    classes = load_knob_classes(path, cat)
    assert classes.note_for("mode") == "not reachable"
    assert "piano_voice.kNotYet" in classes.pending_landing
    assert classes.class_of("piano_voice.kNotYet") is None


def test_a_group_member_without_a_group_or_an_unknown_class_is_refused(tmp_path):
    cat = _catalogue({"p0.waveform": 1.0})
    with pytest.raises(ValueError, match="compensating_group"):
        load_knob_classes(_write(tmp_path, {"waveform": {"class": "compensating_group"}}), cat)
    with pytest.raises(ValueError, match="not one of"):
        load_knob_classes(_write(tmp_path, {"waveform": {"class": "bogus"}}), cat)


def _shipped_keys():
    data = json.loads(KNOB_CLASSES_PATH.read_text())
    keys = {}
    for pattern in [*data["entries"], *(p for p in data["reach"] if not p.startswith("_"))]:
        concrete = pattern.replace("[*]", "0").replace("*", "x")
        keys[concrete if concrete.split(".")[0].endswith("_voice") else f"p0.{concrete}"] = 1.0
    return keys


def test_the_shipped_table_loads_against_a_catalogue_naming_each_of_its_keys():
    classes = load_knob_classes(KNOB_CLASSES_PATH, _catalogue(_shipped_keys()), strict=True)
    assert not classes.absent
    assert {r.requires for r in classes.reach.values()} == {("motion",), ("stereo",)}
    assert set(classes.groups) == {"piano.excitation_chain"}
    for key in classes.groups["piano.excitation_chain"]:
        assert classes.entries[key].evidence in ("measured", "code reading")
    stage = classes.entries["harpsichord.board_diffuse_db"]
    assert (stage.off_value, stage.off_within) == (-120.0, 1.0)


# --- --spec auto -------------------------------------------------------------


def _offered(spec):
    return {e["tunable"]: e for e in spec}


def test_identity_switch_and_measured_fields_are_not_offered_and_are_counted():
    cat = _catalogue(
        {"p0.waveform": 1.0, "p0.one_shot": 0.0, "p0.pitch_offset_cents": 3.0, "p0.vibrato": 0.5},
        {
            "waveform": (0.0, 1.0),
            "one_shot": (0.0, 1.0),
            "pitch_offset_cents": (-1.0, 1.0),
            "vibrato": (0.0, 1.0),
        },
    )
    classes = _classes(
        _entry("waveform", "identity"),
        _entry("one_shot", "switch"),
        _entry("pitch_offset_cents", "set_by_measurement"),
    )
    plain, _ = auto_spec_report(0, cat)
    spec, report = auto_spec_report(0, cat, classes=classes)
    assert {"p0.waveform", "p0.one_shot"} <= set(_offered(plain))
    assert set(_offered(spec)) == {"p0.vibrato"}
    assert {c: len(k) for c, k in report.excluded.items() if k} == {"identity": 1, "switch": 1}
    assert any("identity 1" in line and "switch 1" in line for line in report.lines())


def test_a_stage_enable_is_left_out_where_the_patch_ships_it_absent():
    cat = _catalogue({"p0.drive": 0.0}, {"drive": (0.0, 1.0)})
    spec, report = auto_spec_report(0, cat, classes=_classes(_entry("drive", "stage_enable")))
    assert spec == []
    assert report.stage_off == ["p0.drive"]


def test_a_stage_enable_that_ships_present_is_cut_clear_of_zero():
    cat = _catalogue({"p0.drive": 0.4}, {"drive": (0.0, 1.0)})
    classes = _classes(_entry("drive", "stage_enable"))
    (knob,) = auto_spec_report(0, cat, classes=classes)[0]
    assert knob["min"] > 0.0
    assert knob["max"] == 1.0
    assert knob["start"] == 0.4
    plain = auto_spec_report(0, cat)[0][0]
    assert plain["min"] == 0.0


def test_a_logarithmic_stage_enable_keeps_a_positive_floor():
    cat = _catalogue({"p0.hp_cutoff_hz": 65.0}, {"hp_cutoff_hz": (0.0, 20000.0)})
    classes = _classes(_entry("hp_cutoff_hz", "stage_enable"))
    (knob,) = auto_spec_report(0, cat, classes=classes)[0]
    assert knob["scale"] == "log"
    assert knob["min"] > 0.0


def test_a_stage_enable_whose_off_value_is_minus_120_is_cut_above_minus_119():
    entry = _entry("harpsichord.board_diffuse_db", "stage_enable", off_value=-120.0, off_within=1.0)
    bounds = {"harpsichord.board_diffuse_db": (-120.0, 0.0)}

    def run(shipped):
        cat = _catalogue({"p0.harpsichord.board_diffuse_db": shipped}, bounds)
        return auto_spec_report(0, cat, classes=_classes(entry))

    spec, report = run(-30.0)
    (knob,) = spec
    assert knob["min"] > -119.0
    assert knob["max"] == 0.0
    assert report.clipped == ["p0.harpsichord.board_diffuse_db"]
    for absent in (-120.0, -119.0):
        spec, report = run(absent)
        assert spec == []
        assert report.stage_off == ["p0.harpsichord.board_diffuse_db"]


def test_a_stage_enable_range_that_never_reached_the_off_value_is_left_alone():
    cat = _catalogue({"p0.board_db": -30.0}, {"board_db": (-60.0, 0.0)})
    entry = _entry("board_db", "stage_enable", off_value=-120.0, off_within=1.0)
    (knob,) = auto_spec_report(0, cat, classes=_classes(entry))[0]
    assert (knob["min"], knob["max"]) == (-60.0, 0.0)


def _group_catalogue():
    return _catalogue(
        {"p0.grp.a": 0.5, "p0.grp.b": 0.25, "grp_voice.kC": 2.0, "p0.other": 0.5},
        {"grp.a": (0.0, 1.0), "grp.b": (0.0, 1.0), "other": (0.0, 1.0)},
    )


def _group_classes():
    return _classes(
        _entry("grp.a", "compensating_group", group="g"),
        _entry("grp.b", "compensating_group", group="g"),
        _entry("grp_voice.kC", "compensating_group", group="g"),
    )


def test_a_group_is_closed_by_default_and_listed():
    spec, report = auto_spec_report(0, _group_catalogue(), classes=_group_classes())
    assert set(_offered(spec)) == {"p0.other"}
    assert report.groups_closed == {"g": 3}
    assert "group g not opened (open it with --open-group g)" in report.lines()


def test_an_opened_group_offers_every_member():
    spec, report = auto_spec_report(
        0, _group_catalogue(), classes=_group_classes(), open_groups=("g",)
    )
    assert set(_offered(spec)) == {"p0.other", "p0.grp.a", "p0.grp.b", "grp_voice.kC"}
    assert report.groups_open == ["g"]
    assert report.groups_closed == {}


def test_a_group_cannot_be_opened_partly_or_by_a_name_nobody_gave():
    cat, classes = _group_catalogue(), _group_classes()
    with pytest.raises(ValueError, match="grp_voice.kC"):
        auto_spec_report(0, cat, classes=classes, open_groups=("g",), patch_only=True)
    with pytest.raises(ValueError, match="no such compensating group"):
        auto_spec_report(0, cat, classes=classes, open_groups=("nope",))
    with pytest.raises(ValueError, match="needs the field classes"):
        auto_spec_report(0, cat, open_groups=("g",))


def test_opening_a_group_with_no_member_on_this_patch_is_refused():
    cat = _catalogue({"p0.other": 0.5}, {"other": (0.0, 1.0)})
    with pytest.raises(ValueError, match="none of its members"):
        auto_spec_report(0, cat, classes=_group_classes(), open_groups=("g",))


# --- a hand spec -------------------------------------------------------------


def _knob(label):
    return Knob(label=label, lo=0.0, hi=1.0, log=False, start_value=0.5, tunable=label)


def test_a_hand_spec_naming_a_held_out_field_keeps_it_and_says_so():
    classes = _classes(
        _entry("waveform", "identity", basis="oscillator family"),
        _entry("one_shot", "switch"),
        _entry("pitch_offset_cents", "set_by_measurement"),
        _entry("drive", "stage_enable"),
    )
    knobs = [
        _knob(f"p0.{n}") for n in ("waveform", "one_shot", "pitch_offset_cents", "drive", "gain")
    ]
    lines = hand_spec_warnings(knobs, classes)
    assert len(lines) == 3
    assert "an identity knob, not searched" in lines[0]
    assert "oscillator family" in lines[0]
    assert "a switch knob, not searched" in lines[1]
    assert "set by measurement, not searched" in lines[2]
    assert hand_spec_warnings(knobs, None) == []


# --- reach: a field only a particular gated dimension can judge ------------------


def _reach_classes(**requires):
    return KnobClasses(
        {}, reach={p: KnobReach(p, tuple(r), "test basis") for p, r in requires.items()}
    )


_REACH_CAT = _catalogue(
    {"p0.lfo_rate_hz": 5.0, "p0.stereo_spread": 0.2, "p0.brightness": 0.5},
    {"lfo_rate_hz": (0.0, 40.0), "stereo_spread": (0.0, 1.0), "brightness": (0.0, 1.0)},
)


def test_motion_fields_are_left_out_when_no_motion_dimension_is_gated():
    classes = _reach_classes(lfo_rate_hz=["motion"], stereo_spread=["stereo"])
    spec, report = auto_spec_report(0, _REACH_CAT, classes=classes, gated={"attack", "tnr"})
    assert set(_offered(spec)) == {"p0.brightness"}
    assert report.unjudged == {"motion": ["p0.lfo_rate_hz"], "stereo": ["p0.stereo_spread"]}
    assert not report.no_gate
    summary = " ".join(report.lines())
    assert "no gated dimension can judge them" in summary
    assert "motion 1" in summary and "stereo 1" in summary


def test_a_motion_field_is_kept_once_a_motion_dimension_is_gated():
    classes = _reach_classes(lfo_rate_hz=["motion"])
    spec, report = auto_spec_report(0, _REACH_CAT, classes=classes, gated={"motion"})
    assert "p0.lfo_rate_hz" in _offered(spec)
    assert report.unjudged == {}


def test_a_stereo_field_is_kept_when_stereo_is_gated_and_out_when_it_is_not():
    classes = _reach_classes(stereo_spread=["stereo"])
    kept, _ = auto_spec_report(0, _REACH_CAT, classes=classes, gated={"stereo", "tnr"})
    out, report = auto_spec_report(0, _REACH_CAT, classes=classes, gated={"tnr"})
    assert "p0.stereo_spread" in _offered(kept)
    assert "p0.stereo_spread" not in _offered(out)
    assert report.unjudged == {"stereo": ["p0.stereo_spread"]}


def test_every_required_dimension_must_be_gated_and_each_missing_one_is_named():
    classes = _reach_classes(lfo_rate_hz=["motion", "stereo"])
    spec, report = auto_spec_report(0, _REACH_CAT, classes=classes, gated={"motion"})
    assert "p0.lfo_rate_hz" not in _offered(spec)
    assert report.unjudged == {"stereo": ["p0.lfo_rate_hz"]}


def test_no_gate_is_read_as_nothing_gated_and_the_report_says_so():
    classes = _reach_classes(lfo_rate_hz=["motion"], stereo_spread=["stereo"])
    spec, report = auto_spec_report(0, _REACH_CAT, classes=classes, gated=None)
    assert set(_offered(spec)) == {"p0.brightness"}
    assert report.no_gate
    assert any("no gate found" in line for line in report.lines())


def test_a_hand_spec_naming_an_unjudged_field_keeps_it_and_warns():
    classes = _reach_classes(lfo_rate_hz=["motion"])
    knobs = [_knob("p0.lfo_rate_hz"), _knob("p0.brightness")]
    lines = hand_spec_warnings(knobs, classes, {"tnr"})
    assert len(lines) == 1
    assert "p0.lfo_rate_hz" in lines[0] and "needs motion" in lines[0]
    assert "searched here because the spec names it" in lines[0] and "test basis" in lines[0]
    assert hand_spec_warnings(knobs, classes, {"motion"}) == []
    assert "no gate found" in hand_spec_warnings(knobs, classes, None)[0]


def test_strict_load_of_a_reach_pattern_that_resolves_nothing_fails_naming_it(tmp_path):
    cat = _catalogue({"p0.lfo_rate_hz": 5.0})
    path = _write(
        tmp_path,
        {},
        reach={
            "lfo_rate_hz": {"requires": ["motion"], "basis": "x"},
            "lfo_renamed_hz": {"requires": ["motion"], "basis": "x"},
        },
    )
    with pytest.raises(ValueError, match="lfo_renamed_hz") as exc:
        load_knob_classes(path, cat, strict=True)
    assert "'lfo_rate_hz'" not in str(exc.value)


def test_a_reach_entry_without_a_dimension_is_refused(tmp_path):
    cat = _catalogue({"p0.lfo_rate_hz": 5.0})
    path = _write(tmp_path, {}, reach={"lfo_rate_hz": {"requires": []}})
    with pytest.raises(ValueError, match="requires"):
        load_knob_classes(path, cat)


def test_the_shipped_reach_table_covers_the_voicing_fields_a_trombone_refit_moved():
    classes = load_knob_classes(KNOB_CLASSES_PATH, _catalogue(_shipped_keys()))
    for key in ("lfo_rate_hz", "lfo_to_pitch_cents", "drift_cents", "drift_rate_hz"):
        assert classes.unjudged_by(f"p0.{key}", {"attack", "body"}) == ["motion"]
    assert classes.unjudged_by("p0.stereo_spread", {"attack"}) == ["stereo"]
    assert classes.unjudged_by("p0.stereo_spread", {"stereo"}) == []


# --- switch candidates -------------------------------------------------------


def test_integer_valued_float_fields_are_listed_as_switch_candidates():
    cat = Catalogue(
        {
            "p0.sel": 1.0,
            "p1.sel": 2.0,
            "p0.amount": 0.5,
            "p1.amount": 2.0,
            "p0.fixed": 3.0,
            "p1.fixed": 3.0,
            "p0.ops0.mode": 0.0,
            "p1.ops0.mode": 1.0,
            "p0.known": 0.0,
            "p1.known": 1.0,
            "eng.kConst": 1.0,
        },
        {(0, 0): "p0", (1, 0): "p1"},
        {},
        {"p0": "x", "p1": "x"},
    )
    classes = _classes(_entry("known", "switch"))
    found = switch_candidates(cat, classes)
    assert set(found) == {"sel", "ops[*].mode"}
    assert found["sel"] == {"class": None, "values": [1.0, 2.0]}


# --- declared ranges ---------------------------------------------------------


def _constant_knob(label, lo, hi):
    return Knob(label=label, lo=lo, hi=hi, log=False, start_value=lo, tunable=label)


def test_a_declared_file_range_is_checked_and_labelled():
    declared = {"percussion_voice.kRate": (10.0, 400.0)}
    knobs = [_constant_knob("percussion_voice.kRate", 5.0, 800.0)]
    ranges, mismatches = declared_ranges(knobs, _catalogue({}), declared)
    assert ranges == {"percussion_voice.kRate": (10.0, 400.0, DECLARED_FILE)}
    assert mismatches == []
    (entry,), refusals = clamp_check(knobs, {}, ranges)
    assert refusals == []
    assert entry["source"] == DECLARED_FILE
    assert entry["inert"] == [[5.0, 10.0], [400.0, 800.0]]
    assert DECLARED_FILE in clamp_warning(entry)
    assert "clamp" not in clamp_warning(entry)


def test_a_range_wholly_outside_a_declared_range_is_refused_by_that_name():
    knobs = [_constant_knob("percussion_voice.kRate", 500.0, 900.0)]
    ranges, _ = declared_ranges(knobs, _catalogue({}), {"percussion_voice.kRate": (10.0, 400.0)})
    _, refusals = clamp_check(knobs, {}, ranges)
    assert len(refusals) == 1
    assert DECLARED_FILE in refusals[0]
    assert "inert" not in refusals[0]


def test_tunable_bound_lines_parse_into_the_catalogue_and_are_labelled_macro():
    text = (
        "percussion_voice.kRate\t100\n"
        "#tunable_bound\tpercussion_voice.kRate\t20\t300\n"
        "#tunable_bound\tmalformed\t1\n"
    )
    cat = parse_dump(text)
    assert cat.defaults == {"percussion_voice.kRate": 100.0}
    assert cat.tunable_bounds == {"percussion_voice.kRate": (20.0, 300.0)}
    knobs = [_constant_knob("percussion_voice.kRate", 30.0, 200.0)]
    ranges, mismatches = declared_ranges(knobs, cat, {})
    assert ranges == {"percussion_voice.kRate": (20.0, 300.0, DECLARED_MACRO)}
    assert mismatches == []


def test_a_key_declared_in_both_places_with_different_ranges_is_a_mismatch():
    cat = parse_dump("#tunable_bound\tpercussion_voice.kRate\t20\t300\n")
    knobs = [_constant_knob("percussion_voice.kRate", 30.0, 200.0)]
    ranges, mismatches = declared_ranges(knobs, cat, {"percussion_voice.kRate": (10.0, 400.0)})
    assert ranges["percussion_voice.kRate"][2] == DECLARED_MACRO
    (line,) = mismatches
    assert DECLARED_FILE in line
    assert DECLARED_MACRO in line
    same = declared_ranges(knobs, cat, {"percussion_voice.kRate": (20.0, 300.0)})[1]
    assert same == []


def test_a_patch_field_takes_no_declared_range():
    cat = _catalogue({"p0.gain": 1.0}, tunable_bounds={"p0.gain": (0.0, 1.0)})
    ranges, _ = declared_ranges([_constant_knob("p0.gain", 0.0, 2.0)], cat, {"p0.gain": (0.0, 1.0)})
    assert ranges == {}


def test_the_declared_bounds_file_reads_only_its_bounds_block(tmp_path):
    path = tmp_path / "tunable-bounds.json"
    path.write_text(
        json.dumps(
            {
                "bounds": {"a_voice.kX": {"lo": 1, "hi": 2}},
                "patch_fields_pending_clamp": {"percussion.noise_burst_count": {"lo": 0, "hi": 8}},
            }
        )
    )
    assert load_declared_bounds(path) == {"a_voice.kX": (1.0, 2.0)}
    path.write_text(json.dumps({"bounds": {"a_voice.kX": {"lo": 2, "hi": 2}}}))
    with pytest.raises(ValueError, match="a_voice.kX"):
        load_declared_bounds(path)


def _refuse(knobs, classes, *, catalogue=None, covered=(), probe=False):
    refuse_absent_knobs(
        knobs,
        classes,
        catalogue=catalogue or _catalogue({}),
        covered=set(covered),
        probe=probe,
    )


# --- an absent entry is refused where a fit names it --------------------------


def _knob(tunable):
    return Knob(tunable, 0.0, 1.0, False, 0.5, tunable=tunable)


def _absent_entry_classes(tmp_path):
    cat = _catalogue({"p0.brass.mute": 0.0, "p0.waveform": 1.0})
    path = _write(
        tmp_path,
        {
            "brass.mute": {"class": "identity", "lane": "sustained"},
            "piano_voice.kHammerJunction": {"class": "identity", "lane": "piano"},
        },
    )
    return load_knob_classes(path, cat)


def test_an_absent_entry_loads_and_is_listed_with_its_lane(tmp_path):
    classes = _absent_entry_classes(tmp_path)
    assert list(classes.absent) == ["piano_voice.kHammerJunction"]
    assert classes.absent["piano_voice.kHammerJunction"].lane == "piano"
    assert classes.class_of("p0.brass.mute") == "identity"
    assert absent_summary(classes) == (
        "knob classes: 1 entry not in this library "
        "(piano: piano_voice.kHammerJunction); fits naming it are refused"
    )


def test_a_fit_naming_an_absent_entry_is_refused_naming_entry_and_lane(tmp_path):
    classes = _absent_entry_classes(tmp_path)
    with pytest.raises(ValueError, match="piano_voice.kHammerJunction") as exc:
        _refuse([_knob("p0.waveform"), _knob("piano_voice.kHammerJunction")], classes)
    assert "lane piano" in str(exc.value)
    assert "uncommitted code" in str(exc.value)


def test_a_fit_not_naming_an_absent_entry_proceeds(tmp_path):
    classes = _absent_entry_classes(tmp_path)
    _refuse([_knob("p0.waveform"), _knob("p0.brass.mute")], classes)
    assert absent_summary(_classes()) == ""


def test_a_patch_relative_absent_entry_matches_a_patch_qualified_knob(tmp_path):
    cat = _catalogue({"p0.waveform": 1.0})
    classes = load_knob_classes(_write(tmp_path, {"ks.pluck_ms": {"class": "identity"}}), cat)
    with pytest.raises(ValueError, match=r"ks\.pluck_ms"):
        _refuse([_knob("banjo.ks.pluck_ms")], classes)


def test_an_absent_reach_pattern_loads_and_refuses_only_a_fit_under_it(tmp_path):
    cat = _catalogue({"p0.lfo_rate_hz": 5.0})
    path = _write(
        tmp_path,
        {},
        reach={
            "lfo_rate_hz": {"requires": ["motion"], "basis": "x"},
            "drift_*": {"requires": ["motion"], "basis": "x"},
        },
    )
    classes = load_knob_classes(path, cat)
    assert list(classes.absent) == ["drift_*"]
    assert "drift_*" not in classes.reach
    _refuse([_knob("p0.lfo_rate_hz")], classes)
    with pytest.raises(ValueError, match=r"drift_\*"):
        _refuse([_knob("p0.drift_cents")], classes)


# --- pinned by ruling --------------------------------------------------------

_PINNED = {
    "class": "pinned",
    "value": 0.286475,
    "basis": "ruled: too few terms read it",
    "lane": "sustained",
}


def _pinned_classes(tmp_path, defaults=None):
    cat = Catalogue(
        defaults or {"clarinet.body_mix": 0.286475, "oboe.body_mix": 0.5, "clarinet.gain": 1.0},
        {(71, 0): "clarinet", (68, 0): "oboe"},
        {},
        {"clarinet": "reed", "oboe": "reed"},
    )
    return cat, load_knob_classes(_write(tmp_path, {"clarinet.body_mix": _PINNED}), cat)


def test_a_pinned_entry_is_never_offered_by_auto_and_counts_as_excluded(tmp_path):
    cat, classes = _pinned_classes(tmp_path)
    spec, report = auto_spec_report(71, cat, classes=classes)
    names = [e["tunable"] for e in spec]
    assert "clarinet.body_mix" not in names
    assert report.excluded["pinned"] == ["clarinet.body_mix"]
    spec, _ = auto_spec_report(68, cat, classes=classes)
    assert "oboe.body_mix" in [e["tunable"] for e in spec]


def test_a_spec_naming_a_pinned_field_is_refused_with_key_value_and_basis(tmp_path):
    _, classes = _pinned_classes(tmp_path)
    with pytest.raises(ValueError, match=r"clarinet\.body_mix is pinned at 0\.286475") as exc:
        _refuse([_knob("clarinet.gain"), _knob("clarinet.body_mix")], classes)
    assert "lane sustained" in str(exc.value)
    assert "too few terms read it" in str(exc.value)
    assert hand_spec_warnings([_knob("clarinet.body_mix")], classes) == []


def test_a_pinned_entry_is_scoped_to_its_voice(tmp_path):
    _, classes = _pinned_classes(tmp_path)
    _refuse([_knob("oboe.body_mix"), _knob("clarinet.gain")], classes)
    assert classes.class_of("oboe.body_mix") is None


def test_the_start_up_line_prints_the_voice_pins_and_flags_a_stale_value(tmp_path):
    cat, classes = _pinned_classes(tmp_path)
    (line,) = pinned_lines(classes, cat, "clarinet")
    assert line.startswith("pinned by ruling: clarinet.body_mix = 0.286475 (ships 0.286475;")
    assert pinned_lines(classes, cat, "oboe") == []
    cat2, classes2 = _pinned_classes(tmp_path, {"clarinet.body_mix": 0.3})
    assert "STALE: the library ships 0.3" in pinned_lines(classes2, cat2, "clarinet")[0]


@pytest.mark.parametrize(
    "entry",
    [
        {**_PINNED, "value": None},
        {**_PINNED, "class": "identity"},
        {**_PINNED, "value": "0.3"},
        {**_PINNED, "basis": ""},
        {**_PINNED, "lane": ""},
    ],
)
def test_a_malformed_pinned_entry_is_refused_at_load(tmp_path, entry):
    entry = {k: v for k, v in entry.items() if v is not None}
    cat = _catalogue({"clarinet.body_mix": 0.286475})
    with pytest.raises(ValueError, match="clarinet.body_mix"):
        load_knob_classes(_write(tmp_path, {"clarinet.body_mix": entry}), cat)


def test_a_pinned_entry_must_name_one_patch_field(tmp_path):
    cat = _catalogue({"p0.body_mix": 0.3})
    with pytest.raises(ValueError, match="pinned entry"):
        load_knob_classes(_write(tmp_path, {"body_mix": _PINNED}), cat)
    with pytest.raises(ValueError, match="pinned entry"):
        load_knob_classes(_write(tmp_path, {"clar*.body_mix": _PINNED}), cat)


# --- an engine constant is shared by every voice of its engine ---------------

_BRASS = ["brass_section", "french_horn", "muted_trumpet", "trombone", "trumpet", "tuba"]
_BELL = "brass_voice.kBellRadiationNorm"


def _engine_catalogue():
    modes = {p: "brass" for p in _BRASS}
    modes.update({"clarinet": "reed", "tenor_sax": "reed", "solo": "vocal_solo"})
    defaults = {_BELL: 0.35, "reed_voice.kBellRadiationNorm": 0.35, "gs_effects.kTank": 1.0}
    defaults.update({"vocal_solo_voice.kGain": 1.0, "trombone.brass.bell_cutoff_hz": 900.0})
    return Catalogue(defaults, {(57, 0): "trombone"}, {}, modes)


def test_an_engine_stem_resolves_to_the_engines_whole_voice_list():
    cat = _engine_catalogue()
    assert shared_engine(_BELL, cat) == ("brass", _BRASS)
    assert shared_engine("reed_voice.kBellRadiationNorm", cat) == (
        "reed",
        ["clarinet", "tenor_sax"],
    )


@pytest.mark.parametrize(
    "key",
    ["gs_effects.kTank", "trombone.brass.bell_cutoff_hz", "native_synth_voice.kX", "brass_voice"],
)
def test_a_key_that_is_no_reported_engines_constant_belongs_to_no_engine(key):
    assert shared_engine(key, _engine_catalogue()) is None


def test_a_brass_constant_is_refused_on_a_trombone_only_fit_naming_the_uncovered_voices():
    with pytest.raises(ValueError) as exc:
        _refuse([_knob(_BELL)], _classes(), catalogue=_engine_catalogue(), covered={"trombone"})
    text = str(exc.value)
    assert _BELL in text and "engine 'brass'" in text
    assert "the fit covers trombone" in text
    assert "uncovered: brass_section, french_horn, muted_trumpet, trumpet, tuba" in text
    assert "--dry-run" in text and "--program-only" in text


def test_a_brass_constant_is_allowed_when_the_fit_covers_every_brass_voice():
    _refuse([_knob(_BELL)], _classes(), catalogue=_engine_catalogue(), covered=set(_BRASS))


def test_a_brass_constant_is_allowed_on_a_declared_probe():
    _refuse(
        [_knob(_BELL)], _classes(), catalogue=_engine_catalogue(), covered={"trombone"}, probe=True
    )


def test_a_fit_of_one_engine_naming_another_engines_constant_is_refused():
    with pytest.raises(ValueError, match="uncovered: clarinet, tenor_sax"):
        _refuse(
            [_knob("reed_voice.kBellRadiationNorm")],
            _classes(),
            catalogue=_engine_catalogue(),
            covered={"trombone"},
        )


def test_a_one_voice_engines_constant_passes_for_a_fit_that_scores_that_voice():
    cat = Catalogue({"vocal_solo_voice.kGain": 1.0}, {}, {}, {"solo": "vocal_solo"})
    _refuse([_knob("vocal_solo_voice.kGain")], _classes(), catalogue=cat, covered={"solo"})


def test_a_patch_field_and_a_non_engine_stem_are_never_refused_as_shared():
    _refuse(
        [_knob("trombone.brass.bell_cutoff_hz"), _knob("gs_effects.kTank")],
        _classes(),
        catalogue=_engine_catalogue(),
        covered={"trombone"},
    )


def test_the_start_up_line_says_how_far_a_shared_constant_reaches():
    cat = _engine_catalogue()
    (full,) = shared_lines([_knob(_BELL)], cat, set(_BRASS), False)
    assert full == f"shared engine constant: {_BELL} (engine 'brass') covers all 6 voices"
    (probe,) = shared_lines([_knob(_BELL)], cat, {"trombone"}, True)
    assert "probe, nothing written back; 5 of 6 voices are not scored: brass_section" in probe
    assert shared_lines([_knob("gs_effects.kTank")], cat, set(), False) == []


# --- --spec auto drops what it may not own, a probe keeps it ------------------


def _auto_catalogue():
    cat = _engine_catalogue()
    cat.defaults.update(
        {
            "brass_voice.kBellRadiationMakeup": 2.45,
            "trombone.brass.bell_cutoff_hz": 900.0,
            "trombone.brass.mute": 0.0,
        }
    )
    return cat


_SHARED = ["brass_voice.kBellRadiationMakeup", "brass_voice.kBellRadiationNorm"]


def _auto_names(**kw):
    spec, report = auto_spec_report(57, _auto_catalogue(), classes=_classes(), **kw)
    return [e["tunable"] for e in spec], report


def test_auto_on_a_single_brass_voice_drops_the_engine_constants_and_reports_them():
    names, report = _auto_names(covered={"trombone"})
    assert not [n for n in names if n.startswith("brass_voice.")]
    assert "trombone.brass.bell_cutoff_hz" in names
    assert sorted(report.shared) == _SHARED
    assert "shared engine constant 2 (fit covers 1 of 6 voices)" in " ".join(report.lines())


def test_auto_on_a_joint_fit_of_every_brass_voice_offers_them():
    names, report = _auto_names(covered=set(_BRASS))
    assert set(_SHARED) <= set(names)
    assert report.shared == []


def test_auto_on_a_probe_offers_them_though_the_fit_covers_one_voice():
    names, report = _auto_names(covered={"trombone"}, probe=True)
    assert set(_SHARED) <= set(names)
    assert report.shared == []


def test_auto_without_a_coverage_argument_checks_nothing():
    names, _ = _auto_names()
    assert set(_SHARED) <= set(names)
