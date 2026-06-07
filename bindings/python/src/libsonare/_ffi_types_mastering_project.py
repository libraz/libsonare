"""ctypes structure and constant definitions for libsonare."""

from __future__ import annotations

import ctypes

from ._ffi_types_core import SonareAutomationPoint


class SonareMasteringConfig(ctypes.Structure):
    """Maps to SonareMasteringConfig in sonare_c.h."""

    _fields_ = [
        ("target_lufs", ctypes.c_float),
        ("ceiling_db", ctypes.c_float),
        ("true_peak_oversample", ctypes.c_int32),
    ]


class SonareMasteringResult(ctypes.Structure):
    """Maps to SonareMasteringResult in sonare_c.h."""

    _fields_ = [
        ("samples", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
        ("input_lufs", ctypes.c_float),
        ("output_lufs", ctypes.c_float),
        ("applied_gain_db", ctypes.c_float),
        ("latency_samples", ctypes.c_int32),
    ]


class SonareMasteringParam(ctypes.Structure):
    """Maps to SonareMasteringParam in sonare_c.h."""

    _fields_ = [
        ("key", ctypes.c_char_p),
        ("value", ctypes.c_double),
    ]


class SonareMasteringStereoResult(ctypes.Structure):
    """Maps to SonareMasteringStereoResult in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
        ("input_lufs", ctypes.c_float),
        ("output_lufs", ctypes.c_float),
        ("applied_gain_db", ctypes.c_float),
        ("latency_samples", ctypes.c_int32),
    ]


class SonareMasteringChainResult(ctypes.Structure):
    """Maps to SonareMasteringChainResult in sonare_c.h."""

    _fields_ = [
        ("samples", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
        ("input_lufs", ctypes.c_float),
        ("output_lufs", ctypes.c_float),
        ("applied_gain_db", ctypes.c_float),
        ("stages", ctypes.POINTER(ctypes.c_char_p)),
        ("stages_count", ctypes.c_size_t),
    ]


class SonareMasteringChainStereoResult(ctypes.Structure):
    """Maps to SonareMasteringChainStereoResult in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
        ("input_lufs", ctypes.c_float),
        ("output_lufs", ctypes.c_float),
        ("applied_gain_db", ctypes.c_float),
        ("stages", ctypes.POINTER(ctypes.c_char_p)),
        ("stages_count", ctypes.c_size_t),
    ]


class SonareStreamingPlatform(ctypes.Structure):
    """Maps to SonareStreamingPlatform in sonare_c.h."""

    _fields_ = [
        ("name", ctypes.c_char_p),
        ("target_lufs", ctypes.c_float),
        ("ceiling_db", ctypes.c_float),
    ]


SONARE_EQ_MAX_BANDS = 24
SONARE_EQ_SPECTRUM_STREAM_CAPACITY = 256
SONARE_EQ_SPECTRUM_PROFILE_BANDS = 16


class SonareEqSnapshot(ctypes.Structure):
    """Maps to SonareEqSnapshot in sonare_c.h."""

    _fields_ = [
        ("pre_left", ctypes.c_float * SONARE_EQ_SPECTRUM_STREAM_CAPACITY),
        ("pre_right", ctypes.c_float * SONARE_EQ_SPECTRUM_STREAM_CAPACITY),
        ("post_left", ctypes.c_float * SONARE_EQ_SPECTRUM_STREAM_CAPACITY),
        ("post_right", ctypes.c_float * SONARE_EQ_SPECTRUM_STREAM_CAPACITY),
        ("pre_count", ctypes.c_size_t),
        ("post_count", ctypes.c_size_t),
        ("band_gain_db", ctypes.c_float * SONARE_EQ_MAX_BANDS),
        ("profile_db", ctypes.c_float * SONARE_EQ_SPECTRUM_PROFILE_BANDS),
        ("last_auto_gain_db", ctypes.c_float),
        ("seq", ctypes.c_uint64),
    ]


# Progress callback: void(float progress, const char* stage, void* user_data).
# Maps to SonareMasteringProgressCallback in sonare_c.h.
SonareMasteringProgressCallback = ctypes.CFUNCTYPE(
    None,
    ctypes.c_float,
    ctypes.c_char_p,
    ctypes.c_void_p,
)


# --- Headless arrangement / DAW project (sonare_c_project.h) ---

