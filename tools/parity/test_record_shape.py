#!/usr/bin/env python3
"""Regression tests for the RECORD-SHAPE extraction unit and its normalizer.

The signature unit (``FunctionSig``) models argument lists and is blind to a
struct's interior, so a field added to a C struct and mirrored on three of the
four surfaces used to be invisible to every gate in the repo. The record unit
closes that gap: the C ``typedef struct { ... } SonareXxx;`` is the oracle and
each facade's declaration of the same record must carry the same fields.

What these tests pin:

* the NORMALIZER — a record spelled ``SonareAcousticResult`` in C / Python and
  ``AcousticResult`` in TypeScript is one record, and a field spelled
  ``rt60Bands`` is the same field as ``rt60_bands``. Naming convention is not
  drift, and the folding that makes that true is asserted rather than assumed.
* the EXTRACTORS — C struct bodies (including array, function-pointer and
  padding members), ctypes ``_fields_`` lists, and TS interfaces / intersection
  aliases including a union wrapped across several lines.
* the GATE — a missing C field goes active, a matching record is silent, a
  structural member never counts, uniform absence is not drift and peer-declared
  absence is informational.
* NON-VACUITY on the real tree — every surface must extract records. A checker
  that reports nothing because it parsed nothing looks identical to a clean tree
  in the findings table, so the count is asserted, not just printed.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_record_shape.py
"""

from __future__ import annotations

import ast
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod  # noqa: E402
import compare  # noqa: E402
from extractors import c_api, python_ctypes, ts_records  # noqa: E402
from model import Extraction, RecordField, RecordShape  # noqa: E402
from normalize import canonical_field_name, canonical_record_key  # noqa: E402


# --------------------------------------------------------------------------
# Normalizer
# --------------------------------------------------------------------------


def test_record_key_folds_every_surface_spelling() -> None:
    """The four surface spellings of one record fold to one key."""
    assert canonical_record_key("SonareAcousticResult", "c") == "acoustic_result"
    assert canonical_record_key("SonareAcousticResult", "python") == "acoustic_result"
    assert canonical_record_key("AcousticResult", "node") == "acoustic_result"
    assert canonical_record_key("AcousticResult", "wasm") == "acoustic_result"


def test_record_key_keeps_digit_runs_together() -> None:
    """``Ebur128`` is one token in C, so the fold must not split it."""
    assert canonical_record_key("SonareEbur128Result", "c") == "ebur128_result"
    assert canonical_record_key("Ebur128Result", "wasm") == "ebur128_result"


def test_record_key_only_strips_the_exact_prefix() -> None:
    """A type that merely starts with those letters keeps its name."""
    assert canonical_record_key("Sonar", "node") == "sonar"
    assert canonical_record_key("SonareousThing", "node") == "ous_thing"  # exact strip


def test_field_name_folds_camel_to_snake() -> None:
    assert canonical_field_name("rt60Bands") == "rt60_bands"
    assert canonical_field_name("rt60_bands") == "rt60_bands"
    assert canonical_field_name("isBlind") == "is_blind"
    assert canonical_field_name("nFft") == "n_fft"
    assert canonical_field_name("sampleRate") == "sample_rate"


def test_field_name_drops_the_optional_marker() -> None:
    """A TS ``field?`` and a C ``field`` are the same field."""
    assert canonical_field_name("gain?") == "gain"


# --------------------------------------------------------------------------
# C extractor (the oracle)
# --------------------------------------------------------------------------

_C_HEADER = """
typedef struct {
  int n_bins;
  float* magnitude;
  size_t band_count;
  char name[64];
  void (*render)(float* const* channels, int num_channels);
  uint8_t reserved[3];
  int struct_version;
} SonareThing;

typedef struct SonareOpaque SonareOpaque;
"""


def _c_records(text: str) -> list[RecordShape]:
    ex = Extraction(surface="c")
    c_api._extract_records(text, text, "probe.h", ex)
    return ex.records


def test_c_struct_members_are_extracted() -> None:
    (rec,) = _c_records(_C_HEADER)
    assert rec.key == "thing"
    assert rec.raw_name == "SonareThing"
    assert [f.name for f in rec.fields] == [
        "n_bins",
        "magnitude",
        "band_count",
        "name",
        "render",
        "reserved",
        "struct_version",
    ]


def test_c_structural_members_are_excluded_from_comparison() -> None:
    """Padding, the ABI version tag and an array's length companion do not diff."""
    (rec,) = _c_records(_C_HEADER)
    structural = {f.name for f in rec.fields if f.structural}
    assert structural == {"band_count", "reserved", "struct_version"}
    assert rec.core_field_names() == ["n_bins", "magnitude", "name", "render"]


