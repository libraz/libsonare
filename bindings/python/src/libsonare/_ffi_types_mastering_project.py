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
SONARE_ERROR_UNKNOWN = 99


__all__ = [name for name in globals() if name.startswith(("Sonare", "SONARE_"))]