# Mirror of SonareProjectTrackKind.
SONARE_TRACK_AUDIO = 0
SONARE_TRACK_MIDI = 1
SONARE_TRACK_AUX = 2


class SonareProjectTrackDesc(ctypes.Structure):
    """Maps to SonareProjectTrackDesc in sonare_c_project.h."""

    _fields_ = [
        ("kind", ctypes.c_int),
        ("name", ctypes.c_char_p),
    ]


class SonareProjectClipDesc(ctypes.Structure):
    """Maps to SonareProjectClipDesc in sonare_c_project.h."""

    _fields_ = [
        ("track_id", ctypes.c_uint32),
        ("is_midi", ctypes.c_int),
        ("start_ppq", ctypes.c_double),
        ("length_ppq", ctypes.c_double),
        ("source_offset_ppq", ctypes.c_double),
        ("gain", ctypes.c_float),
        ("audio_interleaved", ctypes.POINTER(ctypes.c_float)),
        ("audio_frames", ctypes.c_int64),
        ("audio_channels", ctypes.c_int),
        ("audio_sample_rate", ctypes.c_int),
        ("source_uri", ctypes.c_char_p),
    ]


class SonareProjectWarpAnchor(ctypes.Structure):
    """Maps to SonareProjectWarpAnchor in sonare_c_project.h."""

    _fields_ = [
        ("warp_sample", ctypes.c_double),
        ("source_sample", ctypes.c_double),
    ]


class SonareProjectWarpMapDesc(ctypes.Structure):
    """Maps to SonareProjectWarpMapDesc in sonare_c_project.h."""

    _fields_ = [
        ("id", ctypes.c_uint32),
        ("name", ctypes.c_char_p),
        ("anchors", ctypes.POINTER(SonareProjectWarpAnchor)),
        ("anchor_count", ctypes.c_size_t),
    ]


class SonareProjectDiagnostic(ctypes.Structure):
    """Maps to SonareProjectDiagnostic in sonare_c_project.h."""

    _fields_ = [
        ("code", ctypes.c_uint32),
        ("severity", ctypes.c_uint32),
        ("target_id", ctypes.c_uint32),
    ]


class SonareProjectCompileResult(ctypes.Structure):
    """Maps to SonareProjectCompileResult in sonare_c_project.h."""

    _fields_ = [
        ("diagnostics", ctypes.POINTER(SonareProjectDiagnostic)),
        ("diagnostic_count", ctypes.c_size_t),
        ("messages", ctypes.c_char_p),
        ("has_timeline", ctypes.c_int),
    ]


class SonareProjectBounceOptions(ctypes.Structure):
    """Maps to SonareProjectBounceOptions in sonare_c_project.h."""

    _fields_ = [
        ("total_frames", ctypes.c_int64),
        ("block_size", ctypes.c_int),
        ("num_channels", ctypes.c_int),
        ("sample_rate", ctypes.c_int),
        ("instrument_latency_samples", ctypes.c_int),
    ]


class SonareMidiEventPod(ctypes.Structure):
    """Maps to SonareMidiEventPod in sonare_c_project.h."""

    _fields_ = [
        ("ppq", ctypes.c_double),
        ("data0", ctypes.c_uint32),
        ("data1", ctypes.c_uint32),
    ]


class SonareNotePairValidation(ctypes.Structure):
    """Maps to SonareNotePairValidation in sonare_c_project.h."""

    _fields_ = [
        ("ok", ctypes.c_int32),
        ("unmatched_note_ons", ctypes.c_uint32),
        ("unmatched_note_offs", ctypes.c_uint32),
    ]


class SonareMidiRouteConfig(ctypes.Structure):
    """Maps to SonareMidiRouteConfig in sonare_c_project.h (sizeof 16)."""

    _fields_ = [
        ("filter_group", ctypes.c_int),
        ("filter_channel", ctypes.c_int),
        ("remap_channel", ctypes.c_int),
        ("thru", ctypes.c_int),
    ]


class SonareMidiCcBinding(ctypes.Structure):
    """Maps to SonareMidiCcBinding in sonare_c_project.h (sizeof 20).

    The `reserved` uint16 at offset 6 is load-bearing: drop it and every
    field after it misaligns, which segfaults on access.
    """

    _fields_ = [
        ("cc_number", ctypes.c_uint8),
        ("channel", ctypes.c_uint8),
        ("kind", ctypes.c_uint8),
        ("cc_lsb_number", ctypes.c_uint8),
        ("selector_msb", ctypes.c_uint8),
        ("selector_lsb", ctypes.c_uint8),
        ("reserved", ctypes.c_uint16),
        ("param_id", ctypes.c_uint32),
        ("min_value", ctypes.c_float),
        ("max_value", ctypes.c_float),
    ]


