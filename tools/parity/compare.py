"""Build the cross-surface parity matrix and the drift categories.

Comparison is always anchored on the C API (the canonical ABI). For each
canonical key we line up the C signature against each language surface and emit:

1. coverage   — a free-function DSP C function missing from a surface, OR a
                surface-only symbol with no C counterpart. Handle/class C funcs
                (audio_*, eq_*, engine_*, ...) are bucketed separately as
                informational, and CLI coverage gaps are informational (the CLI
                is a curated subset).
2. default    — Node / WASM / Python disagree on a parameter's default (C has
                none, so this is a facade-vs-facade comparison).
3. order      — a surface's CONFIG param list (after stripping the leading
                audio-input role group from both sides) diverges from the C
                canonical order / count / names.
4. input      — the audio-input params are named inconsistently across facades
                or vs C (e.g. C `sr` vs python `sample_rate`).
5. enum       — the accepted enum/string-union value set for a param differs
                across surfaces.
6. core_default — a facade default diverges from the C++ core design default
                (config-struct field initializer / free-fn default argument).
7. wasm_internal — the WASM binding is inconsistent across its own three files
                (embind registration -> SonareModule type -> index.ts facade);
                catches a wiring break the index.ts-only checks cannot see.
8. record    — a facade's declared RECORD SHAPE diverges from the C struct that
                is its oracle: a C field the facade never declares, or a field
                the facade declares that C does not have. This is the second
                extraction unit (``RecordShape``, not ``FunctionSig``): the six
                signature checks model argument lists only and are blind to a
                struct's interior.

A finding may be marked ``informational``: it is reported but does not count
toward the non-zero exit code (CI gate). Allowlisted findings are suppressed
entirely (but counted).
"""

from __future__ import annotations

from dataclasses import dataclass, field

import re

from allowlist import Allowlist
from core_defaults import CoreConfig
from extractors.wasm_internal import WasmInternal
from model import Extraction, FunctionSig, RecordShape
from normalize import (
    canonical_core_default,
    canonical_default,
    is_empty_collection_default,
)

# Language facade surfaces that share defaults (C carries none).
_FACADE_SURFACES = ("python", "node", "wasm")

# Default audio-input role names (overridable via allowlist [input_roles]).
# A leading run (first 1-2 params) of these is the audio-input group, stripped
# from both sides before the config-order comparison.
DEFAULT_INPUT_ROLES = (
    "samples",
    "sample_rate",
    "sr",
    "left",
    "right",
    "mono",
    "audio",
    "x",
    "y",
    "buffer",
    "data",
    "length",
    # Primary feature-buffer inputs to reconstruction / tempo functions. These
    # play the same leading-input role as ``samples`` (the array the function
    # operates on); facades may rename them for clarity (``mel`` -> ``mel_power``,
    # ``mfcc`` -> ``mfcc_coefficients``). The input-naming check compares the
    # spellings; the order check strips them as the leading input group.
    "mel",
    "mel_power",
    "mfcc",
    "mfcc_coefficients",
    "mfcc_coeffs",
    "onset_envelope",
    "tempogram_data",
    "values",
)

# C prefixes that denote handle / class-based APIs (create/destroy/handle ops
# exposed as class methods on the facades, not free functions). Overridable via
# allowlist [handle_prefixes].
DEFAULT_HANDLE_PREFIXES = (
    "audio",
    "clip_page_provider",
    "eq",
    "mixer",
    "engine",
    "strip",
    "stream",
    "streaming",
    "realtime",
    "voice",
    "free",
    "master",  # master_audio etc. handled by free-fn detection below if needed
    "project",  # SonareProject handle: project_* ops are Project class methods
)

# Free-function keys that share a handle prefix but ARE plain DSP free functions
# and must stay in the free-function coverage bucket.
HANDLE_PREFIX_FREEFN_EXCEPTIONS = (
    "master_audio",
    "master_audio_stereo",
    "voice_change",
)

# Full handle-instance prefixes. A C handle key (e.g. ``mixer_add_bus``) carries
# the handle type as a leading token group; the facades expose the same op as a
# bare class method (``Mixer.add_bus`` -> key ``add_bus``). To credit coverage we
# strip the longest matching prefix and retry the tail against the facade's
# method/free keys. Ordered LONGEST-FIRST so multi-token prefixes win over their
# single-token shadows (``stream_analyzer_`` before ``stream_``-style tokens).
_HANDLE_FULL_PREFIXES = (
    "streaming_mastering_chain_",
    "stream_analyzer_",
    "realtime_voice_changer_",
    "clip_page_provider_",
    "engine_",
    "mixer_",
    "strip_",
    "eq_",
    "audio_",
    # SonareProject handle: e.g. ``project_split_clip`` -> facade ``Project``
    # method ``split_clip`` / ``splitClip``. Ops renamed on the facade
    # (serialize -> to_json, deserialize -> from_json, create -> ctor) are
    # credited via ``_ALIAS_COVERAGE`` / ``_is_lifecycle_key`` below.
    "project_",
)


def _is_lifecycle_key(key: str) -> bool:
    """A C op every facade expresses through its OBJECT MODEL, not a free fn.

    Constructors (``*_create`` / ``*_create_json``), destructors (``*_destroy``)
    and heap/buffer release helpers (``free_*`` / ``*_free`` / ``*_free_*``) have
    no free-function facade counterpart by design — the facades construct via a
    class constructor and release via GC / RAII. These are reported
    informationally, never gated (a "missing" one is not a coverage bug).
    """
    return (
        key.startswith("free_")
        or key.endswith("_free")
        or "_free_" in key
        or key.endswith("_create")
        or key.endswith("_create_json")
        or key.endswith("_destroy")
    )


