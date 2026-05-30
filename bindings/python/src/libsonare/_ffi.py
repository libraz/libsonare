"""Low-level ctypes wrapper for libsonare."""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
from pathlib import Path

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


class SonareLufsResult(ctypes.Structure):
    """Maps to SonareLufsResult in sonare_c.h."""

    _fields_ = [
        ("integrated_lufs", ctypes.c_float),
        ("momentary_lufs", ctypes.c_float),
        ("short_term_lufs", ctypes.c_float),
        ("loudness_range", ctypes.c_float),
    ]


class SonareMixMeterSnapshot(ctypes.Structure):
    """Maps to SonareMixMeterSnapshot in sonare_c.h."""

    _fields_ = [
        ("peak_db_l", ctypes.c_float),
        ("peak_db_r", ctypes.c_float),
        ("rms_db_l", ctypes.c_float),
        ("rms_db_r", ctypes.c_float),
        ("correlation", ctypes.c_float),
        ("mono_compat_width", ctypes.c_float),
        ("mono_compat_peak", ctypes.c_float),
        ("mono_compat_side_rms", ctypes.c_float),
        ("likely_mono_compatible", ctypes.c_int),
        ("momentary_lufs", ctypes.c_float),
        ("short_term_lufs", ctypes.c_float),
        ("integrated_lufs", ctypes.c_float),
        ("gain_reduction_db", ctypes.c_float),
        ("true_peak_db_l", ctypes.c_float),
        ("true_peak_db_r", ctypes.c_float),
        ("max_true_peak_db", ctypes.c_float),
        ("seq", ctypes.c_uint64),
    ]


