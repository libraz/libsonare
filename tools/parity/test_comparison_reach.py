"""Regression tests for the reach behind each category's finding count.

A parity check reports a divergence it found. It has no way to report a
divergence it never looked for, so an extractor that stops feeding a category
produces the same output a clean tree does: zero findings, a `_none_` line, and
a summary row of zeroes. The category with the least reach in this repository is
``default`` — a facade that spells its defaults inside a request-object
normalizer puts them out of the signature the extractor reads, and roughly
ninety-six percent of the candidate pairs carry nothing to compare because of
it. That is a real limit, and it has to be legible as one rather than as a pass.

So the count of verdicts reached is an output, and a category that reached none
at all fails. Both halves are pinned here, each against a control that can go
the other way.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_comparison_reach.py
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod
import check_parity
import compare
import report as report_mod
from core_defaults import CoreConfig
from extractors import c_api
from model import SURFACES, Extraction, FunctionSig, Param


def _facade(surface: str, key: str, param: str, default: str | None) -> Extraction:
    ex = Extraction(surface=surface)
    ex.functions = [
        FunctionSig(
            key=key,
            surface=surface,
            raw_name=key,
            params=[Param(name=param, raw_name=param, default=default)],
            file=f"{surface}.src",
            line=1,
        )
    ]
    return ex


def _c(key: str, param: str) -> Extraction:
    ex = Extraction(surface="c")
    ex.functions = [
        FunctionSig(
            key=key,
            surface="c",
            raw_name=key,
            params=[Param(name=param, raw_name=param)],
            file="c.h",
            line=1,
        )
    ]
    return ex


def _report(allow, *facades: Extraction):
    extractions = {"c": _c("normalize", "target_db")}
    for ex in facades:
        extractions[ex.surface] = ex
    return compare.build_report(extractions, allow, ["c", *[ex.surface for ex in facades]])


# ---------------------------------------------------------------------------
# The count itself.


def test_two_facades_that_each_spell_a_default_reach_a_verdict() -> None:
    """The positive control for everything below: this is what reach looks like."""
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", "0.0"),
    )
    assert rep.comparison_counts()["default"] == 1


def test_a_default_only_one_facade_spells_reaches_no_verdict() -> None:
    """The shape the report used to render as a clean row.

    Same two facades, same parameter, one default removed — the comparison is
    declined, nothing is found, and the finding count is identical to the case
    above. Only the reach separates them.
    """
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", None),
    )
    assert rep.comparison_counts().get("default", 0) == 0
    assert [f for f in rep.active() if f.category == "default"] == []
    assert [n["category"] for n in rep.not_compared if n["category"] == "default"]


def test_a_divergence_is_still_found_when_the_comparison_runs() -> None:
    """Non-vacuity for the counter: counting a verdict has not replaced reaching one."""
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "-20.0"),
        _facade("node", "normalize", "target_db", "0.0"),
    )
    assert rep.comparison_counts()["default"] == 1
    assert [f.key for f in rep.active() if f.category == "default"] == ["normalize"]


# ---------------------------------------------------------------------------
# A default spelled as absence.


def _core(default: str) -> dict[str, CoreConfig]:
    return {
        "normalize": CoreConfig(
            key="normalize",
            header="core.h",
            name="NormalizeConfig",
            fields={"target_db": default},
        )
    }


def _core_default_findings(facade_default: str) -> tuple[list, int]:
    allow = allowlist_mod.Allowlist()
    extractions = {
        "c": _c("normalize", "target_db"),
        "python": _facade("python", "normalize", "target_db", facade_default),
    }
    rep = compare.build_report(extractions, allow, ["c", "python"], core_configs=_core("-1.0f"))
    findings = [f for f in rep.active() if f.category == "core_default"]
    declined = [n for n in rep.not_compared if n["category"] == "core_default"]
    return findings, len(declined)


def test_a_default_spelled_as_absence_is_declined_not_compared() -> None:
    findings, declined = _core_default_findings("None")
    assert findings == []
    assert declined == 1


def test_a_concrete_default_that_diverges_from_the_core_is_still_found() -> None:
    """Non-vacuity for the decline above: only absence is exempt."""
    findings, declined = _core_default_findings("0.0")
    assert [f.key for f in findings] == ["normalize"]
    assert declined == 0


# ---------------------------------------------------------------------------
# The gate.
#
# ``_blind_categories`` judges the run the caller asked for, so these pass the
# full surface list alongside a two-facade report: the selection is the
# function's input, not something it rereads off the report.


def test_a_category_that_reached_no_verdict_is_named() -> None:
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", None),
    )
    assert "default" in check_parity._blind_categories(rep, list(SURFACES))


def test_a_category_that_reached_one_verdict_is_not_named() -> None:
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", "0.0"),
    )
    assert "default" not in check_parity._blind_categories(rep, list(SURFACES))


def test_a_partial_run_is_not_judged() -> None:
    """``--surface c,python`` deliberately does not build the WASM wiring check."""
    allow = allowlist_mod.Allowlist()
    rep = _report(allow, _facade("python", "normalize", "target_db", None))
    assert check_parity._blind_categories(rep, ["c", "python"]) == []


# ---------------------------------------------------------------------------
# What a reader sees.


def test_the_none_line_carries_the_reach_behind_it() -> None:
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "0.0"),
        _facade("node", "normalize", "target_db", None),
    )
    section = report_mod.to_markdown(rep).split("## Default drift (cross-facade) —")[1]
    line = section.split("\n")[2]
    assert "_none_" in line and "**0** comparison" in line, line


def test_the_real_tree_has_no_blind_category() -> None:
    """The case the gate exists for: an extractor that stops feeding a check.

    Every category has candidates in this repository, so a zero here is a tool
    that broke rather than a tree that is clean.
    """
    rep = check_parity.run()
    assert check_parity._blind_categories(rep, list(SURFACES)) == []


def _main() -> int:
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except AssertionError as exc:
                failures += 1
                print(f"FAIL {name}: {exc}")
    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


def test_a_config_struct_before_the_itemized_params_is_the_options_bag() -> None:
    assert compare._order_verdict("k", ["config", "ops"], ["ops"]) == compare.AGREED
    assert compare._order_verdict("k", ["config", "ops", "gain"], ["gain"]) == compare.DIVERGED


def _structural(decl: str) -> list[str]:
    params = c_api._mark_derived_counts([c_api._parse_param(p) for p in decl.split(",")])
    return [p.name for p in params if p.structural]


def test_a_count_after_its_array_is_derived() -> None:
    assert "n_ops" in _structural("const SonareSpectralEditOp* ops, size_t n_ops, float** out")
    assert "frames" in _structural("const float* samples, size_t frames, int channels")
    assert "frames" in _structural("const float* samples, size_t frames, int num_channels")
    linked = _structural(
        "const float* const* channels, size_t num_channels, size_t length, int sample_rate"
    )
    assert "num_channels" in linked


def test_the_dimensions_after_a_matrix_are_derived_and_the_config_after_them_is_not() -> None:
    matrix = _structural("const float* features, int n_features, int n_frames, int width")
    assert {"n_features", "n_frames"} <= set(matrix) and "width" not in matrix
    cqt = _structural(
        "const float* magnitude, int n_bins, int n_frames, int sample_rate, int hop_length"
    )
    assert {"n_bins", "n_frames"} <= set(cqt) and "hop_length" not in cqt
    gl = _structural(
        "const float* magnitude, size_t input_length, int n_bins, int n_frames, int n_fft"
    )
    assert {"n_bins", "n_frames"} <= set(gl) and "n_fft" not in gl
    pair = _structural(
        "const float* x, int x_rows, int x_cols, const float* y, int y_rows, int y_cols, int k"
    )
    assert {"x_rows", "x_cols", "y_rows", "y_cols"} <= set(pair) and "k" not in pair
    assert "n" in _structural("const float* recurrence, int n, int pad")
    mel = _structural("const float* coefficients, int n_mfcc, int n_frames, int n_mels")
    assert {"n_mfcc", "n_frames"} <= set(mel) and "n_mels" not in mel


def test_a_dimension_name_away_from_its_matrix_stays_config() -> None:
    cqt = _structural(
        "const float* samples, size_t length, int sample_rate, int hop_length, int n_bins"
    )
    assert "n_bins" not in cqt


def _order_findings(facade_params: list[str]) -> list:
    decl = "const float* s, int n_features, int n_frames, int n_components, int n_iter"
    c = Extraction(surface="c")
    c.functions = [
        FunctionSig(
            key="decompose",
            surface="c",
            raw_name="decompose",
            params=c_api._mark_derived_counts([c_api._parse_param(p) for p in decl.split(",")]),
            file="c.h",
            line=1,
        )
    ]
    py = Extraction(surface="python")
    py.functions = [
        FunctionSig(
            key="decompose",
            surface="python",
            raw_name="decompose",
            params=[Param(name=n, raw_name=n) for n in facade_params],
            file="py.pyi",
            line=1,
        )
    ]
    rep = compare.build_report({"c": c, "python": py}, allowlist_mod.Allowlist(), ["c", "python"])
    return [f for f in rep.active() if f.category == "order"]


def test_matrix_dimensions_agree_whether_the_facade_derives_or_passes_them() -> None:
    assert _order_findings(["s", "n_components", "n_iter"]) == []
    assert _order_findings(["s", "n_features", "n_frames", "n_components", "n_iter"]) == []
    assert [f.key for f in _order_findings(["s", "n_iter", "n_components"])] == ["decompose"]


def _segments_findings(positional: list[str], keyword_only: list[str]) -> list:
    c = _c("note_segments", "f0_hz")
    c.functions[0].params = [
        Param(name=n, raw_name=n) for n in ("f0_hz", "voiced_prob", "voiced", "frame_rate")
    ]
    py = Extraction(surface="python")
    py.functions = [
        FunctionSig(
            key="note_segments",
            surface="python",
            raw_name="note_segments",
            params=[Param(name=n, raw_name=n) for n in positional]
            + [Param(name=n, raw_name=n, keyword_only=True) for n in keyword_only],
            file="py.pyi",
            line=1,
        )
    ]
    rep = compare.build_report({"c": c, "python": py}, allowlist_mod.Allowlist(), ["c", "python"])
    return [f for f in rep.active() if f.category == "order"]


def test_a_keyword_only_parameter_has_no_position() -> None:
    assert _segments_findings(["f0_hz", "frame_rate"], ["voiced", "voiced_prob"]) == []
    assert [f.key for f in _segments_findings(["frame_rate", "f0_hz"], ["voiced"])] == [
        "note_segments"
    ]
    assert [f.key for f in _segments_findings(["f0_hz", "frame_rate", "voiced"], [])] == [
        "note_segments"
    ]


def _norm_findings(facade_default: str) -> list:
    core = {
        "vector_normalize": CoreConfig(
            key="vector_normalize",
            header="core.h",
            name="normalize",
            fields={"norm": "NormType::Inf"},
        )
    }
    extractions = {
        "c": _c("vector_normalize", "norm"),
        "python": _facade("python", "vector_normalize", "norm", facade_default),
    }
    rep = compare.build_report(
        extractions, allowlist_mod.Allowlist(), ["c", "python"], core_configs=core
    )
    return [f for f in rep.active() if f.category == "core_default"]


def test_a_norm_token_reads_as_the_core_member_it_selects() -> None:
    assert _norm_findings("'max'") == []
    assert [f.key for f in _norm_findings("'l2'")] == ["vector_normalize"]


def test_a_none_default_beside_a_value_is_absence_and_not_a_second_value() -> None:
    allow = allowlist_mod.Allowlist()
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "None"),
        _facade("node", "normalize", "target_db", "4"),
        _facade("wasm", "normalize", "target_db", "4"),
    )
    assert [f for f in rep.active() if f.category == "default"] == []
    declined = [n for n in rep.not_compared if n["category"] == "default"]
    assert [n["surface"] for n in declined] == ["python"]
    rep = _report(
        allow,
        _facade("python", "normalize", "target_db", "None"),
        _facade("node", "normalize", "target_db", "4"),
        _facade("wasm", "normalize", "target_db", "8"),
    )
    assert [f.key for f in rep.active() if f.category == "default"] == ["normalize"]


if __name__ == "__main__":
    raise SystemExit(_main())
