"""Tests for ``facts.field_bindings``: name pairing, unpaired rows, and non-vacuity.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_facts.py
"""

from __future__ import annotations

import sys
from collections import Counter
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import facts
from model import FunctionSig, Param, RecordField, RecordShape


def _sig(surface: str, *params: tuple[str, str]) -> FunctionSig:
    return FunctionSig(
        key="demo",
        surface=surface,
        params=[Param(name=n, raw_name=r) for n, r in params],
        file=f"{surface}.src",
        line=7,
    )


def test_param_pairs_by_canonical_name_and_reports_a_rename_as_two_rows():
    c = _sig("c", ("hop_length", "hop_length"), ("sr", "sr"), ("config", "config"))
    node = _sig(
        "node", ("hop_length", "hopLength"), ("sample_rate", "sampleRate"), ("request", "request")
    )
    rows = {(b.c_subject, b.surface_spelling) for b in facts._param_rows(c, node, "node")}
    assert ("hop_length", "hopLength") in rows, rows
    assert ("sr", None) in rows and (None, "sampleRate") in rows, rows
    assert ("config", None) not in rows, rows
    assert (None, "request") in rows, rows


def test_facade_bag_is_unpaired_unless_c_takes_the_same_bag():
    c = _sig("c", ("preset", "preset"), ("options", "options"))
    py = _sig("python", ("config", "config"), ("options", "options"))
    rows = {(b.c_subject, b.surface_spelling) for b in facts._param_rows(c, py, "python")}
    assert rows == {("preset", None), (None, "config")}, rows


def test_structural_sample_rate_pairs_with_the_facade_spelling_only_when_bound():
    c = FunctionSig(
        key="demo",
        surface="c",
        params=[
            Param(name="samples", raw_name="samples", structural=True),
            Param(name="sample_rate", raw_name="sample_rate", structural=True),
        ],
        file="c.h",
        line=1,
    )
    rows = {
        (b.c_subject, b.surface_spelling)
        for b in facts._param_rows(c, _sig("python", ("sr", "sr")), "python")
    }
    assert rows == {("sample_rate", "sr")}, rows
    assert facts._param_rows(c, _sig("node"), "node") == []


def test_record_skips_facade_only_fields_and_locates_unbound_rows_in_c():
    c = RecordShape(
        key="demo",
        surface="c",
        fields=[RecordField("gain_db", "gain_db"), RecordField("q", "q")],
        file="c.h",
        line=3,
    )
    ts = RecordShape(
        key="demo",
        surface="wasm",
        fields=[RecordField("gain_db", "gainDb"), RecordField("on_progress", "onProgress")],
        file="w.ts",
        line=9,
    )
    rows = facts._record_rows(c, ts, "wasm")
    by = {b.c_subject: b for b in rows}
    assert by["gain_db"].surface_spelling == "gainDb" and by["gain_db"].file == "w.ts"
    assert by["q"].surface_spelling is None and by["q"].file == "c.h"
    assert all(b.c_subject is not None for b in rows), rows


def test_a_second_form_parameter_is_a_subject_of_the_key_it_folds_into():
    base = FunctionSig(key="align", surface="c", params=[Param(name="rate", raw_name="rate")])
    ex = FunctionSig(
        key="align_ex",
        surface="c",
        params=[Param(name="rate", raw_name="rate"), Param(name="take_rate", raw_name="take_rate")],
    )
    other = FunctionSig(key="other_ex", surface="c", params=[])
    folded = facts._folded_variants([base, ex, other], {"align": base})
    assert folded == {"align": [ex]}, folded


def test_real_tree_yields_bound_rows_of_both_kinds_on_every_facade():
    rows = facts.field_bindings(_HERE.parent.parent)
    bound = Counter((b.surface, b.kind) for b in rows if b.c_subject and b.surface_spelling)
    for surface in facts.FACADES:
        for kind in ("param", "record_field"):
            assert bound[(surface, kind)] > 0, f"{surface} {kind}: no paired row"


def _c_param(name: str, type_: str) -> Param:
    return Param(name=name, raw_name=name, type=type_)


def _record(surface: str, raw: str, key: str, *fields: tuple[str, str]) -> RecordShape:
    return RecordShape(
        key=key, surface=surface, raw_name=raw, fields=[RecordField(n, r) for n, r in fields]
    )