class SonareMixGoniometerPoint(ctypes.Structure):
    """Maps to SonareMixGoniometerPoint in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.c_float),
        ("right", ctypes.c_float),
    ]


class SonareBpmCandidate(ctypes.Structure):
    """Maps to SonareBpmCandidate in sonare_c.h."""

    _fields_ = [
        ("bpm", ctypes.c_float),
        ("confidence", ctypes.c_float),
    ]


class SonareBpmAnalysisResult(ctypes.Structure):
    """Maps to SonareBpmAnalysisResult in sonare_c.h."""

    _fields_ = [
        ("bpm", ctypes.c_float),
        ("confidence", ctypes.c_float),
        ("candidates", ctypes.POINTER(SonareBpmCandidate)),
        ("candidate_count", ctypes.c_size_t),
        ("autocorrelation", ctypes.POINTER(ctypes.c_float)),
        ("autocorrelation_count", ctypes.c_size_t),
        ("tempogram", ctypes.POINTER(ctypes.c_float)),
        ("tempogram_count", ctypes.c_size_t),
    ]


class SonareAcousticResult(ctypes.Structure):
    """Maps to SonareAcousticResult in sonare_c.h."""

    _fields_ = [
        ("rt60", ctypes.c_float),
        ("edt", ctypes.c_float),
        ("c50", ctypes.c_float),
        ("c80", ctypes.c_float),
        ("d50", ctypes.c_float),
        ("rt60_bands", ctypes.POINTER(ctypes.c_float)),
        ("edt_bands", ctypes.POINTER(ctypes.c_float)),
        ("c50_bands", ctypes.POINTER(ctypes.c_float)),
        ("c80_bands", ctypes.POINTER(ctypes.c_float)),
        ("band_count", ctypes.c_size_t),
        ("confidence", ctypes.c_float),
        ("is_blind", ctypes.c_int),
    ]


class SonareRhythmResult(ctypes.Structure):
    """Maps to SonareRhythmResult in sonare_c.h."""

    _fields_ = [
        ("bpm", ctypes.c_float),
        ("time_signature", SonareTimeSignature),
        ("groove_type", ctypes.c_int32),
        ("syncopation", ctypes.c_float),
        ("pattern_regularity", ctypes.c_float),
        ("tempo_stability", ctypes.c_float),
        ("beat_intervals", ctypes.POINTER(ctypes.c_float)),
        ("beat_interval_count", ctypes.c_size_t),
    ]


class SonareDynamicsResult(ctypes.Structure):
    """Maps to SonareDynamicsResult in sonare_c.h."""

    _fields_ = [
        ("dynamic_range_db", ctypes.c_float),
        ("peak_db", ctypes.c_float),
        ("rms_db", ctypes.c_float),
        ("crest_factor", ctypes.c_float),
        ("loudness_range_db", ctypes.c_float),
        ("is_compressed", ctypes.c_int),
        ("loudness_times", ctypes.POINTER(ctypes.c_float)),
        ("loudness_rms_db", ctypes.POINTER(ctypes.c_float)),
        ("loudness_count", ctypes.c_size_t),
    ]


class SonareClippingRegion(ctypes.Structure):
    """Maps to SonareClippingRegion in sonare_c.h."""

    _fields_ = [
        ("start_sample", ctypes.c_size_t),
        ("end_sample", ctypes.c_size_t),
        ("length", ctypes.c_size_t),
        ("peak", ctypes.c_float),
    ]


class SonareClippingResult(ctypes.Structure):
    """Maps to SonareClippingResult in sonare_c.h."""

    _fields_ = [
        ("clipped_samples", ctypes.c_size_t),
        ("clipping_ratio", ctypes.c_float),
        ("max_clipped_peak", ctypes.c_float),
        ("regions", ctypes.POINTER(SonareClippingRegion)),
        ("region_count", ctypes.c_size_t),
    ]


class SonareDynamicRangeResult(ctypes.Structure):
    """Maps to SonareDynamicRangeResult in sonare_c.h."""

    _fields_ = [
        ("dynamic_range_db", ctypes.c_float),
        ("low_percentile_db", ctypes.c_float),
        ("high_percentile_db", ctypes.c_float),
        ("window_rms_db", ctypes.POINTER(ctypes.c_float)),
        ("window_count", ctypes.c_size_t),
    ]


class SonareVectorscopePoint(ctypes.Structure):
    """Maps to SonareVectorscopePoint in sonare_c.h."""

    _fields_ = [
        ("mid", ctypes.c_float),
        ("side", ctypes.c_float),
    ]


class SonareVectorscopeResult(ctypes.Structure):
    """Maps to SonareVectorscopeResult in sonare_c.h."""

    _fields_ = [
        ("points", ctypes.POINTER(SonareVectorscopePoint)),
        ("point_count", ctypes.c_size_t),
    ]


class SonarePhaseScopePoint(ctypes.Structure):
    """Maps to SonarePhaseScopePoint in sonare_c.h."""

    _fields_ = [
        ("mid", ctypes.c_float),
        ("side", ctypes.c_float),
        ("radius", ctypes.c_float),
        ("angle_rad", ctypes.c_float),
    ]


class SonarePhaseScopeResult(ctypes.Structure):
    """Maps to SonarePhaseScopeResult in sonare_c.h."""

    _fields_ = [
        ("points", ctypes.POINTER(SonarePhaseScopePoint)),
        ("point_count", ctypes.c_size_t),
        ("correlation", ctypes.c_float),
        ("average_abs_angle_rad", ctypes.c_float),
        ("max_radius", ctypes.c_float),
    ]


class SonareSpectrumResult(ctypes.Structure):
    """Maps to SonareSpectrumResult in sonare_c.h."""

    _fields_ = [
        ("frequencies", ctypes.POINTER(ctypes.c_float)),
        ("magnitude", ctypes.POINTER(ctypes.c_float)),
        ("power", ctypes.POINTER(ctypes.c_float)),
        ("db", ctypes.POINTER(ctypes.c_float)),
        ("bin_count", ctypes.c_size_t),
        ("n_fft", ctypes.c_int),
        ("sample_rate", ctypes.c_int),
    ]


class SonareDeclickConfig(ctypes.Structure):
    """Maps to SonareDeclickConfig in sonare_c.h."""

    _fields_ = [
        ("threshold", ctypes.c_float),
        ("neighbor_ratio", ctypes.c_float),
        ("max_click_samples", ctypes.c_size_t),
        ("lpc_order", ctypes.c_int),
        ("residual_ratio", ctypes.c_float),
    ]


# SonareDenoiseClassicalConfig.mode values.
SONARE_DENOISE_MODE_LOG_MMSE = 0
SONARE_DENOISE_MODE_MMSE_STSA = 1
SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION = 2

# SonareDenoiseClassicalConfig.noise_estimator values.
SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE = 0
SONARE_DENOISE_NOISE_ESTIMATOR_MCRA = 1
SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA = 2


class SonareDenoiseClassicalConfig(ctypes.Structure):
    """Maps to SonareDenoiseClassicalConfig in sonare_c.h."""

    _fields_ = [
        ("mode", ctypes.c_int),
        ("noise_estimator", ctypes.c_int),
        ("n_fft", ctypes.c_int),
        ("hop_length", ctypes.c_int),
        ("dd_alpha", ctypes.c_float),
        ("gain_floor", ctypes.c_float),
        ("over_subtraction", ctypes.c_float),
        ("spectral_floor", ctypes.c_float),
        ("noise_estimation_quantile", ctypes.c_float),
        ("speech_presence_gain", ctypes.c_int),
        ("gain_smoothing", ctypes.c_int),
    ]


class SonareDeclipConfig(ctypes.Structure):
    """Maps to SonareDeclipConfig in sonare_c.h."""

    _fields_ = [
        ("clip_threshold", ctypes.c_float),
        ("lpc_order", ctypes.c_int),
        ("iterations", ctypes.c_int),
        ("lpc_blend", ctypes.c_float),
    ]


# SonareDecrackleConfig.mode values.
SONARE_DECRACKLE_MODE_MEDIAN = 0
SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE = 1


class SonareDecrackleConfig(ctypes.Structure):
    """Maps to SonareDecrackleConfig in sonare_c.h."""

    _fields_ = [
        ("threshold", ctypes.c_float),
        ("mode", ctypes.c_int),
        ("levels", ctypes.c_int),
    ]


class SonareDehumConfig(ctypes.Structure):
    """Maps to SonareDehumConfig in sonare_c.h."""

    _fields_ = [
        ("fundamental_hz", ctypes.c_float),
        ("harmonics", ctypes.c_int),
        ("q", ctypes.c_float),
        ("adaptive", ctypes.c_int),
        ("search_range_hz", ctypes.c_float),
        ("adaptation", ctypes.c_float),
        ("frame_size", ctypes.c_int),
        ("pll_bandwidth", ctypes.c_float),
    ]


class SonareDereverbClassicalConfig(ctypes.Structure):
    """Maps to SonareDereverbClassicalConfig in sonare_c.h."""

    _fields_ = [
        ("threshold", ctypes.c_float),
        ("attenuation", ctypes.c_float),
        ("n_fft", ctypes.c_int),
        ("hop_length", ctypes.c_int),
        ("t60_sec", ctypes.c_float),
        ("late_delay_ms", ctypes.c_float),
        ("over_subtraction", ctypes.c_float),
        ("spectral_floor", ctypes.c_float),
        ("wpe_enabled", ctypes.c_int),
        ("wpe_iterations", ctypes.c_int),
        ("wpe_taps", ctypes.c_int),
        ("wpe_strength", ctypes.c_float),
    ]


# SonareTrimSilenceConfig.mode values.
SONARE_TRIM_SILENCE_MODE_PEAK = 0
SONARE_TRIM_SILENCE_MODE_LUFS_GATED = 1


class SonareTrimSilenceConfig(ctypes.Structure):
    """Maps to SonareTrimSilenceConfig in sonare_c.h."""

    _fields_ = [
        ("threshold", ctypes.c_float),
        ("padding_samples", ctypes.c_size_t),
        ("mode", ctypes.c_int),
        ("gate_lufs", ctypes.c_float),
        ("window_ms", ctypes.c_float),
    ]


# SonareVoiceCharacterPreset enum (mirrors sonare_c.h ordinals).
SONARE_VC_PRESET_NEUTRAL_MONITOR = 0
SONARE_VC_PRESET_BRIGHT_IDOL = 1
SONARE_VC_PRESET_SOFT_WHISPER = 2
SONARE_VC_PRESET_DEEP_NARRATOR = 3
SONARE_VC_PRESET_ROBOT_MASCOT = 4
SONARE_VC_PRESET_DARK_VILLAIN = 5


class SonareRealtimeVoiceChangerConfig(ctypes.Structure):
    """Flat POD mirror of editing::voice_changer::RealtimeVoiceChangerConfig.

    Layout matches sonare_c.h exactly: 32 float fields + 2 int fields, no
    padding. ``sizeof`` must equal ``34 * sizeof(c_float)``.
    """

    _fields_ = [
        ("input_gain_db", ctypes.c_float),
        ("output_gain_db", ctypes.c_float),
        ("wet_mix", ctypes.c_float),
        ("retune_semitones", ctypes.c_float),
        ("retune_mix", ctypes.c_float),
        ("retune_grain_size", ctypes.c_int),
        ("formant_factor", ctypes.c_float),
        ("formant_amount", ctypes.c_float),
        ("formant_body", ctypes.c_float),
        ("formant_brightness", ctypes.c_float),
        ("formant_nasal", ctypes.c_float),
        ("eq_highpass_hz", ctypes.c_float),
        ("eq_body_db", ctypes.c_float),
        ("eq_presence_db", ctypes.c_float),
        ("eq_air_db", ctypes.c_float),
        ("gate_threshold_db", ctypes.c_float),
        ("gate_attack_ms", ctypes.c_float),
        ("gate_release_ms", ctypes.c_float),
        ("gate_range_db", ctypes.c_float),
        ("compressor_threshold_db", ctypes.c_float),
        ("compressor_ratio", ctypes.c_float),
        ("compressor_attack_ms", ctypes.c_float),
        ("compressor_release_ms", ctypes.c_float),
        ("compressor_makeup_gain_db", ctypes.c_float),
        ("deesser_frequency_hz", ctypes.c_float),
        ("deesser_threshold_db", ctypes.c_float),
        ("deesser_ratio", ctypes.c_float),
        ("deesser_range_db", ctypes.c_float),
        ("reverb_mix", ctypes.c_float),
        ("reverb_time_ms", ctypes.c_float),
        ("reverb_damping", ctypes.c_float),
        ("reverb_seed", ctypes.c_int),
        ("limiter_ceiling_db", ctypes.c_float),
        ("limiter_release_ms", ctypes.c_float),
    ]


class SonareTimbreResult(ctypes.Structure):
    """Maps to SonareTimbreResult in sonare_c.h."""

    _fields_ = [
        ("brightness", ctypes.c_float),
        ("warmth", ctypes.c_float),
        ("density", ctypes.c_float),
        ("roughness", ctypes.c_float),
        ("complexity", ctypes.c_float),
        ("spectral_centroid", ctypes.POINTER(ctypes.c_float)),
        ("spectral_centroid_count", ctypes.c_size_t),
        ("spectral_flatness", ctypes.POINTER(ctypes.c_float)),
        ("spectral_flatness_count", ctypes.c_size_t),
        ("spectral_rolloff", ctypes.POINTER(ctypes.c_float)),
        ("spectral_rolloff_count", ctypes.c_size_t),
    ]


class SonareChord(ctypes.Structure):
    """Maps to SonareChord in sonare_c.h."""

    _fields_ = [
        ("root", ctypes.c_int32),
        ("quality", ctypes.c_int32),
        ("start", ctypes.c_float),
        ("end", ctypes.c_float),
        ("confidence", ctypes.c_float),
        ("bass", ctypes.c_int32),
    ]


class SonareChordAnalysisResult(ctypes.Structure):
    """Maps to SonareChordAnalysisResult in sonare_c.h."""

    _fields_ = [
        ("chords", ctypes.POINTER(SonareChord)),
        ("chord_count", ctypes.c_size_t),
    ]


class SonareChordDetectionOptions(ctypes.Structure):
    """Maps to SonareChordDetectionOptions in sonare_c.h."""

    _fields_ = [
        ("min_duration", ctypes.c_float),
        ("smoothing_window", ctypes.c_float),
        ("threshold", ctypes.c_float),
        ("use_triads_only", ctypes.c_int),
        ("n_fft", ctypes.c_int),
        ("hop_length", ctypes.c_int),
        ("use_beat_sync", ctypes.c_int),
        ("use_hmm", ctypes.c_int),
        ("hmm_beam_width", ctypes.c_int),
        ("use_key_context", ctypes.c_int),
        ("key_root", ctypes.c_int32),
        ("key_mode", ctypes.c_int32),
        ("detect_inversions", ctypes.c_int),
        ("chroma_method", ctypes.c_int),
    ]


class SonareSection(ctypes.Structure):
    """Maps to SonareSection in sonare_c.h."""

    _fields_ = [
        ("type", ctypes.c_int32),
        ("start", ctypes.c_float),
        ("end", ctypes.c_float),
        ("energy_level", ctypes.c_float),
        ("confidence", ctypes.c_float),
    ]


class SonareSectionResult(ctypes.Structure):
    """Maps to SonareSectionResult in sonare_c.h."""

    _fields_ = [
        ("sections", ctypes.POINTER(SonareSection)),
        ("section_count", ctypes.c_size_t),
    ]


class SonareMelodyPoint(ctypes.Structure):
    """Maps to SonareMelodyPoint in sonare_c.h."""

    _fields_ = [
        ("time", ctypes.c_float),
        ("frequency", ctypes.c_float),
        ("confidence", ctypes.c_float),
    ]


class SonareMelodyResult(ctypes.Structure):
    """Maps to SonareMelodyResult in sonare_c.h."""

    _fields_ = [
        ("points", ctypes.POINTER(SonareMelodyPoint)),
        ("point_count", ctypes.c_size_t),
        ("pitch_range_octaves", ctypes.c_float),
        ("pitch_stability", ctypes.c_float),
        ("mean_frequency", ctypes.c_float),
        ("vibrato_rate", ctypes.c_float),
    ]


class SonareCqtResult(ctypes.Structure):
    """Maps to SonareCqtResult in sonare_c.h."""

    _fields_ = [
        ("n_bins", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("magnitude", ctypes.POINTER(ctypes.c_float)),
        ("frequencies", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareInverseResult(ctypes.Structure):
    """Maps to SonareInverseResult in sonare_c.h."""

    _fields_ = [
        ("rows", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("data", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareStreamConfig(ctypes.Structure):
    """Maps to SonareStreamConfig in sonare_c.h."""

    _fields_ = [
        ("sample_rate", ctypes.c_int32),
        ("n_fft", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("n_mels", ctypes.c_int32),
        ("fmin", ctypes.c_float),
        ("fmax", ctypes.c_float),
        ("tuning_ref_hz", ctypes.c_float),
        ("compute_magnitude", ctypes.c_int32),
        ("compute_mel", ctypes.c_int32),
        ("compute_chroma", ctypes.c_int32),
        ("compute_onset", ctypes.c_int32),
        ("compute_spectral", ctypes.c_int32),
        ("emit_every_n_frames", ctypes.c_int32),
        ("magnitude_downsample", ctypes.c_int32),
        ("key_update_interval_sec", ctypes.c_float),
        ("bpm_update_interval_sec", ctypes.c_float),
        ("window", ctypes.c_int32),
        ("output_format", ctypes.c_int32),
    ]


class SonareStreamChordChange(ctypes.Structure):
    _fields_ = [
        ("root", ctypes.c_int32),
        ("quality", ctypes.c_int32),
        ("start_time", ctypes.c_float),
        ("confidence", ctypes.c_float),
    ]


class SonareStreamBarChord(ctypes.Structure):
    _fields_ = [
        ("bar_index", ctypes.c_int32),
        ("root", ctypes.c_int32),
        ("quality", ctypes.c_int32),
        ("start_time", ctypes.c_float),
        ("confidence", ctypes.c_float),
    ]


class SonareStreamPatternScore(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 64),
        ("score", ctypes.c_float),
    ]


class SonareStreamFrames(ctypes.Structure):
    """Maps to SonareStreamFrames in sonare_c.h."""

    _fields_ = [
        ("n_frames", ctypes.c_int32),
        ("n_mels", ctypes.c_int32),
        ("timestamps", ctypes.POINTER(ctypes.c_float)),
        ("mel", ctypes.POINTER(ctypes.c_float)),
        ("chroma", ctypes.POINTER(ctypes.c_float)),
        ("onset_strength", ctypes.POINTER(ctypes.c_float)),
        ("rms_energy", ctypes.POINTER(ctypes.c_float)),
        ("spectral_centroid", ctypes.POINTER(ctypes.c_float)),
        ("spectral_flatness", ctypes.POINTER(ctypes.c_float)),
        ("chord_root", ctypes.POINTER(ctypes.c_int32)),
        ("chord_quality", ctypes.POINTER(ctypes.c_int32)),
        ("chord_confidence", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareStreamFramesU8(ctypes.Structure):
    _fields_ = [
        ("n_frames", ctypes.c_int32),
        ("n_mels", ctypes.c_int32),
        ("timestamps", ctypes.POINTER(ctypes.c_float)),
        ("mel", ctypes.POINTER(ctypes.c_uint8)),
        ("chroma", ctypes.POINTER(ctypes.c_uint8)),
        ("onset_strength", ctypes.POINTER(ctypes.c_uint8)),
        ("rms_energy", ctypes.POINTER(ctypes.c_uint8)),
        ("spectral_centroid", ctypes.POINTER(ctypes.c_uint8)),
        ("spectral_flatness", ctypes.POINTER(ctypes.c_uint8)),
    ]


class SonareStreamFramesI16(ctypes.Structure):
    _fields_ = [
        ("n_frames", ctypes.c_int32),
        ("n_mels", ctypes.c_int32),
        ("timestamps", ctypes.POINTER(ctypes.c_float)),
        ("mel", ctypes.POINTER(ctypes.c_int16)),
        ("chroma", ctypes.POINTER(ctypes.c_int16)),
        ("onset_strength", ctypes.POINTER(ctypes.c_int16)),
        ("rms_energy", ctypes.POINTER(ctypes.c_int16)),
        ("spectral_centroid", ctypes.POINTER(ctypes.c_int16)),
        ("spectral_flatness", ctypes.POINTER(ctypes.c_int16)),
    ]


class SonareStreamStats(ctypes.Structure):
    """Maps to SonareStreamStats in sonare_c.h."""

    _fields_ = [
        ("total_frames", ctypes.c_int32),
        ("total_samples", ctypes.c_size_t),
        ("duration_seconds", ctypes.c_float),
        ("bpm", ctypes.c_float),
        ("bpm_confidence", ctypes.c_float),
        ("bpm_candidate_count", ctypes.c_int32),
        ("key", ctypes.c_int32),
        ("key_minor", ctypes.c_int32),
        ("key_confidence", ctypes.c_float),
        ("chord_root", ctypes.c_int32),
        ("chord_quality", ctypes.c_int32),
        ("chord_confidence", ctypes.c_float),
        ("chord_start_time", ctypes.c_float),
        ("current_bar", ctypes.c_int32),
        ("bar_duration", ctypes.c_float),
        ("chord_progression_count", ctypes.c_size_t),
        ("chord_progression", ctypes.POINTER(SonareStreamChordChange)),
        ("bar_chord_progression_count", ctypes.c_size_t),
        ("bar_chord_progression", ctypes.POINTER(SonareStreamBarChord)),
        ("pattern_length", ctypes.c_int32),
        ("voted_pattern_count", ctypes.c_size_t),
        ("voted_pattern", ctypes.POINTER(SonareStreamBarChord)),
        ("detected_pattern_name", ctypes.c_char * 64),
        ("detected_pattern_score", ctypes.c_float),
        ("all_pattern_scores_count", ctypes.c_size_t),
        ("all_pattern_scores", ctypes.POINTER(SonareStreamPatternScore)),
        ("accumulated_seconds", ctypes.c_float),
        ("used_frames", ctypes.c_int32),
        ("updated", ctypes.c_int32),
    ]


class SonareStftResult(ctypes.Structure):
    """Maps to SonareStftResult in sonare_c.h."""

    _fields_ = [
        ("n_bins", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("n_fft", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("magnitude", ctypes.POINTER(ctypes.c_float)),
        ("power", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareMelResult(ctypes.Structure):
    """Maps to SonareMelResult in sonare_c.h."""

    _fields_ = [
        ("n_mels", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("power", ctypes.POINTER(ctypes.c_float)),
        ("db", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareMfccResult(ctypes.Structure):
    """Maps to SonareMfccResult in sonare_c.h."""

    _fields_ = [
        ("n_mfcc", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("coefficients", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareChromaResult(ctypes.Structure):
    """Maps to SonareChromaResult in sonare_c.h."""

    _fields_ = [
        ("n_chroma", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("features", ctypes.POINTER(ctypes.c_float)),
        ("mean_energy", ctypes.POINTER(ctypes.c_float)),
    ]


class SonarePitchResult(ctypes.Structure):
    """Maps to SonarePitchResult in sonare_c.h."""

    _fields_ = [
        ("n_frames", ctypes.c_int32),
        ("f0", ctypes.POINTER(ctypes.c_float)),
        ("voiced_prob", ctypes.POINTER(ctypes.c_float)),
        ("voiced_flag", ctypes.POINTER(ctypes.c_int32)),
        ("median_f0", ctypes.c_float),
        ("mean_f0", ctypes.c_float),
    ]


class SonareHpssResult(ctypes.Structure):
    """Maps to SonareHpssResult in sonare_c.h."""

    _fields_ = [
        ("harmonic", ctypes.POINTER(ctypes.c_float)),
        ("percussive", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
    ]


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


# --- Error codes ---

SONARE_OK = 0
SONARE_ERROR_FILE_NOT_FOUND = 1
SONARE_ERROR_INVALID_FORMAT = 2
SONARE_ERROR_DECODE_FAILED = 3
SONARE_ERROR_INVALID_PARAMETER = 4
SONARE_ERROR_OUT_OF_MEMORY = 5
SONARE_ERROR_NOT_SUPPORTED = 6
SONARE_ERROR_UNKNOWN = 99


# --- Library discovery ---


def _find_library() -> str:
    """Find the libsonare shared library.

    Search order:
        1. SONARE_LIB_PATH environment variable
        2. Package-adjacent (wheel distribution)
        3. Build directory (development)
        4. System library path
    """
    env_path = os.environ.get("SONARE_LIB_PATH")
    if env_path and Path(env_path).exists():
        return env_path

    pkg_dir = Path(__file__).parent
    # In editable/source checkouts, prefer the freshly built shared library over
    # any package-adjacent copy that may have been left by an older build.
    project_root = pkg_dir.parent.parent.parent.parent
    lib_name = "libsonare.dylib" if platform.system() == "Darwin" else "libsonare.so"
    build_path = project_root / "build" / "lib" / lib_name
    if build_path.exists():
        return str(build_path)

    for name in ("libsonare.dylib", "libsonare.so", "sonare.dll"):
        candidate = pkg_dir / name
        if candidate.exists():
            return str(candidate)

    path = ctypes.util.find_library("sonare")
    if path:
        return path

    raise OSError(
        "libsonare shared library not found. "
        "Set SONARE_LIB_PATH or build with: cmake --build build --parallel"
    )


def load_library(lib_path: str | None = None) -> ctypes.CDLL:
    """Load libsonare and configure function signatures.

    Args:
        lib_path: Explicit path to the shared library. If None, searches
            standard locations.

    Returns:
        Loaded ctypes.CDLL with typed function signatures.

    Raises:
        OSError: If the library cannot be found or loaded.
    """
    path = lib_path or _find_library()
    lib = ctypes.CDLL(path)

    # --- Audio functions ---

    # sonare_audio_from_buffer
    lib.sonare_audio_from_buffer.restype = ctypes.c_int32
    lib.sonare_audio_from_buffer.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    # sonare_audio_from_memory
    lib.sonare_audio_from_memory.restype = ctypes.c_int32
    lib.sonare_audio_from_memory.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    # sonare_audio_from_file
    lib.sonare_audio_from_file.restype = ctypes.c_int32
    lib.sonare_audio_from_file.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    # sonare_audio_free
    lib.sonare_audio_free.restype = None
    lib.sonare_audio_free.argtypes = [ctypes.c_void_p]

    # sonare_audio_data
    lib.sonare_audio_data.restype = ctypes.POINTER(ctypes.c_float)
    lib.sonare_audio_data.argtypes = [ctypes.c_void_p]

    # sonare_audio_length
    lib.sonare_audio_length.restype = ctypes.c_size_t
    lib.sonare_audio_length.argtypes = [ctypes.c_void_p]

    # sonare_audio_sample_rate
    lib.sonare_audio_sample_rate.restype = ctypes.c_int
    lib.sonare_audio_sample_rate.argtypes = [ctypes.c_void_p]

    # sonare_audio_duration
    lib.sonare_audio_duration.restype = ctypes.c_float
    lib.sonare_audio_duration.argtypes = [ctypes.c_void_p]

    lib.sonare_audio_detect_bpm.restype = ctypes.c_int32
    lib.sonare_audio_detect_bpm.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]

    lib.sonare_audio_detect_key.restype = ctypes.c_int32
    lib.sonare_audio_detect_key.argtypes = [ctypes.c_void_p, ctypes.POINTER(SonareKey)]

    lib.sonare_audio_detect_beats.restype = ctypes.c_int32
    lib.sonare_audio_detect_beats.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_audio_detect_downbeats.restype = ctypes.c_int32
    lib.sonare_audio_detect_downbeats.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_audio_detect_onsets.restype = ctypes.c_int32
    lib.sonare_audio_detect_onsets.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_audio_analyze.restype = ctypes.c_int32
    lib.sonare_audio_analyze.argtypes = [ctypes.c_void_p, ctypes.POINTER(SonareAnalysisResult)]

    # --- Quick detection functions ---

    # sonare_detect_bpm
    lib.sonare_detect_bpm.restype = ctypes.c_int32
    lib.sonare_detect_bpm.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
    ]

    # sonare_detect_key
    lib.sonare_detect_key.restype = ctypes.c_int32
    lib.sonare_detect_key.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareKey),
    ]

    # sonare_detect_key_with_options
    lib.sonare_detect_key_with_options.restype = ctypes.c_int32
    lib.sonare_detect_key_with_options.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(SonareKey),
    ]

    lib.sonare_detect_key_with_options_and_modes.restype = ctypes.c_int32
    lib.sonare_detect_key_with_options_and_modes.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.POINTER(SonareKey),
    ]

    lib.sonare_detect_key_with_extended_options.restype = ctypes.c_int32
    lib.sonare_detect_key_with_extended_options.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.c_int32,
        ctypes.c_char_p,
        ctypes.POINTER(SonareKey),
    ]

    lib.sonare_detect_key_candidates.restype = ctypes.c_int32
    lib.sonare_detect_key_candidates.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(SonareKeyCandidate)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_detect_key_candidates_with_modes.restype = ctypes.c_int32
    lib.sonare_detect_key_candidates_with_modes.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(SonareKeyCandidate)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_detect_key_candidates_with_extended_options.restype = ctypes.c_int32
    lib.sonare_detect_key_candidates_with_extended_options.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.c_int32,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.POINTER(SonareKeyCandidate)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_free_key_candidates.restype = None
    lib.sonare_free_key_candidates.argtypes = [ctypes.POINTER(SonareKeyCandidate)]

    # sonare_detect_beats
    lib.sonare_detect_beats.restype = ctypes.c_int32
    lib.sonare_detect_beats.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_detect_downbeats.restype = ctypes.c_int32
    lib.sonare_detect_downbeats.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_detect_onsets
    lib.sonare_detect_onsets.restype = ctypes.c_int32
    lib.sonare_detect_onsets.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Full analysis ---

    # sonare_analyze
    lib.sonare_analyze.restype = ctypes.c_int32
    lib.sonare_analyze.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareAnalysisResult),
    ]

    lib.sonare_analyze_bpm.restype = ctypes.c_int32
    lib.sonare_analyze_bpm.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareBpmAnalysisResult),
    ]

    lib.sonare_analyze_impulse_response.restype = ctypes.c_int32
    lib.sonare_analyze_impulse_response.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareAcousticResult),
    ]

    lib.sonare_detect_acoustic.restype = ctypes.c_int32
    lib.sonare_detect_acoustic.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareAcousticResult),
    ]

    lib.sonare_analyze_rhythm.restype = ctypes.c_int32
    lib.sonare_analyze_rhythm.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareRhythmResult),
    ]

    lib.sonare_analyze_dynamics.restype = ctypes.c_int32
    lib.sonare_analyze_dynamics.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(SonareDynamicsResult),
    ]

    lib.sonare_analyze_timbre.restype = ctypes.c_int32
    lib.sonare_analyze_timbre.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(SonareTimbreResult),
    ]

    lib.sonare_detect_chords.restype = ctypes.c_int32
    lib.sonare_detect_chords.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareChordAnalysisResult),
    ]

    lib.sonare_detect_chords_ex.restype = ctypes.c_int32
    lib.sonare_detect_chords_ex.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareChordDetectionOptions),
        ctypes.POINTER(SonareChordAnalysisResult),
    ]

    if hasattr(lib, "sonare_analyze_sections"):
        lib.sonare_analyze_sections.restype = ctypes.c_int32
        lib.sonare_analyze_sections.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.POINTER(SonareSectionResult),
        ]
        lib.sonare_free_section_result.restype = None
        lib.sonare_free_section_result.argtypes = [ctypes.POINTER(SonareSectionResult)]

    if hasattr(lib, "sonare_analyze_melody"):
        lib.sonare_analyze_melody.restype = ctypes.c_int32
        lib.sonare_analyze_melody.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.POINTER(SonareMelodyResult),
        ]
        lib.sonare_free_melody_result.restype = None
        lib.sonare_free_melody_result.argtypes = [ctypes.POINTER(SonareMelodyResult)]

    if hasattr(lib, "sonare_cqt"):
        lib.sonare_cqt.restype = ctypes.c_int32
        lib.sonare_cqt.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(SonareCqtResult),
        ]
        lib.sonare_vqt.restype = ctypes.c_int32
        lib.sonare_vqt.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.POINTER(SonareCqtResult),
        ]
        lib.sonare_free_cqt_result.restype = None
        lib.sonare_free_cqt_result.argtypes = [ctypes.POINTER(SonareCqtResult)]

    # --- Features - Inverse reconstruction ---

    if hasattr(lib, "sonare_mel_to_stft"):
        lib.sonare_mel_to_stft.restype = ctypes.c_int32
        lib.sonare_mel_to_stft.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.POINTER(SonareInverseResult),
        ]
        lib.sonare_mel_to_audio.restype = ctypes.c_int32
        lib.sonare_mel_to_audio.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_mfcc_to_mel.restype = ctypes.c_int32
        lib.sonare_mfcc_to_mel.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(SonareInverseResult),
        ]
        lib.sonare_mfcc_to_audio.restype = ctypes.c_int32
        lib.sonare_mfcc_to_audio.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_free_inverse_result.restype = None
        lib.sonare_free_inverse_result.argtypes = [ctypes.POINTER(SonareInverseResult)]

    # --- Streaming - StreamAnalyzer ---

    if hasattr(lib, "sonare_stream_analyzer_create"):
        lib.sonare_stream_analyzer_config_default.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_config_default.argtypes = [
            ctypes.POINTER(SonareStreamConfig),
        ]
        lib.sonare_stream_analyzer_create.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_create.argtypes = [
            ctypes.POINTER(SonareStreamConfig),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        lib.sonare_stream_analyzer_destroy.restype = None
        lib.sonare_stream_analyzer_destroy.argtypes = [ctypes.c_void_p]
        lib.sonare_stream_analyzer_process.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_process.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
        ]
        lib.sonare_stream_analyzer_process_with_offset.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_process_with_offset.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_size_t,
        ]
        lib.sonare_stream_analyzer_available_frames.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_available_frames.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_stream_analyzer_read_frames.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_read_frames.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.POINTER(SonareStreamFrames),
        ]
        if hasattr(lib, "sonare_stream_analyzer_read_frames_u8"):
            lib.sonare_stream_analyzer_read_frames_u8.restype = ctypes.c_int32
            lib.sonare_stream_analyzer_read_frames_u8.argtypes = [
                ctypes.c_void_p,
                ctypes.c_size_t,
                ctypes.POINTER(SonareStreamFramesU8),
            ]
        if hasattr(lib, "sonare_stream_analyzer_read_frames_i16"):
            lib.sonare_stream_analyzer_read_frames_i16.restype = ctypes.c_int32
            lib.sonare_stream_analyzer_read_frames_i16.argtypes = [
                ctypes.c_void_p,
                ctypes.c_size_t,
                ctypes.POINTER(SonareStreamFramesI16),
            ]
        lib.sonare_stream_analyzer_reset.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_reset.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        lib.sonare_stream_analyzer_stats.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_stats.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareStreamStats),
        ]
        if hasattr(lib, "sonare_free_stream_stats"):
            lib.sonare_free_stream_stats.restype = None
            lib.sonare_free_stream_stats.argtypes = [ctypes.POINTER(SonareStreamStats)]
        lib.sonare_stream_analyzer_frame_count.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_frame_count.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.sonare_stream_analyzer_current_time.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_current_time.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float),
        ]
        lib.sonare_stream_analyzer_sample_rate.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_sample_rate.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.sonare_stream_analyzer_set_expected_duration.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_set_expected_duration.argtypes = [
            ctypes.c_void_p,
            ctypes.c_float,
        ]
        lib.sonare_stream_analyzer_set_normalization_gain.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_set_normalization_gain.argtypes = [
            ctypes.c_void_p,
            ctypes.c_float,
        ]
        lib.sonare_stream_analyzer_set_tuning_ref_hz.restype = ctypes.c_int32
        lib.sonare_stream_analyzer_set_tuning_ref_hz.argtypes = [
            ctypes.c_void_p,
            ctypes.c_float,
        ]
        lib.sonare_free_stream_frames.restype = None
        lib.sonare_free_stream_frames.argtypes = [ctypes.POINTER(SonareStreamFrames)]
        if hasattr(lib, "sonare_free_stream_frames_u8"):
            lib.sonare_free_stream_frames_u8.restype = None
            lib.sonare_free_stream_frames_u8.argtypes = [ctypes.POINTER(SonareStreamFramesU8)]
        if hasattr(lib, "sonare_free_stream_frames_i16"):
            lib.sonare_free_stream_frames_i16.restype = None
            lib.sonare_free_stream_frames_i16.argtypes = [ctypes.POINTER(SonareStreamFramesI16)]

    # --- Memory management ---

    # sonare_free_floats
    lib.sonare_free_floats.restype = None
    lib.sonare_free_floats.argtypes = [ctypes.POINTER(ctypes.c_float)]

    # sonare_free_result
    lib.sonare_free_result.restype = None
    lib.sonare_free_result.argtypes = [ctypes.POINTER(SonareAnalysisResult)]

    lib.sonare_free_bpm_analysis_result.restype = None
    lib.sonare_free_bpm_analysis_result.argtypes = [ctypes.POINTER(SonareBpmAnalysisResult)]

    lib.sonare_free_acoustic_result.restype = None
    lib.sonare_free_acoustic_result.argtypes = [ctypes.POINTER(SonareAcousticResult)]

    lib.sonare_free_rhythm_result.restype = None
    lib.sonare_free_rhythm_result.argtypes = [ctypes.POINTER(SonareRhythmResult)]

    lib.sonare_free_dynamics_result.restype = None
    lib.sonare_free_dynamics_result.argtypes = [ctypes.POINTER(SonareDynamicsResult)]

    lib.sonare_free_timbre_result.restype = None
    lib.sonare_free_timbre_result.argtypes = [ctypes.POINTER(SonareTimbreResult)]

    lib.sonare_free_chord_analysis_result.restype = None
    lib.sonare_free_chord_analysis_result.argtypes = [ctypes.POINTER(SonareChordAnalysisResult)]

    # --- Error handling ---

    # sonare_error_message
    lib.sonare_error_message.restype = ctypes.c_char_p
    lib.sonare_error_message.argtypes = [ctypes.c_int32]

    # sonare_last_error_message: returns the detailed thread-local message for the
    # most recent error. Returns an empty (but non-NULL) string when nothing has been
    # recorded. Only meaningful when a preceding C API call returned non-OK.
    lib.sonare_last_error_message.restype = ctypes.c_char_p
    lib.sonare_last_error_message.argtypes = []

    # --- Version ---

    # sonare_version
    lib.sonare_version.restype = ctypes.c_char_p
    lib.sonare_version.argtypes = []

    lib.sonare_engine_abi_version.restype = ctypes.c_uint32
    lib.sonare_engine_abi_version.argtypes = []

    lib.sonare_voice_changer_abi_version.restype = ctypes.c_uint32
    lib.sonare_voice_changer_abi_version.argtypes = []

    # sonare_has_ffmpeg_support: returns 1 if the shared library was compiled
    # with FFmpeg-backed decoding for M4A/AAC/FLAC/OGG, 0 otherwise.
    lib.sonare_has_ffmpeg_support.restype = ctypes.c_int
    lib.sonare_has_ffmpeg_support.argtypes = []

    # --- Effects ---

    # sonare_hpss
    lib.sonare_hpss.restype = ctypes.c_int32
    lib.sonare_hpss.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareHpssResult),
    ]

    # sonare_harmonic
    lib.sonare_harmonic.restype = ctypes.c_int32
    lib.sonare_harmonic.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_percussive
    lib.sonare_percussive.restype = ctypes.c_int32
    lib.sonare_percussive.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_time_stretch
    lib.sonare_time_stretch.restype = ctypes.c_int32
    lib.sonare_time_stretch.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_pitch_shift
    lib.sonare_pitch_shift.restype = ctypes.c_int32
    lib.sonare_pitch_shift.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_pitch_correct_to_midi
    lib.sonare_pitch_correct_to_midi.restype = ctypes.c_int32
    lib.sonare_pitch_correct_to_midi.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_note_stretch
    lib.sonare_note_stretch.restype = ctypes.c_int32
    lib.sonare_note_stretch.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_voice_change
    lib.sonare_voice_change.restype = ctypes.c_int32
    lib.sonare_voice_change.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_realtime_voice_changer_create_json.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_create_json.argtypes = [
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    if hasattr(lib, "sonare_realtime_voice_changer_preset_config"):
        lib.sonare_realtime_voice_changer_preset_config.restype = ctypes.c_int32
        lib.sonare_realtime_voice_changer_preset_config.argtypes = [
            ctypes.c_int,
            ctypes.POINTER(SonareRealtimeVoiceChangerConfig),
        ]
    if hasattr(lib, "sonare_realtime_voice_changer_create"):
        lib.sonare_realtime_voice_changer_create.restype = ctypes.c_int32
        lib.sonare_realtime_voice_changer_create.argtypes = [
            ctypes.POINTER(SonareRealtimeVoiceChangerConfig),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_void_p),
        ]
    if hasattr(lib, "sonare_realtime_voice_changer_set_config"):
        lib.sonare_realtime_voice_changer_set_config.restype = ctypes.c_int32
        lib.sonare_realtime_voice_changer_set_config.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareRealtimeVoiceChangerConfig),
        ]
    if hasattr(lib, "sonare_realtime_voice_changer_get_config"):
        lib.sonare_realtime_voice_changer_get_config.restype = ctypes.c_int32
        lib.sonare_realtime_voice_changer_get_config.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareRealtimeVoiceChangerConfig),
        ]
    lib.sonare_realtime_voice_changer_destroy.restype = None
    lib.sonare_realtime_voice_changer_destroy.argtypes = [ctypes.c_void_p]
    lib.sonare_realtime_voice_changer_reset.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_reset.argtypes = [ctypes.c_void_p]
    lib.sonare_realtime_voice_changer_set_config_json.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_set_config_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.sonare_realtime_voice_changer_process_mono.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_process_mono.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
    ]
    lib.sonare_realtime_voice_changer_process_interleaved.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_process_interleaved.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
    ]
    lib.sonare_realtime_voice_changer_latency_samples.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_latency_samples.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_realtime_voice_changer_config_json.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_config_json.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.sonare_realtime_voice_changer_preset_names.restype = ctypes.c_char_p
    lib.sonare_realtime_voice_changer_preset_names.argtypes = []
    lib.sonare_realtime_voice_changer_preset_json.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_preset_json.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.sonare_realtime_voice_changer_validate_preset_json.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_validate_preset_json.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_void_p),
    ]

    # Realtime engine / offline DAW engine
    lib.sonare_engine_create.restype = ctypes.c_int32
    lib.sonare_engine_create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    lib.sonare_engine_destroy.restype = None
    lib.sonare_engine_destroy.argtypes = [ctypes.c_void_p]
    lib.sonare_engine_prepare.restype = ctypes.c_int32
    lib.sonare_engine_prepare.argtypes = [
        ctypes.c_void_p,
        ctypes.c_double,
        ctypes.c_int,
        ctypes.c_size_t,
        ctypes.c_size_t,
    ]
    lib.sonare_engine_play.restype = ctypes.c_int32
    lib.sonare_engine_play.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.sonare_engine_stop.restype = ctypes.c_int32
    lib.sonare_engine_stop.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.sonare_engine_seek_sample.restype = ctypes.c_int32
    lib.sonare_engine_seek_sample.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64]
    lib.sonare_engine_seek_ppq.restype = ctypes.c_int32
    lib.sonare_engine_seek_ppq.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_int64]
    lib.sonare_engine_set_tempo.restype = ctypes.c_int32
    lib.sonare_engine_set_tempo.argtypes = [ctypes.c_void_p, ctypes.c_double]
    lib.sonare_engine_set_time_signature.restype = ctypes.c_int32
    lib.sonare_engine_set_time_signature.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.sonare_engine_set_loop.restype = ctypes.c_int32
    lib.sonare_engine_set_loop.argtypes = [
        ctypes.c_void_p,
        ctypes.c_double,
        ctypes.c_double,
        ctypes.c_int,
    ]
    lib.sonare_engine_add_parameter.restype = ctypes.c_int32
    lib.sonare_engine_add_parameter.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareParameterInfo),
    ]
    lib.sonare_engine_parameter_count.restype = ctypes.c_int32
    lib.sonare_engine_parameter_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_parameter_info_by_index.restype = ctypes.c_int32
    lib.sonare_engine_parameter_info_by_index.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(SonareParameterInfo),
    ]
    lib.sonare_engine_parameter_info.restype = ctypes.c_int32
    lib.sonare_engine_parameter_info.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(SonareParameterInfo),
    ]
    lib.sonare_engine_set_automation_lane.restype = ctypes.c_int32
    lib.sonare_engine_set_automation_lane.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(SonareAutomationPoint),
        ctypes.c_size_t,
    ]
    lib.sonare_engine_automation_lane_count.restype = ctypes.c_int32
    lib.sonare_engine_automation_lane_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_set_markers.restype = ctypes.c_int32
    lib.sonare_engine_set_markers.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineMarker),
        ctypes.c_size_t,
    ]
    lib.sonare_engine_marker_count.restype = ctypes.c_int32
    lib.sonare_engine_marker_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_marker_by_index.restype = ctypes.c_int32
    lib.sonare_engine_marker_by_index.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(SonareEngineMarker),
    ]
    lib.sonare_engine_marker.restype = ctypes.c_int32
    lib.sonare_engine_marker.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(SonareEngineMarker),
    ]
    lib.sonare_engine_seek_marker.restype = ctypes.c_int32
    lib.sonare_engine_seek_marker.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int64]
    lib.sonare_engine_set_loop_from_markers.restype = ctypes.c_int32
    lib.sonare_engine_set_loop_from_markers.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
    ]
    lib.sonare_engine_set_metronome.restype = ctypes.c_int32
    lib.sonare_engine_set_metronome.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineMetronomeConfig),
    ]
    lib.sonare_engine_metronome.restype = ctypes.c_int32
    lib.sonare_engine_metronome.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineMetronomeConfig),
    ]
    lib.sonare_engine_count_in_end_sample.restype = ctypes.c_int32
    lib.sonare_engine_count_in_end_sample.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int64,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int64),
    ]
    lib.sonare_engine_set_clips.restype = ctypes.c_int32
    lib.sonare_engine_set_clips.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineClip),
        ctypes.c_size_t,
    ]
    lib.sonare_engine_clip_count.restype = ctypes.c_int32
    lib.sonare_engine_clip_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_set_capture_buffer.restype = ctypes.c_int32
    lib.sonare_engine_set_capture_buffer.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineCaptureBuffer),
    ]
    lib.sonare_engine_arm_capture.restype = ctypes.c_int32
    lib.sonare_engine_arm_capture.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    lib.sonare_engine_set_capture_punch.restype = ctypes.c_int32
    lib.sonare_engine_set_capture_punch.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int64,
        ctypes.c_int64,
        ctypes.c_int,
    ]
    lib.sonare_engine_reset_capture.restype = ctypes.c_int32
    lib.sonare_engine_reset_capture.argtypes = [ctypes.c_void_p]
    lib.sonare_engine_capture_status.restype = ctypes.c_int32
    lib.sonare_engine_capture_status.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineCaptureStatus),
    ]
    lib.sonare_engine_set_graph.restype = ctypes.c_int32
    lib.sonare_engine_set_graph.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineGraphSpec),
    ]
    lib.sonare_engine_graph_node_count.restype = ctypes.c_int32
    lib.sonare_engine_graph_node_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_graph_connection_count.restype = ctypes.c_int32
    lib.sonare_engine_graph_connection_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_process.restype = ctypes.c_int32
    lib.sonare_engine_process.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_int,
        ctypes.c_int,
    ]
    lib.sonare_engine_process_with_monitor.restype = ctypes.c_int32
    lib.sonare_engine_process_with_monitor.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_int,
        ctypes.c_int,
    ]
    lib.sonare_engine_render_offline.restype = ctypes.c_int32
    lib.sonare_engine_render_offline.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_int,
        ctypes.c_int64,
        ctypes.c_int,
    ]
    lib.sonare_engine_bounce_offline.restype = ctypes.c_int32
    lib.sonare_engine_bounce_offline.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineBounceOptions),
        ctypes.POINTER(SonareEngineBounceResult),
    ]
    lib.sonare_free_bounce_result.restype = None
    lib.sonare_free_bounce_result.argtypes = [ctypes.POINTER(SonareEngineBounceResult)]
    lib.sonare_engine_freeze_offline.restype = ctypes.c_int32
    lib.sonare_engine_freeze_offline.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineFreezeOptions),
        ctypes.POINTER(SonareEngineFreezeResult),
    ]
    lib.sonare_engine_drain_telemetry.restype = ctypes.c_int32
    lib.sonare_engine_drain_telemetry.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineTelemetry),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    if hasattr(lib, "sonare_engine_drain_meter_telemetry"):
        lib.sonare_engine_drain_meter_telemetry.restype = ctypes.c_int32
        lib.sonare_engine_drain_meter_telemetry.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareMeterTelemetryRecord),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
    if hasattr(lib, "sonare_engine_set_parameter"):
        lib.sonare_engine_set_parameter.restype = ctypes.c_int32
        lib.sonare_engine_set_parameter.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_float,
            ctypes.c_int64,
        ]
        lib.sonare_engine_set_parameter_smoothed.restype = ctypes.c_int32
        lib.sonare_engine_set_parameter_smoothed.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_float,
            ctypes.c_int64,
        ]
    if hasattr(lib, "sonare_engine_get_transport_state"):
        lib.sonare_engine_get_transport_state.restype = ctypes.c_int32
        lib.sonare_engine_get_transport_state.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareTransportState),
        ]

    # sonare_normalize
    lib.sonare_normalize.restype = ctypes.c_int32
    lib.sonare_normalize.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_trim
    lib.sonare_trim.restype = ctypes.c_int32
    lib.sonare_trim.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Mastering: offline repair processors ---

    if hasattr(lib, "sonare_mastering_repair_declick"):
        lib.sonare_mastering_repair_declick.restype = ctypes.c_int32
        lib.sonare_mastering_repair_declick.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDeclickConfig),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    if hasattr(lib, "sonare_mastering_repair_denoise_classical"):
        lib.sonare_mastering_repair_denoise_classical.restype = ctypes.c_int32
        lib.sonare_mastering_repair_denoise_classical.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDenoiseClassicalConfig),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    for _name, _cfg in (
        ("sonare_mastering_repair_declip", SonareDeclipConfig),
        ("sonare_mastering_repair_decrackle", SonareDecrackleConfig),
        ("sonare_mastering_repair_dehum", SonareDehumConfig),
        ("sonare_mastering_repair_dereverb_classical", SonareDereverbClassicalConfig),
        ("sonare_mastering_repair_trim_silence", SonareTrimSilenceConfig),
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_int32
            _fn.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(_cfg),
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.POINTER(ctypes.c_size_t),
            ]

    # --- Features - Spectrogram ---

    # sonare_stft
    lib.sonare_stft.restype = ctypes.c_int32
    lib.sonare_stft.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareStftResult),
    ]

    # sonare_stft_db
    lib.sonare_stft_db.restype = ctypes.c_int32
    lib.sonare_stft_db.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
    ]

    # --- Features - Mel ---

    # sonare_mel_spectrogram
    lib.sonare_mel_spectrogram.restype = ctypes.c_int32
    lib.sonare_mel_spectrogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareMelResult),
    ]

    # sonare_mfcc
    lib.sonare_mfcc.restype = ctypes.c_int32
    lib.sonare_mfcc.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareMfccResult),
    ]

    # --- Features - Chroma ---

    # sonare_chroma
    lib.sonare_chroma.restype = ctypes.c_int32
    lib.sonare_chroma.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareChromaResult),
    ]

    # --- Features - Spectral ---

    # sonare_spectral_centroid
    lib.sonare_spectral_centroid.restype = ctypes.c_int32
    lib.sonare_spectral_centroid.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_bandwidth
    lib.sonare_spectral_bandwidth.restype = ctypes.c_int32
    lib.sonare_spectral_bandwidth.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_rolloff
    lib.sonare_spectral_rolloff.restype = ctypes.c_int32
    lib.sonare_spectral_rolloff.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_flatness
    lib.sonare_spectral_flatness.restype = ctypes.c_int32
    lib.sonare_spectral_flatness.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_zero_crossing_rate
    lib.sonare_zero_crossing_rate.restype = ctypes.c_int32
    lib.sonare_zero_crossing_rate.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_rms_energy
    lib.sonare_rms_energy.restype = ctypes.c_int32
    lib.sonare_rms_energy.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Features - Pitch ---

    # sonare_pitch_yin
    lib.sonare_pitch_yin.restype = ctypes.c_int32
    lib.sonare_pitch_yin.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonarePitchResult),
    ]

    # sonare_pitch_pyin
    lib.sonare_pitch_pyin.restype = ctypes.c_int32
    lib.sonare_pitch_pyin.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonarePitchResult),
    ]

    # --- Core - Conversion ---

    lib.sonare_hz_to_mel.restype = ctypes.c_float
    lib.sonare_hz_to_mel.argtypes = [ctypes.c_float]

    lib.sonare_mel_to_hz.restype = ctypes.c_float
    lib.sonare_mel_to_hz.argtypes = [ctypes.c_float]

    lib.sonare_hz_to_midi.restype = ctypes.c_float
    lib.sonare_hz_to_midi.argtypes = [ctypes.c_float]

    lib.sonare_midi_to_hz.restype = ctypes.c_float
    lib.sonare_midi_to_hz.argtypes = [ctypes.c_float]

    lib.sonare_hz_to_note.restype = ctypes.c_char_p
    lib.sonare_hz_to_note.argtypes = [ctypes.c_float]

    lib.sonare_note_to_hz.restype = ctypes.c_float
    lib.sonare_note_to_hz.argtypes = [ctypes.c_char_p]

    lib.sonare_frames_to_time.restype = ctypes.c_float
    lib.sonare_frames_to_time.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    lib.sonare_time_to_frames.restype = ctypes.c_int32
    lib.sonare_time_to_frames.argtypes = [ctypes.c_float, ctypes.c_int, ctypes.c_int]

    lib.sonare_frames_to_samples.restype = ctypes.c_int32
    lib.sonare_frames_to_samples.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    lib.sonare_samples_to_frames.restype = ctypes.c_int32
    lib.sonare_samples_to_frames.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    for name in (
        "sonare_power_to_db",
        "sonare_amplitude_to_db",
    ):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    for name in (
        "sonare_db_to_power",
        "sonare_db_to_amplitude",
    ):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    for name in ("sonare_preemphasis", "sonare_deemphasis"):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    lib.sonare_trim_silence.restype = ctypes.c_int32
    lib.sonare_trim_silence.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_split_silence.restype = ctypes.c_int32
    lib.sonare_split_silence.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_frame_signal.restype = ctypes.c_int32
    lib.sonare_frame_signal.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    for name in ("sonare_pad_center", "sonare_fix_length"):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    lib.sonare_fix_frames.restype = ctypes.c_int32
    lib.sonare_fix_frames.argtypes = [
        ctypes.POINTER(ctypes.c_int),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_peak_pick.restype = ctypes.c_int32
    lib.sonare_peak_pick.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_vector_normalize.restype = ctypes.c_int32
    lib.sonare_vector_normalize.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_pcen.restype = ctypes.c_int32
    lib.sonare_pcen.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_tonnetz.restype = ctypes.c_int32
    lib.sonare_tonnetz.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_tempogram.restype = ctypes.c_int32
    lib.sonare_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_tempogram_with_mode.restype = ctypes.c_int32
    lib.sonare_tempogram_with_mode.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_cyclic_tempogram.restype = ctypes.c_int32
    lib.sonare_cyclic_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_plp.restype = ctypes.c_int32
    lib.sonare_plp.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_onset_strength
    lib.sonare_onset_strength.restype = ctypes.c_int32
    lib.sonare_onset_strength.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_fourier_tempogram
    lib.sonare_fourier_tempogram.restype = ctypes.c_int32
    lib.sonare_fourier_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_tempogram_ratio
    lib.sonare_tempogram_ratio.restype = ctypes.c_int32
    lib.sonare_tempogram_ratio.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_nnls_chroma
    lib.sonare_nnls_chroma.restype = ctypes.c_int32
    lib.sonare_nnls_chroma.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_lufs
    lib.sonare_lufs.restype = ctypes.c_int32
    lib.sonare_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareLufsResult),
    ]

    # sonare_momentary_lufs
    lib.sonare_momentary_lufs.restype = ctypes.c_int32
    lib.sonare_momentary_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_short_term_lufs
    lib.sonare_short_term_lufs.restype = ctypes.c_int32
    lib.sonare_short_term_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Metering (offline) — basic / true peak / clipping / dynamic range ---

    for _name in (
        "sonare_metering_peak_db",
        "sonare_metering_rms_db",
        "sonare_metering_crest_factor_db",
        "sonare_metering_dc_offset",
    ):
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
        ]

    lib.sonare_metering_true_peak_db.restype = ctypes.c_int32
    lib.sonare_metering_true_peak_db.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
    ]

    lib.sonare_metering_detect_clipping.restype = ctypes.c_int32
    lib.sonare_metering_detect_clipping.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_size_t,
        ctypes.POINTER(SonareClippingResult),
    ]

    lib.sonare_free_clipping_result.restype = None
    lib.sonare_free_clipping_result.argtypes = [ctypes.POINTER(SonareClippingResult)]

    lib.sonare_metering_dynamic_range.restype = ctypes.c_int32
    lib.sonare_metering_dynamic_range.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareDynamicRangeResult),
    ]

    lib.sonare_free_dynamic_range_result.restype = None
    lib.sonare_free_dynamic_range_result.argtypes = [ctypes.POINTER(SonareDynamicRangeResult)]

    # --- Metering (offline) — stereo / phase-scope / spectrum ---

    for _name in ("sonare_metering_stereo_correlation", "sonare_metering_stereo_width"):
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
        ]

    lib.sonare_metering_vectorscope.restype = ctypes.c_int32
    lib.sonare_metering_vectorscope.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareVectorscopeResult),
    ]

    lib.sonare_free_vectorscope_result.restype = None
    lib.sonare_free_vectorscope_result.argtypes = [ctypes.POINTER(SonareVectorscopeResult)]

    lib.sonare_metering_phase_scope.restype = ctypes.c_int32
    lib.sonare_metering_phase_scope.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonarePhaseScopeResult),
    ]

    lib.sonare_free_phase_scope_result.restype = None
    lib.sonare_free_phase_scope_result.argtypes = [ctypes.POINTER(SonarePhaseScopeResult)]

    lib.sonare_metering_spectrum.restype = ctypes.c_int32
    lib.sonare_metering_spectrum.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareSpectrumResult),
    ]

    lib.sonare_free_spectrum_result.restype = None
    lib.sonare_free_spectrum_result.argtypes = [ctypes.POINTER(SonareSpectrumResult)]

    # --- Editing - Scale quantizer ---

    for _name in ("sonare_scale_quantize_midi", "sonare_scale_correction_semitones"):
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.c_int,
            ctypes.c_uint16,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.POINTER(ctypes.c_float),
        ]

    lib.sonare_scale_pitch_class_enabled.restype = ctypes.c_int32
    lib.sonare_scale_pitch_class_enabled.argtypes = [
        ctypes.c_int,
        ctypes.c_uint16,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
    ]

    # --- Core - Resample ---

    lib.sonare_resample.restype = ctypes.c_int32
    lib.sonare_resample.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    if hasattr(lib, "sonare_mastering_process"):
        lib.sonare_mastering_process.restype = ctypes.c_int32
        lib.sonare_mastering_process.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringConfig),
            ctypes.POINTER(SonareMasteringResult),
        ]
        lib.sonare_free_mastering_result.restype = None
        lib.sonare_free_mastering_result.argtypes = [ctypes.POINTER(SonareMasteringResult)]
        lib.sonare_mastering_apply_processor.restype = ctypes.c_int32
        lib.sonare_mastering_apply_processor.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(SonareMasteringResult),
        ]
        lib.sonare_mastering_apply_processor_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_apply_processor_stereo.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(SonareMasteringStereoResult),
        ]
        lib.sonare_mastering_processor_names.restype = ctypes.c_char_p
        lib.sonare_mastering_processor_names.argtypes = []
        lib.sonare_mastering_pair_processor_names.restype = ctypes.c_char_p
        lib.sonare_mastering_pair_processor_names.argtypes = []
        lib.sonare_mastering_pair_analysis_names.restype = ctypes.c_char_p
        lib.sonare_mastering_pair_analysis_names.argtypes = []
        lib.sonare_mastering_stereo_analysis_names.restype = ctypes.c_char_p
        lib.sonare_mastering_stereo_analysis_names.argtypes = []
        lib.sonare_mastering_apply_pair_processor.restype = ctypes.c_int32
        lib.sonare_mastering_apply_pair_processor.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(SonareMasteringResult),
        ]
        lib.sonare_mastering_analyze_pair.restype = ctypes.c_int32
        lib.sonare_mastering_analyze_pair.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        lib.sonare_mastering_analyze_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_analyze_stereo.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        if hasattr(lib, "sonare_mastering_streaming_preview"):
            lib.sonare_mastering_streaming_preview.restype = ctypes.c_int32
            lib.sonare_mastering_streaming_preview.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareStreamingPlatform),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_void_p),
            ]
        if hasattr(lib, "sonare_mastering_assistant_suggest"):
            lib.sonare_mastering_assistant_suggest.restype = ctypes.c_int32
            lib.sonare_mastering_assistant_suggest.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_void_p),
            ]
        if hasattr(lib, "sonare_mastering_audio_profile"):
            lib.sonare_mastering_audio_profile.restype = ctypes.c_int32
            lib.sonare_mastering_audio_profile.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_void_p),
            ]
        lib.sonare_free_mastering_stereo_result.restype = None
        lib.sonare_free_mastering_stereo_result.argtypes = [
            ctypes.POINTER(SonareMasteringStereoResult)
        ]
        lib.sonare_free_string.restype = None
        lib.sonare_free_string.argtypes = [ctypes.c_void_p]
        if hasattr(lib, "sonare_mastering_chain"):
            lib.sonare_mastering_chain.restype = ctypes.c_int32
            lib.sonare_mastering_chain.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(SonareMasteringChainResult),
            ]
            lib.sonare_mastering_chain_stereo.restype = ctypes.c_int32
            lib.sonare_mastering_chain_stereo.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(SonareMasteringChainStereoResult),
            ]
            lib.sonare_free_mastering_chain_result.restype = None
            lib.sonare_free_mastering_chain_result.argtypes = [
                ctypes.POINTER(SonareMasteringChainResult)
            ]
            lib.sonare_free_mastering_chain_stereo_result.restype = None
            lib.sonare_free_mastering_chain_stereo_result.argtypes = [
                ctypes.POINTER(SonareMasteringChainStereoResult)
            ]
        if hasattr(lib, "sonare_streaming_mastering_chain_create"):
            lib.sonare_streaming_mastering_chain_create.restype = ctypes.c_void_p
            lib.sonare_streaming_mastering_chain_create.argtypes = [
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
            ]
            lib.sonare_streaming_mastering_chain_prepare.restype = ctypes.c_int32
            lib.sonare_streaming_mastering_chain_prepare.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_streaming_mastering_chain_process_mono.restype = ctypes.c_int32
            lib.sonare_streaming_mastering_chain_process_mono.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
            ]
            lib.sonare_streaming_mastering_chain_process_stereo.restype = ctypes.c_int32
            lib.sonare_streaming_mastering_chain_process_stereo.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
            ]
            lib.sonare_streaming_mastering_chain_reset.restype = ctypes.c_int32
            lib.sonare_streaming_mastering_chain_reset.argtypes = [ctypes.c_void_p]
            lib.sonare_streaming_mastering_chain_latency_samples.restype = ctypes.c_int
            lib.sonare_streaming_mastering_chain_latency_samples.argtypes = [ctypes.c_void_p]
            lib.sonare_streaming_mastering_chain_destroy.restype = None
            lib.sonare_streaming_mastering_chain_destroy.argtypes = [ctypes.c_void_p]
        if hasattr(lib, "sonare_eq_create"):
            lib.sonare_eq_create.restype = ctypes.c_void_p
            lib.sonare_eq_create.argtypes = [ctypes.c_double, ctypes.c_int]
            lib.sonare_eq_destroy.restype = None
            lib.sonare_eq_destroy.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_set_band.restype = ctypes.c_int32
            lib.sonare_eq_set_band.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
            lib.sonare_eq_clear.restype = None
            lib.sonare_eq_clear.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_set_phase_mode.restype = ctypes.c_int32
            lib.sonare_eq_set_phase_mode.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_eq_match.restype = ctypes.c_int32
            lib.sonare_eq_match.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_eq_set_auto_gain.restype = None
            lib.sonare_eq_set_auto_gain.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_eq_last_auto_gain_db.restype = ctypes.c_float
            lib.sonare_eq_last_auto_gain_db.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_set_gain_scale.restype = ctypes.c_int32
            lib.sonare_eq_set_gain_scale.argtypes = [ctypes.c_void_p, ctypes.c_float]
            lib.sonare_eq_set_output_gain_db.restype = ctypes.c_int32
            lib.sonare_eq_set_output_gain_db.argtypes = [ctypes.c_void_p, ctypes.c_float]
            lib.sonare_eq_set_output_pan.restype = ctypes.c_int32
            lib.sonare_eq_set_output_pan.argtypes = [ctypes.c_void_p, ctypes.c_float]
            lib.sonare_eq_latency_samples.restype = ctypes.c_int
            lib.sonare_eq_latency_samples.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_set_sidechain.restype = ctypes.c_int32
            lib.sonare_eq_set_sidechain.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_eq_clear_sidechain.restype = None
            lib.sonare_eq_clear_sidechain.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_process.restype = ctypes.c_int32
            lib.sonare_eq_process.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_eq_spectrum.restype = ctypes.c_int32
            lib.sonare_eq_spectrum.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareEqSnapshot),
            ]
        if hasattr(lib, "sonare_mastering_preset_names"):
            lib.sonare_mastering_preset_names.restype = ctypes.c_char_p
            lib.sonare_mastering_preset_names.argtypes = []
        if hasattr(lib, "sonare_master_audio"):
            lib.sonare_master_audio.restype = ctypes.c_int32
            lib.sonare_master_audio.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(SonareMasteringChainResult),
            ]
            lib.sonare_master_audio_stereo.restype = ctypes.c_int32
            lib.sonare_master_audio_stereo.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(SonareMasteringChainStereoResult),
            ]
        if hasattr(lib, "sonare_mastering_chain_with_progress"):
            lib.sonare_mastering_chain_with_progress.restype = ctypes.c_int32
            lib.sonare_mastering_chain_with_progress.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                SonareMasteringProgressCallback,
                ctypes.c_void_p,
                ctypes.POINTER(SonareMasteringChainResult),
            ]
            lib.sonare_mastering_chain_stereo_with_progress.restype = ctypes.c_int32
            lib.sonare_mastering_chain_stereo_with_progress.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                SonareMasteringProgressCallback,
                ctypes.c_void_p,
                ctypes.POINTER(SonareMasteringChainStereoResult),
            ]
        if hasattr(lib, "sonare_master_audio_with_progress"):
            lib.sonare_master_audio_with_progress.restype = ctypes.c_int32
            lib.sonare_master_audio_with_progress.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                SonareMasteringProgressCallback,
                ctypes.c_void_p,
                ctypes.POINTER(SonareMasteringChainResult),
            ]
            lib.sonare_master_audio_stereo_with_progress.restype = ctypes.c_int32
            lib.sonare_master_audio_stereo_with_progress.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                SonareMasteringProgressCallback,
                ctypes.c_void_p,
                ctypes.POINTER(SonareMasteringChainStereoResult),
            ]

    if hasattr(lib, "sonare_mixer_create"):
        lib.sonare_mixer_create.restype = ctypes.c_void_p
        lib.sonare_mixer_create.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.sonare_mixer_add_strip.restype = ctypes.c_void_p
        lib.sonare_mixer_add_strip.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.sonare_strip_set_input_trim_db.restype = ctypes.c_int32
        lib.sonare_strip_set_input_trim_db.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.sonare_strip_set_fader_db.restype = ctypes.c_int32
        lib.sonare_strip_set_fader_db.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.sonare_strip_set_pan.restype = ctypes.c_int32
        lib.sonare_strip_set_pan.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_int]
        lib.sonare_strip_set_dual_pan.restype = ctypes.c_int32
        lib.sonare_strip_set_dual_pan.argtypes = [
            ctypes.c_void_p,
            ctypes.c_float,
            ctypes.c_float,
        ]
        lib.sonare_strip_set_width.restype = ctypes.c_int32
        lib.sonare_strip_set_width.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.sonare_strip_set_muted.restype = ctypes.c_int32
        lib.sonare_strip_set_muted.argtypes = [ctypes.c_void_p, ctypes.c_int]
        if hasattr(lib, "sonare_strip_set_soloed"):
            lib.sonare_strip_set_soloed.restype = ctypes.c_int32
            lib.sonare_strip_set_soloed.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_strip_set_solo_safe.restype = ctypes.c_int32
            lib.sonare_strip_set_solo_safe.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_strip_set_polarity_invert.restype = ctypes.c_int32
            lib.sonare_strip_set_polarity_invert.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_strip_set_pan_law.restype = ctypes.c_int32
            lib.sonare_strip_set_pan_law.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_strip_set_channel_delay_samples.restype = ctypes.c_int32
            lib.sonare_strip_set_channel_delay_samples.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_strip_set_vca_offset_db.restype = ctypes.c_int32
            lib.sonare_strip_set_vca_offset_db.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.sonare_strip_add_send.restype = ctypes.c_int32
        lib.sonare_strip_add_send.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_strip_set_send_db.restype = ctypes.c_int32
        lib.sonare_strip_set_send_db.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_float,
        ]
        lib.sonare_strip_meter.restype = ctypes.c_int32
        lib.sonare_strip_meter.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareMixMeterSnapshot),
        ]
        if hasattr(lib, "sonare_strip_meter_tap"):
            lib.sonare_strip_meter_tap.restype = ctypes.c_int32
            lib.sonare_strip_meter_tap.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.POINTER(SonareMixMeterSnapshot),
            ]
        lib.sonare_strip_read_goniometer_latest.restype = ctypes.c_size_t
        lib.sonare_strip_read_goniometer_latest.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareMixGoniometerPoint),
            ctypes.c_size_t,
        ]
        lib.sonare_mixer_from_scene_json.restype = ctypes.c_void_p
        lib.sonare_mixer_from_scene_json.argtypes = [
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int,
        ]
        lib.sonare_mixer_to_scene_json.restype = ctypes.c_int32
        lib.sonare_mixer_to_scene_json.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        if hasattr(lib, "sonare_mixer_compile"):
            lib.sonare_mixer_compile.restype = ctypes.c_int32
            lib.sonare_mixer_compile.argtypes = [ctypes.c_void_p]
        if hasattr(lib, "sonare_mixer_get_strip_count"):
            lib.sonare_mixer_get_strip_count.restype = ctypes.c_int32
            lib.sonare_mixer_get_strip_count.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_size_t),
            ]
        if hasattr(lib, "sonare_mixer_add_bus"):
            lib.sonare_mixer_add_bus.restype = ctypes.c_int32
            lib.sonare_mixer_add_bus.argtypes = [
                ctypes.c_void_p,
                ctypes.c_char_p,
                ctypes.c_char_p,
            ]
            lib.sonare_mixer_remove_bus.restype = ctypes.c_int32
            lib.sonare_mixer_remove_bus.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            lib.sonare_mixer_bus_count.restype = ctypes.c_int32
            lib.sonare_mixer_bus_count.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_size_t),
            ]
            lib.sonare_mixer_add_vca_group.restype = ctypes.c_int32
            lib.sonare_mixer_add_vca_group.argtypes = [
                ctypes.c_void_p,
                ctypes.c_char_p,
                ctypes.c_float,
                ctypes.POINTER(ctypes.c_char_p),
                ctypes.c_size_t,
            ]
            lib.sonare_mixer_remove_vca_group.restype = ctypes.c_int32
            lib.sonare_mixer_remove_vca_group.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            lib.sonare_mixer_vca_group_count.restype = ctypes.c_int32
            lib.sonare_mixer_vca_group_count.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_size_t),
            ]
        if hasattr(lib, "sonare_mixer_strip_count"):
            lib.sonare_mixer_strip_count.restype = ctypes.c_size_t
            lib.sonare_mixer_strip_count.argtypes = [ctypes.c_void_p]
            lib.sonare_mixer_strip_at.restype = ctypes.c_void_p
            lib.sonare_mixer_strip_at.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            lib.sonare_mixer_strip_by_id.restype = ctypes.c_void_p
            lib.sonare_mixer_strip_by_id.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            lib.sonare_strip_schedule_insert_automation.restype = ctypes.c_int32
            lib.sonare_strip_schedule_insert_automation.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint,
                ctypes.c_uint,
                ctypes.c_int64,
                ctypes.c_float,
                ctypes.c_int,
            ]
        if hasattr(lib, "sonare_strip_schedule_fader_automation"):
            for _name in (
                "sonare_strip_schedule_fader_automation",
                "sonare_strip_schedule_pan_automation",
                "sonare_strip_schedule_width_automation",
            ):
                _fn = getattr(lib, _name)
                _fn.restype = ctypes.c_int32
                _fn.argtypes = [
                    ctypes.c_void_p,
                    ctypes.c_int64,
                    ctypes.c_float,
                    ctypes.c_int,
                ]
            lib.sonare_strip_schedule_send_automation.restype = ctypes.c_int32
            lib.sonare_strip_schedule_send_automation.argtypes = [
                ctypes.c_void_p,
                ctypes.c_size_t,
                ctypes.c_int64,
                ctypes.c_float,
                ctypes.c_int,
            ]
        lib.sonare_mixer_process_stereo.restype = ctypes.c_int32
        lib.sonare_mixer_process_stereo.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
        ]
        lib.sonare_mixing_scene_preset_names.restype = ctypes.c_char_p
        lib.sonare_mixing_scene_preset_names.argtypes = []
        lib.sonare_mixing_scene_preset_json.restype = ctypes.c_int32
        lib.sonare_mixing_scene_preset_json.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        lib.sonare_mixer_destroy.restype = None
        lib.sonare_mixer_destroy.argtypes = [ctypes.c_void_p]

    # --- Free functions for result types ---

    lib.sonare_free_stft_result.restype = None
    lib.sonare_free_stft_result.argtypes = [ctypes.POINTER(SonareStftResult)]

    lib.sonare_free_mel_result.restype = None
    lib.sonare_free_mel_result.argtypes = [ctypes.POINTER(SonareMelResult)]

    lib.sonare_free_mfcc_result.restype = None
    lib.sonare_free_mfcc_result.argtypes = [ctypes.POINTER(SonareMfccResult)]

    lib.sonare_free_chroma_result.restype = None
    lib.sonare_free_chroma_result.argtypes = [ctypes.POINTER(SonareChromaResult)]

    lib.sonare_free_pitch_result.restype = None
    lib.sonare_free_pitch_result.argtypes = [ctypes.POINTER(SonarePitchResult)]

    lib.sonare_free_hpss_result.restype = None
    lib.sonare_free_hpss_result.argtypes = [ctypes.POINTER(SonareHpssResult)]

    lib.sonare_free_ints.restype = None
    lib.sonare_free_ints.argtypes = [ctypes.POINTER(ctypes.c_int32)]

    return lib
