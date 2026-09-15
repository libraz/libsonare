#!/usr/bin/env python3
"""Regression tests for the allowlist audit: its three states, and held-empty sections.

An allowlist entry is a recorded decision about one divergence. Once that
divergence is fixed the entry stops describing anything, but it does not stop
asserting: the next symbol to take the name inherits a blessing nobody granted
it. The audit exists to make that moment visible, so what these tests pin is
that it neither misses a dead entry nor accuses a live one. Each state gets a
case: ``suppressed``, ``stale`` (compared, and the surfaces agreed) and
``unconsulted`` (no comparison looked the name up, because a fold on one side
left nothing to compare — which the run derives and reports on its own).

The rest is the ratchet. ``[core_default]`` and ``[enum]`` are empty, and
an entry in either is far more often a surface that computes or accepts
something else than the reviewed alias the section was written for -- a
distinction no checker can draw. So they are held at zero and widening one means
editing ``RATCHETED_SECTIONS``, which puts the decision in the diff.

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_allowlist_audit.py
"""

from __future__ import annotations

from pathlib import Path
import sys

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod  # noqa: E402
import check_parity  # noqa: E402
import compare  # noqa: E402
from model import (  # noqa: E402
    Extraction,
    FunctionSig,
    Param,
    RecordField,
    RecordShape,
    SURFACES,
)


def _c(*keys: str) -> Extraction:
    ex = Extraction(surface="c")
    ex.functions = [
        FunctionSig(key=k, surface="c", raw_name=k, file="c.h", line=1) for k in keys
    ]
    return ex


def _facade(surface: str, *keys: str) -> Extraction:
    ex = Extraction(surface=surface)
    ex.functions = [
        FunctionSig(key=k, surface=surface, raw_name=k, file=f"{surface}.ts", line=1)
        for k in keys
    ]
    return ex


def _report(c: Extraction, facade: Extraction, allow):
    return compare.build_report(
        {"c": c, facade.surface: facade}, allow, ["c", facade.surface]
    )


def _sig(surface: str, key: str, *param_names: str) -> Extraction:
    """One-function extraction for ``surface``, with a positional parameter list."""
    ex = Extraction(surface=surface)
    ex.functions = [
        FunctionSig(
            key=key,
            surface=surface,
            raw_name=key,
            params=[Param(name=p, raw_name=p) for p in param_names],
            file=f"{surface}.src",
            line=1,
        )
    ]
    return ex


def _state(allow, scope: str, pattern: str) -> str:
    states = {(s, p): state for s, p, state in allow.classify()}
    return states[(scope, pattern)]


def test_an_entry_that_suppressed_a_gap_is_not_reported() -> None:
    allow = allowlist_mod.Allowlist()
    allow.coverage = {"python": ["mastering_apply"]}
    _report(_c("mastering_apply"), _facade("python"), allow)
    assert allow.expired_entries() == [], allow.expired_entries()


def test_an_entry_whose_gap_was_fixed_is_reported_with_its_section() -> None:
    """The facade now exposes the key, so the coverage check found no gap."""
    allow = allowlist_mod.Allowlist()
    allow.coverage = {"python": ["mastering_apply"]}
    _report(_c("mastering_apply"), _facade("python", "mastering_apply"), allow)
    assert allow.expired_entries() == [
        ("coverage.python", "mastering_apply", allowlist_mod.STALE)
    ]


def test_a_wildcard_is_used_when_any_name_matches_it() -> None:
    allow = allowlist_mod.Allowlist()
    allow.coverage = {"python": ["mastering_*"]}
    _report(_c("mastering_apply", "mastering_analyze"), _facade("python"), allow)
    assert allow.expired_entries() == [], allow.expired_entries()


def test_a_shared_surface_only_entry_counts_as_used_from_any_surface() -> None:
    """``[surface_only] any`` is consulted per surface; one hit is enough."""
    allow = allowlist_mod.Allowlist()
    allow.surface_only = {"any": ["require_module"]}
    assert allow.surface_only_ok("require_module", "wasm", allowlist_mod.DIVERGED)
    assert allow.expired_entries() == [], allow.expired_entries()


