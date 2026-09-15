"""ctypes structure and constant definitions for libsonare."""

from __future__ import annotations

import ctypes

from ._cstruct import CStruct


class SonareDeclickConfig(CStruct):
    """Maps to SonareDeclickConfig in sonare_c.h."""

    _fields_ = [
        ("threshold", ctypes.c_float),
        ("neighbor_ratio", ctypes.c_float),
        ("max_click_samples", ctypes.c_size_t),
        ("lpc_order", ctypes.c_int),
        ("residual_ratio", ctypes.c_float),
    ]


class SonareClickDetection(CStruct):
    """Maps to SonareClickDetection in sonare_c.h. Counts runs, not samples."""

    _fields_ = [
        ("count", ctypes.c_size_t),
        ("rejected", ctypes.c_size_t),
        ("longest_run_samples", ctypes.c_size_t),
        ("per_second", ctypes.c_float),
    ]


class SonareDeclickReport(CStruct):
    """Maps to SonareDeclickReport in sonare_c.h."""

    _fields_ = [
        ("detected", SonareClickDetection),
        ("repaired_runs", ctypes.c_size_t),
        ("repaired_samples", ctypes.c_size_t),
        ("linked_runs", ctypes.c_size_t),
        ("lpc_model_used", ctypes.c_int),
    ]


class SonareDeclickStereoResult(CStruct):
    """Maps to SonareDeclickStereoResult in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("left_report", SonareDeclickReport),
        ("right_report", SonareDeclickReport),
    ]


# SonareCompressorConfig.detector values.
SONARE_COMPRESSOR_DETECTOR_PEAK = 0
SONARE_COMPRESSOR_DETECTOR_RMS = 1
SONARE_COMPRESSOR_DETECTOR_LOG_RMS = 2


class SonareCompressorConfig(CStruct):
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


class SonareGateConfig(CStruct):
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


class SonareTransientShaperConfig(CStruct):
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


class SonareDenoiseClassicalConfig(CStruct):
    """Maps to SonareDenoiseClassicalConfig in sonare_c.h."""

    _fields_ = [
        ("mode", ctypes.c_int),
        ("noise_estimator", ctypes.c_int),
        ("n_fft", ctypes.c_int),
        ("hop_length", ctypes.c_int),
        ("dd_alpha", ctypes.c_float),
        ("reduction_db", ctypes.c_float),
        ("over_subtraction", ctypes.c_float),
        ("spectral_floor", ctypes.c_float),
        ("noise_estimation_quantile", ctypes.c_float),
        ("speech_presence_gain", ctypes.c_int),
        ("gain_smoothing", ctypes.c_int),
    ]


class SonareDeclipConfig(CStruct):
    """Maps to SonareDeclipConfig in sonare_c.h."""

    _fields_ = [
        ("clip_threshold", ctypes.c_float),
        ("lpc_order", ctypes.c_int),
        ("iterations", ctypes.c_int),
        ("lpc_blend", ctypes.c_float),
    ]


class SonareClipDetection(CStruct):
    """Maps to SonareClipDetection in sonare_c.h."""

    _fields_ = [
        ("sample_count", ctypes.c_size_t),
        ("sample_fraction", ctypes.c_float),
        ("run_count", ctypes.c_size_t),
        ("longest_run_samples", ctypes.c_size_t),
    ]


class SonareDeclipReport(CStruct):
    """Maps to SonareDeclipReport in sonare_c.h."""

    _fields_ = [
        ("detected", SonareClipDetection),
        ("lpc_reconstructed_runs", ctypes.c_size_t),
        ("interpolated_runs", ctypes.c_size_t),
        ("repaired_samples", ctypes.c_size_t),
        ("linked_runs", ctypes.c_size_t),
    ]


class SonareDeclipStereoResult(CStruct):
    """Maps to SonareDeclipStereoResult in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("left_report", SonareDeclipReport),
        ("right_report", SonareDeclipReport),
    ]


# SonareDecrackleConfig.mode values.
SONARE_DECRACKLE_MODE_MEDIAN = 0
SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE = 1


class SonareDecrackleConfig(CStruct):
    """Maps to SonareDecrackleConfig in sonare_c.h."""

    _fields_ = [
        ("threshold", ctypes.c_float),
        ("mode", ctypes.c_int),
        ("levels", ctypes.c_int),
    ]


class SonareDehumConfig(CStruct):
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