class SonareProjectTempoSegment(ctypes.Structure):
    """Maps to SonareProjectTempoSegment in sonare_c_project.h (sizeof 32)."""

    _fields_ = [
        ("start_ppq", ctypes.c_double),
        ("bpm", ctypes.c_double),
        ("start_sample", ctypes.c_double),
        ("end_bpm", ctypes.c_double),
    ]


class SonareProjectTimeSignatureSegment(ctypes.Structure):
    """Maps to SonareProjectTimeSignatureSegment in sonare_c_project.h (sizeof 16)."""

    _fields_ = [
        ("start_ppq", ctypes.c_double),
        ("numerator", ctypes.c_int),
        ("denominator", ctypes.c_int),
    ]


# Built-in synth waveform ordinals (mirror SonareSynthWaveform).
SONARE_SYNTH_WAVEFORM_SINE = 0
SONARE_SYNTH_WAVEFORM_SAW = 1
SONARE_SYNTH_WAVEFORM_SQUARE = 2
SONARE_SYNTH_WAVEFORM_TRIANGLE = 3


class SonareBuiltinSynthConfig(ctypes.Structure):
    """Maps to SonareBuiltinSynthConfig in sonare_c_project.h.

    Every numeric field uses "0 (or non-positive) => sensible default", so a
    zero-initialized config is the default sine patch.
    """

    _fields_ = [
        ("waveform", ctypes.c_int),
        ("gain", ctypes.c_float),
        ("attack_ms", ctypes.c_float),
        ("decay_ms", ctypes.c_float),
        ("sustain", ctypes.c_float),
        ("release_ms", ctypes.c_float),
        ("polyphony", ctypes.c_int),
    ]


class SonareBuiltinInstrumentBinding(ctypes.Structure):
    """Maps to SonareBuiltinInstrumentBinding in sonare_c_project.h."""

    _fields_ = [
        ("destination_id", ctypes.c_uint32),
        ("config", SonareBuiltinSynthConfig),
    ]


# Source backend ordinals (mirror SonareSourceBackend).
SONARE_SOURCE_BACKEND_SYNTH = 0
SONARE_SOURCE_BACKEND_SF2 = 1


class SonareSf2ProgramStatus(ctypes.Structure):
    """Maps to SonareSf2ProgramStatus in sonare_c_project.h."""

    _fields_ = [
        ("channel", ctypes.c_uint8),
        ("program", ctypes.c_uint8),
        ("bank", ctypes.c_uint16),
        ("backend", ctypes.c_int),
        ("preset_name", ctypes.c_char * 64),
    ]


class SonareSf2InstrumentConfig(ctypes.Structure):
    """Maps to SonareSf2InstrumentConfig in sonare_c_project.h.

    Versioned struct: struct_version 0 is treated as the current version 1;
    every other field uses "0 => default".
    """

    _fields_ = [
        ("struct_version", ctypes.c_int),
        ("gain", ctypes.c_float),
        ("polyphony", ctypes.c_int),
    ]


class SonareSf2InstrumentBinding(ctypes.Structure):
    """Maps to SonareSf2InstrumentBinding in sonare_c_project.h."""

    _fields_ = [
        ("destination_id", ctypes.c_uint32),
        ("config", SonareSf2InstrumentConfig),
    ]


# The engine-side SonareEngineSf2InstrumentConfig has the identical layout;
# the SonareSf2InstrumentConfig mirror is reused for both (same convention as
# SonareBuiltinSynthConfig / SonareEngineBuiltinSynthConfig).


class SonareSynthModRouting(ctypes.Structure):
    """Maps to SonareSynthModRouting in sonare_c_types.h."""

    _fields_ = [
        ("source", ctypes.c_int),
        ("destination", ctypes.c_int),
        ("depth", ctypes.c_float),
    ]


SONARE_SYNTH_PATCH_MOD_ROUTINGS = 8
SONARE_SYNTH_PRESET_NAME_MAX = 32


