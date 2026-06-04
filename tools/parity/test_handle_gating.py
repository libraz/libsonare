#!/usr/bin/env python3
"""Regression tests for handle/class coverage GATING.

The cross-binding checker treats handle/class C ops (``engine_*``, ``audio_*``,
``project_*``, ...) the same way it treats free functions: a handle op the C ABI
exposes but a facade does NOT — and that is neither an idiomatic rename
(``_ALIAS_COVERAGE``), an object-lifecycle op (``_is_lifecycle_key``), nor an
allowlisted intentional omission — is an ACTIVE coverage gap that fails CI.

Before this, handle ops were unconditionally ``informational`` (never gated), so
a new C op wired on some facades but forgotten on another slipped through. These
tests pin the gate: covered-by-method / covered-by-alias / lifecycle stay quiet,
a genuine gap goes active, and the real repo stays green (the curated alias map +
allowlist fully account for today's handle surface).

Stdlib only; no build needed. Run directly:

    python3 tools/parity/test_handle_gating.py
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import allowlist as allowlist_mod  # noqa: E402
import compare  # noqa: E402
from model import Extraction, FunctionSig  # noqa: E402


def _c(*keys: str) -> Extraction:
    ex = Extraction(surface="c")
    ex.functions = [
        FunctionSig(key=k, surface="c", raw_name=k, file="c.h", line=1) for k in keys
    ]
    return ex


def _py(
    methods: dict[str, str] | None = None, frees: list[str] | None = None
) -> Extraction:
    """Build a Python extraction. ``methods`` maps key -> owning class name."""
    ex = Extraction(surface="python")
    for key, cls in (methods or {}).items():
        ex.functions.append(
            FunctionSig(
                key=key, surface="python", raw_name=f"{cls}.{key}", file="py.py", line=1
            )
        )
    for key in frees or []:
        ex.functions.append(
            FunctionSig(key=key, surface="python", raw_name=key, file="py.py", line=1)
        )
    return ex


def _active(rep) -> set[tuple[str, str]]:
    return {(f.key, f.surface) for f in rep.active() if f.category == "coverage"}


def _report(c: Extraction, py: Extraction, allow=None):
    allow = allow or allowlist_mod.Allowlist()
    return compare.build_report({"c": c, "python": py}, allow, ["c", "python"])


def test_genuine_handle_gap_is_active() -> None:
    """A handle op present in C but absent from the facade fails the gate."""
    rep = _report(_c("engine_set_tempo"), _py(methods={}))
    assert ("engine_set_tempo", "python") in _active(rep), _active(rep)


def test_handle_op_covered_by_method_is_silent() -> None:
    """The same op, exposed as a class method (prefix-stripped), is covered."""
    rep = _report(_c("engine_set_tempo"), _py(methods={"set_tempo": "Engine"}))
    assert ("engine_set_tempo", "python") not in _active(rep), _active(rep)


def test_handle_op_covered_by_alias_is_silent() -> None:
    """An idiomatic rename in ``_ALIAS_COVERAGE`` (serialize -> to_json) is covered."""
    rep = _report(_c("project_serialize"), _py(methods={"to_json": "Project"}))
    assert ("project_serialize", "python") not in _active(rep), _active(rep)


def test_alias_does_not_match_unrelated_member() -> None:
    """An alias only credits its declared target, not a same-prefix neighbour.

    ``eq_set_sidechain`` is credited by ``set_sidechain_mono``/``_stereo`` only;
    a facade that exposes some other ``set_*`` must still fail the gate.
    """
    rep = _report(_c("eq_set_sidechain"), _py(methods={"set_gain": "Eq"}))
    assert ("eq_set_sidechain", "python") in _active(rep), _active(rep)
    rep_ok = _report(_c("eq_set_sidechain"), _py(methods={"set_sidechain_mono": "Eq"}))
    assert ("eq_set_sidechain", "python") not in _active(rep_ok), _active(rep_ok)


def test_lifecycle_ops_are_informational_not_active() -> None:
    """Constructors / destructors / free helpers never gate (object model / GC)."""
    rep = _report(
        _c(
            "engine_create",
            "engine_destroy",
            "free_floats",
            "project_free_compile_result",
        ),
        _py(),
    )
    assert _active(rep) == set(), _active(rep)
    # ...but they ARE still reported (informationally) so they stay visible.
    informational = {(f.key, f.surface) for f in rep.reported() if f.informational}
    assert ("engine_create", "python") in informational, informational
    assert ("free_floats", "python") in informational, informational


def test_real_repo_has_zero_active_coverage_gaps() -> None:
    """End-to-end: with the curated alias map + allowlist, the repo gate is green.

    This is the anti-regression guard for the handle-gating rollout — if a future
    change drops a handle op from a facade (or removes an alias/allowlist entry)
    without accounting for it, this turns red.
    """
    repo = _HERE.parent.parent
    if not (repo / "include").exists():
        return  # not in the libsonare tree; skip
    import check_parity

    rep = check_parity.run(repo)  # type: ignore[attr-defined]
    active_cov = [f for f in rep.active() if f.category == "coverage"]
    assert active_cov == [], [f"{f.key}/{f.surface}: {f.message}" for f in active_cov]


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