# Explicit alias map for handle ops whose capability IS exposed on the facades
# under a DIFFERENT canonical name (an idiomatic rename, NOT an omission). Each
# C key maps to the facade canonical key(s) delivering the same capability;
# coverage is credited when ANY listed alias is present as a method / free
# function on the surface. Kept EXPLICIT (not a fuzzy ``get_``/``set_``/``_json``
# transform) so a getter is never silently matched to a setter — every entry is
# anchored to a verified facade member. canonical_key folds camelCase to snake,
# so one alias serves Python (``data``), Node (``getData``->``get_data``) and
# WASM (``setConfig``->``set_config``) alike.
_ALIAS_COVERAGE = {
    # C ABI returns a JSON document; facades parse it into the public descriptor.
    "capabilities_json": ("capabilities",),
    "capability_catalog_json": ("capability_catalog",),
    # Accessors exposed as bare properties or get-prefixed getters.
    "audio_data": ("data", "get_data"),
    "audio_length": ("length", "get_length"),
    "audio_duration": ("duration", "get_duration"),
    "engine_get_transport_state": ("transport_state",),
    "mixer_get_strip_count": ("strip_count",),
    "realtime_voice_changer_get_config": ("config", "config_json", "config_pod"),
    # Whole-object JSON serialize/deserialize -> to_json / from_json.
    "project_serialize": ("to_json",),
    "project_deserialize": ("from_json",),
    # JSON-config setter -> the typed set_config the facades expose.
    "realtime_voice_changer_set_config_json": ("set_config",),
    # The default config is the public neutral-monitor preset config. Facades
    # expose the latter rather than a duplicate no-argument default helper.
    "realtime_voice_changer_config_default": ("realtime_voice_changer_preset_config",),
    # Standalone preset validator (the facade reorders the name to lead with the
    # verb): validate_realtime_voice_changer_preset_json.
    "realtime_voice_changer_validate_preset_json": (
        "validate_realtime_voice_changer_preset_json",
    ),
    # Quantized read _ex variants -> the public read_frames_{i16,u8}.
    "stream_analyzer_read_frames_i16_ex": ("read_frames_i16",),
    "stream_analyzer_read_frames_u8_ex": ("read_frames_u8",),
    # Explicit-Mel-range forward transforms -> the base mel_spectrogram / mfcc
    # facade, which exposes the fmin/fmax/htk arguments and routes to the _ex C
    # entry point. Anchored to the base member so the credit holds only while the
    # base facade actually exists (and, by the params it now carries, exposes the
    # explicit Mel range needed to round-trip with mel_to_stft / mel_to_audio).
    "mel_spectrogram_ex": ("mel_spectrogram",),
    "mfcc_ex": ("mfcc",),
    # Extended one-shot variants folded into their base facade functions. Each
    # base function exposes the extended fields and routes to this C entry point.
    "analyze_json_ex": ("analyze",),
    # The C entry point returns the estimate as JSON; each facade parses it and
    # exposes the same operation under the unsuffixed name.
    "estimate_meter_json": ("estimate_meter",),
    # Onset facades take the complete extended option set under the base name.
    "detect_onsets_ex": ("detect_onsets",),
    "chroma_cens_ex": ("chroma_cens",),
    "chroma_cqt_ex": ("chroma_cqt",),
    "mfcc_to_audio_ex2": ("mfcc_to_audio",),
    "mfcc_to_mel_ex": ("mfcc_to_mel",),
    "spectral_bandwidth_ex": ("spectral_bandwidth",),
    "nnls_chroma_ex": ("nnls_chroma",),
    # Additive DSP-option variants remain the same public operation on every
    # facade.  The facade method owns the options bag and routes non-default
    # values to the extended C entry point.
    "hpss_ex": ("hpss",),
    "time_stretch_ex": ("time_stretch",),
    "pitch_shift_ex": ("pitch_shift",),
    "normalize_rms": ("normalize",),
    "trim_ex": ("trim",),
    "nnls_chroma_ex2": ("nnls_chroma",),
    "analyze_impulse_response_ex": ("analyze_impulse_response",),
    # NMF warm-start variant -> the base decompose facade, which exposes the
    # `init` initialiser argument and routes to sonare_decompose_with_init.
    # (Python / WASM expose decompose_with_init by name, matched directly before
    # this; the alias credits the Node fold onto the base decompose() function.)
    "decompose_with_init": ("decompose",),
    # Progress-callback mastering variants -> base fn with an optional callback.
    "master_audio_with_progress": ("master_audio",),
    # The mixing assistant's C entry points are named after the subsystem; the
    # facades name them after what the caller asks for, which is a suggested mix
    # scene. Anchored to the facade members that actually deliver the operation,
    # so the credit lapses if one of them is removed.
    "mixing_assistant_suggest": ("suggest_mix_scene",),
    "mixing_assistant_suggest_scene_json": ("suggest_mix_scene_json",),
    "mixing_assistant_source_class_names": ("mix_source_class_names",),
    "mixing_assistant_source_class_from_name": ("mix_source_class_from_name",),
    "master_audio_stereo_with_progress": ("master_audio_stereo",),
    # C cancellation is additive in `_ex` variants; every facade folds it into
    # the same progress-capable method rather than exposing a second spelling.
    "analyze_json_with_progress_ex": ("analyze_with_progress",),
    "mastering_chain_with_progress_ex": ("mastering_chain",),
    "mastering_chain_stereo_with_progress_ex": ("mastering_chain_stereo",),
    "master_audio_with_progress_ex": ("master_audio",),
    "master_audio_stereo_with_progress_ex": ("master_audio_stereo",),
    # The offline render's `finalize` flag is additive in the `_ex` variant;
    # every facade folds it into the one render_offline / renderOffline member
    # (a request field on Node and WASM, a keyword argument on Python) rather
    # than exposing a second spelling. Anchored to the base member, so the
    # credit lapses if that facade method ever disappears.
    "engine_render_offline_ex": ("render_offline",),
    # Sidechain EQ -> the mono/stereo-specific setters the facades expose.
    "eq_set_sidechain": ("set_sidechain_mono", "set_sidechain_stereo"),
    # Plural builtin-instrument bounce -> singular facade method (one or many).
    "project_bounce_with_builtin_instruments": ("bounce_with_builtin_instrument",),
    # Plural SF2-instrument bounce -> singular facade method (one or many).
    "project_bounce_with_sf2_instruments": ("bounce_with_sf2_instrument",),
    # Plural NativeSynth-instrument bounce -> singular facade method (one or many).
    "project_bounce_with_synth_instruments": ("bounce_with_synth_instrument",),
    # The Project facade presents aggregate descriptors rather than the C ABI's
    # component accessors / name resolver.
    "project_fade_curve_from_name": ("set_clip_fade",),
    "project_marker_name_by_index": ("marker_by_index",),
    "project_set_marker_ex_name": ("set_marker_ex",),
    "project_unresolved_audio_source_count": ("unresolved_audio_source_ids",),
    "project_unresolved_audio_source_id_by_index": ("unresolved_audio_source_ids",),
    # Typed automation is an additive C descriptor overload. Each facade keeps
    # one add/edit method and dispatches to `_ex` only when targetKind is
    # supplied, so coverage belongs to that canonical public method rather than
    # a redundant second spelling.
    "project_add_automation_lane_ex": ("add_automation_lane",),
    "project_edit_automation_lane_ex": ("edit_automation_lane",),
    # ProjectSource is the facade's aggregate read model; its source-by-index
    # projection includes the owning audio metadata strings for audio sources.
    "project_get_audio_source_metadata": ("source_by_index",),
    # prepare() accepts maxChannels and routes to this C entry point when it is
    # available, retaining a fallback for older native libraries in Python.
    "engine_prepare_with_channels": ("prepare",),
    # Python's segmentation facade uses concise names; the C ABI namespaces
    # these helpers with `segment_`.
    "segment_agglomerative": ("agglomerative",),
    "segment_cross_similarity": ("cross_similarity",),
    "segment_lag_to_recurrence": ("lag_to_recurrence",),
    "segment_path_enhance": ("path_enhance",),
    "segment_recurrence_matrix": ("recurrence_matrix",),
    "segment_recurrence_to_lag": ("recurrence_to_lag",),
    "segment_subsegment": ("subsegment",),
    # SoundFont ops: the C symbols spell "soundfont" as one word while the
    # Node/WASM camelCase methods (`loadSoundFont`) fold to `load_sound_font`.
    # An idiomatic spelling difference shared by both JS facades (Python matches
    # the C spelling directly), not an omission.
    "project_load_soundfont": ("load_sound_font",),
    "project_clear_soundfont": ("clear_sound_font",),
    "project_soundfont_preset_count": ("sound_font_preset_count",),
    "project_soundfont_manifest": ("sound_font_manifest",),
    "engine_load_soundfont": ("load_sound_font",),
    # GM / GM2 naming helpers: every facade (Python `gm_instrument_name`, Node
    # `gmInstrumentName`, WASM `gmInstrumentName`) drops the `midi_` prefix the C
    # symbol carries. An idiomatic rename shared by all three facades, not an
    # omission -- credit it against the facade member that exists.
    "midi_gm_instrument_name": ("gm_instrument_name",),
    "midi_gm_program_for_name": ("gm_program_for_name",),
    "midi_gm_family_name": ("gm_family_name",),
    "midi_gm_family_first_program": ("gm_family_first_program",),
    "midi_gm2_instrument_name": ("gm2_instrument_name",),
    "midi_gm_drum_name": ("gm_drum_name",),
    "midi_gm_drum_note_for_name": ("gm_drum_note_for_name",),
    "midi_gm2_drum_set_name": ("gm2_drum_set_name",),
    "midi_gm2_drum_name": ("gm2_drum_name",),
    # Per-note controller name likewise drops the `midi_` prefix on every facade.
    "midi_per_note_controller_name": ("per_note_controller_name",),
}