class SonareSynthPatch(ctypes.Structure):
    """Maps to SonareSynthPatch in sonare_c_types.h (struct_version 1).

    Versioned NativeSynth patch: the base is the named ``preset`` (or the
    default subtractive patch when empty) and every non-zero field overrides
    the base ("0 => keep"). Explicit zero numeric overrides are not
    representable in struct_version 1. Enum fields reserve 0 as "keep".
    """

    _fields_ = [
        ("struct_version", ctypes.c_int),
        ("preset", ctypes.c_char * SONARE_SYNTH_PRESET_NAME_MAX),
        ("engine_mode", ctypes.c_int),
        ("waveform", ctypes.c_int),
        ("unison", ctypes.c_int),
        ("detune_cents", ctypes.c_float),
        ("drift_cents", ctypes.c_float),
        ("drive", ctypes.c_float),
        ("filter_model", ctypes.c_int),
        ("filter_output", ctypes.c_int),
        ("cutoff_hz", ctypes.c_float),
        ("resonance_q", ctypes.c_float),
        ("key_track", ctypes.c_float),
        ("env_to_cutoff_cents", ctypes.c_float),
        ("vel_to_cutoff_cents", ctypes.c_float),
        ("amp_attack_ms", ctypes.c_float),
        ("amp_decay_ms", ctypes.c_float),
        ("amp_sustain", ctypes.c_float),
        ("amp_release_ms", ctypes.c_float),
        ("filter_attack_ms", ctypes.c_float),
        ("filter_decay_ms", ctypes.c_float),
        ("filter_sustain", ctypes.c_float),
        ("filter_release_ms", ctypes.c_float),
        ("lfo_rate_hz", ctypes.c_float),
        ("lfo_to_pitch_cents", ctypes.c_float),
        ("lfo2_rate_hz", ctypes.c_float),
        ("glide_ms", ctypes.c_float),
        ("body", ctypes.c_int),
        ("body_mix", ctypes.c_float),
        ("stereo_spread", ctypes.c_float),
        ("num_mod_routings", ctypes.c_int),
        ("mod_routings", SonareSynthModRouting * SONARE_SYNTH_PATCH_MOD_ROUTINGS),
        ("gain", ctypes.c_float),
        ("polyphony", ctypes.c_int),
        ("bus_drive", ctypes.c_float),
    ]


class SonareSynthInstrumentBinding(ctypes.Structure):
    """Maps to SonareSynthInstrumentBinding in sonare_c_project.h."""

    _fields_ = [
        ("destination_id", ctypes.c_uint32),
        ("patch", SonareSynthPatch),
    ]


# External-instrument host shim callbacks (sonare_c_project.h
# SonareInstrumentCallbacks). All run synchronously on the bounce's calling
# thread, so the ctypes callbacks hold the GIL for their duration.
#
# prepare: void(void* user_data, double sample_rate, int max_block_size)
SonareInstrumentPrepareCallback = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.c_double,
    ctypes.c_int,
)
# on_event: void(void* user_data, uint32_t destination_id,
#                const uint32_t* ump_words, int word_count, int64_t render_frame)
SonareInstrumentOnEventCallback = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.c_uint32,
    ctypes.POINTER(ctypes.c_uint32),
    ctypes.c_int,
    ctypes.c_int64,
)
# render: void(void* user_data, float* const* channels,
#              int num_channels, int num_frames)
SonareInstrumentRenderCallback = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
    ctypes.c_int,
    ctypes.c_int,
)


class SonareInstrumentCallbacks(ctypes.Structure):
    """Maps to SonareInstrumentCallbacks in sonare_c_project.h."""

    _fields_ = [
        ("user_data", ctypes.c_void_p),
        ("prepare", SonareInstrumentPrepareCallback),
        ("on_event", SonareInstrumentOnEventCallback),
        ("render", SonareInstrumentRenderCallback),
        ("latency_samples", ctypes.c_int),
        ("tail_samples", ctypes.c_int),
    ]


class SonareInstrumentBinding(ctypes.Structure):
    """Maps to SonareInstrumentBinding in sonare_c_project.h."""

    _fields_ = [
        ("destination_id", ctypes.c_uint32),
        ("callbacks", SonareInstrumentCallbacks),
    ]


# Clip fade-curve ordinals (mirror SonareProjectFadeCurve).
SONARE_FADE_CURVE_LINEAR = 0
SONARE_FADE_CURVE_EQUAL_POWER = 1
SONARE_FADE_CURVE_EXPONENTIAL = 2
SONARE_FADE_CURVE_LOGARITHMIC = 3

