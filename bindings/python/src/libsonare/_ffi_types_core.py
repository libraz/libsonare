"""ctypes structure and constant definitions for libsonare."""

from __future__ import annotations

import ctypes

# --- C structures ---


class SonareKey(ctypes.Structure):
    """Maps to SonareKey in sonare_c.h."""

    _fields_ = [
        ("root", ctypes.c_int32),
        ("mode", ctypes.c_int32),
        ("confidence", ctypes.c_float),
    ]


class SonareKeyCandidate(ctypes.Structure):
    """Maps to SonareKeyCandidate in sonare_c.h."""

    _fields_ = [
        ("key", SonareKey),
        ("correlation", ctypes.c_float),
    ]


class SonareTimeSignature(ctypes.Structure):
    """Maps to SonareTimeSignature in sonare_c.h."""

    _fields_ = [
        ("numerator", ctypes.c_int32),
        ("denominator", ctypes.c_int32),
        ("confidence", ctypes.c_float),
    ]


class SonareAnalysisResult(ctypes.Structure):
    """Maps to SonareAnalysisResult in sonare_c.h."""

    _fields_ = [
        ("bpm", ctypes.c_float),
        ("bpm_confidence", ctypes.c_float),
        ("key", SonareKey),
        ("time_signature", SonareTimeSignature),
        ("beat_times", ctypes.POINTER(ctypes.c_float)),
        ("beat_count", ctypes.c_size_t),
    ]


class SonareEngineTelemetry(ctypes.Structure):
    """Maps to SonareEngineTelemetry in sonare_c.h."""

    _fields_ = [
        ("type", ctypes.c_int32),
        ("error", ctypes.c_int32),
        ("render_frame", ctypes.c_int64),
        ("timeline_sample", ctypes.c_int64),
        ("audible_timeline_sample", ctypes.c_int64),
        ("graph_latency_samples_q8", ctypes.c_int32),
        ("value", ctypes.c_uint32),
    ]


class SonareMeterTelemetryRecord(ctypes.Structure):
    """Maps to SonareMeterTelemetryRecord in sonare_c.h."""

    _fields_ = [
        ("target_id", ctypes.c_uint32),
        ("render_frame", ctypes.c_int64),
        ("seq", ctypes.c_uint64),
        ("peak_db_l", ctypes.c_float),
        ("peak_db_r", ctypes.c_float),
        ("rms_db_l", ctypes.c_float),
        ("rms_db_r", ctypes.c_float),
        ("true_peak_db_l", ctypes.c_float),
        ("true_peak_db_r", ctypes.c_float),
        ("max_true_peak_db", ctypes.c_float),
        ("correlation", ctypes.c_float),
        ("mono_compat_width", ctypes.c_float),
        ("momentary_lufs", ctypes.c_float),
        ("short_term_lufs", ctypes.c_float),
        ("integrated_lufs", ctypes.c_float),
        ("gain_reduction_db", ctypes.c_float),
        ("dropped_records", ctypes.c_uint32),
    ]


class SonareTransportState(ctypes.Structure):
    """Maps to SonareTransportState in sonare_c.h."""

    _fields_ = [
        ("playing", ctypes.c_int),
        ("looping", ctypes.c_int),
        ("render_frame", ctypes.c_int64),
        ("sample_position", ctypes.c_int64),
        ("ppq_position", ctypes.c_double),
        ("bpm", ctypes.c_double),
        ("loop_start_ppq", ctypes.c_double),
        ("loop_end_ppq", ctypes.c_double),
        ("sample_rate", ctypes.c_double),
        # Musical position derived from the tempo map (computed every block).
        # Appended after the original fields to preserve struct layout.
        ("bar_start_ppq", ctypes.c_double),
        ("bar_count", ctypes.c_int64),
        ("time_signature", SonareTimeSignature),
    ]


class SonareParameterInfo(ctypes.Structure):
    """Maps to SonareParameterInfo in sonare_c.h."""

    _fields_ = [
        ("id", ctypes.c_uint32),
        ("name", ctypes.c_char * 64),
        ("unit", ctypes.c_char * 16),
        ("min_value", ctypes.c_float),
        ("max_value", ctypes.c_float),
        ("default_value", ctypes.c_float),
        ("rt_safe", ctypes.c_int),
        ("default_curve", ctypes.c_int),
    ]


class SonareAutomationPoint(ctypes.Structure):
    """Maps to SonareAutomationPoint in sonare_c.h."""

    _fields_ = [
        ("ppq", ctypes.c_double),
        ("value", ctypes.c_float),
        ("curve_to_next", ctypes.c_int),
    ]


class SonareEngineMarker(ctypes.Structure):
    """Maps to SonareEngineMarker in sonare_c.h."""

    _fields_ = [
        ("id", ctypes.c_uint32),
        ("ppq", ctypes.c_double),
        ("name", ctypes.c_char * 64),
    ]


class SonareEngineMetronomeConfig(ctypes.Structure):
    """Maps to SonareEngineMetronomeConfig in sonare_c.h."""

    _fields_ = [
        ("enabled", ctypes.c_int),
        ("beat_gain", ctypes.c_float),
        ("accent_gain", ctypes.c_float),
        ("click_samples", ctypes.c_int),
        ("click_seconds", ctypes.c_double),
    ]