class SonareDereverbClassicalConfig(CStruct):
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


class SonareTrimSilenceConfig(CStruct):
    """Maps to SonareTrimSilenceConfig in sonare_c.h."""

    _fields_ = [
        ("threshold", ctypes.c_float),
        ("padding_samples", ctypes.c_size_t),
        ("mode", ctypes.c_int),
        ("gate_lufs", ctypes.c_float),
        ("window_ms", ctypes.c_float),
    ]


# SonareSpectralRegionOp.mode values.
SONARE_SPECTRAL_EDIT_MODE_GAIN = 0
SONARE_SPECTRAL_EDIT_MODE_ATTENUATE = 1
SONARE_SPECTRAL_EDIT_MODE_MUTE = 2
SONARE_SPECTRAL_EDIT_MODE_HEAL = 3


class SonareSpectralEditConfig(CStruct):
    """Maps to SonareSpectralEditConfig in sonare_c_effects.h."""

    _fields_ = [
        ("n_fft", ctypes.c_int),
        ("hop_length", ctypes.c_int),
        ("window", ctypes.c_int),
        ("heal_radius_frames", ctypes.c_int),
    ]


class SonareSpectralRegionOp(CStruct):
    """Maps to SonareSpectralRegionOp in sonare_c_effects.h."""

    _fields_ = [
        ("start_sample", ctypes.c_int64),
        ("end_sample", ctypes.c_int64),
        ("low_hz", ctypes.c_float),
        ("high_hz", ctypes.c_float),
        ("gain_db", ctypes.c_float),
        ("mode", ctypes.c_int),
    ]


class SonareNoteExtractorConfig(CStruct):
    """Maps to SonareNoteExtractorConfig in sonare_c_effects.h."""

    _fields_ = [
        ("struct_version", ctypes.c_int32),
        ("segmentation_threshold_cents", ctypes.c_float),
        ("min_note_ms", ctypes.c_float),
        ("reference_hz", ctypes.c_float),
        ("voiced_threshold", ctypes.c_float),
    ]


class SonareNoteRenderConfig(CStruct):
    """Maps to SonareNoteRenderConfig in sonare_c_effects.h."""

    _fields_ = [
        ("struct_version", ctypes.c_int32),
        ("fade_ms", ctypes.c_float),
        ("vibrato_cutoff_hz", ctypes.c_float),
    ]


class SonareNoteEdit(CStruct):
    """Maps to SonareNoteEdit in sonare_c_effects.h."""

    _fields_ = [
        ("time_offset_samples", ctypes.c_int64),
        ("envelope_offset", ctypes.c_int64),
        ("envelope_count", ctypes.c_size_t),
        ("pitch_shift_semitones", ctypes.c_float),
        ("gain_db", ctypes.c_float),
        ("time_stretch_ratio", ctypes.c_float),
        ("formant_shift_semitones", ctypes.c_float),
        ("vibrato_depth_change", ctypes.c_float),
        ("drift_change", ctypes.c_float),
        ("muted", ctypes.c_int32),
    ]


class SonareNoteObject(CStruct):
    """Maps to SonareNoteObject in sonare_c_effects.h."""

    _fields_ = [
        ("onset_sample", ctypes.c_int64),
        ("offset_sample", ctypes.c_int64),
        ("amplitude_offset", ctypes.c_int64),
        ("frame_start", ctypes.c_int32),
        ("frame_end", ctypes.c_int32),
        ("median_hz", ctypes.c_float),
        ("median_cents", ctypes.c_float),
        ("f0_stability", ctypes.c_float),
        ("edit", SonareNoteEdit),
    ]


class SonareNoteObjectsResult(CStruct):
    """Maps to SonareNoteObjectsResult in sonare_c_effects.h."""

    _fields_ = [
        ("notes", ctypes.POINTER(SonareNoteObject)),
        ("count", ctypes.c_size_t),
        ("amplitude", ctypes.POINTER(ctypes.c_float)),
        ("amplitude_count", ctypes.c_size_t),
        ("envelopes", ctypes.POINTER(ctypes.c_float)),
        ("envelope_count", ctypes.c_size_t),
    ]