def test_caller_filled_struct_members_bind_to_keywords_or_request_fields():
    desc = _record("c", "SonareEditDesc", "edit_desc", ("clip_id", "clip_id"), ("gain", "gain"))
    c = FunctionSig(key="edit", surface="c", params=[_c_param("desc", "const SonareEditDesc*")])
    py = _sig("python", ("clip_id", "clip_id"), ("gain", "gain"))
    rows = {
        (b.c_subject, b.surface_spelling)
        for b in facts._param_rows(c, py, "python", {desc.raw_name: desc})
    }
    assert rows == {("clip_id", "clip_id"), ("gain", "gain")}, rows
    request = _record(
        "node", "EditRequest", "edit_request", ("clip_id", "clipId"), ("gain", "gain")
    )
    node = FunctionSig(
        key="edit",
        surface="node",
        params=[Param(name="request", raw_name="request", type="EditRequest")],
    )
    rows = {
        (b.c_subject, b.surface_spelling)
        for b in facts._param_rows(
            c, node, "node", {desc.raw_name: desc}, {request.raw_name: request}
        )
    }
    assert rows == {("clip_id", "clipId"), ("gain", "gain")}, rows


def test_a_struct_the_facade_takes_as_its_mirror_is_left_to_the_record_rows():
    cfg = _record("c", "SonareMeterConfig", "meter_config", ("enabled", "enabled"))
    c = FunctionSig(
        key="meter", surface="c", params=[_c_param("config", "const SonareMeterConfig*")]
    )
    mirror = _record("node", "MeterConfig", "meter_config", ("enabled", "enabled"))
    node = FunctionSig(
        key="meter",
        surface="node",
        params=[Param(name="options", raw_name="options", type="MeterConfig")],
    )
    assert facts._param_rows(c, node, "node", {cfg.raw_name: cfg}, {mirror.raw_name: mirror}) == []


def test_a_c_argument_folded_into_a_request_field_binds_to_that_field():
    c = _sig("c", ("hop_length", "hop_length"))
    request = _record("node", "MfccRequest", "mfcc_request", ("hop_length", "hopLength"))
    node = FunctionSig(
        key="demo",
        surface="node",
        params=[
            Param(
                name="options", raw_name="options", type="OptionsOf<MfccRequest>", structural=True
            )
        ],
    )
    rows = {
        (b.c_subject, b.surface_spelling)
        for b in facts._param_rows(c, node, "node", {}, {request.raw_name: request})
    }
    assert rows == {("hop_length", "hopLength")}, rows


def test_a_handle_op_drops_its_handle_argument_for_a_class_method():
    c = FunctionSig(
        key="project_edit",
        surface="c",
        params=[_c_param("project", "SonareProject*"), _c_param("clip_id", "uint32_t")],
    )
    assert [p.name for p in facts._as_method(c, True, {}).params] == ["clip_id"]
    assert facts._as_method(c, False, {}) is c


def test_a_second_form_reports_under_the_key_the_c_layer_folds_it_to():
    assert facts.entry_key("analyze_json_ex") == "analyze_json"
    assert facts.entry_key("analyze_json_ex_with_progress") == "analyze_json_ex"
    assert facts.entry_key("mfcc") == "mfcc"


def test_real_tree_binds_a_handle_op_desc_and_every_reached_struct_entry():
    root = _HERE.parent.parent
    rows = facts.field_bindings(root)
    vocal = {
        (b.surface, b.c_subject)
        for b in rows
        if b.c_key == "project_apply_vocal_edit" and b.surface_spelling is not None
    }
    for surface in facts.FACADES:
        assert (surface, "clip_id") in vocal, f"{surface}: desc member clip_id unbound"
    assert facts.struct_entries_without_bindings(root, rows) == []
    stripped = [b for b in rows if b.c_key != "project_apply_vocal_edit"]
    assert "project_apply_vocal_edit" in facts.struct_entries_without_bindings(root, stripped)


def test_real_tree_binds_constructor_keywords_a_method_forwards_through_kwargs():
    rows = [b for b in facts.field_bindings(_HERE.parent.parent) if b.surface == "python"]
    bound = {(b.c_key, b.c_subject) for b in rows if b.surface_spelling is not None}
    unbound = {(b.c_key, b.c_subject) for b in rows if b.surface_spelling is None}
    assert ("polyphonic_analyze", "cent_max_hz") in bound
    assert ("project_apply_vocal_edit", "render_token") in bound
    assert ("chord_functions", "start_sec") not in unbound


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
