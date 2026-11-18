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
from model import Extraction, FunctionSig, Param


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

    All three export ``ebur128LoudnessRangeLu`` (formerly ``ebur128LoudnessRange``) as a free function. With the
    matching ``Audio`` member taken out of the extraction, a C
    ``sonare_audio_ebur128_loudness_range`` must still read as a gap — the free
    export cannot carry the claim a handle op makes. Removing the member rather
    than assuming its absence keeps this true once the facades grow one.
    """
    repo = _HERE.parent.parent
    if not (repo / "include").exists():
        return  # not in the libsonare tree; skip
    import check_parity

    for surface in ("python", "node", "wasm"):
        ex = check_parity._EXTRACTORS[surface](repo)
        # The unit-suffixed spelling is the current one; either names the same export.
        tail = next(
            (
                t
                for t in ("ebur128_loudness_range_lu", "ebur128_loudness_range")
                if t in compare._free_keys(ex)
            ),
            None,
        )
        assert tail is not None, surface
        key = f"audio_{tail}"
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


def test_vocal_render_job_uses_the_canonical_c_prefix() -> None:
    """Render-job methods are owned by ``VocalRenderJob`` under the C prefix."""
    rep = _report(
        _c("vocal_render_job_next"),
        _py(methods={"next": "VocalRenderJob"}),
    )
    assert ("vocal_render_job_next", "python") not in _active(rep), _active(rep)


def test_vocal_handle_aliases_credit_the_actual_facade_methods() -> None:
    """Vocal C handles retain coverage when facades use descriptive method names."""
    cases = (
        ("vocal_session_output_length", "output_length_samples", "VocalEditSession"),
        (
            "vocal_session_source_to_destination",
            "source_sample_to_destination_sample",
            "VocalEditSession",
        ),
        ("vocal_session_capture_snapshot", "capture_render_snapshot", "VocalEditSession"),
        (
            "vocal_draft_source_to_destination",
            "source_sample_to_destination_sample",
            "VocalEditDraft",
        ),
        ("vocal_draft_capture_snapshot", "capture_render_snapshot", "VocalEditDraft"),
        ("vocal_snapshot_output_length", "output_length_samples", "VocalRenderSnapshot"),
        ("vocal_render_job_begin", "begin_render_job", "VocalRenderSnapshot"),
        ("vocal_session_restore", "restore_vocal_edit_session", ""),
    )
    for c_key, facade_key, owner in cases:
        methods = {} if not owner else {facade_key: owner}
        frees = [] if owner else [facade_key]
        rep = _report(_c(c_key), _py(methods=methods, frees=frees))
        assert (c_key, "python") not in _active(rep), (c_key, _active(rep))

    unrelated = _report(
        _c("vocal_render_job_begin"),
        _py(methods={"begin_render_job": "UnrelatedHandle"}),
    )
    assert ("vocal_render_job_begin", "python") in _active(unrelated), _active(unrelated)


def test_an_init_suffix_alone_is_not_a_seeder() -> None:
    """Only a one-struct ``*_init`` is a seeder; the name alone credits nothing."""
    native = {"python": [("py/x.py", "lib.sonare_decompose_with_init(s)\n")]}
    bare = compare.build_report(
        {"c": _c("vocal_note_edit_init"), "python": _py()},
        allowlist_mod.Allowlist(),
        ["c", "python"],
        native_sources=native,
    )
    assert ("vocal_note_edit_init", "python") in _active(bare), _active(bare)

    genuine = compare.build_report(
        {"c": _c("decompose_with_init"), "python": _py()},
        allowlist_mod.Allowlist(),
        ["c", "python"],
        native_sources=native,
    )
    # Called but not a seeder: the binding consumes it, and no seeder credit is taken.
    assert ("decompose_with_init", "python") not in _active(genuine), _active(genuine)
    assert genuine.binding_consumed == [("decompose_with_init", "python", "py/x.py:1")]


def test_vocal_session_factory_alias_credits_only_its_own_key() -> None:
    """The session factory alias credits the create op and nothing else."""
    missing = _report(_c("project_apply_vocal_edit"), _py())
    assert ("project_apply_vocal_edit", "python") in _active(missing), _active(missing)

    factory = _report(_c("vocal_session_create"), _py(frees=["create_vocal_edit_session"]))
    assert ("create_vocal_edit_session", "python") not in _active(factory), _active(factory)
    other = _report(_c("vocal_session_create"), _py(frees=["create_other_session"]))
    assert ("create_other_session", "python") in _active(other), _active(other)

    factory_only = _report(_c("project_apply_vocal_edit"), _py(frees=["create_vocal_edit_session"]))
    assert ("project_apply_vocal_edit", "python") in _active(factory_only), _active(factory_only)


def test_vocal_abi_version_needs_the_direct_export() -> None:
    """A session's capabilities no longer stand in for the module-level ABI version."""
    session = _report(
        _c("vocal_edit_abi_version"),
        _py(methods={"capabilities": "VocalEditSession"}),
    )
    assert ("vocal_edit_abi_version", "python") in _active(session), _active(session)

    direct = _report(_c("vocal_edit_abi_version"), _py(frees=["vocal_edit_abi_version"]))
    assert ("vocal_edit_abi_version", "python") not in _active(direct), _active(direct)