@dataclass
class Finding:
    category: (
        # coverage | default | core_default | order | input | enum |
        # wasm_internal | record
        str
    )
    key: str
    surface: str  # surface the finding is attributed to (or 'cross')
    message: str
    detail: dict = field(default_factory=dict)
    location: str = ""
    allowlisted: bool = False
    informational: bool = False


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    matrix: dict[str, dict[str, bool]] = field(default_factory=dict)
    surfaces: list[str] = field(default_factory=list)
    unparsed: dict[str, int] = field(default_factory=dict)
    unparsed_notes: dict[str, list[str]] = field(default_factory=dict)
    surface_only: dict[str, list[str]] = field(default_factory=dict)
    handle_keys: list[str] = field(default_factory=list)
    # Records extracted per surface (the record unit's vacuity metric: a checker
    # that reports nothing because it parsed nothing is the failure mode to see).
    record_counts: dict[str, int] = field(default_factory=dict)

    def active(self) -> list[Finding]:
        """Findings that count toward failure (non-allowlisted, non-informational)."""
        return [f for f in self.findings if not f.allowlisted and not f.informational]

    def reported(self) -> list[Finding]:
        """All non-allowlisted findings (including informational)."""
        return [f for f in self.findings if not f.allowlisted]


def _index(ex: Extraction) -> dict[str, FunctionSig]:
    # First definition wins for a key on a surface, with one exception: a FREE
    # function (raw_name has no '.') is preferred over a CLASS METHOD for the
    # same key. A facade often exposes both a free ``resample(samples, srcSr,
    # targetSr)`` and an ``Audio.resample(targetSr)`` convenience method that
    # drops the instance-supplied leading args; the free function is the
    # canonical-shaped signature for the positional/order comparison, so it must
    # win regardless of which file was parsed first.
    out: dict[str, FunctionSig] = {}
    for f in ex.functions:
        existing = out.get(f.key)
        if existing is None:
            out[f.key] = f
        elif "." in existing.raw_name and "." not in f.raw_name:
            out[f.key] = f  # upgrade a method entry to the free-function form
    return out


def _method_keys(ex: Extraction | None) -> set[str]:
    """Canonical keys that a surface exposes as a CLASS METHOD (raw_name has a '.')."""
    if ex is None:
        return set()
    return {f.key for f in ex.functions if "." in f.raw_name}


def _class_method_keys(ex: Extraction | None, class_name: str) -> set[str]:
    """Canonical keys exposed specifically by ``class_name``.

    Handle coverage must not let an identically named method on another class
    satisfy the C operation. For example, ``RealtimeEngine.clipCount`` does not
    expose ``sonare_project_clip_count`` on ``Project``.
    """
    if ex is None:
        return set()
    prefix = f"{class_name}."
    return {f.key for f in ex.functions if f.raw_name.startswith(prefix)}


def _free_keys(ex: Extraction | None) -> set[str]:
    """Canonical keys exposed as free functions rather than class members."""
    if ex is None:
        return set()
    return {f.key for f in ex.functions if "." not in f.raw_name}


def _is_handle_key(key: str, prefixes: tuple[str, ...]) -> bool:
    if key in HANDLE_PREFIX_FREEFN_EXCEPTIONS:
        return False
    head = key.split("_", 1)[0]
    return head in prefixes


def _leading_input_group(names: list[str], roles: set[str]) -> int:
    """Return the count (0-2) of leading params that form the audio-input group."""
    n = 0
    for nm in names[:2]:
        if nm in roles:
            n += 1
        else:
            break
    return n


