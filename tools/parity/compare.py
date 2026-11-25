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

import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from allowlist import AGREED, DIVERGED, Allowlist
from core_defaults import CoreConfig
from extractors import native_defaults as native_defaults_mod
from extractors.c_api import matrix_dimensions
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
    "coefficients",
    "mfcc_coefficients",
    "mfcc_coeffs",
    "onset_envelope",
    "tempogram",
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
    "live",
    "strip",
    "stream",
    "streaming",
    "realtime",
    "voice",
    "free",
    "master",  # master_audio etc. handled by free-fn detection below if needed
    "project",  # SonareProject handle: project_* ops are Project class methods
    "sample_bank",  # SonareSampleBank handle: sample_bank_* ops are SampleBank methods
    # SonarePolyphonicAnalysis handle: polyphonic_* ops are methods on the analysis
    # object each facade returns.
    "polyphonic",
    # SonarePlaybackRenderer / SonarePlaybackLoudnessMeter handles: playback_* ops
    # are methods on the class each facade returns. The one plain free function
    # sharing the prefix (playback_render_interleaved) is excepted below.
    "playback",
    # SonareHrtfSet handle: hrtf_set_* ops are methods on the HrtfSet class.
    "hrtf",
    "vocal",
)

# Free-function keys that share a handle prefix but ARE plain DSP free functions
# and must stay in the free-function coverage bucket.
HANDLE_PREFIX_FREEFN_EXCEPTIONS = (
    "master_audio",
    "master_audio_stereo",
    "voice_change",
    # Shares the "playback" head token with the renderer/meter handles but takes
    # the audio directly rather than holding a handle -- a plain DSP free
    # function, credited via the render_playback alias in _ALIAS_COVERAGE.
    "playback_render_interleaved",
)

# Full handle-instance prefixes, each paired with the facade class that owns the
# op. A C handle key (e.g. ``mixer_add_bus``) carries the handle type as a leading
# token group; the facades expose the same op as a bare class method
# (``Mixer.add_bus`` -> key ``add_bus``). To credit coverage we strip the longest
# matching prefix and retry the tail against THAT class's methods plus the
# surface's free functions, so an identically named method on an unrelated class
# cannot stand in for a missing one. Ordered LONGEST-FIRST so multi-token prefixes
# win over their single-token shadows (``stream_analyzer_`` before ``stream_``).
# One class name serves every facade: Node, WASM and Python spell these classes
# alike, and ``python_pyi`` flattens each mixin method onto the concrete class.
_HANDLE_FULL_PREFIXES = (
    ("vocal_snapshot_", "VocalRenderSnapshot"),
    ("vocal_session_", "VocalEditSession"),
    ("vocal_draft_", "VocalEditDraft"),
    ("vocal_render_job_", "VocalRenderJob"),
    ("streaming_mastering_chain_", "StreamingMasteringChain"),
    ("streaming_retune_", "StreamingRetune"),
    ("stream_analyzer_", "StreamAnalyzer"),
    ("realtime_voice_changer_", "RealtimeVoiceChanger"),
    ("clip_page_provider_", "ClipPageProvider"),
    ("engine_", "RealtimeEngine"),
    ("live_session_", "LiveSession"),
    ("mixer_", "Mixer"),
    # A strip is addressed through its owning mixer; no facade exposes it as a
    # type of its own, so ``strip_set_gain`` is ``Mixer.setStripGain``'s job.
    ("strip_", "Mixer"),
    ("eq_", "StreamingEqualizer"),
    ("audio_", "Audio"),
    ("sample_bank_", "SampleBank"),
    # SonarePolyphonicAnalysis handle. ``polyphonic_analysis_`` is listed ahead of the
    # shorter token so the destroy entry strips to a bare lifecycle key.
    ("polyphonic_analysis_", "PolyphonicAnalysis"),
    ("polyphonic_", "PolyphonicAnalysis"),
    # SonarePlaybackLoudnessMeter / SonarePlaybackRenderer / SonareHrtfSet handles.
    # ``playback_loudness_meter_`` is listed ahead of ``playback_renderer_`` even
    # though the two never collide, to keep every ``playback_*`` handle grouped.
    ("playback_loudness_meter_", "PlaybackLoudnessMeter"),
    ("playback_renderer_", "PlaybackRenderer"),
    ("hrtf_set_", "HrtfSet"),
    # SonareProject handle: e.g. ``project_split_clip`` -> facade ``Project``
    # method ``split_clip`` / ``splitClip``. Ops renamed on the facade
    # (serialize -> to_json, deserialize -> from_json, create -> ctor) are
    # credited via ``_ALIAS_COVERAGE`` / ``_is_lifecycle_key`` below.
    ("project_", "Project"),
)

# A small number of handle operations are factories on the neighbouring
# handle that owns the returned object. Keep those aliases class-scoped so a
# same-named method on an unrelated class cannot satisfy a missing operation.
# A binding can reach a C entry's capability by calling the core function the
# entry wraps: c_key -> (facade member, core functions). Credited on a surface only
# where the C entry's body and that surface's addon or embind code both call one of
# them (_core_evidence), and listed apart from alias credits because the call skips
# the entry's own validation. Rows keyed on an `_ex` symbol retire with that symbol.
_CORE_COVERAGE: dict[str, tuple[str, tuple[str, ...]]] = {
    "mastering_apply_pair_processor_ex": (
        "mastering_apply_pair_processor",
        ("apply_named_pair_processor",),
    ),
    "mastering_analyze_pair_ex": ("mastering_analyze_pair", ("analyze_named_pair",)),
    "mastering_analyze_pair_stereo": ("mastering_analyze_pair", ("analyze_named_pair",)),
    "mastering_process": ("mastering_normalize_loudness", ("loudness_optimize",)),
}

# Most direct-core variants are reached through the native layer on both JS
# surfaces. A variant can name a narrower route when only one binding bypasses
# the C entry; other surfaces still use the explicit facade alias below.
_CORE_COVERAGE_SURFACES: dict[str, tuple[str, ...]] = {
    "mastering_analyze_pair_stereo": ("wasm",),
}
_CORE_COVERAGE_ARITIES: dict[str, int] = {
    "mastering_analyze_pair_stereo": 10,
}

_HANDLE_ALIAS_OWNERS = {
    "live_audio_devices": "LiveSession",
    "live_midi_sources": "LiveSession",
    "vocal_render_job_begin": "VocalRenderSnapshot",
}


def _is_lifecycle_key(key: str) -> bool:
    """A C op every facade expresses through its OBJECT MODEL, not a free fn.

    Constructors (``*_create`` / ``*_create_json`` / ``*_create_ex``),
    destructors (``*_destroy``) and heap/buffer release helpers (``free_*`` /
    ``*_free`` / ``*_free_*``) have no free-function facade counterpart by design
    — the facades construct via a class constructor and release via GC / RAII.
    These are reported informationally, never gated (a "missing" one is not a
    coverage bug).

    A constructor variant is still a constructor: ``_create_json`` and
    ``_create_ex`` carry the extra arguments as constructor options
    (``loudnessStaticGainDb`` on the streaming mastering chain), and
    ``_create_default`` / ``_create_from_memory`` (the HRTF set's two factories)
    name the source rather than adding an argument -- none of the four was ever
    going to match a free function. Tested by suffix, so the ``_ex`` variant of a
    lifecycle op is gated the moment it is added while its plain sibling never
    is -- which is a family the list has to name, not an entry per instance. A
    substring test (matching any ``_create*``) was considered and rejected: it
    would also swallow a future non-constructor like ``*_create_clip``, silently
    dropping it out of coverage instead of flagging it.
    """
    return (
        key.startswith("free_")
        or key.endswith(
            (
                "_free",
                "_create",
                "_create_json",
                "_create_ex",
                "_create_default",
                "_create_from_memory",
                "_destroy",
            )
        )
        or "_free_" in key
    )