def test_optional_callback_parser_sentinel_matches_none() -> None:
    """TS arrow callbacks can parse as an empty default; omission equals Python ``None``."""
    py = Extraction(surface="python")
    py.functions = [
        FunctionSig(
            key="rehydrate_vocal_edits",
            surface="python",
            raw_name="Project.rehydrate_vocal_edits",
            params=[Param(name="cancel", default="none")],
        )
    ]
    node = Extraction(surface="node")
    node.functions = [
        FunctionSig(
            key="rehydrate_vocal_edits",
            surface="node",
            raw_name="Project.rehydrateVocalEdits",
            params=[Param(name="cancel", default="")],
        )
    ]
    rep = compare.build_report(
        {"python": py, "node": node}, allowlist_mod.Allowlist(), ["python", "node"]
    )
    defaults = [f for f in rep.active() if f.category == "default"]
    assert defaults == [], defaults


def test_vocal_threadlocal_error_helper_is_explicitly_allowlisted() -> None:
    """Error detail is consumed by exceptions; only that exact helper is allowed."""
    allow = allowlist_mod.load(_HERE / "allowlist.toml")
    rep = compare.build_report(
        {"c": _c("vocal_last_error_detail"), "python": _py()},
        allow,
        ["c", "python"],
    )
    assert ("vocal_last_error_detail", "python") not in _active(rep), _active(rep)

    unrelated = compare.build_report(
        {"c": _c("project_apply_vocal_edit"), "python": _py()},
        allow,
        ["c", "python"],
    )
    assert ("project_apply_vocal_edit", "python") in _active(unrelated), _active(unrelated)


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


def test_named_constructor_variants_are_lifecycle_not_active() -> None:
    """A constructor named after its source, not just ``_ex``/``_json``, is still one.

    The HRTF set's two factories name what they build from rather than adding an
    argument, so ``_is_lifecycle_key`` names both suffixes explicitly, the same
    way it names ``_create_json`` and ``_create_ex``.
    """
    rep = _report(_c("hrtf_set_create_default", "hrtf_set_create_from_memory"), _py())
    assert _active(rep) == set(), _active(rep)
    informational = {(f.key, f.surface) for f in rep.reported() if f.informational}
    assert ("hrtf_set_create_default", "python") in informational, informational
    assert ("hrtf_set_create_from_memory", "python") in informational, informational


def test_one_shot_render_is_covered_by_its_alias() -> None:
    """``playback_render_interleaved`` is a plain free function, not a handle op.

    It shares the ``playback`` head token with the renderer/meter handles, so it
    needs both the freefn exception (out of the handle bucket) and the alias (its
    facades all drop ``_interleaved``).
    """
    rep = _report(_c("playback_render_interleaved"), _py(frees=["render_playback"]))
    assert ("playback_render_interleaved", "python") not in _active(rep), _active(rep)

    missing = _report(_c("playback_render_interleaved"), _py())
    assert ("playback_render_interleaved", "python") in _active(missing), _active(missing)


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


def test_a_get_accessor_is_reached_by_its_getter() -> None:
    """`project_get_x` is read by a `Project.x` getter; another class's `x` does not count."""
    rep = _report(_c("project_get_overlap_policy"), _py(methods={"overlap_policy": "Project"}))
    assert ("project_get_overlap_policy", "python") not in _active(rep), _active(rep)
    other = _report(_c("project_get_overlap_policy"), _py(methods={"overlap_policy": "Mixer"}))
    assert ("project_get_overlap_policy", "python") in _active(other), _active(other)


def test_a_count_and_by_index_pair_is_reached_by_its_collection() -> None:
    """Both halves of a count / by-index pair are read by the plural collection member."""
    keys = ("project_track_count", "project_track_by_index")
    rep = _report(_c(*keys), _py(methods={"tracks": "Project"}))
    for key in keys:
        assert (key, "python") not in _active(rep), _active(rep)
    missing = _report(_c(*keys), _py(methods={"track_names": "Project"}))
    for key in keys:
        assert (key, "python") in _active(missing), _active(missing)