def _strip_leading_input(names: list[str], roles: set[str]) -> list[str]:
    return names[_leading_input_group(names, roles) :]


def build_report(
    extractions: dict[str, Extraction],
    allow: Allowlist,
    selected: list[str],
    core_configs: dict[str, CoreConfig] | None = None,
    wasm_internal: WasmInternal | None = None,
) -> Report:
    rep = Report(surfaces=selected)
    indexed = {s: _index(ex) for s, ex in extractions.items()}
    for s, ex in extractions.items():
        rep.unparsed[s] = ex.unparsed
        rep.unparsed_notes[s] = ex.unparsed_notes
        rep.surface_only[s] = []

    roles = set(allow.input_roles or DEFAULT_INPUT_ROLES)
    handle_prefixes = tuple(allow.handle_prefixes or DEFAULT_HANDLE_PREFIXES)

    c_index = indexed.get("c", {})
    all_keys = set()
    for idx in indexed.values():
        all_keys.update(idx.keys())

    # Class-method keys per facade (for handle/class matching).
    method_keys = {s: _method_keys(extractions.get(s)) for s in selected}
    project_method_keys = {
        s: _class_method_keys(extractions.get(s), "Project") for s in selected
    }
    free_keys = {s: _free_keys(extractions.get(s)) for s in selected}

    rep.handle_keys = sorted(k for k in c_index if _is_handle_key(k, handle_prefixes))

    # --- Coverage matrix ---
    for key in sorted(all_keys):
        rep.matrix[key] = {s: (key in indexed.get(s, {})) for s in selected}

    # --- 1. Coverage gaps ---
    handle_set = set(rep.handle_keys)
    for key in sorted(c_index):
        is_handle = key in handle_set
        for s in selected:
            if s == "c":
                continue
            # Project handle reachability is class-specific. Without this rule,
            # an unrelated class method with the same tail (notably
            # RealtimeEngine.clipCount) can hide a missing Project method.
            candidate_methods = (
                project_method_keys.get(s, set())
                if key.startswith("project_")
                else method_keys.get(s, set())
            )
            candidate_symbols = (
                candidate_methods | free_keys.get(s, set())
                if key.startswith("project_")
                else candidate_methods | set(indexed.get(s, {}))
            )
            present_free = key in indexed.get(s, {})
            present_method = key in candidate_methods
            if present_free or present_method:
                continue
            # Handle-instance C key (``mixer_add_bus``): the facade exposes the
            # same op as a bare class method (``Mixer.add_bus`` -> key
            # ``add_bus``), so the handle prefix is stripped there. Strip the
            # longest matching handle prefix and retry the tail against this
            # surface's method-keys AND free-function keys; a match means the op
            # IS exposed -- covered, no finding.
            stripped = None
            for prefix in _HANDLE_FULL_PREFIXES:
                if key.startswith(prefix) and len(key) > len(prefix):
                    stripped = key[len(prefix) :]
                    break
            if stripped is not None and stripped in candidate_symbols:
                continue
            # Idiomatic rename: the capability is exposed under a different
            # canonical name (verified alias). Credit it when any listed alias is
            # present as a method / free function on this surface.
            aliases = _ALIAS_COVERAGE.get(key)
            if aliases and any(a in candidate_symbols for a in aliases):
                continue
            if allow.coverage_ok(key, s):
                rep.findings.append(Finding("coverage", key, s, "", allowlisted=True))
                continue
            # Constructors / destructors / heap-release helpers are handled by the
            # facade object model (ctor / GC / RAII), not a free function. Report
            # them informationally, never as an active gap.
            is_lifecycle = _is_lifecycle_key(key)
            # GATING: a handle/class op that is neither aliased, lifecycle, nor
            # allowlisted is a REAL cross-binding gap and counts toward failure --
            # this is what catches a new C op wired on some facades but not others.
            # Lifecycle helpers and the curated CLI surface stay informational.
            informational = is_lifecycle or s == "cli"
            if is_lifecycle:
                msg = (
                    f"C lifecycle/memory helper '{key}' has no free-function "
                    f"counterpart in {s} (facade uses constructor / GC / RAII)"
                )
            elif s == "cli":
                msg = f"C function '{key}' not exposed by the (curated) CLI"
            elif is_handle:
                msg = (
                    f"C handle/class op '{key}' is not exposed on the {s} facade "
                    "(no method, prefix-strip, or alias match)"
                )
            else:
                msg = f"C function '{key}' is not exposed in {s}"
            rep.findings.append(
                Finding(
                    category="coverage",
                    key=key,
                    surface=s,
                    message=msg,
                    location=f"{c_index[key].file}:{c_index[key].line}",
                    informational=informational,
                )
            )

    # Surface-only symbols (no C counterpart).
    for s in selected:
        if s == "c":
            continue
        for key, sig in indexed.get(s, {}).items():
            # A C JSON getter can deliberately become a parsed, idiomatic
            # facade name. The forward alias above credits C coverage; this
            # reverse check keeps that same facade name out of surface-only
            # findings. Keep it explicit rather than stripping every `_json`.
            is_c_alias = any(key in aliases for aliases in _ALIAS_COVERAGE.values())
            if key in c_index or is_c_alias:
                continue
            rep.surface_only[s].append(key)
            allowlisted = allow.surface_only_ok(key, s)
            # A surface-only symbol whose raw_name carries a '.' is a facade
            # CLASS METHOD (e.g. "Mixer.add_bus", "Audio.fromBuffer"): an
            # ergonomic handle/instance method with no C free-function
            # counterpart. Keep it visible but informational (out of the
            # CI-failing set). CLI-only commands are likewise an expected
            # curated surface -> informational. Free-function surface-only
            # symbols stay active (real coverage signal worth triaging).
            is_method = "." in sig.raw_name
            informational = is_method or s == "cli"
            rep.findings.append(
                Finding(
                    category="coverage",
                    key=key,
                    surface=s,
                    message=f"{s}-only symbol '{key}' has no C-API counterpart",
                    location=f"{sig.file}:{sig.line}",
                    detail={"raw_name": sig.raw_name},
                    allowlisted=allowlisted,
                    informational=informational and not allowlisted,
                )
            )

    # --- 2. Default drift (facade vs facade) ---
    _default_drift(indexed, allow, rep, roles)

    # --- 3. Arg order / count / name vs C canonical (config params only) ---
    _order_drift(c_index, indexed, allow, rep, selected, roles)

    # --- 4. Input-param naming consistency ---
    _input_naming(c_index, indexed, allow, rep, selected, roles)

    # --- 5. Enum value-set drift ---
    _enum_drift(indexed, allow, rep)

    # --- 6. Core-default drift (facade vs C++ core struct design default) ---
    if core_configs:
        _core_default_drift(indexed, core_configs, allow, rep, roles)

    # --- 7. WASM-internal wiring consistency (embind <-> SonareModule <-> facade) ---
    if wasm_internal is not None:
        _wasm_internal_drift(wasm_internal, allow, rep)

    # --- 8. Record-shape drift (facade record vs the C struct that is its oracle) ---
    _record_drift(extractions, allow, rep, selected)

    return rep