class SonareEngineClip(ctypes.Structure):
    """Maps to SonareEngineClip in sonare_c.h."""

    _fields_ = [
        ("id", ctypes.c_uint32),
        ("channels", ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
        ("num_channels", ctypes.c_int),
        ("num_samples", ctypes.c_int64),
        ("start_ppq", ctypes.c_double),
        ("clip_offset_samples", ctypes.c_int64),
        ("length_samples", ctypes.c_int64),
        ("loop", ctypes.c_int),
        ("gain", ctypes.c_float),
        ("fade_in_samples", ctypes.c_int64),
        ("fade_out_samples", ctypes.c_int64),
    ]


class SonareEngineCaptureBuffer(ctypes.Structure):
    """Maps to SonareEngineCaptureBuffer in sonare_c.h."""

    _fields_ = [
        ("channels", ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
        ("num_channels", ctypes.c_int),
        ("capacity_frames", ctypes.c_int64),
    ]


class SonareEngineCaptureStatus(ctypes.Structure):
    """Maps to SonareEngineCaptureStatus in sonare_c.h."""

    _fields_ = [
        ("captured_frames", ctypes.c_int64),
        ("overflow_count", ctypes.c_uint32),
        ("armed", ctypes.c_int),
        ("punch_enabled", ctypes.c_int),
    ]


class SonareEngineBounceOptions(ctypes.Structure):
    """Maps to SonareEngineBounceOptions in sonare_c.h."""

    _fields_ = [
        ("total_frames", ctypes.c_int64),
        ("block_size", ctypes.c_int),
        ("num_channels", ctypes.c_int),
        ("target_sample_rate", ctypes.c_int),
        ("source_sample_rate", ctypes.c_int),
        ("normalize_lufs", ctypes.c_int),
        ("target_lufs", ctypes.c_float),
        ("dither", ctypes.c_int),
        ("dither_bits", ctypes.c_int),
        ("dither_seed", ctypes.c_uint32),
    ]


class SonareEngineBounceResult(ctypes.Structure):
    """Maps to SonareEngineBounceResult in sonare_c.h."""

    _fields_ = [
        ("interleaved", ctypes.POINTER(ctypes.c_float)),
        ("sample_count", ctypes.c_size_t),
        ("frames", ctypes.c_int64),
        ("num_channels", ctypes.c_int),
        ("sample_rate", ctypes.c_int),
        ("integrated_lufs", ctypes.c_float),
    ]


class SonareEngineFreezeOptions(ctypes.Structure):
    """Maps to SonareEngineFreezeOptions in sonare_c.h."""

    _fields_ = [
        ("total_frames", ctypes.c_int64),
        ("block_size", ctypes.c_int),
        ("num_channels", ctypes.c_int),
        ("clip_id", ctypes.c_uint32),
        ("start_ppq", ctypes.c_double),
        ("gain", ctypes.c_float),
    ]


class SonareEngineFreezeResult(ctypes.Structure):
    """Maps to SonareEngineFreezeResult in sonare_c.h."""

    _fields_ = [
        ("clip_id", ctypes.c_uint32),
        ("frames", ctypes.c_int64),
        ("num_channels", ctypes.c_int),
    ]


class SonareEngineGraphNode(ctypes.Structure):
    """Maps to SonareEngineGraphNode in sonare_c.h."""

    _fields_ = [
        ("id", ctypes.c_char * 64),
        ("type", ctypes.c_int),
        ("gain_db", ctypes.c_float),
        ("num_ports", ctypes.c_int),
    ]


class SonareEngineGraphConnection(ctypes.Structure):
    """Maps to SonareEngineGraphConnection in sonare_c.h."""

    _fields_ = [
        ("source_node", ctypes.c_char * 64),
        ("source_port", ctypes.c_int),
        ("dest_node", ctypes.c_char * 64),
        ("dest_port", ctypes.c_int),
        ("mix", ctypes.c_int),
    ]


class SonareEngineGraphParameterBinding(ctypes.Structure):
    """Maps to SonareEngineGraphParameterBinding in sonare_c.h."""

    _fields_ = [
        ("param_id", ctypes.c_uint32),
        ("node_id", ctypes.c_char * 64),
    ]


class SonareEngineGraphSpec(ctypes.Structure):
    """Maps to SonareEngineGraphSpec in sonare_c.h."""

    _fields_ = [
        ("nodes", ctypes.POINTER(SonareEngineGraphNode)),
        ("node_count", ctypes.c_size_t),
        ("connections", ctypes.POINTER(SonareEngineGraphConnection)),
        ("connection_count", ctypes.c_size_t),
        ("parameter_bindings", ctypes.POINTER(SonareEngineGraphParameterBinding)),
        ("parameter_binding_count", ctypes.c_size_t),
        ("input_node", ctypes.c_char * 64),
        ("output_node", ctypes.c_char * 64),
        ("num_channels", ctypes.c_int),
    ]


__all__ = [name for name in globals() if name.startswith(("Sonare", "SONARE_"))]
