"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_core_signatures(lib: ctypes.CDLL) -> None:
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

    # sonare_decode_channels / sonare_downmix
    # Additive to the audio ABI; optional for the same reason as the channel-count
    # probe below.
    lib.sonare_decode_channels.restype = ctypes.c_int32
    lib.sonare_decode_channels.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_downmix_options_init.restype = None
    lib.sonare_downmix_options_init.argtypes = [ctypes.POINTER(SonareDownmixOptions)]
    lib.sonare_downmix.restype = ctypes.c_int32
    lib.sonare_downmix.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDownmixOptions),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
    ]

    # sonare_audio_from_file
    lib.sonare_audio_from_file.restype = ctypes.c_int32
    lib.sonare_audio_from_file.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    # sonare_audio_from_file_channel
    # Optional for the same reason as the channel-count probe below.
    lib.sonare_audio_from_file_channel.restype = ctypes.c_int32
    lib.sonare_audio_from_file_channel.argtypes = [
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    # sonare_audio_file_num_channels
    # This is additive to the audio-file ABI. Keep the signature optional so
    # callers that deliberately load an older feature-gated library still get
    # the usual runtime "missing capability" error instead of a load-time
    # AttributeError while configuring unrelated symbols.
    lib.sonare_audio_file_num_channels.restype = ctypes.c_int32
    lib.sonare_audio_file_num_channels.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_audio_destroy
    lib.sonare_audio_destroy.restype = None
    lib.sonare_audio_destroy.argtypes = [ctypes.c_void_p]

    # sonare_audio_data
    lib.sonare_audio_data.restype = ctypes.POINTER(ctypes.c_float)
    lib.sonare_audio_data.argtypes = [ctypes.c_void_p]

    # sonare_audio_length
    lib.sonare_audio_length.restype = ctypes.c_size_t
    lib.sonare_audio_length.argtypes = [ctypes.c_void_p]

    # sonare_audio_sample_rate
    lib.sonare_audio_sample_rate.restype = ctypes.c_int
    lib.sonare_audio_sample_rate.argtypes = [ctypes.c_void_p]

    # sonare_audio_duration_sec
    lib.sonare_audio_duration_sec.restype = ctypes.c_float
    lib.sonare_audio_duration_sec.argtypes = [ctypes.c_void_p]

    lib.sonare_audio_detect_bpm.restype = ctypes.c_int32
    lib.sonare_audio_detect_bpm.argtypes = [
        ctypes.c_void_p,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_float),
    ]

    lib.sonare_audio_detect_key.restype = ctypes.c_int32
    lib.sonare_audio_detect_key.argtypes = [ctypes.c_void_p, ctypes.POINTER(SonareKey)]

    lib.sonare_audio_detect_beats.restype = ctypes.c_int32
    lib.sonare_audio_detect_beats.argtypes = [
        ctypes.c_void_p,
        ctypes.c_float,
        ctypes.c_float,
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

    # --- Metering (handle form) ---

    lib.sonare_audio_peak_db.restype = ctypes.c_int32
    lib.sonare_audio_peak_db.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]

    lib.sonare_audio_rms_db.restype = ctypes.c_int32
    lib.sonare_audio_rms_db.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]

    lib.sonare_audio_dc_offset.restype = ctypes.c_int32
    lib.sonare_audio_dc_offset.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]

    lib.sonare_audio_crest_factor_db.restype = ctypes.c_int32
    lib.sonare_audio_crest_factor_db.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]

    lib.sonare_audio_silence_ratio.restype = ctypes.c_int32
    lib.sonare_audio_silence_ratio.argtypes = [
        ctypes.c_void_p,
        ctypes.c_float,
        ctypes.c_size_t,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_float),
    ]

    lib.sonare_audio_true_peak_dbtp.restype = ctypes.c_int32
    lib.sonare_audio_true_peak_dbtp.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
    ]

    lib.sonare_audio_detect_clipping.restype = ctypes.c_int32
    lib.sonare_audio_detect_clipping.argtypes = [
        ctypes.c_void_p,
        ctypes.c_float,
        ctypes.c_size_t,
        ctypes.POINTER(SonareClippingResult),
    ]

    lib.sonare_audio_dynamic_range.restype = ctypes.c_int32
    lib.sonare_audio_dynamic_range.argtypes = [
        ctypes.c_void_p,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareDynamicRangeResult),
    ]

    lib.sonare_audio_spectrum.restype = ctypes.c_int32
    lib.sonare_audio_spectrum.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareSpectrumResult),
    ]

    lib.sonare_audio_spectrum_frame.restype = ctypes.c_int32
    lib.sonare_audio_spectrum_frame.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareSpectrumResult),
    ]

    lib.sonare_audio_ebur128_loudness_range_lu.restype = ctypes.c_int32
    lib.sonare_audio_ebur128_loudness_range_lu.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
    ]

    # --- Quick detection functions ---

    # sonare_detect_bpm
    lib.sonare_detect_bpm.restype = ctypes.c_int32
    lib.sonare_detect_bpm.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_float),
    ]

    lib.sonare_detect_key.restype = ctypes.c_int32
    lib.sonare_detect_key.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDetectKeyOptions),
        ctypes.POINTER(SonareKey),
        ctypes.POINTER(ctypes.c_float),
    ]
    lib.sonare_detect_key_options_init.restype = None
    lib.sonare_detect_key_options_init.argtypes = [ctypes.POINTER(SonareDetectKeyOptions)]

    lib.sonare_detect_key_candidates.restype = ctypes.c_int32
    lib.sonare_detect_key_candidates.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDetectKeyOptions),
        ctypes.POINTER(ctypes.POINTER(SonareKeyCandidate)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_float),
    ]

    lib.sonare_free_key_candidates.restype = None
    lib.sonare_free_key_candidates.argtypes = [ctypes.POINTER(SonareKeyCandidate)]

    # sonare_detect_beats
    lib.sonare_detect_beats.restype = ctypes.c_int32
    lib.sonare_detect_beats.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
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

    lib.sonare_detect_onsets.restype = ctypes.c_int32
    lib.sonare_detect_onsets.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareOnsetDetectConfig),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_onset_detect_config_init.restype = None
    lib.sonare_onset_detect_config_init.argtypes = [ctypes.POINTER(SonareOnsetDetectConfig)]

    # --- Full analysis ---

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
        ctypes.POINTER(SonareAnalyzeImpulseResponseOptions),
        ctypes.POINTER(SonareAcousticResult),
    ]
    lib.sonare_analyze_impulse_response_options_init.restype = None
    lib.sonare_analyze_impulse_response_options_init.argtypes = [
        ctypes.POINTER(SonareAnalyzeImpulseResponseOptions)
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
        ctypes.POINTER(SonareChordDetectionOptions),
        ctypes.POINTER(SonareChordAnalysisResult),
    ]

    lib.sonare_chord_detection_options_init.restype = None
    lib.sonare_chord_detection_options_init.argtypes = [ctypes.POINTER(SonareChordDetectionOptions)]

    lib.sonare_chord_functional_analysis.restype = ctypes.c_int32
    lib.sonare_chord_functional_analysis.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareChordDetectionOptions),
        ctypes.c_int32,
        ctypes.c_int32,
        ctypes.POINTER(SonareChordAnalysisResult),
        ctypes.POINTER(SonareStringArray),
        ctypes.POINTER(SonareStringArray),
    ]

    lib.sonare_chord_functions.restype = ctypes.c_int32
    lib.sonare_chord_functions.argtypes = [
        ctypes.POINTER(SonareChord),
        ctypes.c_size_t,
        ctypes.c_int32,
        ctypes.c_int32,
        ctypes.POINTER(SonareStringArray),
        ctypes.POINTER(SonareStringArray),
    ]

    lib.sonare_material_preset_name.restype = ctypes.c_char_p
    lib.sonare_material_preset_name.argtypes = [ctypes.c_int]
    lib.sonare_acoustic_mode_name.restype = ctypes.c_char_p
    lib.sonare_acoustic_mode_name.argtypes = [ctypes.c_int]
    lib.sonare_rir_synth_config_init.restype = None
    lib.sonare_rir_synth_config_init.argtypes = [ctypes.POINTER(SonareRirSynthConfig)]
    lib.sonare_room_morph_config_init.restype = None
    lib.sonare_room_morph_config_init.argtypes = [ctypes.POINTER(SonareRoomMorphConfig)]
    lib.sonare_room_geometry_from_estimate.restype = ctypes.c_int32
    lib.sonare_room_geometry_from_estimate.argtypes = [
        ctypes.POINTER(SonareRoomEstimate),
        ctypes.POINTER(SonareRirSynthConfig),
    ]

    lib.sonare_synthesize_rir.restype = ctypes.c_int32
    lib.sonare_synthesize_rir.argtypes = [
        ctypes.POINTER(SonareRirSynthConfig),
        ctypes.c_int,
        ctypes.POINTER(SonareRirSynthResult),
    ]
    lib.sonare_free_rir_synth_result.restype = None
    lib.sonare_free_rir_synth_result.argtypes = [ctypes.POINTER(SonareRirSynthResult)]

    lib.sonare_estimate_room.restype = ctypes.c_int32
    lib.sonare_estimate_room.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareRoomEstimateConfig),
        ctypes.POINTER(SonareRoomEstimate),
    ]
    lib.sonare_free_room_estimate.restype = None
    lib.sonare_free_room_estimate.argtypes = [ctypes.POINTER(SonareRoomEstimate)]

    lib.sonare_room_morph.restype = ctypes.c_int32
    lib.sonare_room_morph.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareRoomMorphConfig),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_room_morph_stereo.restype = ctypes.c_int32
    lib.sonare_room_morph_stereo.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareRoomMorphConfig),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_section_options_init.restype = None
    lib.sonare_section_options_init.argtypes = [ctypes.POINTER(SonareSectionOptions)]
    lib.sonare_analyze_sections.restype = ctypes.c_int32
    lib.sonare_analyze_sections.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareSectionOptions),
        ctypes.POINTER(SonareSectionResult),
    ]
    lib.sonare_free_section_result.restype = None
    lib.sonare_free_section_result.argtypes = [ctypes.POINTER(SonareSectionResult)]

    lib.sonare_boundary_options_init.restype = None
    lib.sonare_boundary_options_init.argtypes = [ctypes.POINTER(SonareBoundaryOptions)]
    lib.sonare_detect_boundaries.restype = ctypes.c_int32
    lib.sonare_detect_boundaries.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareBoundaryOptions),
        ctypes.POINTER(SonareBoundaryResult),
    ]
    lib.sonare_free_boundary_result.restype = None
    lib.sonare_free_boundary_result.argtypes = [ctypes.POINTER(SonareBoundaryResult)]

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
        ctypes.POINTER(SonareAnalyzeMelodyOptions),
        ctypes.POINTER(SonareMelodyResult),
    ]
    lib.sonare_analyze_melody_options_init.restype = None
    lib.sonare_analyze_melody_options_init.argtypes = [ctypes.POINTER(SonareAnalyzeMelodyOptions)]
    lib.sonare_free_melody_result.restype = None
    lib.sonare_free_melody_result.argtypes = [ctypes.POINTER(SonareMelodyResult)]

    lib.sonare_cqt.restype = ctypes.c_int32
    lib.sonare_cqt.argtypes = [
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
    lib.sonare_pseudo_cqt.restype = ctypes.c_int32
    lib.sonare_pseudo_cqt.argtypes = [
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
    lib.sonare_hybrid_cqt.restype = ctypes.c_int32
    lib.sonare_hybrid_cqt.argtypes = [
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
        ctypes.c_float,
        ctypes.POINTER(SonareCqtResult),
    ]
    lib.sonare_free_cqt_result.restype = None
    lib.sonare_free_cqt_result.argtypes = [ctypes.POINTER(SonareCqtResult)]
    lib.sonare_cqt_to_audio.restype = ctypes.c_int32
    lib.sonare_cqt_to_audio.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_vqt_to_audio.restype = ctypes.c_int32
    lib.sonare_vqt_to_audio.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Features - Inverse reconstruction ---

    lib.sonare_mel_to_stft.restype = ctypes.c_int32
    lib.sonare_mel_to_stft.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareMelToStftOptions),
        ctypes.POINTER(SonareInverseResult),
    ]
    lib.sonare_mel_to_stft_options_init.restype = None
    lib.sonare_mel_to_stft_options_init.argtypes = [ctypes.POINTER(SonareMelToStftOptions)]
    lib.sonare_mel_to_audio.restype = ctypes.c_int32
    lib.sonare_mel_to_audio.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(SonareMelToAudioOptions),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_mel_to_audio_options_init.restype = None
    lib.sonare_mel_to_audio_options_init.argtypes = [ctypes.POINTER(SonareMelToAudioOptions)]
    lib.sonare_griffin_lim.restype = ctypes.c_int32
    lib.sonare_griffin_lim.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_mfcc_to_mel.restype = ctypes.c_int32
    lib.sonare_mfcc_to_mel.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareMfccToMelOptions),
        ctypes.POINTER(SonareInverseResult),
    ]
    lib.sonare_mfcc_to_mel_options_init.restype = None
    lib.sonare_mfcc_to_mel_options_init.argtypes = [ctypes.POINTER(SonareMfccToMelOptions)]
    lib.sonare_mfcc_to_audio.restype = ctypes.c_int32
    lib.sonare_mfcc_to_audio.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(SonareMfccToAudioOptions),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_mfcc_to_audio_options_init.restype = None
    lib.sonare_mfcc_to_audio_options_init.argtypes = [ctypes.POINTER(SonareMfccToAudioOptions)]
    lib.sonare_free_inverse_result.restype = None
    lib.sonare_free_inverse_result.argtypes = [ctypes.POINTER(SonareInverseResult)]

    # --- Streaming - StreamAnalyzer ---

    lib.sonare_stream_config_init.restype = None
    lib.sonare_stream_config_init.argtypes = [ctypes.POINTER(SonareStreamConfig)]
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
    lib.sonare_stream_analyzer_finalize.restype = ctypes.c_int32
    lib.sonare_stream_analyzer_finalize.argtypes = [ctypes.c_void_p]
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
    lib.sonare_stream_analyzer_read_frames_u8.restype = ctypes.c_int32
    lib.sonare_stream_analyzer_read_frames_u8.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareStreamQuantizeConfig),
        ctypes.c_size_t,
        ctypes.POINTER(SonareStreamFramesU8),
    ]
    lib.sonare_stream_analyzer_read_frames_i16.restype = ctypes.c_int32
    lib.sonare_stream_analyzer_read_frames_i16.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareStreamQuantizeConfig),
        ctypes.c_size_t,
        ctypes.POINTER(SonareStreamFramesI16),
    ]
    lib.sonare_stream_quantize_config_init.restype = None
    lib.sonare_stream_quantize_config_init.argtypes = [ctypes.POINTER(SonareStreamQuantizeConfig)]
    lib.sonare_stream_analyzer_reset.restype = ctypes.c_int32
    lib.sonare_stream_analyzer_reset.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.sonare_stream_analyzer_stats.restype = ctypes.c_int32
    lib.sonare_stream_analyzer_stats.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareStreamStats),
    ]
    lib.sonare_free_stream_stats.restype = None
    lib.sonare_free_stream_stats.argtypes = [ctypes.POINTER(SonareStreamStats)]
    lib.sonare_stream_analyzer_frame_count.restype = ctypes.c_int32
    lib.sonare_stream_analyzer_frame_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_stream_analyzer_current_time_sec.restype = ctypes.c_int32
    lib.sonare_stream_analyzer_current_time_sec.argtypes = [
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
    lib.sonare_stream_analyzer_set_reference_hz.restype = ctypes.c_int32
    lib.sonare_stream_analyzer_set_reference_hz.argtypes = [
        ctypes.c_void_p,
        ctypes.c_float,
    ]
    lib.sonare_free_stream_frames.restype = None
    lib.sonare_free_stream_frames.argtypes = [ctypes.POINTER(SonareStreamFrames)]
    lib.sonare_free_stream_frames_u8.restype = None
    lib.sonare_free_stream_frames_u8.argtypes = [ctypes.POINTER(SonareStreamFramesU8)]
    lib.sonare_free_stream_frames_i16.restype = None
    lib.sonare_free_stream_frames_i16.argtypes = [ctypes.POINTER(SonareStreamFramesI16)]

    # --- Memory management ---

    # sonare_free_floats
    lib.sonare_free_floats.restype = None
    lib.sonare_free_floats.argtypes = [ctypes.POINTER(ctypes.c_float)]

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

    lib.sonare_free_string_array.restype = None
    lib.sonare_free_string_array.argtypes = [ctypes.POINTER(SonareStringArray)]

    # --- Error handling ---

    # sonare_error_message
    lib.sonare_error_message.restype = ctypes.c_char_p
    lib.sonare_error_message.argtypes = [ctypes.c_int32]

    # sonare_last_error_message: returns the detailed thread-local message for the
    # most recent error. Returns an empty (but non-NULL) string when nothing has been
    # recorded. Only meaningful when a preceding C API call returned non-OK.
    lib.sonare_last_error_message.restype = ctypes.c_char_p
    lib.sonare_last_error_message.argtypes = []

    # The subjects of the last parameter refusal, and its message rendered with others
    # in their place; a facade that flattens a struct drops the struct's root.
    lib.sonare_last_refusal_subject_count.restype = ctypes.c_size_t
    lib.sonare_last_refusal_subject_count.argtypes = []
    lib.sonare_last_refusal_subject.restype = ctypes.c_char_p
    lib.sonare_last_refusal_subject.argtypes = [ctypes.c_size_t]
    lib.sonare_last_refusal_entry.restype = ctypes.c_char_p
    lib.sonare_last_refusal_entry.argtypes = []
    lib.sonare_last_refusal_render.restype = ctypes.c_int32
    lib.sonare_last_refusal_render.argtypes = [
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(ctypes.c_double),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_char_p),
    ]

    # sonare_last_error_code: the thread-local code recorded beside the message, so a
    # pointer-returning constructor that returned NULL still carries a real code.
    lib.sonare_last_error_code.restype = ctypes.c_int
    lib.sonare_last_error_code.argtypes = []

    # The warning channel: the diagnostics the last call published, each with its
    # own code, message and severity.
    lib.sonare_last_diagnostic_count.restype = ctypes.c_size_t
    lib.sonare_last_diagnostic_count.argtypes = []
    lib.sonare_diagnostic_init.restype = None
    lib.sonare_diagnostic_init.argtypes = [ctypes.POINTER(SonareDiagnostic)]
    lib.sonare_last_diagnostic.restype = ctypes.c_int
    lib.sonare_last_diagnostic.argtypes = [ctypes.c_size_t, ctypes.POINTER(SonareDiagnostic)]

    # --- Version ---

    # sonare_version
    lib.sonare_version.restype = ctypes.c_char_p
    lib.sonare_version.argtypes = []

    # sonare_capabilities_json: build and platform descriptor, caller-owned
    # (released with sonare_free_string).
    lib.sonare_capabilities_json.restype = ctypes.c_int32
    lib.sonare_capabilities_json.argtypes = [ctypes.POINTER(ctypes.c_char_p)]

    # sonare_system_info_json: host cpu/memory/parallel document, caller-owned
    # (released with sonare_free_string).
    lib.sonare_system_info_json.restype = ctypes.c_int32
    lib.sonare_system_info_json.argtypes = [ctypes.POINTER(ctypes.c_char_p)]

    # sonare_abi_version: aggregate ABI version folding every subsystem ABI macro
    # into one 32-bit value (see sonare_c.h).
    lib.sonare_abi_version.restype = ctypes.c_uint32
    lib.sonare_abi_version.argtypes = []

    lib.sonare_engine_abi_version.restype = ctypes.c_uint32
    lib.sonare_engine_abi_version.argtypes = []

    lib.sonare_voice_changer_abi_version.restype = ctypes.c_uint32
    lib.sonare_voice_changer_abi_version.argtypes = []

    # sonare_has_feature: 1 if the named capabilities() feature key is compiled in.
    lib.sonare_has_feature.restype = ctypes.c_int
    lib.sonare_has_feature.argtypes = [ctypes.c_char_p]

    # --- Full analysis JSON ---

    # sonare_analyze_json: full analysis serialized to a heap-allocated camelCase
    # JSON string. Free *out_json with sonare_free_string.
    lib.sonare_analyze_json.restype = ctypes.c_int32
    lib.sonare_analyze_json.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareMusicAnalyzeOptions),
        ctypes.POINTER(ctypes.c_char_p),
    ]
    lib.sonare_music_analyze_options_init.restype = None
    lib.sonare_music_analyze_options_init.argtypes = [ctypes.POINTER(SonareMusicAnalyzeOptions)]

    # sonare_estimate_meter_json: meter estimation over a caller-supplied beat
    # series, with no audio and no re-analysis. Free *out_json with
    # sonare_free_string.
    lib.sonare_meter_options_init.restype = None
    lib.sonare_meter_options_init.argtypes = [ctypes.POINTER(SonareMeterOptions)]
    lib.sonare_estimate_meter_json.restype = ctypes.c_int32
    lib.sonare_estimate_meter_json.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.POINTER(SonareMeterOptions),
        ctypes.POINTER(ctypes.c_char_p),
    ]

    # sonare_free_string: free a heap-allocated C string (e.g. JSON output).
    lib.sonare_free_string.restype = None
    lib.sonare_free_string.argtypes = [ctypes.c_char_p]
