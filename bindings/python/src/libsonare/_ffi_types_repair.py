"""ctypes structure and constant definitions for libsonare."""

from __future__ import annotations

import ctypes


class SonareDeclickConfig(ctypes.Structure):
    """Maps to SonareDeclickConfig in sonare_c.h."""

    _fields_ = [
        ("threshold", ctypes.c_float),
        ("neighbor_ratio", ctypes.c_float),
        ("max_click_samples", ctypes.c_size_t),
        ("lpc_order", ctypes.c_int),
        ("residual_ratio", ctypes.c_float),
    ]


# SonareCompressorConfig.detector values.
SONARE_COMPRESSOR_DETECTOR_PEAK = 0
SONARE_COMPRESSOR_DETECTOR_RMS = 1
SONARE_COMPRESSOR_DETECTOR_LOG_RMS = 2


class SonareCompressorConfig(ctypes.Structure):
    """Maps to SonareCompressorConfig in sonare_c.h."""

    _fields_ = [
        ("threshold_db", ctypes.c_float),
        ("ratio", ctypes.c_float),
        ("attack_ms", ctypes.c_float),
        ("release_ms", ctypes.c_float),
        ("knee_db", ctypes.c_float),
        ("makeup_gain_db", ctypes.c_float),
        ("auto_makeup", ctypes.c_int),
        ("detector", ctypes.c_int),
        ("sidechain_hpf_enabled", ctypes.c_int),
        ("sidechain_hpf_hz", ctypes.c_float),
        ("pdr_time_ms", ctypes.c_float),
        ("pdr_release_scale", ctypes.c_float),
    ]


class SonareGateConfig(ctypes.Structure):
    """Maps to SonareGateConfig in sonare_c.h."""

    _fields_ = [
        ("threshold_db", ctypes.c_float),
        ("attack_ms", ctypes.c_float),
        ("release_ms", ctypes.c_float),
        ("range_db", ctypes.c_float),
        ("hold_ms", ctypes.c_float),
        ("close_threshold_db", ctypes.c_float),
        ("key_hpf_hz", ctypes.c_float),
    ]


class SonareTransientShaperConfig(ctypes.Structure):
    """Maps to SonareTransientShaperConfig in sonare_c.h."""

    _fields_ = [
        ("attack_gain_db", ctypes.c_float),
        ("sustain_gain_db", ctypes.c_float),
        ("fast_attack_ms", ctypes.c_float),
        ("fast_release_ms", ctypes.c_float),
        ("slow_attack_ms", ctypes.c_float),
        ("slow_release_ms", ctypes.c_float),
        ("sensitivity", ctypes.c_float),
        ("max_gain_db", ctypes.c_float),
        ("gain_smoothing_ms", ctypes.c_float),
        ("lookahead_ms", ctypes.c_float),
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


__all__ = [name for name in globals() if name.startswith(("Sonare", "SONARE_"))]