def _config_names(sig: FunctionSig, roles: set[str]) -> list[str]:
    """Core (non-structural) config param names.

    The leading audio-input group is removed, and any audio-input ROLE name
    (buffers like ``left`` / ``right``, the sample-rate alias ``sr`` /
    ``sample_rate``, the buffer-companion ``length``, ...) is dropped wherever it
    appears -- a non-leading buffer arg (e.g. the ``left`` / ``right`` channels of
    ``master_audio_stereo``, which sit AFTER ``preset_name`` in the C signature)
    is still an input the facades surface positionally, so it carries no
    config-order signal. Progress-callback params the facades add for streaming
    reporting (``on_progress``) and cancellation (``cancel``) are facade-only
    control hooks. C supplies those through separate `_with_progress` / `_ex`
    entry points, so they carry no config-order signal.
    """
    names = [p.name for p in sig.core_params()]
    names = _strip_leading_input(names, roles)
    return [
        n
        for n in names
        if n not in roles
        and n not in _CALLBACK_NAMES
        and n not in _BUFFER_COMPANION_NAMES
    ]


# Progress / streaming and cancellation callback params the facades add for
# ergonomic control. They have no C config counterpart (C uses separate
# ``_with_progress`` / ``_ex`` entries) and carry no config-order signal.
_CALLBACK_NAMES = {"on_progress", "onprogress", "cancel"}


def _default_drift(indexed, allow, rep: Report, roles: set[str]) -> None:
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    keys = set()
    for s in facades:
        keys.update(indexed[s].keys())
    for key in sorted(keys):
        sigs = {s: indexed[s][key] for s in facades if key in indexed[s]}
        if len(sigs) < 2:
            continue
        param_names: list[str] = []
        for sig in sigs.values():
            for p in sig.core_params():
                if p.name in roles:
                    continue
                if p.name not in param_names:
                    param_names.append(p.name)
        for pname in param_names:
            if allow.default_ok(key, pname):
                continue
            declared: dict[str, str] = {}
            for s, sig in sigs.items():
                match = next((p for p in sig.core_params() if p.name == pname), None)
                if match is None or match.default is None:
                    continue
                declared[s] = match.default
            # Canonicalize for the distinctness test (enum-member vs camelCase
            # string-union literals fold to a common form). The raw surface
            # spelling is still shown in the message.
            canon = {s: canonical_default(v) for s, v in declared.items()}
            # Collection-sentinel equivalence: when at least one facade spells
            # the default as an empty collection (``[]`` / ``{}``) and another
            # spells it ``None``, both mean "no value" -- fold ``none`` to the
            # empty-collection form so they compare equal.
            if any(is_empty_collection_default(v) for v in declared.values()):
                for s, v in declared.items():
                    if canon[s] == "none" or is_empty_collection_default(v):
                        canon[s] = "\0empty-collection\0"
            distinct = set(canon.values())
            if len(distinct) > 1:
                rep.findings.append(
                    Finding(
                        category="default",
                        key=key,
                        surface="cross",
                        message=(
                            f"default drift for '{key}.{pname}': "
                            + ", ".join(f"{s}={declared[s]}" for s in sorted(declared))
                        ),
                        detail={"param": pname, "defaults": declared},
                        location="; ".join(
                            f"{s}={sigs[s].file}:{sigs[s].line}"
                            for s in sorted(declared)
                        ),
                    )
                )


# C config param names that are opaque struct/options pointers the facades
# legitimately flatten into individual fields (or fold into an options/request
# bag). A request object is a facade-only grouping of the same positional
# inputs, so it has no C-level order signal of its own.
_STRUCT_BAG_NAMES = {"config", "params", "options", "overrides", "opts", "request"}

# Sample-rate spellings. In several C functions (tempogram/mel/mfcc/pcen
# families) ``sr`` / ``sample_rate`` is a POSITIONAL C argument that sits inside
# what the facades expose as a leading config field rather than in the
# audio-input group. The facades consistently spell it ``sample_rate`` /
# ``sampleRate`` and place it just after the primary buffer. Treat it as an
# alias of the C positional ``sr`` and drop it from the config-order comparison
# (it is an intentional, consistent facade convention, not order drift).
_SAMPLE_RATE_NAMES = {"sample_rate", "sr"}

# Buffer-companion length params (the count that accompanies a ``const float*``
# buffer). C carries them as a positional arg; facades derive length from the
# array, so they have no facade counterpart. Stripped before the audio-input
# naming check so the buffer names line up.
_BUFFER_COMPANION_NAMES = {"length", "size", "len", "input_length"}