def _seeder_c() -> Extraction:
    ex = Extraction(surface="c")
    ex.functions = [
        FunctionSig(
            key="vocal_widget_config_init",
            surface="c",
            raw_name="vocal_widget_config_init",
            params=[Param(name="config", type="SonareWidgetConfig*")],
            file="c.h",
            line=1,
        ),
        FunctionSig(
            key="vocal_widget_run",
            surface="c",
            raw_name="vocal_widget_run",
            params=[Param(name="config", type="const SonareWidgetConfig*")],
            file="c.h",
            line=2,
        ),
        FunctionSig(
            key="vocal_widget_describe",
            surface="c",
            raw_name="vocal_widget_describe",
            params=[Param(name="out", type="SonareWidgetConfig*")],
            file="c.h",
            line=3,
        ),
    ]
    return ex


def _seeder_report(native: str):
    allow = allowlist_mod.Allowlist()
    return compare.build_report(
        {"c": _seeder_c(), "python": _py()},
        allow,
        ["c", "python"],
        native_sources={"python": [("py/_widget.py", native)]},
    )


def test_a_seeder_the_native_code_calls_is_covered() -> None:
    """A facade that seeds the struct it fills reaches the seeder."""
    rep = _seeder_report(
        "cfg = SonareWidgetConfig()\n"
        "lib.sonare_vocal_widget_config_init(ctypes.byref(cfg))\n"
        "lib.sonare_vocal_widget_run(ctypes.byref(cfg))\n"
    )
    found = [f for f in rep.findings if f.key == "vocal_widget_config_init"]
    assert found == [], found


def test_a_struct_passed_in_without_its_seeder_is_active() -> None:
    """Handing the library an unseeded struct sends a zero struct_size."""
    rep = _seeder_report(
        "cfg = SonareWidgetConfig()\n"
        "# sonare_vocal_widget_config_init(cfg) is not called\n"
        "lib.sonare_vocal_widget_run(ctypes.byref(cfg))\n"
    )
    active = [f for f in rep.active() if f.key == "vocal_widget_config_init"]
    assert len(active) == 1, rep.findings
    assert "without calling" in active[0].message and "py/_widget.py:3" in active[0].location


def test_a_struct_the_surface_only_receives_needs_no_seeder() -> None:
    """An out-parameter is filled by the entry that writes it."""
    rep = _seeder_report(
        "cfg = SonareWidgetConfig()\nlib.sonare_vocal_widget_describe(ctypes.byref(cfg))\n"
    )
    found = [f for f in rep.findings if f.key == "vocal_widget_config_init"]
    assert [f.informational for f in found] == [True], found


_C_BODY = """SonareError sonare_widget_run(const float* x, size_t n) {
  if (!validate_audio_params(x, n)) return SONARE_ERROR_INVALID_PARAMETER;
  return run_widget(x, n);
}
"""


def _core_report(addon: str, c_body: str = _C_BODY, claims=None):
    claims = claims if claims is not None else {"widget_run": ("process_widget", ("run_widget",))}
    node = Extraction(surface="node")
    node.functions = [FunctionSig(key="process_widget", surface="node", raw_name="processWidget")]
    saved = compare._CORE_COVERAGE
    compare._CORE_COVERAGE = claims
    try:
        return compare.build_report(
            {"c": _c("widget_run"), "node": node},
            allowlist_mod.Allowlist(),
            ["c", "node"],
            native_sources={"c": [("c.cpp", c_body)], "node": [("addon.cpp", addon)]},
        )
    finally:
        compare._CORE_COVERAGE = saved


def test_a_core_claim_both_sides_call_is_credited_and_listed() -> None:
    """The facade reaches the capability through the core the C entry wraps."""
    rep = _core_report("auto r = sonare::run_widget(audio, n);\n")
    assert ("widget_run", "node") not in _active(rep), _active(rep)
    assert [(k, s, fn) for k, s, fn, _ in rep.core_reached] == [
        ("widget_run", "node", "run_widget")
    ]
    assert not any(f.key == "process_widget" for f in rep.findings), rep.findings


def test_a_core_claim_the_c_entry_does_not_make_is_refused() -> None:
    body = "SonareError sonare_widget_run(const float* x, size_t n) { return other(x, n); }\n"
    rep = _core_report("auto r = sonare::run_widget(audio, n);\n", c_body=body)
    found = [f for f in rep.active() if f.key == "widget_run"]
    assert len(found) == 1 and "refused" in found[0].message, rep.findings
    assert rep.core_reached == []


def test_a_core_claim_the_surface_does_not_make_is_a_gap() -> None:
    rep = _core_report("// run_widget(audio, n) is not called here\n")
    assert ("widget_run", "node") in _active(rep), _active(rep)
    assert rep.core_reached == []