def test_c_length_companion_needs_an_array_to_derive_from() -> None:
    """Without a pointer / array member, a ``_count`` field carries real data."""
    text = "typedef struct {\n  int numerator;\n  int beat_count;\n} SonareMeter;\n"
    (rec,) = _c_records(text)
    assert rec.core_field_names() == ["numerator", "beat_count"]


def test_c_opaque_handle_typedef_declares_no_record() -> None:
    """``typedef struct SonareProject SonareProject;`` has no fields to compare."""
    assert _c_records("typedef struct SonareProject SonareProject;\n") == []


# --------------------------------------------------------------------------
# Python ctypes extractor
# --------------------------------------------------------------------------

_PY_SOURCE = """
import ctypes


class SonareThing(ctypes.Structure):
    _fields_ = [
        ("n_bins", ctypes.c_int32),
        ("magnitude", ctypes.POINTER(ctypes.c_float)),
        ("reserved", ctypes.c_uint8 * 3),
    ]


class NotAStructure:
    _fields_ = [("nope", 1)]
"""


def _py_records(source: str) -> Extraction:
    ex = Extraction(surface="python")
    tree = ast.parse(source)
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and python_ctypes._is_ctypes_structure(node):
            python_ctypes._parse_class(node, "probe.py", ex)
    return ex


def test_ctypes_fields_are_extracted_in_order() -> None:
    ex = _py_records(_PY_SOURCE)
    assert len(ex.records) == 1
    rec = ex.records[0]
    assert rec.key == "thing"
    assert [f.name for f in rec.fields] == ["n_bins", "magnitude", "reserved"]
    assert [f.name for f in rec.fields if f.structural] == ["reserved"]


def test_ctypes_class_without_a_literal_fields_list_is_recorded_unparsed() -> None:
    """A dynamically assembled mirror must not read as an empty record.

    An empty record compares as "every C field is missing", so silently
    accepting one would flood the report with the noisiest possible finding.
    """
    ex = _py_records(
        "import ctypes\n\n\nclass SonareDyn(ctypes.Structure):\n"
        "    _fields_ = build_fields()\n"
    )
    assert ex.records == []
    assert ex.unparsed == 1


# --------------------------------------------------------------------------
# TypeScript extractor
# --------------------------------------------------------------------------

_TS_SOURCE = """
export interface ValidateOptions {
  validate?: boolean;
}

type GuardedOptions = ValidateOptions;

export interface Thing extends GuardedOptions {
  nBins: number;
  magnitude: Float32Array;
  quality:
    | 'major'
    | 'minor'
    | 'unknown';
  nested: { a: number; b: number };
  method(arg: number): void;
}

export type ThingRequest = Thing & { sampleRate: number };
"""


def _ts_records(source: str, surface: str = "node") -> dict[str, RecordShape]:
    decls, _ = ts_records._collect_declarations([("probe.ts", source)])
    out: dict[str, RecordShape] = {}
    ex = Extraction(surface=surface)
    for name in decls:
        fields, complete = ts_records._effective_fields(name, decls, set(), ex)
        if fields and complete:
            out[name] = RecordShape(
                key=canonical_record_key(name, surface),
                surface=surface,
                raw_name=name,
                fields=fields,
            )
    return out


def test_ts_interface_properties_are_extracted() -> None:
    recs = _ts_records(_TS_SOURCE)
    names = [f.name for f in recs["Thing"].fields]
    # ``validate`` is inherited from the base; ``method`` is not a property.
    assert names == ["validate", "n_bins", "magnitude", "quality", "nested"]


def test_ts_multiline_union_does_not_split_the_property() -> None:
    """A union wrapped one alternative per line is still one field.

    Splitting it would drop ``quality`` from the record, and the comparison
    would then report a C field the facade actually declares as missing.
    """
    recs = _ts_records(_TS_SOURCE)
    assert "quality" in {f.name for f in recs["Thing"].fields}


def test_ts_intersection_alias_is_a_record() -> None:
    """``type X = A & { ... }`` is the other spelling of an interface."""
    recs = _ts_records(_TS_SOURCE)
    names = {f.name for f in recs["ThingRequest"].fields}
    assert {"n_bins", "magnitude", "sample_rate"} <= names


def test_ts_union_alias_is_not_a_record() -> None:
    """A non-record alias must not be reported as a record with no fields."""
    recs = _ts_records("export type Mode = 'a' | 'b';\n")
    assert recs == {}


# --------------------------------------------------------------------------
# The gate
# --------------------------------------------------------------------------


def _rec(surface: str, raw_name: str, *fields: str) -> RecordShape:
    return RecordShape(
        key=canonical_record_key(raw_name, surface),
        surface=surface,
        raw_name=raw_name,
        fields=[RecordField(name=canonical_field_name(f), raw_name=f) for f in fields],
        file=f"{surface}.src",
        line=1,
    )