def test_a_core_default_entry_is_reported_while_the_section_is_held_empty() -> None:
    """The ratchet: an entry fails until its section is taken off the tuple."""
    allow = allowlist_mod.Allowlist()
    allow.core_default = ["analyze.sample_rate"]
    assert allow.ratcheted_entries() == [("core_default.params", "analyze.sample_rate")]


def test_an_enum_entry_is_reported_too() -> None:
    allow = allowlist_mod.Allowlist()
    allow.enum = ["master_audio.preset"]
    assert allow.ratcheted_entries() == [("enum.params", "master_audio.preset")]


def test_a_representation_section_is_left_alone() -> None:
    """`[default]` takes same-value representation differences and keeps three."""
    allow = allowlist_mod.Allowlist()
    allow.default = ["analyze.hop_length"]
    allow.input_naming = ["analyze.samples"]
    assert allow.ratcheted_entries() == []


def test_an_order_entry_that_excuses_a_live_divergence_is_suppressed() -> None:
    """The comparison ran, the surfaces disagreed, and the entry excused it."""
    allow = allowlist_mod.Allowlist()
    allow.order = {"node": ["resample"]}
    rep = _report(
        _sig("c", "resample", "target_rate", "quality"),
        _sig("node", "resample", "quality", "target_rate"),
        allow,
    )
    assert _state(allow, "order.node", "resample") == allowlist_mod.SUPPRESSED
    assert allow.expired_entries() == []
    assert [f for f in rep.active() if f.category == "order"] == []


def test_an_order_entry_whose_surfaces_agree_is_reported_as_stale() -> None:
    """The comparison ran and matched, so there was nothing left to excuse."""
    allow = allowlist_mod.Allowlist()
    allow.order = {"node": ["resample"]}
    _report(
        _sig("c", "resample", "target_rate", "quality"),
        _sig("node", "resample", "target_rate", "quality"),
        allow,
    )
    assert _state(allow, "order.node", "resample") == allowlist_mod.STALE
    assert allow.expired_entries() == [
        ("order.node", "resample", allowlist_mod.STALE)
    ]


def test_an_order_entry_the_facade_folded_out_of_reach_is_unconsulted() -> None:
    """A request-object facade carries no param order to compare.

    The structural rule already decides that, ahead of the allowlist, so the
    entry duplicates a fact the tool derives and is reported for removal.
    """
    allow = allowlist_mod.Allowlist()
    allow.order = {"node": ["resample"]}
    _report(
        _sig("c", "resample", "target_rate", "quality"),
        _sig("node", "resample", "request"),
        allow,
    )
    assert _state(allow, "order.node", "resample") == allowlist_mod.UNCONSULTED
    assert allow.expired_entries() == [
        ("order.node", "resample", allowlist_mod.UNCONSULTED)
    ]


def test_a_comparison_the_facade_folded_away_is_reported_as_not_compared() -> None:
    """The declined comparison is derived and reported where it would have run.

    Deriving it is what makes the exclusion expire on its own: a facade that
    goes back to positional parameters is compared again with no edit anywhere,
    which a hand-written entry could not do.
    """
    allow = allowlist_mod.Allowlist()
    folded = _report(
        _sig("c", "resample", "target_rate", "quality"),
        _sig("node", "resample", "request"),
        allow,
    )
    order = [n for n in folded.not_compared if n["category"] == "order"]
    assert [(n["key"], n["surface"]) for n in order] == [("resample", "node")], order
    assert "request object" in order[0]["reason"]

    positional = _report(
        _sig("c", "resample", "target_rate", "quality"),
        _sig("node", "resample", "target_rate", "quality"),
        allowlist_mod.Allowlist(),
    )
    assert [n for n in positional.not_compared if n["category"] == "order"] == []


def test_an_entry_no_comparison_looked_up_is_reported_as_unconsulted() -> None:
    """No symbol carries the name at all, on any surface."""
    allow = allowlist_mod.Allowlist()
    allow.order = {"node": ["resample"]}
    _report(_sig("c", "decimate", "target_rate"), _sig("node", "decimate", "target_rate"), allow)
    assert _state(allow, "order.node", "resample") == allowlist_mod.UNCONSULTED
    assert allow.expired_entries() == [
        ("order.node", "resample", allowlist_mod.UNCONSULTED)
    ]