# Some facades are exposed in their EXTENDED (``_ex``) form: the facade config is
# the base C function's order followed by the extra fields the ``_ex`` variant
# adds. These trailing extended fields are legitimate, so the order check accepts
# the base C order as a prefix and the known tail after it.
#   detect_chords  -> base + the SonareChordDetectionOptions fields used by
#                     sonare_detect_chords_ex.
#   mel_spectrogram -> base + the explicit Mel range (fmin/fmax/htk) used by
#                     sonare_mel_spectrogram_ex.
#   mfcc -> base + the explicit Mel range (fmin/fmax/htk) and cepstral lifter
#                     used by sonare_mfcc_ex.
#   decompose -> base + the NMF initialiser (init) used by
#                     sonare_decompose_with_init.
_EXTENDED_FIELD_TAILS = {
    "detect_chords": (
        "use_hmm",
        "hmm_beam_width",
        "use_key_context",
        "key_root",
        "key_mode",
        "detect_inversions",
        "chroma_method",
    ),
    "mel_spectrogram": ("fmin", "fmax", "htk"),
    "mfcc": ("fmin", "fmax", "htk", "lifter"),
    "chroma_cens": ("bins_per_octave",),
    "chroma_cqt": ("bins_per_octave",),
    "mfcc_to_mel": ("lifter",),
    "decompose": ("init",),
    # These facade base names expose the corresponding additive C `_ex`
    # variants.  Their trailing request fields intentionally extend the base
    # C order rather than indicate an argument-order mismatch.
    "analyze_impulse_response": ("min_decay_db",),
    "hpss": ("n_fft", "hop_length", "hard_mask"),
    "hpss_with_residual": ("n_fft", "hop_length", "hard_mask"),
    "pitch_shift": ("n_fft", "hop_length"),
    "time_stretch": ("n_fft", "hop_length"),
    "trim": ("frame_length", "hop_length"),
    # The facade base name exposes the C `_ex` Minkowski exponent; p=2 is the
    # legacy C base behavior.
    "spectral_bandwidth": ("p",),
}


def _order_drift(
    c_index, indexed, allow, rep: Report, selected, roles: set[str]
) -> None:
    for key, csig in c_index.items():
        c_cfg = _config_names(csig, roles)
        # When C itemizes no config params, or its only config param is an opaque
        # struct/options pointer, the facade flattening it is the bag convention,
        # not order drift. (The itemized C order lives in a `_with_options`
        # variant, compared on its own key.)
        if not c_cfg or all(n in _STRUCT_BAG_NAMES for n in c_cfg):
            continue
        for s in _FACADE_SURFACES:
            if s not in selected or key not in indexed.get(s, {}):
                continue
            if allow.order_ok(key, s):
                continue
            ssig = indexed[s][key]
            s_cfg = _config_names(ssig, roles)
            # Facade may also flatten/fold into a bag: a facade whose config is a
            # subset bag (all names are struct/bag names) carries no order signal.
            if all(n in _STRUCT_BAG_NAMES for n in s_cfg):
                continue
            # A trailing versioned C config struct is commonly flattened into
            # facade keyword/request fields.  The positional prefix still has
            # to match exactly; the struct's interior has no C-level parameter
            # order to compare here.  This is distinct from an arbitrary tail
            # because the canonical final parameter is explicitly an opaque bag.
            if c_cfg[-1] in _STRUCT_BAG_NAMES and s_cfg[: len(c_cfg) - 1] == c_cfg[:-1]:
                continue
            # Facades fold variadic C scalar tails into an options bag, so a
            # facade exposing a STRICT PREFIX of the C config order is fine.
            if s_cfg == c_cfg[: len(s_cfg)]:
                continue
            # Facades exposed in their EXTENDED (``_ex``) form: C base order
            # followed by the known extra-field tail of the _ex variant.
            tail = _EXTENDED_FIELD_TAILS.get(key)
            if (
                tail is not None
                and s_cfg[: len(c_cfg)] == c_cfg
                and tuple(s_cfg[len(c_cfg) :]) == tail
            ):
                continue
            rep.findings.append(
                Finding(
                    category="order",
                    key=key,
                    surface=s,
                    message=(
                        f"config param order/name mismatch vs C for '{key}': "
                        f"C={c_cfg} {s}={s_cfg}"
                    ),
                    detail={"c": c_cfg, s: s_cfg},
                    location=f"{ssig.file}:{ssig.line} (C {csig.file}:{csig.line})",
                )
            )


# Audio-input role names that carry no naming signal across surfaces: the
# buffer-companion length count (C-only, facades derive it from the array) and
# the sample-rate alias (a consistent facade convention, see _SAMPLE_RATE_NAMES).
_INPUT_NOISE_NAMES = _BUFFER_COMPANION_NAMES | _SAMPLE_RATE_NAMES


def _input_names(sig: FunctionSig, roles: set[str]) -> list[str]:
    """Leading audio-input buffer param names of a signature.

    The buffer-companion length and sample-rate alias are dropped so only the
    actual buffer parameter names (``samples`` / ``left`` / ``right`` / ...) are
    compared for naming consistency.
    """
    names = [p.name for p in sig.params]
    n = _leading_input_group(names, roles)
    return [nm for nm in names[:n] if nm not in _INPUT_NOISE_NAMES]


def _input_naming(
    c_index, indexed, allow, rep: Report, selected, roles: set[str]
) -> None:
    """Flag when the audio-input params are named inconsistently across surfaces."""
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    keys = set()
    for s in facades:
        keys.update(indexed[s].keys())
    for key in sorted(keys):
        if allow.input_naming_ok(key):
            continue
        groups: dict[str, list[str]] = {}
        for s in facades:
            if key in indexed[s]:
                g = _input_names(indexed[s][key], roles)
                if g:
                    groups[s] = g
        # Include C's input naming where C declares input-role names. Drop the
        # buffer-companion length and the sample-rate alias so only the buffer
        # parameter names are compared.
        if key in c_index:
            cg = [
                p.name
                for p in c_index[key].params
                if p.name in roles and p.name not in _INPUT_NOISE_NAMES
            ]
            if cg:
                groups["c"] = cg
        if len(groups) < 2:
            continue
        distinct = {tuple(v) for v in groups.values()}
        if len(distinct) > 1:
            rep.findings.append(
                Finding(
                    category="input",
                    key=key,
                    surface="cross",
                    message=(
                        f"audio-input naming differs for '{key}': "
                        + "; ".join(f"{s}={groups[s]}" for s in sorted(groups))
                    ),
                    detail={"groups": groups},
                    location="; ".join(
                        f"{s}={indexed[s][key].file}:{indexed[s][key].line}"
                        for s in sorted(groups)
                        if s in indexed and key in indexed[s]
                    ),
                )
            )


