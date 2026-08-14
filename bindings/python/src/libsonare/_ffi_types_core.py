"""ctypes structure and constant definitions for libsonare."""

from __future__ import annotations

import ctypes

# Cancellation callback: int(void* user_data), nonzero requests cancellation.
# Maps to SonareCancelCallback in sonare_c.h.
SonareCancelCallback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)

# --- C structures ---

# SonarePitchCorrectionConfig.target_mode values.
SONARE_PITCH_TARGET_FIXED_MIDI = 0
SONARE_PITCH_TARGET_SCALE = 1


class SonarePitchCorrectionConfig(ctypes.Structure):
    """Maps to SonarePitchCorrectionConfig in sonare_c.h."""

    _fields_ = [
        ("target_mode", ctypes.c_int32),
        ("target_midi", ctypes.c_float),
        ("scale_root", ctypes.c_int32),
        ("scale_mode_mask", ctypes.c_uint32),
        ("scale_reference_midi", ctypes.c_float),
        ("retune_amount", ctypes.c_float),
        ("max_correction_semitones", ctypes.c_float),
        ("retune_speed_ms", ctypes.c_float),
        ("vibrato_threshold_cents", ctypes.c_float),
    ]


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


class SonareOnsetDetectConfig(ctypes.Structure):
    """Maps to SonareOnsetDetectConfig in sonare_c_types_functions.h."""

    _fields_ = [
        ("n_fft", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("threshold", ctypes.c_float),
        ("pre_max", ctypes.c_int32),
        ("post_max", ctypes.c_int32),
        ("pre_avg", ctypes.c_int32),
        ("post_avg", ctypes.c_int32),
        ("delta", ctypes.c_float),
        ("wait", ctypes.c_int32),
        ("backtrack", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 3),
        ("backtrack_range", ctypes.c_int32),
    ]


class SonareTimeSignature(ctypes.Structure):
    """Maps to SonareTimeSignature in sonare_c.h."""

    _fields_ = [
        ("numerator", ctypes.c_int32),
        ("denominator", ctypes.c_int32),
        ("confidence", ctypes.c_float),
    ]


class SonareAnalysisBpmCandidate(ctypes.Structure):
    """Maps to SonareAnalysisBpmCandidate in sonare_c.h."""

    _fields_ = [
        ("value", ctypes.c_float),
        ("confidence", ctypes.c_float),
        ("relation", ctypes.c_int32),
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
        ("bpm_candidates", ctypes.POINTER(SonareAnalysisBpmCandidate)),
        ("bpm_candidate_count", ctypes.c_size_t),
        ("time_signature_candidates", ctypes.POINTER(SonareTimeSignature)),
        ("time_signature_candidate_count", ctypes.c_size_t),
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


class SonareClipPageRequest(ctypes.Structure):
    """Maps to SonareClipPageRequest in sonare_c.h."""

    _fields_ = [
        ("clip_id", ctypes.c_uint32),
        ("channel", ctypes.c_uint32),
        ("sample", ctypes.c_int64),
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


SONARE_METER_MAX_CHANNELS = 8


class SonareMeterTelemetryRecordWide(ctypes.Structure):
    """Maps to SonareMeterTelemetryRecordWide in sonare_c.h."""

    _fields_ = [
        ("target_id", ctypes.c_uint32),
        ("render_frame", ctypes.c_int64),
        ("seq", ctypes.c_uint64),
        ("channel_count", ctypes.c_int32),
        ("peak_db", ctypes.c_float * SONARE_METER_MAX_CHANNELS),
        ("rms_db", ctypes.c_float * SONARE_METER_MAX_CHANNELS),
        ("true_peak_db", ctypes.c_float * SONARE_METER_MAX_CHANNELS),
        ("max_true_peak_db", ctypes.c_float),
        ("correlation", ctypes.c_float),
        ("mono_compat_width", ctypes.c_float),
        ("momentary_lufs", ctypes.c_float),
        ("short_term_lufs", ctypes.c_float),
        ("integrated_lufs", ctypes.c_float),
        ("gain_reduction_db", ctypes.c_float),
        ("dropped_records", ctypes.c_uint32),
    ]


class SonareExternalMidiEvent(ctypes.Structure):
    """Maps to SonareExternalMidiEvent in sonare_c.h."""

    _fields_ = [
        ("destination_id", ctypes.c_uint32),
        ("byte_count", ctypes.c_uint32),
        ("render_frame", ctypes.c_int64),
        ("bytes", ctypes.c_uint8 * 3),
        ("reserved", ctypes.c_uint8 * 5),
    ]


SONARE_SCOPE_MAX_BANDS = 64
SONARE_SCOPE_MAX_POINTS = 32


class SonareScopeTelemetryRecord(ctypes.Structure):
    """Maps to SonareScopeTelemetryRecord in sonare_c.h."""

    _fields_ = [
        ("target_id", ctypes.c_uint32),
        ("render_frame", ctypes.c_int64),
        ("seq", ctypes.c_uint64),
        ("dropped_records", ctypes.c_uint32),
        ("band_count", ctypes.c_uint32),
        ("bands", ctypes.c_float * SONARE_SCOPE_MAX_BANDS),
        ("point_count", ctypes.c_uint32),
        ("points", ctypes.c_float * (SONARE_SCOPE_MAX_POINTS * 2)),
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
        # One-based beat within the bar plus the fractional beat position; see
        # SonareTransportState in sonare_c_types_engine.h.
        ("beat", ctypes.c_int64),
        ("beat_fraction", ctypes.c_double),
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
    """Maps to SonareEngineMarker in sonare_c.h.

    ``kind`` / ``key_fifths`` / ``key_minor`` occupy the 4-byte padding hole
    after ``id``; the mirror MUST keep this exact layout (id@0, kind@4,
    key_fifths@5, key_minor@6, ppq@8, name@16) or ctypes calls segfault.
    """

    _fields_ = [
        ("id", ctypes.c_uint32),
        ("kind", ctypes.c_uint8),
        ("key_fifths", ctypes.c_int8),
        ("key_minor", ctypes.c_uint8),
        ("ppq", ctypes.c_double),
        ("name", ctypes.c_char * 64),
    ]


class SonareProjectMarker(ctypes.Structure):
    """Maps to SonareProjectMarker in sonare_c_project.h.

    Same shape / offsets as :class:`SonareEngineMarker` so one binding shape
    serves both surfaces.
    """

    _fields_ = [
        ("id", ctypes.c_uint32),
        ("kind", ctypes.c_uint8),
        ("key_fifths", ctypes.c_int8),
        ("key_minor", ctypes.c_uint8),
        ("ppq", ctypes.c_double),
        ("name", ctypes.c_char * 64),
    ]


class SonareProjectTrack(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("kind", ctypes.c_uint32),
        ("midi_destination_id", ctypes.c_uint32),
        ("gain", ctypes.c_float),
        ("pan", ctypes.c_float),
        ("mute", ctypes.c_uint8),
        ("solo", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 2),
        ("name", ctypes.c_char * 64),
    ]


class SonareProjectClip(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("track_id", ctypes.c_uint32),
        ("source_id", ctypes.c_uint32),
        ("source_kind", ctypes.c_uint32),
        ("start_ppq", ctypes.c_double),
        ("length_ppq", ctypes.c_double),
        ("source_offset_ppq", ctypes.c_double),
        ("gain", ctypes.c_float),
        ("loop_mode", ctypes.c_uint32),
        ("loop_length_ppq", ctypes.c_double),
    ]


class SonareProjectSource(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("kind", ctypes.c_uint32),
        ("channel_count", ctypes.c_uint32),
        ("storage_handle_id", ctypes.c_uint32),
        ("sample_rate_hint", ctypes.c_double),
        ("name_or_uri", ctypes.c_char * 128),
    ]


class SonareProjectAudioSourceMetadata(ctypes.Structure):
    """Maps to the heap-owned audio-source metadata descriptor."""

    _fields_ = [
        ("content_hash", ctypes.c_char_p),
        ("external_stem_role", ctypes.c_char_p),
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
        ("track_id", ctypes.c_uint32),
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
        ("warp_mode", ctypes.c_int),
        ("warp_anchors", ctypes.c_void_p),
        ("warp_anchor_count", ctypes.c_size_t),
        ("page_provider", ctypes.c_void_p),
    ]


class SonareEngineTrackSend(ctypes.Structure):
    """Maps to SonareEngineTrackSend in sonare_c.h."""

    _fields_ = [
        ("bus_id", ctypes.c_uint32),
        ("level_db", ctypes.c_float),
        ("enabled", ctypes.c_int),
        ("send_timing", ctypes.c_int),
    ]


class SonareEngineTrackLane(ctypes.Structure):
    """Maps to SonareEngineTrackLane in sonare_c.h."""

    _fields_ = [
        ("track_id", ctypes.c_uint32),
        ("sends", ctypes.POINTER(SonareEngineTrackSend)),
        ("send_count", ctypes.c_size_t),
        ("output_bus_id", ctypes.c_uint32),
        ("source_channel_layout", ctypes.c_uint8),
    ]


class SonareEngineBus(ctypes.Structure):
    """Maps to SonareEngineBus in sonare_c.h."""

    _fields_ = [
        ("bus_id", ctypes.c_uint32),
        ("gain_db", ctypes.c_float),
        ("channel_layout", ctypes.c_uint8),
    ]


class SonareEngineWarpAnchor(ctypes.Structure):
    """Maps to SonareEngineWarpAnchor in sonare_c.h."""

    _fields_ = [
        ("warp_sample", ctypes.c_double),
        ("source_sample", ctypes.c_double),
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
        ("source", ctypes.c_int),
        ("record_offset_samples", ctypes.c_int64),
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