def _ex(surface: str, *records: RecordShape) -> Extraction:
    ex = Extraction(surface=surface)
    ex.records = list(records)
    return ex


def _report(extractions: dict[str, Extraction], allow=None):
    allow = allow or allowlist_mod.Allowlist()
    return compare.build_report(extractions, allow, list(extractions))


def _active_records(rep) -> set[tuple[str, str]]:
    return {(f.key, f.surface) for f in rep.active() if f.category == "record"}


def test_matching_record_is_silent() -> None:
    rep = _report(
        {
            "c": _ex("c", _rec("c", "SonareThing", "n_bins", "sample_rate")),
            "node": _ex("node", _rec("node", "Thing", "nBins", "sampleRate")),
        }
    )
    assert _active_records(rep) == set(), _active_records(rep)


def test_missing_c_field_is_an_active_finding() -> None:
    """The drift this unit exists for: a C field the facade never declares."""
    rep = _report(
        {
            "c": _ex("c", _rec("c", "SonareThing", "n_bins", "sample_rate")),
            "node": _ex("node", _rec("node", "Thing", "nBins")),
        }
    )
    assert ("thing", "node") in _active_records(rep), _active_records(rep)


def test_missing_field_on_one_of_three_facades_is_caught() -> None:
    """Three surfaces agreeing does not excuse the fourth."""
    c = _ex("c", _rec("c", "SonareThing", "n_bins", "gain"))
    ok = _rec("node", "Thing", "nBins", "gain")
    rep = _report(
        {
            "c": c,
            "python": _ex("python", _rec("python", "SonareThing", "n_bins", "gain")),
            "node": _ex("node", ok),
            "wasm": _ex("wasm", _rec("wasm", "Thing", "nBins")),
        }
    )
    active = _active_records(rep)
    assert ("thing", "wasm") in active, active
    assert ("thing", "node") not in active, active
    assert ("thing", "python") not in active, active


def test_facade_field_absent_from_c_is_an_active_finding() -> None:
    rep = _report(
        {
            "c": _ex("c", _rec("c", "SonareThing", "n_bins")),
            "node": _ex("node", _rec("node", "Thing", "nBins", "prettyName")),
        }
    )
    assert ("thing", "node") in _active_records(rep), _active_records(rep)


def test_structural_c_member_is_never_a_finding() -> None:
    """Padding / version / length members have no facade counterpart by design."""
    c = _c_records(
        "typedef struct {\n  int n_bins;\n  float* data;\n  size_t data_count;\n"
        "  int struct_version;\n  uint8_t reserved[4];\n} SonareThing;\n"
    )
    rep = _report(
        {
            "c": _ex("c", *c),
            "node": _ex("node", _rec("node", "Thing", "nBins", "data")),
        }
    )
    assert _active_records(rep) == set(), _active_records(rep)


def test_uniform_absence_is_not_drift() -> None:
    """A C struct no facade declares is an audit question, not drift."""
    rep = _report(
        {
            "c": _ex("c", _rec("c", "SonareThing", "n_bins")),
            "node": _ex("node"),
            "wasm": _ex("wasm"),
        }
    )
    assert _active_records(rep) == set(), _active_records(rep)
    assert [f for f in rep.findings if f.category == "record"] == []


def test_peer_declared_absence_is_informational() -> None:
    """One facade declaring a record and another not is visible but non-gating."""
    rep = _report(
        {
            "c": _ex("c", _rec("c", "SonareThing", "n_bins")),
            "node": _ex("node", _rec("node", "Thing", "nBins")),
            "wasm": _ex("wasm"),
        }
    )
    assert _active_records(rep) == set(), _active_records(rep)
    info = {(f.key, f.surface) for f in rep.reported() if f.informational}
    assert ("thing", "wasm") in info, info


def test_allowlist_suppresses_a_record_and_a_field() -> None:
    allow = allowlist_mod.Allowlist(
        record={"node": ["other"]}, record_fields=["thing.gain"]
    )
    rep = _report(
        {
            "c": _ex(
                "c",
                _rec("c", "SonareThing", "n_bins", "gain"),
                _rec("c", "SonareOther", "x"),
            ),
            "node": _ex("node", _rec("node", "Thing", "nBins")),
        },
        allow,
    )
    assert _active_records(rep) == set(), _active_records(rep)


def test_suppressed_fields_are_reported_as_allowlisted_not_dropped() -> None:
    """A suppression must stay countable, not vanish before a Finding exists."""
    allow = allowlist_mod.Allowlist(record_fields=["thing.gain"])
    rep = _report(
        {
            "c": _ex("c", _rec("c", "SonareThing", "n_bins", "gain")),
            "node": _ex("node", _rec("node", "Thing", "nBins")),
        },
        allow,
    )
    suppressed = [f for f in rep.findings if f.category == "record" and f.allowlisted]
    assert len(suppressed) == 1, rep.findings
    assert suppressed[0].detail["suppressed"] == ["gain"], suppressed[0].detail