# Clip loop-mode ordinals (mirror SonareProjectLoopMode).
SONARE_LOOP_MODE_OFF = 0
SONARE_LOOP_MODE_LOOP = 1


class SonareProjectClipFade(ctypes.Structure):
    """Maps to SonareProjectClipFade in sonare_c_project.h."""

    _fields_ = [
        ("length_ppq", ctypes.c_double),
        ("curve", ctypes.c_uint32),
    ]


class SonareProjectClipTake(ctypes.Structure):
    """Maps to SonareProjectClipTake in sonare_c_project.h."""

    _fields_ = [
        ("id", ctypes.c_uint32),
        ("source_id", ctypes.c_uint32),
        ("source_offset_ppq", ctypes.c_double),
        ("name", ctypes.c_char_p),
    ]


class SonareProjectClipCompSegment(ctypes.Structure):
    """Maps to SonareProjectClipCompSegment in sonare_c_project.h."""

    _fields_ = [
        ("start_ppq", ctypes.c_double),
        ("end_ppq", ctypes.c_double),
        ("take_id", ctypes.c_uint32),
    ]


class SonareProjectLoopRecordingDesc(ctypes.Structure):
    """Maps to SonareProjectLoopRecordingDesc in sonare_c_project.h."""

    _fields_ = [
        ("track_id", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("start_ppq", ctypes.c_double),
        ("loop_length_ppq", ctypes.c_double),
        ("audio_interleaved", ctypes.POINTER(ctypes.c_float)),
        ("audio_frames", ctypes.c_int64),
        ("audio_channels", ctypes.c_int),
        ("audio_sample_rate", ctypes.c_int),
    ]


class SonareAutomationLaneDesc(ctypes.Structure):
    """Maps to SonareAutomationLaneDesc in sonare_c_project.h."""

    _fields_ = [
        ("target_param_id", ctypes.c_uint32),
        ("points", ctypes.POINTER(SonareAutomationPoint)),
        ("point_count", ctypes.c_size_t),
    ]


class SonareProjectKeySegment(ctypes.Structure):
    """Maps to SonareProjectKeySegment in sonare_c_project.h."""

    _fields_ = [
        ("start_ppq", ctypes.c_double),
        ("end_ppq", ctypes.c_double),
        ("tonic_pc", ctypes.c_uint32),
        ("mode", ctypes.c_uint32),
    ]


class SonareProjectChordSymbol(ctypes.Structure):
    """Maps to SonareProjectChordSymbol in sonare_c_project.h."""

    _fields_ = [
        ("start_ppq", ctypes.c_double),
        ("end_ppq", ctypes.c_double),
        ("root_pc", ctypes.c_uint32),
        ("quality", ctypes.c_uint32),
        ("extensions", ctypes.POINTER(ctypes.c_uint8)),
        ("extension_count", ctypes.c_size_t),
        ("slash_bass_pc", ctypes.c_uint32),
        ("roman_numeral", ctypes.c_char_p),
        ("modulation_boundary", ctypes.c_int),
    ]


class SonareProjectAssistSidecar(ctypes.Structure):
    """Maps to SonareProjectAssistSidecar in sonare_c_project.h."""

    _fields_ = [
        ("module_id", ctypes.c_char_p),
        ("schema_version", ctypes.c_uint32),
        ("target_track_id", ctypes.c_uint32),
        ("region_start_ppq", ctypes.c_double),
        ("region_end_ppq", ctypes.c_double),
        ("payload", ctypes.POINTER(ctypes.c_uint8)),
        ("payload_len", ctypes.c_size_t),
    ]


# --- Error codes ---

SONARE_OK = 0
SONARE_ERROR_FILE_NOT_FOUND = 1
SONARE_ERROR_INVALID_FORMAT = 2
SONARE_ERROR_DECODE_FAILED = 3
SONARE_ERROR_INVALID_PARAMETER = 4
SONARE_ERROR_OUT_OF_MEMORY = 5
SONARE_ERROR_NOT_SUPPORTED = 6
SONARE_ERROR_INVALID_STATE = 7
SONARE_ERROR_UNKNOWN = 99


__all__ = [name for name in globals() if name.startswith(("Sonare", "SONARE_"))]
