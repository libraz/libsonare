"""ctypes structure and constant definitions for libsonare."""

from __future__ import annotations

import ctypes

from ._ffi_types_core import SonareTimeSignature

# Progress callback: void(float progress, const char* stage, void* user_data).
# Maps to SonareAnalyzeProgressCallback in sonare_c.h.
SonareAnalyzeProgressCallback = ctypes.CFUNCTYPE(
    None,
    ctypes.c_float,
    ctypes.c_char_p,
    ctypes.c_void_p,
)


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


class SonareRirSynthConfig(ctypes.Structure):
    """Maps to SonareRirSynthConfig in sonare_c_acoustic.h."""

    _fields_ = [
        ("length_m", ctypes.c_float),
        ("width_m", ctypes.c_float),
        ("height_m", ctypes.c_float),
        ("source_x", ctypes.c_float),
        ("source_y", ctypes.c_float),
        ("source_z", ctypes.c_float),
        ("listener_x", ctypes.c_float),
        ("listener_y", ctypes.c_float),
        ("listener_z", ctypes.c_float),
        ("absorption", ctypes.c_float),
        ("max_seconds", ctypes.c_float),
        ("mixing_time_ms", ctypes.c_float),
        ("crossfade_ms", ctypes.c_float),
        ("ism_order", ctypes.c_int),
        ("late_model", ctypes.c_int),
        ("seed", ctypes.c_uint),
        # Optional per-octave-band absorption tail (NULL/0 keeps the scalar path).
        ("absorption_bands", ctypes.POINTER(ctypes.c_float)),
        ("absorption_band_count", ctypes.c_size_t),
        ("material_preset", ctypes.c_int),
    ]


class SonareRirSynthResult(ctypes.Structure):
    """Maps to SonareRirSynthResult in sonare_c_acoustic.h."""

    _fields_ = [
        ("rir", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int),
        ("has_error", ctypes.c_int),
    ]


class SonareRoomEstimateConfig(ctypes.Structure):
    """Maps to SonareRoomEstimateConfig in sonare_c_acoustic.h."""

    _fields_ = [
        ("aspect_hint_lw", ctypes.c_float),
        ("aspect_hint_lh", ctypes.c_float),
        ("reference_absorption", ctypes.c_float),
        ("min_decay_db", ctypes.c_float),
        ("noise_floor_margin_db", ctypes.c_float),
        ("prefer_eyring", ctypes.c_int),
        ("n_octave_bands", ctypes.c_int),
        ("mode", ctypes.c_int),
    ]


class SonareRoomEstimate(ctypes.Structure):
    """Maps to SonareRoomEstimate in sonare_c_acoustic.h."""

    _fields_ = [
        ("volume", ctypes.c_float),
        ("length_m", ctypes.c_float),
        ("width_m", ctypes.c_float),
        ("height_m", ctypes.c_float),
        ("drr_db", ctypes.c_float),
        ("confidence", ctypes.c_float),
        ("absorption_bands", ctypes.POINTER(ctypes.c_float)),
        ("rt60_bands", ctypes.POINTER(ctypes.c_float)),
        ("band_count", ctypes.c_size_t),
    ]


class SonareRoomMorphConfig(ctypes.Structure):
    """Maps to SonareRoomMorphConfig in sonare_c_acoustic.h."""

    _fields_ = [
        ("length_m", ctypes.c_float),
        ("width_m", ctypes.c_float),
        ("height_m", ctypes.c_float),
        ("source_x", ctypes.c_float),
        ("source_y", ctypes.c_float),
        ("source_z", ctypes.c_float),
        ("listener_x", ctypes.c_float),
        ("listener_y", ctypes.c_float),
        ("listener_z", ctypes.c_float),
        ("absorption", ctypes.c_float),
        ("source_tail_suppression", ctypes.c_float),
        ("wet", ctypes.c_float),
        ("max_seconds", ctypes.c_float),
        ("mixing_time_ms", ctypes.c_float),
        ("crossfade_ms", ctypes.c_float),
        ("ism_order", ctypes.c_int),
        ("late_model", ctypes.c_int),
        ("seed", ctypes.c_uint),
        # Optional per-octave-band absorption tail (NULL/0 keeps the scalar path).
        ("absorption_bands", ctypes.POINTER(ctypes.c_float)),
        ("absorption_band_count", ctypes.c_size_t),
        ("material_preset", ctypes.c_int),
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


__all__ = [name for name in globals() if name.startswith(("Sonare", "SONARE_"))]
