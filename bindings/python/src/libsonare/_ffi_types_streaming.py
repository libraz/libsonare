"""ctypes structure and constant definitions for libsonare."""

from __future__ import annotations

import ctypes

SONARE_VC_PRESET_NEUTRAL_MONITOR = 0
SONARE_VC_PRESET_BRIGHT_IDOL = 1
SONARE_VC_PRESET_SOFT_WHISPER = 2
SONARE_VC_PRESET_DEEP_NARRATOR = 3
SONARE_VC_PRESET_ROBOT_MASCOT = 4
SONARE_VC_PRESET_DARK_VILLAIN = 5


class SonareRealtimeVoiceChangerConfig(ctypes.Structure):
    """Flat POD mirror of editing::voice_changer::RealtimeVoiceChangerConfig.

    Layout matches sonare_c.h exactly: 33 float fields + 3 int fields, no
    padding. ``sizeof`` must equal ``36 * sizeof(c_float)`` (ABI version 2).
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
        # Appended in ABI version 2 (must stay at the END to match the C layout).
        ("limiter_enable_isp_limiter", ctypes.c_int),
        ("limiter_isp_ceiling_dbtp", ctypes.c_float),
    ]


class SonareTimbreFrame(ctypes.Structure):
    """Maps to SonareTimbreFrame in sonare_c.h."""

    _fields_ = [
        ("brightness", ctypes.c_float),
        ("warmth", ctypes.c_float),
        ("density", ctypes.c_float),
        ("roughness", ctypes.c_float),
        ("complexity", ctypes.c_float),
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
        ("timbre_over_time", ctypes.POINTER(SonareTimbreFrame)),
        ("timbre_over_time_count", ctypes.c_size_t),
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


class SonareStringArray(ctypes.Structure):
    """Maps to SonareStringArray in sonare_c.h."""

    _fields_ = [
        ("items", ctypes.POINTER(ctypes.c_char_p)),
        ("count", ctypes.c_size_t),
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


__all__ = [name for name in globals() if name.startswith(("Sonare", "SONARE_"))]
