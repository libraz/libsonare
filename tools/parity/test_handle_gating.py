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

import allowlist as allowlist_mod
import compare
from model import Extraction, FunctionSig


def _c(*keys: str) -> Extraction:
    ex = Extraction(surface="c")
    ex.functions = [FunctionSig(key=k, surface="c", raw_name=k, file="c.h", line=1) for k in keys]
    return ex


def _py(methods: dict[str, str] | None = None, frees: list[str] | None = None) -> Extraction:
    """Build a Python extraction. ``methods`` maps key -> owning class name."""
    ex = Extraction(surface="python")
    for key, cls in (methods or {}).items():
        ex.functions.append(
            FunctionSig(key=key, surface="python", raw_name=f"{cls}.{key}", file="py.py", line=1)
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
    """The same op, exposed on the handle's own class, is covered."""
    rep = _report(_c("engine_set_tempo"), _py(methods={"set_tempo": "RealtimeEngine"}))
    assert ("engine_set_tempo", "python") not in _active(rep), _active(rep)


def test_project_op_requires_a_project_method() -> None:
    """A same-named method on another handle cannot hide a Project gap."""
    rep = _report(
        _c("project_clip_count"),
        _py(methods={"clip_count": "RealtimeEngine"}),
    )
    assert ("project_clip_count", "python") in _active(rep), _active(rep)

    covered = _report(
        _c("project_clip_count"),
        _py(methods={"clip_count": "Project"}),
    )
    assert ("project_clip_count", "python") not in _active(covered), _active(covered)


def test_every_handle_prefix_requires_its_own_class() -> None:
    """The class scoping holds for every mapped prefix, not just ``project_``.

    Ranging over the map is the point: a prefix added later without a class, or
    with the wrong one, is what this catches — the rule is only as good as the
    set it covers.
    """
    for prefix, cls in compare._HANDLE_FULL_PREFIXES:
        key = f"{prefix}probe_op"
        foreign = _report(_c(key), _py(methods={"probe_op": "UnrelatedHandle"}))
        assert (key, "python") in _active(foreign), (prefix, _active(foreign))

        own = _report(_c(key), _py(methods={"probe_op": cls}))
        assert (key, "python") not in _active(own), (prefix, _active(own))


def test_a_free_function_does_not_credit_a_handle_op() -> None:
    """A free function sharing the stripped tail is a different capability.

    It takes the audio rather than holding it, so it is no evidence that the
    handle carries the op. Without this, a whole handle tier ships green on the
    one claim it makes — that each facade grew the methods.
    """
    for prefix, cls in compare._HANDLE_FULL_PREFIXES:
        key = f"{prefix}probe_op"
        free_only = _report(_c(key), _py(frees=["probe_op"]))
        assert (key, "python") in _active(free_only), (prefix, _active(free_only))

        method = _report(_c(key), _py(methods={"probe_op": cls}))
        assert (key, "python") not in _active(method), (prefix, _active(method))


def test_an_alias_may_still_name_a_free_function() -> None:
    """Aliases keep free-function reach; the blanket free credit is what went.

    Six live entries resolve only to a free function (the voice-changer preset
    helpers), and folding an op onto one deliberately is what an entry is for.
    """
    rep = _report(
        _c("realtime_voice_changer_config_default"),
        _py(frees=["realtime_voice_changer_preset_config"]),
    )
    assert ("realtime_voice_changer_config_default", "python") not in _active(rep), _active(rep)


def test_a_free_export_does_not_credit_a_handle_op_of_the_same_name() -> None:
    """Against the real facades, not a fixture: a free export is not a method.

    All three export ``ebur128LoudnessRange`` as a free function. With the
    matching ``Audio`` member taken out of the extraction, a C
    ``sonare_audio_ebur128_loudness_range`` must still read as a gap — the free
    export cannot carry the claim a handle op makes. Removing the member rather
    than assuming its absence keeps this true once the facades grow one.
    """
    repo = _HERE.parent.parent
    if not (repo / "include").exists():
        return  # not in the libsonare tree; skip
    import check_parity

    key, tail = "audio_ebur128_loudness_range", "ebur128_loudness_range"
    for surface in ("python", "node", "wasm"):
        ex = check_parity._EXTRACTORS[surface](repo)
        assert tail in compare._free_keys(ex), surface
        ex.functions = [
            f for f in ex.functions if not (f.raw_name.startswith("Audio.") and f.key == tail)
        ]
        rep = compare.build_report(
            {"c": _c(key), surface: ex},
            allowlist_mod.Allowlist(),
            ["c", surface],
        )
        gaps = {(f.key, f.surface) for f in rep.active() if f.category == "coverage"}
        assert (key, surface) in gaps, surface


def test_handle_prefixes_name_a_class_each_facade_declares() -> None:
    """Every mapped class name is one the real facades actually declare.

    A misspelled class silently scopes to the empty set, which reads as a whole
    handle's worth of coverage gaps rather than as a typo.
    """
    repo = _HERE.parent.parent
    if not (repo / "include").exists():
        return  # not in the libsonare tree; skip
    import check_parity

    for surface in ("python", "node", "wasm"):
        ex = check_parity._EXTRACTORS[surface](repo)
        declared = {f.raw_name.split(".", 1)[0] for f in ex.functions if "." in f.raw_name}
        for prefix, cls in compare._HANDLE_FULL_PREFIXES:
            assert cls in declared, (surface, prefix, cls)


def test_handle_op_covered_by_alias_is_silent() -> None:
    """An idiomatic rename in ``_ALIAS_COVERAGE`` (serialize -> to_json) is covered."""
    rep = _report(_c("project_serialize"), _py(methods={"to_json": "Project"}))
    assert ("project_serialize", "python") not in _active(rep), _active(rep)


_ADDITIVE_DSP_ALIASES = (
    ("hpss_ex", "hpss"),
    ("time_stretch_ex", "time_stretch"),
    ("pitch_shift_ex", "pitch_shift"),
    ("normalize_rms", "normalize"),
    ("trim_ex", "trim"),
    ("nnls_chroma_ex2", "nnls_chroma"),
    ("analyze_impulse_response_ex", "analyze_impulse_response"),
)


def test_additive_dsp_aliases_are_explicit_and_required() -> None:
    """Each extended C op is credited only by its declared public base name."""
    for c_key, facade_key in _ADDITIVE_DSP_ALIASES:
        covered = _report(_c(c_key), _py(frees=[facade_key]))
        assert (c_key, "python") not in _active(covered), _active(covered)

        missing = _report(_c(c_key), _py())
        assert (c_key, "python") in _active(missing), _active(missing)


def test_typed_project_automation_overload_is_covered_by_base_method() -> None:
    """An optional typed descriptor does not require a second facade spelling."""
    rep = _report(
        _c("project_add_automation_lane_ex"),
        _py(methods={"add_automation_lane": "Project"}),
    )
    assert ("project_add_automation_lane_ex", "python") not in _active(rep), _active(rep)


def test_alias_does_not_match_unrelated_member() -> None:
    """An alias only credits its declared target, not a same-prefix neighbour.

    ``eq_set_sidechain`` is credited by ``set_sidechain_mono``/``_stereo`` only;
    a facade that exposes some other ``set_*`` must still fail the gate.
    """
    rep = _report(_c("eq_set_sidechain"), _py(methods={"set_gain": "StreamingEqualizer"}))
    assert ("eq_set_sidechain", "python") in _active(rep), _active(rep)
    rep_ok = _report(
        _c("eq_set_sidechain"), _py(methods={"set_sidechain_mono": "StreamingEqualizer"})
    )
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
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
