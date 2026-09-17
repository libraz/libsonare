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
SONARE_DENOISE_NOISE_ESTIMATOR_SPP = 3


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


# Length of SonareNoiseDetection.band_floor_dbfs; matches
# SONARE_REPAIR_NOISE_BAND_COUNT in sonare_c_mastering.h.
SONARE_REPAIR_NOISE_BAND_COUNT = 32


class SonareNoiseDetection(CStruct):
    """Maps to SonareNoiseDetection in sonare_c.h."""

    _fields_ = [
        ("floor_dbfs", ctypes.c_float),
        ("band_floor_dbfs", ctypes.c_float * SONARE_REPAIR_NOISE_BAND_COUNT),
    ]


class SonareDenoiseReport(CStruct):
    """Maps to SonareDenoiseReport in sonare_c.h."""

    _fields_ = [
        ("detected", SonareNoiseDetection),
        ("mean_reduction_db", ctypes.c_float),
        ("max_reduction_db", ctypes.c_float),
        ("floor_limited_fraction", ctypes.c_float),
    ]


class SonareDenoiseStereoResult(CStruct):
    """Maps to SonareDenoiseStereoResult in sonare_c.h.

    One report, not a pair: the mask is built from the channel-summed power
    and applied unchanged to both channels.
    """

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("report", SonareDenoiseReport),
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
        ("flat_run_count", ctypes.c_size_t),
        ("longest_flat_run_samples", ctypes.c_size_t),
        ("flat_sample_count", ctypes.c_size_t),
        ("flat_level", ctypes.c_float),
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


class SonareCrackleDetection(CStruct):
    """Maps to SonareCrackleDetection in sonare_c.h."""

    _fields_ = [
        ("sample_count", ctypes.c_size_t),
        ("sample_fraction", ctypes.c_float),
        ("per_second", ctypes.c_float),
    ]


class SonareDecrackleReport(CStruct):
    """Maps to SonareDecrackleReport in sonare_c.h."""

    _fields_ = [
        ("detected", SonareCrackleDetection),
        ("replaced_samples", ctypes.c_size_t),
        ("detail_coefficients", ctypes.c_size_t),
        ("shrunk_coefficients", ctypes.c_size_t),
        ("noise_sigma", ctypes.c_float),
    ]


class SonareDecrackleStereoResult(CStruct):
    """Maps to SonareDecrackleStereoResult in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("left_report", SonareDecrackleReport),
        ("right_report", SonareDecrackleReport),
    ]


# SonareDehumConfig.mode values.
SONARE_DEHUM_MODE_SUBTRACT = 0
SONARE_DEHUM_MODE_NOTCH = 1


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
        ("mode", ctypes.c_int),
    ]


# Length of SonareHumDetection.harmonic_dbfs; matches SONARE_DEHUM_MAX_HARMONICS
# in sonare_c_mastering.h.
SONARE_DEHUM_MAX_HARMONICS = 16


class SonareHumDetection(CStruct):
    """Maps to SonareHumDetection in sonare_c.h."""

    _fields_ = [
        ("fundamental_hz", ctypes.c_float),
        ("fundamental_prominence", ctypes.c_float),
        ("harmonics", ctypes.c_int),
        ("harmonic_dbfs", ctypes.c_float * SONARE_DEHUM_MAX_HARMONICS),
    ]


class SonareDehumReport(CStruct):
    """Maps to SonareDehumReport in sonare_c.h."""

    _fields_ = [
        ("detected", SonareHumDetection),
        ("notched_harmonics", ctypes.c_int),
        ("applied_fundamental_hz", ctypes.c_float),
        ("fundamental_drift_hz", ctypes.c_float),
    ]


class SonareDehumStereoResult(CStruct):
    """Maps to SonareDehumStereoResult in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("left_report", SonareDehumReport),
        ("right_report", SonareDehumReport),
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


class SonareReverbDetection(CStruct):
    """Maps to SonareReverbDetection in sonare_c.h."""

    _fields_ = [
        ("late_decay_ratio_db", ctypes.c_float),
        ("late_predictability", ctypes.c_float),
    ]


class SonareDereverbReport(CStruct):
    """Maps to SonareDereverbReport in sonare_c.h."""

    _fields_ = [
        ("detected", SonareReverbDetection),
        ("mean_reduction_db", ctypes.c_float),
        ("suppressed_fraction", ctypes.c_float),
        ("wpe_predictor_norm", ctypes.c_float),
    ]


class SonareDereverbStereoResult(CStruct):
    """Maps to SonareDereverbStereoResult in sonare_c.h.

    One report, not a pair: the mask and the WPE predictors are both built
    across the two channels and applied unchanged to each.
    """

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("report", SonareDereverbReport),
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


class SonareTrimRange(CStruct):
    """Maps to SonareTrimRange in sonare_c.h. Half-open, input-buffer coordinates."""

    _fields_ = [
        ("first", ctypes.c_size_t),
        ("last_exclusive", ctypes.c_size_t),
    ]


class SonareTrimReport(CStruct):
    """Maps to SonareTrimReport in sonare_c.h.

    A pass that kept nothing reports the range ``(length, length)``, which puts
    the whole buffer in ``removed_head_samples`` and leaves the tail at 0.
    """

    _fields_ = [
        ("range", SonareTrimRange),
        ("removed_head_samples", ctypes.c_size_t),
        ("removed_tail_samples", ctypes.c_size_t),
    ]


class SonareTrimSilenceStereoResult(CStruct):
    """Maps to SonareTrimSilenceStereoResult in sonare_c.h.

    ``length`` is an OUTPUT length, not an echo of the call's input length as it
    is on every other repair stereo result: trimming shortens the pair. When
    neither channel carries signal both pointers are NULL and ``length`` is 0,
    which is a success rather than a refusal.

    One report plus two ranges: ``report.range`` is the union applied to both
    channels, and ``left_range`` / ``right_range`` are the per-channel scans it
    was formed from.
    """

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("report", SonareTrimReport),
        ("left_range", SonareTrimRange),
        ("right_range", SonareTrimRange),
    ]


class SonareNormalizeStereoResult(CStruct):
    """Maps to SonareNormalizeStereoResult in sonare_c_effects.h.

    One ``applied_gain_db`` rather than a per-channel pair: the level is measured
    across both channels and the one gain goes to both, so the field would
    otherwise read as though the two could differ. A silent pair comes back
    untouched with the gain at 0.
    """

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("applied_gain_db", ctypes.c_float),
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