def _enum_drift(indexed, allow, rep: Report) -> None:
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    keys = set()
    for s in facades:
        keys.update(indexed[s].keys())
    for key in sorted(keys):
        sigs = {s: indexed[s][key] for s in facades if key in indexed[s]}
        if len(sigs) < 2:
            continue
        param_names: list[str] = []
        for sig in sigs.values():
            for p in sig.params:
                if p.enum_values and p.name not in param_names:
                    param_names.append(p.name)
        for pname in param_names:
            if allow.enum_ok(key, pname):
                continue
            sets: dict[str, tuple[str, ...]] = {}
            for s, sig in sigs.items():
                match = next(
                    (p for p in sig.params if p.name == pname and p.enum_values), None
                )
                if match is not None:
                    sets[s] = match.enum_values
            distinct = {frozenset(v) for v in sets.values()}
            if len(distinct) > 1:
                rep.findings.append(
                    Finding(
                        category="enum",
                        key=key,
                        surface="cross",
                        message=(
                            f"enum value-set drift for '{key}.{pname}': "
                            + "; ".join(f"{s}={sorted(sets[s])}" for s in sorted(sets))
                        ),
                        detail={
                            "param": pname,
                            "sets": {s: list(v) for s, v in sets.items()},
                        },
                    )
                )


def _core_default_drift(
    indexed,
    core_configs: dict[str, CoreConfig],
    allow,
    rep: Report,
    roles: set[str],
) -> None:
    """Flag a facade default that diverges from its C++ core struct initializer.

    The facade-vs-facade default check (``_default_drift``) only sees the three
    facades; it is blind to the case where every facade AGREES on a value the
    C++ core design never intended. This check anchors each mapped facade param
    on the numeric / boolean field initializer of its C++ core config struct
    (see ``core_map.toml`` + ``extractors/cpp_struct.py``) and reports any
    divergence. Only params a facade actually declares a default for, and only
    struct fields with a literal initializer, are compared.
    """
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    for key in sorted(core_configs):
        cfg = core_configs[key]
        if not cfg.fields:
            continue
        for s in facades:
            sig = indexed[s].get(key)
            if sig is None:
                continue
            for p in sig.core_params():
                if p.name in roles or p.default is None:
                    continue
                core_def = cfg.core_default_for(p.name)
                if core_def is None:
                    continue
                if allow.core_default_ok(key, p.name):
                    continue
                if "::" in core_def:
                    # Enum-member core default. The facade may spell it as a
                    # member string ('stft') or a bare integer (0). We can fold
                    # the string spelling to compare, but an integer needs an
                    # enum value table we don't carry -- skip the integer case
                    # rather than risk a false positive (the facade-vs-facade
                    # enum-set check already guards enum consistency).
                    facade_canon = canonical_default(p.default)
                    if facade_canon is not None and re.fullmatch(
                        r"-?\d+", facade_canon
                    ):
                        continue
                    if facade_canon == canonical_core_default(core_def):
                        continue
                elif canonical_default(p.default) == canonical_default(core_def):
                    continue
                field_name = cfg.rename.get(p.name, p.name)
                rep.findings.append(
                    Finding(
                        category="core_default",
                        key=key,
                        surface=s,
                        message=(
                            f"core-default drift for '{key}.{p.name}': "
                            f"{s}={p.default} vs C++ {cfg.name}.{field_name}={core_def}"
                        ),
                        detail={
                            "param": p.name,
                            "facade": p.default,
                            "core": core_def,
                            "anchor": cfg.name,
                            "kind": cfg.kind,
                        },
                        location=f"{sig.file}:{sig.line} (core {cfg.header})",
                    )
                )


def _wasm_internal_drift(wi: WasmInternal, allow, rep: Report) -> None:
    """Cross-validate the WASM binding against ITSELF across its three surfaces.

    The cross-binding checks read the facade's re-export graph alone, so they
    model "exposed in WASM" == "exported from index.ts" and are blind to a
    partial wiring break spread across embind -> the ``SonareModule`` type
    (``sonare.js.d.ts``) -> the facade. This check closes that gap (a function
    registered in embind but missing from the type and/or the facade). Three
    legs:

    1. ACTIVE -- a FREE-function embind registration absent from ``SonareModule``
       (TypeScript cannot call it; a real type/registration break).
    2. ACTIVE -- a facade module calls ``module.X`` / ``requireModule().X`` for a
       name absent from ``SonareModule`` (a TS compile gap).
    3. INFORMATIONAL -- a registered AND typed free function that no facade
       wraps. Often intentional (a raw entry superseded by a richer variant,
       e.g. ``detectKey`` -> ``_detectKeyWithOptions``, or an internal helper the
       facade consumes without re-exporting), so non-gating.

    Locations come from the per-name site the extractor recorded, because both
    the registrations and the facade span dozens of files.
    """

    def _emit(name: str, message: str, location: str, informational: bool) -> None:
        if allow.wasm_internal_ok(name):
            rep.findings.append(
                Finding("wasm_internal", name, "wasm", "", allowlisted=True)
            )
            return
        rep.findings.append(
            Finding(
                category="wasm_internal",
                key=name,
                surface="wasm",
                message=message,
                location=location,
                informational=informational,
            )
        )

    # 1. embind free registration not declared in the SonareModule type.
    for name, site in sorted(wi.embind.items()):
        if name in wi.iface:
            continue
        _emit(
            name,
            f"embind registers free function '{name}' but it is not declared in "
            "the SonareModule interface (TypeScript cannot call it)",
            str(site),
            informational=False,
        )

    # 2. A facade calls module.X for a name the SonareModule type does not declare.
    for name, site in sorted(wi.refs.items()):
        if name in wi.iface:
            continue
        _emit(
            name,
            f"the facade calls module.{name} but it is not declared in the "
            "SonareModule interface",
            str(site),
            informational=False,
        )

    # 3. Registered AND typed, but no facade wraps it (module.X never called).
    for name, site in sorted(wi.embind.items()):
        if name not in wi.iface or name in wi.refs:
            continue
        _emit(
            name,
            f"embind registers free function '{name}' (typed in SonareModule) but "
            f"no facade wraps it (module.{name} is never called)",
            str(site),
            informational=True,
        )


# ---------------------------------------------------------------------------
# 8. Record-shape drift (second extraction unit)
# ---------------------------------------------------------------------------

# Facade surfaces that declare record shapes. The CLI prints values, it does not
# declare record types, so it has nothing to compare.
_RECORD_SURFACES = ("python", "node", "wasm")

# Fields a facade record may carry that no C struct declares, because they are
# the facade's own ergonomic additions rather than mirrored ABI data. Kept as a
# named set rather than allowlist entries: they are a property of the surface
# convention, not of any one record.
_FACADE_ONLY_FIELDS = {
    # JS/TS request-object plumbing: a request record folds the call's control
    # hooks and the validation opt-out in alongside the mirrored config fields.
    "on_progress",
    "cancel",
    "validate",
    "signal",
    "options",
}