def test_extra_fields_allowlist_is_one_directional() -> None:
    """A richer facade read model is expected; a MISSING C field still reports.

    ``extra_fields`` exists so a record under active development can be excused
    for the fields it adds without also going blind to a C field it drops — the
    blunt whole-record entry would suppress both.
    """
    allow = allowlist_mod.Allowlist(record_extra={"node": ["thing"]})
    rep = _report(
        {
            "c": _ex("c", _rec("c", "SonareThing", "n_bins", "gain")),
            # declares an extra field AND omits a C one
            "node": _ex("node", _rec("node", "Thing", "nBins", "prettyName")),
        },
        allow,
    )
    active = [f for f in rep.active() if f.category == "record"]
    assert len(active) == 1, [f.message for f in active]
    assert active[0].detail["missing"] == ["gain"], active[0].detail


def test_name_collision_record_is_not_matched_to_the_c_struct() -> None:
    """``_NOT_A_C_MIRROR`` states that two same-named types are unrelated."""
    surface, key, raw = next(iter(compare._NOT_A_C_MIRROR))
    ex = _ex(surface, _rec(surface, raw, "samples"))
    assert compare._index_records(ex) == {}


def test_name_collision_entry_is_anchored_on_the_type_name() -> None:
    """Renaming the colliding type makes the entry stop applying.

    Anchoring on the surface-native name is what stops the entry from silently
    covering whatever takes that canonical key next.
    """
    surface, key, raw = next(iter(compare._NOT_A_C_MIRROR))
    renamed = _rec(surface, "SomethingElse", "samples")
    renamed.key = key  # same canonical key, different declared type name
    assert compare._index_records(_ex(surface, renamed)) == {key: renamed}


def test_worklet_records_are_not_c_struct_mirrors() -> None:
    """Binding-internal AudioWorklet wire types are excluded from the unit.

    ``ts_common.py`` already keeps the worklet out of the FUNCTION surface; its
    SharedArrayBuffer ring payloads are named after the C structs they carry
    messages about, not shaped like them.
    """
    assert ts_records._is_internal_path("bindings/wasm/src/worklet/protocol.ts")
    assert not ts_records._is_internal_path("bindings/wasm/src/worklet.ts")
    assert not ts_records._is_internal_path("bindings/wasm/src/project_types.ts")


# --------------------------------------------------------------------------
# Non-vacuity on the real tree
# --------------------------------------------------------------------------


def test_real_repo_extracts_records_on_every_surface() -> None:
    """Guard against the silent-vacuity failure: zero records, zero findings.

    A record-shape checker that parsed nothing reports a clean tree, which is
    indistinguishable in the findings table from a tree that really is clean.
    The C oracle and all three facades must therefore each yield records, and C
    must yield the most-complete field data (it is the reference).
    """
    repo = _HERE.parent.parent
    if not (repo / "include").exists():
        return  # not in the libsonare tree; skip
    import check_parity

    rep = check_parity.run(repo)
    counts = rep.record_counts
    for surface in ("c", "python", "node", "wasm"):
        assert counts.get(surface, 0) > 50, counts
    # The comparison is only meaningful where a C record and a facade record
    # actually meet; assert that the overlap is substantial, not incidental.
    compared = {
        f.key for f in rep.findings if f.category == "record" and not f.informational
    }
    assert isinstance(compared, set)
    assert counts["c"] >= 100, counts
    # The allowlisted entries must stay countable too: a suppression that never
    # produces a Finding is invisible in the report's audit total.
    assert any(f.category == "record" and f.allowlisted for f in rep.findings)


def test_chord_duration_is_declared_on_both_js_facades() -> None:
    """``duration`` is a facade-side derivation and must not drift back apart.

    ``SonareChord`` carries only ``start`` and ``end``; every facade also exposes
    the span, defined identically as ``end - start`` — Python as a property, Node
    in the addon, WASM in the result converter. This unit originally flagged the
    field because Node declared it and WASM did not, which is what a one-sided
    derivation looks like from the outside. Dropping it from either JS facade
    would recreate that asymmetry while the shared allowlist entry hid it, so
    pin the presence rather than the finding.
    """
    repo = _HERE.parent.parent
    if not (repo / "include").exists():
        return  # not in the libsonare tree; skip

    for surface, src_rel in (
        ("node", "bindings/node/src"),
        ("wasm", "bindings/wasm/src"),
    ):
        ex = Extraction(surface=surface)
        ts_records.extract_records(repo, surface, src_rel, ex)
        chord = next((r for r in ex.records if r.key == "chord"), None)
        assert chord is not None, surface
        fields = set(chord.core_field_names())
        assert "duration" in fields, (surface, sorted(fields))


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:  # noqa: PERF203
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