# Explicit alias map for C ops whose capability IS exposed on the facades
# under a DIFFERENT canonical name (an idiomatic rename, NOT an omission). Each
# C key maps to the facade canonical key(s) delivering the same capability;
# coverage is credited when ANY listed alias is present as a method / free
# function on the surface. Kept EXPLICIT (not a fuzzy ``get_``/``set_``/``_json``
# transform) so a getter is never silently matched to a setter — every entry is
# anchored to a verified facade member. canonical_key folds camelCase to snake,
# so one alias serves Python (``data``), Node (``getData``->``get_data``) and
# WASM (``setConfig``->``set_config``) alike.
_ALIAS_COVERAGE = {
    "live_audio_devices": ("audio_devices",),
    "live_midi_sources": ("midi_sources",),
    "has_live_devices": ("capabilities",),
    # Vocal handle methods use descriptive facade names for coordinate
    # conversion and snapshot accessors. The render-job begin operation is a
    # factory on VocalRenderSnapshot, while the remaining render-job methods
    # belong to VocalRenderJob and are matched by the prefix above.
    # The mixer's master-output meter keeps the facade names the WASM Mixer
    # shipped before the C ABI carried it.
    "mixer_configure_master_meter": ("configure_meter",),
    "mixer_master_meter": ("meter_snapshot",),
    "vocal_session_output_length": ("output_length_samples",),
    "vocal_session_source_to_destination": ("source_sample_to_destination_sample",),
    "vocal_session_destination_to_source": ("destination_sample_to_source_sample",),
    "vocal_session_capture_snapshot": ("capture_render_snapshot",),
    "vocal_draft_source_to_destination": ("source_sample_to_destination_sample",),
    "vocal_draft_destination_to_source": ("destination_sample_to_source_sample",),
    "vocal_draft_capture_snapshot": ("capture_render_snapshot",),
    "vocal_snapshot_output_length": ("output_length_samples",),
    "vocal_render_job_begin": ("begin_render_job",),
    # Handle construction: a request-object factory on JS, the class on Python. The create op
    # is a lifecycle key, so Python's constructor needs no credit; restore is the classmethod.
    "vocal_session_create": ("create_vocal_edit_session",),
    "vocal_session_restore": ("restore", "restore_vocal_edit_session"),
    # The loudness helper is the facades' `mastering`; Node and WASM are credited by the core claim.
    "mastering_process": ("mastering",),
    # C ABI returns a JSON document; facades parse it into the public descriptor.
    "capabilities_json": ("capabilities",),
    "capability_catalog_json": ("capability_catalog",),
    "system_info_json": ("system_info",),
    # Accessors exposed as bare properties or get-prefixed getters.
    "audio_data": ("data", "get_data"),
    "audio_length": ("length", "get_length"),
    "audio_duration_sec": ("duration_sec", "get_duration_sec"),
    "audio_sample_rate": ("sample_rate", "get_sample_rate"),
    "engine_get_transport_state": ("transport_state",),
    "engine_drain_meter_telemetry": ("drain_meter_telemetry",),
    "engine_prepare": ("prepare",),
    "engine_bind_midi_cc": ("bind_midi_cc",),
    "project_set_marker": ("set_marker",),
    "engine_render_offline": ("render_offline",),
    "project_add_automation_lane": ("add_automation_lane",),
    "project_edit_automation_lane": ("edit_automation_lane",),
    "stream_analyzer_read_quantized_frames_i16": ("read_frames_i16",),
    "stream_analyzer_read_quantized_frames_u8": ("read_frames_u8",),
    # One EQ match method under the handle's verb.
    "eq_match": ("match",),
    # The assistant's suggestion is one C entry; the facades name it by layout.
    "mastering_assistant_suggest": (
        "mastering_assistant_suggest_chain",
        "mastering_assistant_suggest_chain_stereo",
    ),
    # The diagnosing split returns its report beside the spans.
    "split_silence_common": ("split_silence_common_with_report",),
    "mixer_get_strip_count": ("strip_count",),
    "realtime_voice_changer_get_config": ("config", "config_json", "config_pod"),
    # Whole-object JSON serialize/deserialize -> to_json / from_json.
    "project_serialize": ("to_json",),
    # The C name groups with its handle's other ops so the prefix machinery above
    # can strip it; the facades name the operation rather than the handle, as a
    # free function (analyzePolyphonic) or a class-level one (analyze). Since the
    # prefix path stopped crediting free functions this entry carries the op's
    # coverage on Node and WASM outright, as well as keeping the facade name out
    # of surface-only. Measured: deleting it raises four active findings.
    "polyphonic_analyze": ("analyze_polyphonic", "analyze"),
    "project_deserialize": ("from_json",),
    # Standalone preset validator (the facade reorders the name to lead with the
    # verb): validate_realtime_voice_changer_preset_json.
    "realtime_voice_changer_validate_preset_json": ("validate_realtime_voice_changer_preset_json",),
    # The C entry point returns the estimate as JSON; each facade parses it and
    # exposes the same operation under the unsuffixed name.
    "estimate_meter_json": ("estimate_meter",),
    # The RMS normalizer is the facades' normalize with a mode argument.
    "normalize_rms": ("normalize",),
    # Same fold one channel count up: every facade reaches both statistics
    # through one stereo function with a mode argument.
    "normalize_rms_stereo": ("normalize_stereo",),
    # Same operation, and the two names differ because the two CONTRACTS do: the
    # C entry point reads and writes a config the caller owns, while each facade
    # takes the taste options and returns a finished config. Renaming either to
    # match the other would put a name on it that its own behaviour contradicts.
    "mastering_repair_dereverb_apply_room_estimate": ("mastering_repair_dereverb_config_for_room",),
    # Mono and stereo input layouts are one public pair-analysis operation.
    "mastering_analyze_pair_stereo": ("mastering_analyze_pair", "mastering_pair_analyze"),
    # Correction toward a fixed MIDI target is the time-varying corrector with its
    # default target mode, so every facade reaches it through that one function.
    "pitch_correct_to_midi_timevarying": ("pitch_correct_timevarying",),
    # The C entry can only hand back a string, and every facade parses it into the params mapping
    # masteringPresetParams returns, so the `_json` spelling never appears there.
    "mastering_preset_params_json": ("mastering_preset_params",),
    # The JS facades name the multichannel LUFS meters by layout, not storage.
    "lufs_interleaved": ("lufs_multichannel",),
    "lufs_series_interleaved": ("lufs_series_multichannel",),
    # The JS facades parse the preset document and drop the _json suffix.
    "mixing_scene_preset_json": ("mixing_scene_preset",),
    # Sidechain EQ -> the mono/stereo-specific setters the facades expose.
    "eq_set_sidechain": ("set_sidechain_mono", "set_sidechain_stereo"),
    # The N-channel planar block processor, exposed by channel count for the same
    # reason the sidechain setter above is.
    "eq_process": ("process_mono", "process_stereo"),
    # The CLI namespaces these as `project <verb>` subcommands rather than methods
    # on a handle; the three facades reach them through Project and never consult
    # these entries. No facade declares a free function under any of these names,
    # so the credit cannot drift onto one.
    "project_bounce": ("bounce",),
    "project_export_smf": ("export_smf",),
    "project_import_smf": ("import_smf",),
    # The Project facade presents aggregate descriptors rather than the C ABI's
    # component accessors / name resolver.
    "project_fade_curve_from_name": ("set_clip_fade",),
    "project_marker_name_by_index": ("marker_by_index", "markers"),
    "project_track_name_by_index": ("track_by_index", "tracks"),
    "project_source_name_or_uri_by_index": ("source_by_index", "sources"),
    "project_unresolved_audio_source_count": ("unresolved_audio_source_ids",),
    "project_unresolved_audio_source_id_by_index": ("unresolved_audio_source_ids",),
    "project_unresolved_audio_source_by_index": ("unresolved_audio_sources",),
    # The full URI feeds the ProjectSource builder behind source_by_index and
    # unresolved_audio_sources.
    "project_get_audio_source_uri": ("source_by_index", "sources", "unresolved_audio_sources"),
    # ProjectSource is the facade's aggregate read model; its source-by-index
    # projection includes the owning audio metadata strings for audio sources.
    "project_get_audio_source_metadata": ("source_by_index", "sources"),
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
    # One-shot playback render: the C entry point names the operation
    # (`_interleaved`, its only shape); every facade drops that suffix.
    "playback_render_interleaved": ("render_playback",),
    # PlaybackRenderer accessors: the C ABI hands back a JSON document (a
    # setter takes one); every facade parses/serializes it and exposes the
    # unsuffixed name as a property (Python, Node) or bare method (WASM).
    "playback_renderer_config_json": ("config",),
    "playback_renderer_set_config_json": ("set_config",),
    "playback_renderer_diagnostics_json": ("diagnostics",),
    # Members of a collection the count / by-index rule does not name: the
    # sidecar lookup by name and the parameter descriptors read by index.
    "project_get_assist_sidecar": ("assist_sidecars",),
    "engine_parameter_info_by_index": ("parameters",),
    # The fixed-point latency is read in (fractional) samples.
    "engine_graph_latency_samples_q8": ("graph_latency_samples",),
    # One strip family addressed by a strip reference: a track id or 'master'
    # selects the track or master strip, and buses keep a family of their own.
    # The `_json` / `_by_name` spellings take the object or the named parameter.
    "engine_set_track_strip_json": ("set_strip",),
    "engine_set_master_strip_json": ("set_strip",),
    "engine_set_bus_strip_json": ("set_bus_strip",),
    "engine_set_track_strip_pan": ("set_strip_pan",),
    "engine_set_track_strip_pan_law": ("set_strip_pan_law",),
    "engine_set_track_strip_pan_mode": ("set_strip_pan_mode",),
    "engine_set_track_strip_dual_pan": ("set_strip_dual_pan",),
    "engine_set_track_strip_surround_pan": ("set_strip_surround_pan",),
    "engine_set_track_strip_channel_delay_samples": ("set_strip_channel_delay_samples",),
    "engine_set_bus_strip_pan": ("set_bus_pan",),
    "engine_set_bus_strip_pan_law": ("set_bus_pan_law",),
    "engine_set_bus_strip_pan_mode": ("set_bus_pan_mode",),
    "engine_set_bus_strip_dual_pan": ("set_bus_dual_pan",),
    "engine_set_track_strip_eq_band_json": ("set_strip_eq_band",),
    "engine_set_master_strip_eq_band_json": ("set_strip_eq_band",),
    "engine_set_bus_strip_eq_band_json": ("set_bus_eq_band",),
    "engine_set_track_strip_insert_bypassed": ("set_strip_insert_bypassed",),
    "engine_set_master_strip_insert_bypassed": ("set_strip_insert_bypassed",),
    "engine_set_bus_strip_insert_bypassed": ("set_bus_insert_bypassed",),
    "engine_set_track_strip_insert_param_by_name": ("set_strip_insert_param",),
    "engine_set_master_strip_insert_param_by_name": ("set_strip_insert_param",),
    "engine_set_bus_strip_insert_param_by_name": ("set_bus_insert_param",),
    "engine_apply_track_strip_insert_param_by_name_now": ("apply_strip_insert_param_now",),
    "engine_apply_master_strip_insert_param_by_name_now": ("apply_strip_insert_param_now",),
    "engine_apply_bus_strip_insert_param_by_name_now": ("apply_bus_insert_param_now",),
    "engine_restore_track_strip_insert_param_by_name": ("restore_strip_insert_param",),
    "engine_restore_master_strip_insert_param_by_name": ("restore_strip_insert_param",),
    "engine_restore_bus_strip_insert_param_by_name": ("restore_bus_insert_param",),
    "engine_clear_track_insert_parameter_bases": ("clear_strip_insert_parameter_bases",),
    "engine_clear_master_insert_parameter_bases": ("clear_strip_insert_parameter_bases",),
    "engine_resolve_track_lane_automation_id": ("resolve_strip_automation_id",),
    "engine_resolve_master_automation_id": ("resolve_strip_automation_id",),
    "engine_resolve_track_insert_automation_id": ("resolve_strip_insert_automation_id",),
    "engine_resolve_master_insert_automation_id": ("resolve_strip_insert_automation_id",),
    "engine_set_lane_sidechain": ("set_strip_sidechain",),
    "engine_set_master_sidechain": ("set_strip_sidechain",),
    "engine_can_set_lane_sidechain": ("can_set_strip_sidechain",),
    "engine_can_set_master_sidechain": ("can_set_strip_sidechain",),
    # Solo/mute is addressed by track id; the facade resolves the lane.
    "engine_set_solo_mute": ("set_track_solo_mute",),
    # Capture is configured in one call carrying buffer, punch, source, input
    # monitor and record offset.
    "engine_set_capture_buffer": ("configure_capture",),
    "engine_set_capture_punch": ("configure_capture",),
    "engine_set_capture_source": ("configure_capture",),
    "engine_set_input_monitor": ("configure_capture",),
    "engine_set_record_offset_samples": ("configure_capture",),
    "engine_set_capture_timing": ("configure_capture",),
    # A mixer scene is the mixer's document: the facades read and load it as JSON.
    "mixer_to_scene_json": ("to_json",),
    "mixer_from_scene_json": ("from_json",),
    # The voice changer processes planar channels or writes interleaved audio
    # into a caller buffer; the stereo pair is the two-channel planar case.
    "realtime_voice_changer_process_interleaved": ("process_interleaved_into",),
    "realtime_voice_changer_process_planar_stereo": ("process_planar",),
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
    # The allowlist this report was built against, carrying the record of which
    # of its entries actually suppressed something. Kept on the report so a
    # caller can audit the allowlist without rebuilding the comparison.
    allowlist: Allowlist | None = None
    # Comparisons a check declined to make, as {category, key, surface, reason}.
    # A fold on either side leaves no sequence to diff, and deriving that here is
    # what lets the exclusion expire on its own: the facade going back to
    # positional parameters restores the comparison with no edit anywhere.
    not_compared: list[dict[str, str]] = field(default_factory=list)
    # C entries a surface reaches through the core function the entry wraps, as
    # (key, surface, core function, native call site). Each bypasses the entry's
    # own validation, so the report lists them apart from the C-symbol credits.
    core_reached: list[tuple[str, str, str, str]] = field(default_factory=list)
    # C entries a surface calls from its own native or ctypes layer and exposes through no
    # facade, as (key, surface, call site). Informational coverage, derived on every run.
    binding_consumed: list[tuple[str, str, str]] = field(default_factory=list)
    # _ALIAS_COVERAGE keys that name no C function. A key outliving its function keeps
    # its credit parked on a facade name, and the next C symbol to take it inherits it.
    stale_aliases: list[str] = field(default_factory=list)

    def decline(
        self, category: str, key: str, surface: str, reason: str, location: str = ""
    ) -> None:
        """Record a comparison that could not be made, and why (and where, when known)."""
        row = {"category": category, "key": key, "surface": surface, "reason": reason}
        if location:
            row["location"] = location
        self.not_compared.append(row)

    def comparison_counts(self) -> dict[str, int]:
        """Verdicts reached per finding category, counted by the allowlist.

        The denominator behind every finding count. Without it a category that
        compared nothing reports exactly what a clean one does, and the summary
        line is where a reader stops.
        """
        return dict(self.allowlist.comparisons) if self.allowlist is not None else {}

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


def _handle_prefix(key: str) -> str | None:
    """The handle prefix ``key`` carries, or None. First match wins (longest-first)."""
    for prefix, _cls in _HANDLE_FULL_PREFIXES:
        if key.startswith(prefix) and len(key) > len(prefix):
            return prefix
    return None


def _accessor_forms(tail: str) -> list[str]:
    """Handle-class members that read what a C accessor tail reads.

    A `get_x` accessor is the `x` getter, and both halves of an `x_count` /
    `x_by_index` pair are the `xs` collection. Matched on the handle's own class
    only, like the prefix strip.
    """
    forms = [tail[len("get_") :]] if tail.startswith("get_") else []
    forms += [tail[: -len(s)] + "s" for s in ("_count", "_by_index") if tail.endswith(s)]
    return forms


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


_SEEDER_STRUCT = re.compile(r"^(Sonare\w+)\*$")
# Native code a surface runs: the ctypes modules, the Node addon, the embind TUs.
_NATIVE_SOURCE_GLOBS = {
    "python": ("bindings/python/src/libsonare", ("*.py",)),
    "node": ("bindings/node/src/addon", ("*.cpp", "*.cc", "*.h")),
    "wasm": ("src/wasm", ("*.cpp", "*.h")),
    "c": ("src/c_api", ("*.cpp",)),
    # The TypeScript facades, where a class constructor may perform a handle op.
    "node_facade": ("bindings/node/src", ("*.ts",)),
    "wasm_facade": ("bindings/wasm/src", ("*.ts",)),
}
# Where a surface's class constructors live: TypeScript facades, or the ctypes modules.
_CONSTRUCTOR_SOURCES = {"node": "node_facade", "wasm": "wasm_facade", "python": "python"}


def load_native_sources(root: Path) -> dict[str, list[tuple[str, str]]]:
    """Read each surface's native code as (repo-relative path, text) pairs."""
    out: dict[str, list[tuple[str, str]]] = {}
    for surface, (base, patterns) in _NATIVE_SOURCE_GLOBS.items():
        files = sorted({f for pat in patterns for f in (root / base).rglob(pat)})
        out[surface] = [
            (str(f.relative_to(root)), f.read_text(encoding="utf-8", errors="replace"))
            for f in files
        ]
    return out


# A ctypes signature declaration or an availability probe names a C entry without using it.
_BINDING_DECLARATION = re.compile(r"\.(?:restype|argtypes)\b|\bhasattr\s*\(|#\s*include\b")


def _binding_call_site(key: str, surface: str, sources: list[tuple[str, str]]) -> str | None:
    """Where a surface's own native or ctypes layer uses the C entry ``key``, or None."""
    symbol = f"sonare_{key}"
    use = re.compile(rf"\b{re.escape(symbol)}\b")
    for path, text in sources:
        if symbol not in text:
            continue
        if surface == "python" and Path(path).name.startswith("_ffi_signatures"):
            continue
        for lineno, code in _code_lines(path, text):
            if use.search(code) and not _BINDING_DECLARATION.search(code):
                return f"{path}:{lineno}"
    return None


def _seeder_struct(sig: FunctionSig) -> str | None:
    """The struct a ``*_init`` seeder fills, or None when ``sig`` is not a seeder."""
    if not sig.key.endswith("_init") or len(sig.params) != 1:
        return None
    m = _SEEDER_STRUCT.match(sig.params[0].type.replace(" ", ""))
    return m.group(1) if m else None


def _code_lines(path: str, text: str):
    """(line number, code) with the line comment dropped, so prose is not a call."""
    marker = "#" if path.endswith(".py") else "//"
    for lineno, line in enumerate(text.splitlines(), 1):
        yield lineno, line.split(marker, 1)[0]


def _c_entry_bodies(sources: list[tuple[str, str]]) -> dict[str, str]:
    """C key -> the body of its ``sonare_<key>`` definition in the C-ABI sources."""
    bodies: dict[str, str] = {}
    head = re.compile(r"\bsonare_(\w+)\s*\(")
    for path, text in sources:
        code = "\n".join(line for _, line in _code_lines(path, text))
        for m in head.finditer(code):
            i, depth = m.end() - 1, 0
            while i < len(code):
                depth += (code[i] == "(") - (code[i] == ")")
                i += 1
                if depth == 0:
                    break
            j = i
            while j < len(code) and code[j].isspace():
                j += 1
            if j >= len(code) or code[j] != "{":
                continue
            k, depth = j, 0
            while k < len(code):
                depth += (code[k] == "{") - (code[k] == "}")
                k += 1
                if depth == 0:
                    break
            bodies.setdefault(m.group(1), code[j:k])
    return bodies


def _core_evidence(
    key: str, sources: list[tuple[str, str]], c_body: str | None
) -> tuple[str | None, str | None, str | None]:
    """(core function, native call site, refusal) for a ``_CORE_COVERAGE`` claim."""
    _, core_fns = _CORE_COVERAGE[key]
    expected_arity = _CORE_COVERAGE_ARITIES.get(key)

    def matching_call(text: str, fn: str) -> tuple[re.Match[str] | None, str]:
        code = "\n".join(line for _, line in _code_lines("core.cpp", text))
        call = re.compile(rf"\b{re.escape(fn)}\s*\(")
        for match in call.finditer(code):
            args, _ = native_defaults_mod._call_args(code, match.end() - 1)
            if expected_arity is None or len(args) == expected_arity:
                return match, code
        return None, code

    for fn in core_fns:
        c_match = matching_call(c_body, fn)[0] if c_body is not None else None
        if c_match is None:
            return (
                None,
                None,
                f"core claim '{fn}' for '{key}' refused: the C entry does not call it "
                f"with {expected_arity} arguments"
                if expected_arity is not None
                else f"core claim '{fn}' for '{key}' refused: the C entry does not call it",
            )
        for path, text in sources:
            match, code = matching_call(text, fn)
            if match is not None:
                lineno = code.count("\n", 0, match.start()) + 1
                return fn, f"{path}:{lineno}", None
    return None, None, None


def _balanced(text: str, start: int, open_ch: str, close_ch: str) -> int:
    """Index just past the bracket group opening at ``text[start]``."""
    depth, i = 0, start
    while i < len(text):
        depth += (text[i] == open_ch) - (text[i] == close_ch)
        i += 1
        if depth == 0:
            break
    return i


def _constructor_body(path: str, text: str, cls: str) -> str | None:
    """The body of ``cls``'s constructor (TS ``constructor``, Python ``__init__``)."""
    if f"class {cls}" not in text:
        return None
    if path.endswith(".py"):
        m = re.search(rf"^class {re.escape(cls)}\b.*:$", text, re.MULTILINE)
        init = m and re.compile(r"^(\s+)def __init__\(", re.MULTILINE).search(text, m.end())
        if not init or re.search(r"^\S", text[m.end() : init.start()], re.MULTILINE):
            return None
        indent = len(init.group(1))
        signature_end = _balanced(text, init.end() - 1, "(", ")")
        body = []
        for line in text[signature_end:].splitlines()[1:]:
            if line.strip() and len(line) - len(line.lstrip()) <= indent:
                break
            body.append(line)
        return "\n".join(body)
    m = re.search(rf"\bclass {re.escape(cls)}\b", text)
    if not m:
        return None
    end = re.compile(r"^(?:export )?(?:abstract )?class ", re.MULTILINE).search(text, m.end())
    ctor = re.compile(r"^\s*constructor\(", re.MULTILINE).search(
        text, m.end(), end.start() if end else len(text)
    )
    if not ctor:
        return None
    params_end = _balanced(text, text.index("(", ctor.start()), "(", ")")
    brace = text.find("{", params_end)
    return text[brace : _balanced(text, brace, "{", "}")] if brace != -1 else None


def _constructor_performs(
    key: str, cls: str, tail: str, surface: str, sources: list[tuple[str, str]]
) -> bool:
    """Whether ``cls``'s constructor on ``surface`` performs the handle op ``key``."""
    if surface == "python":
        call = re.compile(rf"\bsonare_{re.escape(key)}\s*\(")
    else:
        head, *rest = tail.split("_")
        call = re.compile(rf"\.{re.escape(head + ''.join(w.title() for w in rest))}\s*\(")
    for path, text in sources:
        body = _constructor_body(path, text, cls)
        if body is not None and any(call.search(code) for _, code in _code_lines(path, body)):
            return True
    return False


def _struct_readers(c_index: dict[str, FunctionSig]) -> dict[str, list[str]]:
    """C entries that read each struct (a ``const Sonare*`` parameter), by struct name."""
    readers: dict[str, list[str]] = {}
    for key, sig in c_index.items():
        for p in sig.params:
            m = re.fullmatch(r"const(Sonare\w+)\*", p.type.replace(" ", ""))
            if m:
                readers.setdefault(m.group(1), []).append(key)
    return readers


def _seeder_evidence(
    key: str, readers: list[str], sources: list[tuple[str, str]]
) -> tuple[bool, str | None]:
    """Whether the native code calls ``sonare_<key>``, and where it first passes the
    struct to an entry that reads it. Comments are dropped so prose is not a call.
    """
    symbol = f"sonare_{key}"
    call = re.compile(rf"\b{re.escape(symbol)}\s*\(")
    read_symbols = tuple(f"sonare_{reader}" for reader in readers)
    read = (
        re.compile(rf"\bsonare_(?:{'|'.join(map(re.escape, readers))})\s*\(") if readers else None
    )
    calls, read_at = False, None
    for path, text in sources:
        if symbol not in text and (
            read_at is not None or not any(reader in text for reader in read_symbols)
        ):
            continue
        for lineno, code in _code_lines(path, text):
            if call.search(code):
                calls = True
            elif read_at is None and read is not None and read.search(code):
                read_at = f"{path}:{lineno}"
    return calls, read_at


def build_report(
    extractions: dict[str, Extraction],
    allow: Allowlist,
    selected: list[str],
    core_configs: dict[str, CoreConfig] | None = None,
    wasm_internal: WasmInternal | None = None,
    native_sources: dict[str, list[tuple[str, str]]] | None = None,
    native_defaults: tuple[list, dict[str, str]] | None = None,
) -> Report:
    rep = Report(surfaces=selected, allowlist=allow)
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
    handle_class_keys = {
        (prefix, s): _class_method_keys(extractions.get(s), cls)
        for prefix, cls in _HANDLE_FULL_PREFIXES
        for s in selected
    }
    free_keys = {s: _free_keys(extractions.get(s)) for s in selected}

    struct_readers = _struct_readers(c_index)
    c_bodies = _c_entry_bodies((native_sources or {}).get("c", []))

    rep.handle_keys = sorted(k for k in c_index if _is_handle_key(k, handle_prefixes))
    rep.stale_aliases = sorted(k for k in _ALIAS_COVERAGE if k not in c_index)

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
            # Handle reachability is class-specific. Without this rule a method
            # on an unrelated class stands in for a missing one on the handle's
            # own class whenever the tails happen to agree.
            prefix = _handle_prefix(key)
            candidate_methods = (
                handle_class_keys[(prefix, s)] if prefix else method_keys.get(s, set())
            )
            present_free = key in indexed.get(s, {})
            present_method = key in candidate_methods
            covered = present_free or present_method
            if not covered:  # noqa: SIM102 -- one chain of attempts with its twin below
                # Handle-instance C key (``mixer_add_bus``): the facade exposes
                # the same op as a bare class method (``Mixer.add_bus`` -> key
                # ``add_bus``), so the handle prefix is stripped there. The tail
                # is retried against the handle CLASS's methods only. A free
                # function sharing the tail is a different capability -- it takes
                # the audio instead of holding it -- and crediting it would let a
                # handle tier ship with no facade method on any surface, which is
                # the one claim the tier makes. An op the facades deliberately
                # expose as a free function is named in ``_ALIAS_COVERAGE`` below.
                if prefix:
                    tail = key[len(prefix) :]
                    covered = tail in candidate_methods or any(
                        form in candidate_methods for form in _accessor_forms(tail)
                    )
            if not covered:
                # Idiomatic rename: the capability is exposed under a different
                # canonical name (verified alias). Credit it when any listed alias
                # is present as a method / free function on this surface. Aliases
                # reach free functions because each entry is a reviewed statement
                # about one name, not a blanket rule.
                aliases = _ALIAS_COVERAGE.get(key)
                alias_methods = candidate_methods
                alias_owner = _HANDLE_ALIAS_OWNERS.get(key)
                if alias_owner is not None:
                    alias_methods = _class_method_keys(extractions.get(s), alias_owner)
                if alias_owner is not None:
                    # A class-scoped alias may still have a direct free export
                    # (WASM's vocalEditAbiVersion is one), but must not fall
                    # back to every class on a surface.
                    alias_symbols = alias_methods | free_keys.get(s, set())
                else:
                    alias_symbols = alias_methods | (
                        free_keys.get(s, set()) if prefix else set(indexed.get(s, {}))
                    )
                # Where a core claim is the route (node / wasm), it decides, so the
                # report keeps saying which surfaces skip the entry's own validation.
                core_route = (
                    key in _CORE_COVERAGE
                    and bool(native_sources)
                    and s in _CORE_COVERAGE_SURFACES.get(key, ("node", "wasm"))
                )
                covered = (
                    bool(aliases) and not core_route and any(a in alias_symbols for a in aliases)
                )
            if not covered and prefix and native_sources and s in _CONSTRUCTOR_SOURCES:
                # A handle op the facade performs while constructing the object
                # (the streaming chain prepares in its constructor) is reached
                # without a method of its own.
                covered = _constructor_performs(
                    key,
                    dict(_HANDLE_FULL_PREFIXES)[prefix],
                    key[len(prefix) :],
                    s,
                    native_sources.get(_CONSTRUCTOR_SOURCES[s], []),
                )
            core_refused = None
            if (
                not covered
                and key in _CORE_COVERAGE
                and native_sources
                and s in _CORE_COVERAGE_SURFACES.get(key, ("node", "wasm"))
            ):
                facade = _CORE_COVERAGE[key][0]
                if facade in indexed.get(s, {}):
                    fn, site, core_refused = _core_evidence(
                        key, native_sources.get(s, []), c_bodies.get(key)
                    )
                    if fn is not None:
                        rep.core_reached.append((key, s, fn, site))
                        continue
            # A seeder is reached through the native code that fills its struct.
            seeder = _seeder_struct(c_index[key]) if native_sources else None
            seeder_read_at = None
            if seeder is not None and not covered and s in native_sources:
                calls, seeder_read_at = _seeder_evidence(
                    key, struct_readers.get(seeder, []), native_sources[s]
                )
                if calls:
                    continue
                if seeder_read_at is None:
                    rep.findings.append(
                        Finding(
                            category="coverage",
                            key=key,
                            surface=s,
                            message=f"seeder '{key}': {s} never passes {seeder} in",
                            location=f"{c_index[key].file}:{c_index[key].line}",
                            informational=True,
                        )
                    )
                    continue
            # Every surface reaches a verdict here: the key is either exposed on
            # this surface or it is not, so coverage never declines a comparison.
            if allow.coverage_ok(
                key,
                s,
                AGREED if covered else DIVERGED,
                f"C '{key}' is not exposed in {s}",
            ):
                rep.findings.append(Finding("coverage", key, s, "", allowlisted=True))
                continue
            if covered:
                continue
            # Constructors / destructors / heap-release helpers are handled by the
            # facade object model (ctor / GC / RAII), not a free function. Report
            # them informationally, never as an active gap.
            is_lifecycle = _is_lifecycle_key(key)
            # An entry the surface's own native or ctypes layer calls, with no facade of its
            # own, is consumed there (a token table read while decoding a result, say). Derived
            # per (key, surface) on every run, so the surface that stops calling it gaps again.
            site = (
                _binding_call_site(key, s, native_sources.get(s, []))
                if native_sources and s in _CONSTRUCTOR_SOURCES and not is_lifecycle
                else None
            )
            if site is not None:
                rep.binding_consumed.append((key, s, site))
                rep.findings.append(
                    Finding(
                        category="coverage",
                        key=key,
                        surface=s,
                        message=f"C function '{key}' is consumed by the {s} binding at {site}, "
                        "with no facade of its own",
                        location=site,
                        detail={"consumed_by_binding": site},
                        informational=True,
                    )
                )
                continue
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
            elif core_refused is not None:
                msg = core_refused
            elif seeder_read_at is not None:
                msg = f"{s} passes {seeder} in without calling seeder '{key}', so its struct_size is zero"
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
                    location=seeder_read_at or f"{c_index[key].file}:{c_index[key].line}",
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
            is_c_alias = any(key in aliases for aliases in _ALIAS_COVERAGE.values()) or any(
                key == facade for facade, _ in _CORE_COVERAGE.values()
            )
            if key in c_index or is_c_alias:
                allow.surface_only_ok(key, s, AGREED)
                continue
            rep.surface_only[s].append(key)
            allowlisted = allow.surface_only_ok(
                key, s, DIVERGED, f"{s}-only '{sig.raw_name}' has no C counterpart"
            )
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
    if native_defaults is not None:
        _native_default_drift(native_defaults[0], native_defaults[1], allow, rep)

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
        if n not in roles and n not in _CALLBACK_NAMES and n not in _BUFFER_COMPANION_NAMES
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
        param_names: list[str] = []
        for sig in sigs.values():
            for p in sig.core_params():
                if p.name in roles:
                    continue
                if p.name not in param_names:
                    param_names.append(p.name)
        if len(sigs) < 2:
            rep.decline(
                "default",
                key,
                next(iter(sigs), ""),
                "only one facade exposes the key, so there is no second default",
            )
            continue
        for pname in param_names:
            declared: dict[str, str] = {}
            for s, sig in sigs.items():
                match = next((p for p in sig.core_params() if p.name == pname), None)
                if match is None or match.default is None:
                    continue
                declared[s] = match.default
            # Canonicalize for the distinctness test (enum-member vs camelCase
            # string-union literals fold to a common form). The raw surface
            # spelling is still shown in the message.
            canon = {
                s: canonical_default("none" if pname in _CALLBACK_NAMES and v == "" else v)
                for s, v in declared.items()
            }
            # Collection-sentinel equivalence: when at least one facade spells
            # the default as an empty collection (``[]`` / ``{}``) and another
            # spells it ``None``, both mean "no value" -- fold ``none`` to the
            # empty-collection form so they compare equal.
            if any(is_empty_collection_default(v) for v in declared.values()):
                for s, v in declared.items():
                    if canon[s] == "none" or is_empty_collection_default(v):
                        canon[s] = "\0empty-collection\0"
            # None beside a concrete value is absence: the library resolves it, so it is no second value.
            if len(set(canon.values()) - {"none"}) and "none" in canon.values():
                for s in [s for s, c in canon.items() if c == "none"]:
                    rep.decline(
                        "default",
                        f"{key}.{pname}",
                        s,
                        "the facade spells its default as absence, which resolves "
                        "to the library's own value",
                    )
                    del declared[s], canon[s]
            if len(declared) < 2:
                rep.decline(
                    "default",
                    f"{key}.{pname}",
                    "cross",
                    "fewer than two facades spell a default for this parameter",
                )
                continue
            distinct = set(canon.values())
            message = f"default drift for '{key}.{pname}': " + ", ".join(
                f"{s}={declared[s]}" for s in sorted(declared)
            )
            verdict = DIVERGED if len(distinct) > 1 else AGREED
            if allow.default_ok(key, pname, verdict, message) or verdict == AGREED:
                continue
            rep.findings.append(
                Finding(
                    category="default",
                    key=key,
                    surface="cross",
                    message=message,
                    detail={"param": pname, "defaults": declared},
                    location="; ".join(
                        f"{s}={sigs[s].file}:{sigs[s].line}" for s in sorted(declared)
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
        "tuning_cents",
    ),
    "mel_spectrogram": ("fmin_hz", "fmax_hz", "htk"),
    "mfcc": ("fmin", "fmax", "htk", "lifter"),
    "chroma_cens": ("bins_per_octave",),
    "chroma_cqt": ("bins_per_octave",),
    "mfcc_to_mel": ("lifter",),
    "mfcc_to_audio": ("htk", "lifter"),
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


def _order_not_comparable(c_cfg: list[str], s_cfg: list[str]) -> str | None:
    """Why this pair carries no order to diff, or None when it does.

    Either side can fold its order away entirely, and a fold leaves no sequence
    to compare rather than a sequence that matches.
    """
    # When C itemizes no config params, or its only config param is an opaque
    # struct/options pointer, the facade flattening it is the bag convention, not
    # order drift. (The itemized C order lives in a `_with_options` variant,
    # compared on its own key.)
    if not c_cfg or all(n in _STRUCT_BAG_NAMES for n in c_cfg):
        return "C itemizes no config parameter order to compare against"
    # Facade may also flatten/fold into a bag: a facade whose config is a subset
    # bag (all names are struct/bag names) carries no order signal.
    if all(n in _STRUCT_BAG_NAMES for n in s_cfg):
        return "the facade takes a request object / options bag, which has no order"
    return None


def _order_verdict(key: str, c_cfg: list[str], s_cfg: list[str]) -> str:
    """Whether one facade's config order matches C's, once both itemize one."""
    # A per-call JSON argument is `<root>_json` in C and `<root>` on a facade (json-input-kinds).
    c_cfg = [n.removesuffix("_json") or n for n in c_cfg]
    # A trailing versioned C config struct is commonly flattened into facade
    # keyword/request fields.  The positional prefix still has to match exactly;
    # the struct's interior has no C-level parameter order to compare here.  This
    # is distinct from an arbitrary tail because the canonical final parameter is
    # explicitly an opaque bag.
    if c_cfg[-1] in _STRUCT_BAG_NAMES and s_cfg[: len(c_cfg) - 1] == c_cfg[:-1]:
        return AGREED
    # A config struct anywhere in the C list is the facade's options bag, which has no position.
    c_itemized = [n for n in c_cfg if n not in _STRUCT_BAG_NAMES]
    if len(c_itemized) < len(c_cfg) and s_cfg == c_itemized[: len(s_cfg)]:
        return AGREED
    # Facades fold variadic C scalar tails into an options bag, so a facade
    # exposing a STRICT PREFIX of the C config order is fine.
    if s_cfg == c_cfg[: len(s_cfg)]:
        return AGREED
    # Facades exposed in their EXTENDED (``_ex``) form: C base order followed by
    # the known extra-field tail of the _ex variant.
    tail = _EXTENDED_FIELD_TAILS.get(key)
    if tail is not None and s_cfg[: len(c_cfg)] == c_cfg and tuple(s_cfg[len(c_cfg) :]) == tail:
        return AGREED
    return DIVERGED


def _librosa_mirrors() -> frozenset[str]:
    """Python functions that keep librosa's parameter names (tools/api/librosa_mirrors.toml)."""
    path = Path(__file__).resolve().parents[1] / "api" / "librosa_mirrors.toml"
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    return frozenset(m["name"] for m in data.get("mirror", []))


_LIBROSA_MIRRORS = _librosa_mirrors()


def _mirror_order_verdict(key: str, c_cfg: list[str], s_cfg: list[str]) -> str:
    """A librosa mirror's names are librosa's, so it is held to C's positions only."""
    extended = len(c_cfg) + len(_EXTENDED_FIELD_TAILS.get(key, ()))
    return AGREED if len(s_cfg) <= len(c_cfg) or len(s_cfg) == extended else DIVERGED


def _order_drift(c_index, indexed, allow, rep: Report, selected, roles: set[str]) -> None:
    for key, csig in c_index.items():
        c_cfg = _config_names(csig, roles)
        # A matrix's dimensions carry no order: a facade derives them or passes them where C does.
        dims = matrix_dimensions(csig.params)
        for s in _FACADE_SURFACES:
            if s not in selected or key not in indexed.get(s, {}):
                continue
            ssig = indexed[s][key]
            # A keyword-only parameter is matched by name, so it has no position on either side.
            by_name = {p.name for p in ssig.params if p.keyword_only}
            s_cfg = [n for n in _config_names(ssig, roles) if n not in dims and n not in by_name]
            c_pos = [n for n in c_cfg if n not in by_name]
            declined = _order_not_comparable(c_pos, s_cfg)
            if c_cfg and not c_pos:
                declined = (
                    "the facade takes every C config parameter by keyword, which has no order"
                )
            if declined is not None:
                rep.decline("order", key, s, declined)
                continue
            if s == "python" and key in _LIBROSA_MIRRORS:
                verdict = _mirror_order_verdict(key, c_pos, s_cfg)
            else:
                verdict = _order_verdict(key, c_pos, s_cfg)
            message = f"config param order/name mismatch vs C for '{key}': C={c_pos} {s}={s_cfg}"
            if allow.order_ok(key, s, verdict, f"[{s}] {message}") or verdict == AGREED:
                continue
            rep.findings.append(
                Finding(
                    category="order",
                    key=key,
                    surface=s,
                    message=message,
                    detail={"c": c_pos, s: s_cfg},
                    location=f"{ssig.file}:{ssig.line} (C {csig.file}:{csig.line})",
                )
            )


# Audio-input role names that carry no naming signal across surfaces: the
# buffer-companion length count (C-only, facades derive it from the array) and
# the sample-rate alias (a consistent facade convention, see _SAMPLE_RATE_NAMES).
_INPUT_NOISE_NAMES = _BUFFER_COMPANION_NAMES | _SAMPLE_RATE_NAMES


def _input_names(sig: FunctionSig, roles: set[str]) -> list[str]:
    """Audio-input buffer param names of a signature, wherever they sit.

    Collected the same way as on the C side, so a second buffer behind its
    first one's extents (``x, x_rows, x_cols, y``) is compared too. The
    buffer-companion length and sample-rate alias are dropped so only the
    buffer parameter names (``samples`` / ``left`` / ``right`` / ...) remain.
    """
    return [p.name for p in sig.params if p.name in roles and p.name not in _INPUT_NOISE_NAMES]


def _input_naming(c_index, indexed, allow, rep: Report, selected, roles: set[str]) -> None:
    """Flag when the audio-input params are named inconsistently across surfaces."""
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    keys = set()
    for s in facades:
        keys.update(indexed[s].keys())
    for key in sorted(keys):
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
        # A surface that names no input buffer drops out of the population, and
        # one group left is a spelling, not a comparison.
        if len(groups) < 2:
            # The lone surface that did name one goes in the surface field, so
            # the reason stays one row in the report's per-cause tally.
            rep.decline(
                "input",
                key,
                next(iter(groups), "none"),
                "fewer than two surfaces name an audio-input buffer here",
            )
            continue
        distinct = {tuple(v) for v in groups.values()}
        message = f"audio-input naming differs for '{key}': " + "; ".join(
            f"{s}={groups[s]}" for s in sorted(groups)
        )
        verdict = DIVERGED if len(distinct) > 1 else AGREED
        if allow.input_naming_ok(key, verdict, message) or verdict == AGREED:
            continue
        rep.findings.append(
            Finding(
                category="input",
                key=key,
                surface="cross",
                message=message,
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
        param_names: list[str] = []
        for sig in sigs.values():
            for p in sig.params:
                if p.enum_values and p.name not in param_names:
                    param_names.append(p.name)
        if len(sigs) < 2:
            rep.decline(
                "enum",
                key,
                next(iter(sigs), ""),
                "only one facade exposes the key, so there is no second value-set",
            )
            continue
        for pname in param_names:
            sets: dict[str, tuple[str, ...]] = {}
            for s, sig in sigs.items():
                match = next((p for p in sig.params if p.name == pname and p.enum_values), None)
                if match is not None:
                    sets[s] = match.enum_values
            if len(sets) < 2:
                rep.decline(
                    "enum",
                    f"{key}.{pname}",
                    "cross",
                    "fewer than two facades declare an enum value-set here",
                )
                continue
            distinct = {frozenset(v) for v in sets.values()}
            message = f"enum value-set drift for '{key}.{pname}': " + "; ".join(
                f"{s}={sorted(sets[s])}" for s in sorted(sets)
            )
            verdict = DIVERGED if len(distinct) > 1 else AGREED
            if allow.enum_ok(key, pname, verdict, message) or verdict == AGREED:
                continue
            rep.findings.append(
                Finding(
                    category="enum",
                    key=key,
                    surface="cross",
                    message=message,
                    detail={
                        "param": pname,
                        "sets": {s: list(v) for s, v in sets.items()},
                    },
                )
            )


# Facade tokens that name a core enum member differently: the C ABI and every facade
# spell NormType::Inf as `max` (SONARE_NORM_MAX), and the rest by their own names.
_CORE_ENUM_TOKENS = {
    "NormType": {
        "max": canonical_core_default("NormType::Inf"),
        "l1": canonical_core_default("NormType::L1"),
        "l2": canonical_core_default("NormType::L2"),
        "power": canonical_core_default("NormType::Power"),
    },
}


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
                if p.name in roles:
                    continue
                core_def = cfg.core_default_for(p.name)
                # One of the two values does not exist to be compared.
                if p.default is None or core_def is None:
                    rep.decline(
                        "core_default",
                        f"{key}.{p.name}",
                        s,
                        "the facade declares no default"
                        if p.default is None
                        else "the core struct field has no literal initializer",
                    )
                    continue
                # A default spelled as absence (`None`) hands the call the core's
                # own value, so there is no facade default to hold against it.
                if canonical_default(p.default) == "none":
                    rep.decline(
                        "core_default",
                        f"{key}.{p.name}",
                        s,
                        "the facade spells its default as absence, which resolves "
                        "to the core's own value",
                    )
                    continue
                verdict = AGREED
                if "::" in core_def:
                    # Enum-member core default. The facade may spell it as a
                    # member string ('stft') or a bare integer (0). We can fold
                    # the string spelling to compare, but an integer needs an
                    # enum value table we don't carry -- skip the integer case
                    # rather than risk a false positive (the facade-vs-facade
                    # enum-set check already guards enum consistency).
                    facade_canon = canonical_default(p.default)
                    if facade_canon is not None and re.fullmatch(r"-?\d+", facade_canon):
                        rep.decline(
                            "core_default",
                            f"{key}.{p.name}",
                            s,
                            "the facade spells an enum-member core default as an "
                            "integer, which needs an enum value table to compare",
                        )
                        continue
                    enum_name = core_def.split("::", 1)[0]
                    facade_canon = _CORE_ENUM_TOKENS.get(enum_name, {}).get(
                        facade_canon, facade_canon
                    )
                    if facade_canon != canonical_core_default(core_def):
                        verdict = DIVERGED
                elif canonical_default(p.default) != canonical_default(core_def):
                    verdict = DIVERGED
                field_name = cfg.rename.get(p.name, p.name)
                message = (
                    f"core-default drift for '{key}.{p.name}': "
                    f"{s}={p.default} vs C++ {cfg.name}.{field_name}={core_def}"
                )
                if (
                    allow.core_default_ok(key, p.name, verdict, f"[{s}] {message}")
                    or verdict == AGREED
                ):
                    continue
                rep.findings.append(
                    Finding(
                        category="core_default",
                        key=key,
                        surface=s,
                        message=message,
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


def _native_default_drift(native_defaults, constants: dict[str, str], allow, rep: Report) -> None:
    """Flag a native fallback literal that disagrees with the C++ default it stands in for.

    The addon and embind layers substitute a literal when a value is absent, and
    that literal is a second spelling of a default the core already owns. Each
    site is followed to the core struct field or default argument the value
    lands in (``extractors/native_defaults.py``); a site that cannot be followed
    is reported as not compared, with the reason the resolver gave.
    """
    for nd in native_defaults:
        key = f"{nd.function}.{nd.param or '?'}"
        if nd.unresolved:
            where = f" -> {nd.anchor}" if nd.anchor else ""
            rep.decline(
                "native_default",
                f"{key}={nd.literal}{where}",
                nd.surface,
                nd.unresolved,
                f"{nd.file}:{nd.line}",
            )
            continue
        literal = native_defaults_mod.literal_value(nd.literal, constants)
        if literal is None:
            rep.decline(
                "native_default",
                key,
                nd.surface,
                "the fallback is an expression, not a literal"
                if "." not in nd.literal
                else "the fallback reads a core config's own member",
            )
            continue
        if nd.core is None:
            rep.decline("native_default", key, nd.surface, "the core anchor has no literal default")
            continue
        lit = re.sub(r"(?<=\d)[uUlL]+$", "", literal)
        if "::" in nd.core:
            rep.decline(
                "native_default",
                key,
                nd.surface,
                "the core default is an enum member, which the fallback spells as a "
                "selector the resolver cannot map",
            )
            continue
        verdict = (
            AGREED
            if canonical_default(lit) == canonical_default(re.sub(r"(?<=\d)[uUlL]+$", "", nd.core))
            else DIVERGED
        )
        message = (
            f"native fallback drift for '{nd.param}' in {nd.function}: "
            f"{nd.literal} vs C++ {nd.anchor}={nd.core}"
        )
        if allow.native_default_ok(nd.function, nd.param, verdict, message) or verdict == AGREED:
            continue
        rep.findings.append(
            Finding(
                category="native_default",
                key=key,
                surface=nd.surface,
                message=message,
                detail={
                    "param": nd.param,
                    "native": nd.literal,
                    "core": nd.core,
                    "anchor": nd.anchor,
                },
                location=f"{nd.file}:{nd.line}",
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
        if allow.wasm_internal_ok(name, DIVERGED, message):
            rep.findings.append(Finding("wasm_internal", name, "wasm", "", allowlisted=True))
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
            allow.wasm_internal_ok(name, AGREED)
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
            allow.wasm_internal_ok(name, AGREED)
            continue
        _emit(
            name,
            f"the facade calls module.{name} but it is not declared in the SonareModule interface",
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


def _presence_flag_of_optional(name: str, c_names: set[str], optional: set[str]) -> bool:
    """True for a C ``has_<x>`` flag whose ``<x>`` the facade declares optional.

    The facade spells "not measured" as the field's absence, so the flag has no
    member of its own there; a required ``<x>`` leaves the flag unrepresented.
    """
    target = name.removeprefix("has_")
    return target != name and target in c_names and target in optional


def _side_of_declared_planes(name: str, c_rec: RecordShape, declared: set[str]) -> bool:
    """True for a C ``<x>_l`` / ``<x>_r`` scalar mirroring planes 0 and 1 of a declared ``<x>[]``.

    Both sides and the ``<x>`` array must exist in C, so a lone ``_l`` field or a scalar
    ``<x>`` never qualifies; the facade reads the side as an element of the plane array.
    """
    base, _, side = name.rpartition("_")
    if side not in ("l", "r") or base not in declared:
        return False
    types = {f.name: f.type for f in c_rec.fields}
    return f"{base}_l" in types and f"{base}_r" in types and types.get(base, "").endswith("[]")


def _lifted_members(
    name: str, c_rec: RecordShape, declared: set[str], c_by_raw: dict[str, RecordShape]
) -> list[str]:
    """Core fields of a by-value C struct member ``name`` that the facade lifts onto its record.

    Lifted means the facade declares no ``name`` but declares every core field of the
    member's struct; a partial lift is still drift, so it returns nothing.
    """
    if name in declared:
        return []
    member_type = next((f.type for f in c_rec.fields if f.name == name), "")
    nested = c_by_raw.get(member_type)
    members = nested.core_field_names() if nested is not None else []
    return members if members and all(m in declared for m in members) else []


def _record_verdict(c_rec: RecordShape, shape: RecordShape | None) -> str:
    """Whole-record verdict for one facade against its C struct oracle."""
    if shape is None:
        # A peer facade declares it and this one does not.
        return DIVERGED
    c_names = {x.name for x in c_rec.fields}
    declared = {f.name for f in shape.fields}
    if any(n not in declared for n in c_rec.core_field_names()):
        return DIVERGED
    if any(
        f.name not in c_names and f.name not in _FACADE_ONLY_FIELDS for f in shape.core_fields()
    ):
        return DIVERGED
    return AGREED


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
        s: _index_records(extractions.get(s)) for s in ("c", *_RECORD_SURFACES) if s in extractions
    }
    for s in ("c", *_RECORD_SURFACES):
        if s in extractions:
            rep.record_counts[s] = len(extractions[s].records)

    c_records = indexed.get("c", {})
    c_by_raw = {r.raw_name: r for r in c_records.values()}
    facades = [s for s in _RECORD_SURFACES if s in selected and s in indexed]

    for key in sorted(c_records):
        c_rec = c_records[key]
        c_fields = c_rec.core_field_names()
        if not c_fields:
            continue
        present = [s for s in facades if key in indexed[s]]
        for s in facades:
            shape = indexed[s].get(key)
            if shape is None and not present:
                rep.decline(
                    "record",
                    key,
                    s,
                    "no facade declares the record, so there is no shape to hold against C",
                )
                continue
            # Whole-record verdict, read before the field-level entries narrow
            # it: this entry suppresses everything they would have caught.
            verdict = _record_verdict(c_rec, shape)
            if allow.record_ok(key, s, verdict, f"[{s}] record '{c_rec.raw_name}' shape differs"):
                rep.findings.append(Finding("record", key, s, "", allowlisted=True))
                continue
            if shape is None:
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
            optional = {f.name for f in shape.fields if f.optional}
            c_names = {x.name for x in c_rec.fields}
            lifted = {n: _lifted_members(n, c_rec, declared, c_by_raw) for n in c_fields}
            c_names.update(m for members in lifted.values() for m in members)
            missing: list[str] = []
            extra: list[str] = []
            # Suppressed names are collected rather than dropped, so the
            # allowlisted total stays auditable per record instead of vanishing
            # before a Finding is ever built.
            suppressed: list[str] = []
            for n in c_fields:
                if (
                    n in declared
                    or _presence_flag_of_optional(n, c_names, optional)
                    or _side_of_declared_planes(n, c_rec, declared)
                    or lifted[n]
                ):
                    allow.record_field_ok(key, n, AGREED)
                    continue
                div = f"[{s}] {shape.raw_name} declares no '{n}' of {c_rec.raw_name}"
                (suppressed if allow.record_field_ok(key, n, DIVERGED, div) else missing).append(n)
            raw_extra = [
                f.name
                for f in shape.core_fields()
                if f.name not in c_names and f.name not in _FACADE_ONLY_FIELDS
            ]
            extra_allowed = allow.record_extra_ok(
                key,
                s,
                DIVERGED if raw_extra else AGREED,
                f"[{s}] {shape.raw_name} declares beyond {c_rec.raw_name}: {sorted(raw_extra)}",
            )
            for f in shape.core_fields():
                if f.name in c_names or f.name in _FACADE_ONLY_FIELDS:
                    allow.record_field_ok(key, f.name, AGREED)
                    continue
                div = f"[{s}] {shape.raw_name} declares '{f.name}', absent from {c_rec.raw_name}"
                if extra_allowed or allow.record_field_ok(key, f.name, DIVERGED, div):
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
                        location=f"{shape.file}:{shape.line} (C {c_rec.file}:{c_rec.line})",
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
                        location=f"{shape.file}:{shape.line} (C {c_rec.file}:{c_rec.line})",
                    )
                )