# Records that merely SHARE A NAME with a C struct and are not a mirror of it.
# This is the record unit's analogue of ``_ALIAS_COVERAGE``: a statement about
# what matches what, not an exemption from drift. It belongs here rather than in
# ``allowlist.toml`` because the fact being recorded is "these two types are
# unrelated and collide by coincidence" — allowlisting it would instead assert
# "this record legitimately differs from its C counterpart", which is false: it
# has no C counterpart.
#
# Keyed by ``(surface, canonical key, surface-native type name)``. The type name
# is part of the key deliberately: if the facade renames the colliding type, the
# entry stops applying and the record is compared again, rather than silently
# covering whatever takes the name next.
_NOT_A_C_MIRROR = {
    # The offline mastering dynamics processors return an audio envelope
    # (samples + reported latency). The analysis-side SonareDynamicsResult is a
    # loudness/crest measurement block. Same name, unrelated concepts; Node is
    # unaffected because it spells its mastering result differently.
    ("wasm", "dynamics_result", "DynamicsResult"): (
        "offline mastering dynamics output envelope; unrelated to the "
        "analysis-side SonareDynamicsResult measurement block"
    ),
}


def _index_records(ex: Extraction | None) -> dict[str, RecordShape]:
    """Canonical record key -> shape, first declaration wins.

    A shape listed in :data:`_NOT_A_C_MIRROR` is skipped rather than indexed, so
    a genuine mirror declared elsewhere under the same canonical key can still
    take the slot instead of being shadowed by the collision.
    """
    out: dict[str, RecordShape] = {}
    for r in ex.records if ex else []:
        if (r.surface, r.key, r.raw_name) in _NOT_A_C_MIRROR:
            continue
        out.setdefault(r.key, r)
    return out


def _record_drift(extractions, allow, rep: Report, selected) -> None:
    """Compare each facade's declared record shape against its C struct oracle.

    Anchored on the C ABI exactly like the signature checks: for every public
    ``typedef struct { ... } SonareXxx;`` the C field list is the reference, and
    a facade that declares the same record must declare the same fields. Naming
    convention is not drift — every field name is canonicalized to snake_case
    first (``rt60Bands`` and ``rt60_bands`` are one field), and structural
    members with no facade counterpart by design (padding, the length companion
    of a pointer array) are stripped from the C side.

    Three outcomes:

    * ACTIVE -- a C field the facade's record omits (the drift this unit exists
      for: a struct grown on three surfaces and forgotten on the fourth).
    * ACTIVE -- a field the facade's record declares that C does not have, once
      the facade-only ergonomic names are excluded.
    * INFORMATIONAL -- the facade declares no record for this C struct at all,
      but a PEER facade does. Uniform absence is not reported: a C struct no
      facade exposes is a core-exposure question for an audit, not drift.
    """
    indexed = {
        s: _index_records(extractions.get(s))
        for s in ("c", *_RECORD_SURFACES)
        if s in extractions
    }
    for s in ("c", *_RECORD_SURFACES):
        if s in extractions:
            rep.record_counts[s] = len(extractions[s].records)

    c_records = indexed.get("c", {})
    facades = [s for s in _RECORD_SURFACES if s in selected and s in indexed]

    for key in sorted(c_records):
        c_rec = c_records[key]
        c_fields = c_rec.core_field_names()
        if not c_fields:
            continue
        present = [s for s in facades if key in indexed[s]]
        for s in facades:
            if allow.record_ok(key, s):
                rep.findings.append(Finding("record", key, s, "", allowlisted=True))
                continue
            shape = indexed[s].get(key)
            if shape is None:
                if not present:
                    continue  # uniform absence reads as agreement, not drift
                rep.findings.append(
                    Finding(
                        category="record",
                        key=key,
                        surface=s,
                        message=(
                            f"C record '{c_rec.raw_name}' is declared by "
                            f"{', '.join(present)} but not by {s}"
                        ),
                        location=f"{c_rec.file}:{c_rec.line}",
                        detail={"declared_by": present},
                        informational=True,
                    )
                )
                continue
            declared = {f.name for f in shape.fields}
            c_names = {x.name for x in c_rec.fields}
            missing: list[str] = []
            extra: list[str] = []
            # Suppressed names are collected rather than dropped, so the
            # allowlisted total stays auditable per record instead of vanishing
            # before a Finding is ever built.
            suppressed: list[str] = []
            for n in c_fields:
                if n in declared:
                    continue
                (suppressed if allow.record_field_ok(key, n) else missing).append(n)
            extra_allowed = allow.record_extra_ok(key, s)
            for f in shape.core_fields():
                if f.name in c_names or f.name in _FACADE_ONLY_FIELDS:
                    continue
                if extra_allowed or allow.record_field_ok(key, f.name):
                    suppressed.append(f.name)
                else:
                    extra.append(f.name)
            if suppressed:
                rep.findings.append(
                    Finding(
                        category="record",
                        key=key,
                        surface=s,
                        message=(
                            f"record '{shape.raw_name}': {len(suppressed)} "
                            f"allowlisted field divergence(s): {suppressed}"
                        ),
                        detail={"suppressed": suppressed},
                        location=f"{shape.file}:{shape.line}",
                        allowlisted=True,
                    )
                )
            if missing:
                rep.findings.append(
                    Finding(
                        category="record",
                        key=key,
                        surface=s,
                        message=(
                            f"record '{shape.raw_name}' is missing "
                            f"{len(missing)} C field(s) of '{c_rec.raw_name}': "
                            f"{missing}"
                        ),
                        detail={"missing": missing, "c_fields": c_fields},
                        location=f"{shape.file}:{shape.line} "
                        f"(C {c_rec.file}:{c_rec.line})",
                    )
                )
            if extra:
                rep.findings.append(
                    Finding(
                        category="record",
                        key=key,
                        surface=s,
                        message=(
                            f"record '{shape.raw_name}' declares "
                            f"{len(extra)} field(s) absent from C "
                            f"'{c_rec.raw_name}': {extra}"
                        ),
                        detail={"extra": extra, "c_fields": c_fields},
                        location=f"{shape.file}:{shape.line} "
                        f"(C {c_rec.file}:{c_rec.line})",
                    )
                )