def test_a_core_claim_outranks_an_alias_where_it_is_the_route() -> None:
    """Node and WASM keep the core claim's verdict; the alias reaches the other surfaces."""
    saved = compare._ALIAS_COVERAGE
    compare._ALIAS_COVERAGE = {**saved, "widget_run": ("process_widget",)}
    try:
        called = _core_report("auto r = sonare::run_widget(audio, n);\n")
        skipped = _core_report("// run_widget(audio, n) is not called here\n")
    finally:
        compare._ALIAS_COVERAGE = saved
    assert [(k, s) for k, s, _fn, _site in called.core_reached] == [("widget_run", "node")]
    assert ("widget_run", "node") in _active(skipped), _active(skipped)

    compare._ALIAS_COVERAGE = {**saved, "widget_run": ("process_widget",)}
    try:
        python = _report(_c("widget_run"), _py(frees=["process_widget"]))
    finally:
        compare._ALIAS_COVERAGE = saved
    assert ("widget_run", "python") not in _active(python), _active(python)


def _consumed_report(addon: str, python: str, python_free: list[str] | None = None):
    return compare.build_report(
        {
            "c": _c("widget_name"),
            "node": Extraction(surface="node"),
            "python": _py(frees=python_free),
        },
        allowlist_mod.Allowlist(),
        ["c", "node", "python"],
        native_sources={
            "node": [("addon/render.cpp", addon)],
            "python": [
                (
                    "libsonare/_ffi_signatures_widget.py",
                    "lib.sonare_widget_name.restype = ctypes.c_char_p\n",
                ),
                ("libsonare/_render.py", python),
            ],
        },
    )


def test_an_entry_the_binding_calls_without_a_facade_is_consumed_there() -> None:
    """Derived per surface: the node addon calls it, Python only declares its signature."""
    rep = _consumed_report(
        'out.Set("code", sonare_widget_name(code));\n',
        'if hasattr(lib, "sonare_widget_name"):\n    pass\n',
    )
    assert rep.binding_consumed == [("widget_name", "node", "addon/render.cpp:1")]
    found = [f for f in rep.findings if f.key == "widget_name" and f.surface == "node"]
    assert [(f.informational, f.location) for f in found] == [(True, "addon/render.cpp:1")], found
    assert _active(rep) == {("widget_name", "python")}, _active(rep)


def test_an_entry_a_facade_also_exposes_is_not_reclassified() -> None:
    rep = _consumed_report(
        "// nothing here\n", "name = lib.sonare_widget_name(code)\n", ["widget_name"]
    )
    assert rep.binding_consumed == []
    assert _active(rep) == {("widget_name", "node")}, _active(rep)


_TS_CHAIN = """export class StreamingMasteringChain {
  private native: NativeChain;
  constructor(config: StreamingMasteringChainConfig) {
    this.native = new addon.StreamingMasteringChain(config.params);
    %s
  }

  processMono(samples: Float32Array): Float32Array {
    return this.native.prepare(samples);
  }
}
"""


def _ctor_report(ts: str):
    node = Extraction(surface="node")
    node.functions = [
        FunctionSig(
            key="process_mono",
            surface="node",
            raw_name="StreamingMasteringChain.processMono",
        )
    ]
    return compare.build_report(
        {"c": _c("streaming_mastering_chain_prepare"), "node": node},
        allowlist_mod.Allowlist(),
        ["c", "node"],
        native_sources={"node_facade": [("chain.ts", ts)]},
    )


def test_an_op_the_constructor_performs_is_reached() -> None:
    """The facade prepares in its constructor, so prepare needs no method of its own."""
    rep = _ctor_report(_TS_CHAIN % "this.native.prepare(config.sampleRate, 512, 1);")
    assert ("streaming_mastering_chain_prepare", "node") not in _active(rep), _active(rep)


def test_a_python_init_with_a_wrapped_signature_performs_the_op() -> None:
    """The body starts after the signature, however many lines it takes."""
    py = """class StreamingMasteringChain:
    \"\"\"Docs.\"\"\"

    def __init__(
        self,
        sample_rate: int,
    ) -> None:
        handle = lib.sonare_streaming_mastering_chain_create()
        try:
            rc = lib.sonare_streaming_mastering_chain_prepare(handle, sample_rate)
        except BaseException:
            raise

    def process_mono(self, samples):
        return samples
"""
    rep = compare.build_report(
        {"c": _c("streaming_mastering_chain_prepare"), "python": _py()},
        allowlist_mod.Allowlist(),
        ["c", "python"],
        native_sources={"python": [("chain.py", py)]},
    )
    assert ("streaming_mastering_chain_prepare", "python") not in _active(rep), _active(rep)


def test_an_op_called_outside_the_constructor_is_still_a_gap() -> None:
    """A call in another method is not the constructor performing the op."""
    rep = _ctor_report(_TS_CHAIN % "// prepare is left to the caller")
    assert ("streaming_mastering_chain_prepare", "node") in _active(rep), _active(rep)


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