def test_an_input_naming_entry_whose_surfaces_agree_is_reported_as_stale() -> None:
    """Both remaining surfaces spell the buffer the same way."""
    allow = allowlist_mod.Allowlist()
    allow.input_naming = ["spectral_flux"]
    _report(
        _sig("c", "spectral_flux", "samples"),
        _sig("node", "spectral_flux", "samples"),
        allow,
    )
    assert _state(allow, "input_naming.keys", "spectral_flux") == allowlist_mod.STALE


def test_an_input_naming_entry_with_one_group_left_is_unconsulted() -> None:
    """One surface names no input buffer, leaving nothing to compare against."""
    allow = allowlist_mod.Allowlist()
    allow.input_naming = ["spectral_flux"]
    rep = _report(
        _sig("c", "spectral_flux", "n_fft"),
        _sig("node", "spectral_flux", "samples"),
        allow,
    )
    assert (
        _state(allow, "input_naming.keys", "spectral_flux")
        == allowlist_mod.UNCONSULTED
    )
    assert [n["key"] for n in rep.not_compared if n["category"] == "input"] == [
        "spectral_flux"
    ]


def test_the_audit_records_which_divergence_each_entry_suppressed() -> None:
    """An entry can be live and still not excuse what its reason claims.

    Recording the divergence beside the entry is what lets that be read off the
    report; the tool matches nothing against the prose reason itself.
    """
    allow = allowlist_mod.Allowlist()
    allow.input_naming = ["mfcc_to_mel"]
    _report(
        _sig("c", "mfcc_to_mel", "mfcc"),
        _sig("node", "mfcc_to_mel", "mfcc_coeffs"),
        allow,
    )
    suppressed = allow.suppressed_divergences()[("input_naming.keys", "mfcc_to_mel")]
    assert len(suppressed) == 1, suppressed
    assert "mfcc_coeffs" in suppressed[0] and "mfcc" in suppressed[0]


def test_the_suppression_record_does_not_depend_on_declaration_order() -> None:
    """Reordering declarations produces the same record, not a second one.

    The record is built to be held against a later run's, so anything that moves
    while the surfaces stand still would read as drift that is not there. Two
    orders can vary: the arrival order of the divergences one pattern collects,
    and the field order inside one of them.
    """

    def by_symbol_order(*keys: str):
        allow = allowlist_mod.Allowlist()
        allow.surface_only = {"node": ["helper_*"]}
        compare.build_report(
            {"c": _c("resample"), "node": _facade("node", *keys)}, allow, ["c", "node"]
        )
        return allow.suppressed_divergences()

    assert by_symbol_order("helper_a", "helper_b") == by_symbol_order(
        "helper_b", "helper_a"
    )

    def by_field_order(*field_names: str):
        allow = allowlist_mod.Allowlist()
        allow.record_extra = {"node": ["chord"]}
        c = Extraction(surface="c")
        c.records = [
            RecordShape(
                key="chord",
                surface="c",
                raw_name="SonareChord",
                fields=[RecordField(name="start", raw_name="start")],
                file="c.h",
                line=1,
            )
        ]
        node = Extraction(surface="node")
        node.records = [
            RecordShape(
                key="chord",
                surface="node",
                raw_name="Chord",
                fields=[RecordField(name=n, raw_name=n) for n in field_names],
                file="node.ts",
                line=1,
            )
        ]
        compare.build_report({"c": c, "node": node}, allow, ["c", "node"])
        return allow.suppressed_divergences()

    assert by_field_order("start", "name", "duration") == by_field_order(
        "duration", "start", "name"
    )


def test_the_repository_allowlist_holds_its_ratcheted_sections_empty() -> None:
    """Both are empty today; widening one is a deliberate edit, not a drift."""
    allow = allowlist_mod.load(_HERE / "allowlist.toml")
    assert allow.ratcheted_entries() == [], allow.ratcheted_entries()


def test_the_repository_allowlist_carries_no_expired_entry() -> None:
    """No committed entry sits in front of a comparison that now agrees."""
    root = _HERE.parent.parent
    rep = check_parity.run(root=root, selected=list(SURFACES))
    expired = rep.allowlist.expired_entries()
    assert expired == [], [f"[{s}] {p} ({state})" for s, p, state in expired]


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