class SonarePercussiveEventConfig(CStruct):
    """Maps to SonarePercussiveEventConfig in sonare_c_effects.h."""

    _fields_ = [
        ("struct_version", ctypes.c_int32),
        ("n_fft", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("hpss_kernel_harmonic", ctypes.c_int32),
        ("hpss_kernel_percussive", ctypes.c_int32),
        ("onset_wait", ctypes.c_int32),
        ("onset_delta", ctypes.c_float),
        ("max_event_ms", ctypes.c_float),
        ("min_percussive_ratio", ctypes.c_float),
    ]


class SonarePercussiveRenderConfig(CStruct):
    """Maps to SonarePercussiveRenderConfig in sonare_c_effects.h."""

    _fields_ = [
        ("struct_version", ctypes.c_int32),
        ("n_fft", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("hpss_kernel_harmonic", ctypes.c_int32),
        ("hpss_kernel_percussive", ctypes.c_int32),
        ("fade_ms", ctypes.c_float),
    ]


class SonarePercussiveEventEdit(CStruct):
    """Maps to SonarePercussiveEventEdit in sonare_c_effects.h."""

    _fields_ = [
        ("time_offset_samples", ctypes.c_int64),
        ("gain_db", ctypes.c_float),
        ("muted", ctypes.c_int32),
    ]


class SonarePercussiveEvent(CStruct):
    """Maps to SonarePercussiveEvent in sonare_c_effects.h."""

    _fields_ = [
        ("onset_sample", ctypes.c_int64),
        ("offset_sample", ctypes.c_int64),
        ("strength", ctypes.c_float),
        ("peak_amplitude", ctypes.c_float),
        ("percussive_ratio", ctypes.c_float),
        ("edit", SonarePercussiveEventEdit),
    ]


class SonarePercussiveEventsResult(CStruct):
    """Maps to SonarePercussiveEventsResult in sonare_c_effects.h."""

    _fields_ = [
        ("events", ctypes.POINTER(SonarePercussiveEvent)),
        ("count", ctypes.c_size_t),
    ]


class SonarePitchDecompositionResult(CStruct):
    """Maps to SonarePitchDecompositionResult in sonare_c_effects.h."""

    _fields_ = [
        ("centre_hz", ctypes.c_float),
        ("drift_cents", ctypes.POINTER(ctypes.c_float)),
        ("vibrato_cents", ctypes.POINTER(ctypes.c_float)),
        ("count", ctypes.c_size_t),
    ]


class SonarePolyphonicConfig(CStruct):
    """Maps to SonarePolyphonicConfig in sonare_c_polyphony.h."""

    _fields_ = [
        ("struct_version", ctypes.c_int32),
        ("n_fft", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("win_length", ctypes.c_int32),
        ("cent_ref_hz", ctypes.c_float),
        ("cents_per_bin", ctypes.c_float),
        ("cent_max_hz", ctypes.c_float),
        ("tonality_off", ctypes.c_int32),
        ("salience_harmonics", ctypes.c_int32),
        ("f0_min_hz", ctypes.c_float),
        ("f0_max_hz", ctypes.c_float),
        ("salience_alpha_hz", ctypes.c_float),
        ("salience_beta_hz", ctypes.c_float),
        ("salience_inharmonicity", ctypes.c_float),
        ("max_polyphony", ctypes.c_int32),
        ("min_frame_peak_ratio", ctypes.c_float),
        ("min_separation_cents", ctypes.c_float),
        ("subtraction_factor", ctypes.c_float),
        ("max_jump_cents", ctypes.c_float),
        ("min_ridge_peak_ratio", ctypes.c_float),
        ("min_ridge_duration_ms", ctypes.c_float),
        ("mask_harmonics", ctypes.c_int32),
        ("claim_lobes", ctypes.c_float),
        ("inharmonicity", ctypes.c_float),
        ("estimate_inharmonicity", ctypes.c_int32),
        ("inharmonicity_min_partials", ctypes.c_int32),
        ("inharmonicity_max_residual_bins", ctypes.c_float),
        ("inharmonicity_max_stretch", ctypes.c_float),
        ("window_frames", ctypes.c_int32),
        ("min_partial_separation", ctypes.c_float),
        ("max_fit_residual", ctypes.c_float),
        ("max_weight_modulus", ctypes.c_float),
        ("max_refine_hz", ctypes.c_float),
        ("f0_tolerance_cents", ctypes.c_float),
        ("segmentation_threshold_cents", ctypes.c_float),
        ("min_note_ms", ctypes.c_float),
        ("reference_hz", ctypes.c_float),
    ]
