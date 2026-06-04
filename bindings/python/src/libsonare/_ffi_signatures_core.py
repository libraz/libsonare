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

    lib.sonare_chord_functional_analysis.restype = ctypes.c_int32
    lib.sonare_chord_functional_analysis.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareChordDetectionOptions),
        ctypes.c_int32,
        ctypes.c_int32,
        ctypes.POINTER(SonareStringArray),
    ]

    if hasattr(lib, "sonare_synthesize_rir"):
        lib.sonare_synthesize_rir.restype = ctypes.c_int32
        lib.sonare_synthesize_rir.argtypes = [
            ctypes.POINTER(SonareRirSynthConfig),
            ctypes.c_int,
            ctypes.POINTER(SonareRirSynthResult),
        ]
        lib.sonare_free_rir_synth_result.restype = None
        lib.sonare_free_rir_synth_result.argtypes = [ctypes.POINTER(SonareRirSynthResult)]

    if hasattr(lib, "sonare_estimate_room"):
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

    if hasattr(lib, "sonare_room_morph"):
        lib.sonare_room_morph.restype = ctypes.c_int32
        lib.sonare_room_morph.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareRoomMorphConfig),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
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
        lib.sonare_pseudo_cqt.restype = ctypes.c_int32
        lib.sonare_pseudo_cqt.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
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
        if hasattr(lib, "sonare_stream_analyzer_finalize"):
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
        if hasattr(lib, "sonare_stream_quantize_config_default"):
            lib.sonare_stream_quantize_config_default.restype = ctypes.c_int32
            lib.sonare_stream_quantize_config_default.argtypes = [
                ctypes.POINTER(SonareStreamQuantizeConfig),
            ]
        if hasattr(lib, "sonare_stream_analyzer_read_frames_u8_ex"):
            lib.sonare_stream_analyzer_read_frames_u8_ex.restype = ctypes.c_int32
            lib.sonare_stream_analyzer_read_frames_u8_ex.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareStreamQuantizeConfig),
                ctypes.c_size_t,
                ctypes.POINTER(SonareStreamFramesU8),
            ]
        if hasattr(lib, "sonare_stream_analyzer_read_frames_i16_ex"):
            lib.sonare_stream_analyzer_read_frames_i16_ex.restype = ctypes.c_int32
            lib.sonare_stream_analyzer_read_frames_i16_ex.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareStreamQuantizeConfig),
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

    # --- Version ---

    # sonare_version
    lib.sonare_version.restype = ctypes.c_char_p
    lib.sonare_version.argtypes = []

    # sonare_abi_version: aggregate ABI version folding every subsystem ABI macro
    # into one 32-bit value (see sonare_c.h).
    if hasattr(lib, "sonare_abi_version"):
        lib.sonare_abi_version.restype = ctypes.c_uint32
        lib.sonare_abi_version.argtypes = []

    lib.sonare_engine_abi_version.restype = ctypes.c_uint32
    lib.sonare_engine_abi_version.argtypes = []

    lib.sonare_voice_changer_abi_version.restype = ctypes.c_uint32
    lib.sonare_voice_changer_abi_version.argtypes = []

    # sonare_has_ffmpeg_support: returns 1 if the shared library was compiled
    # with FFmpeg-backed decoding for M4A/AAC/FLAC/OGG, 0 otherwise.
    lib.sonare_has_ffmpeg_support.restype = ctypes.c_int
    lib.sonare_has_ffmpeg_support.argtypes = []

    # --- Full analysis JSON ---

    # sonare_analyze_json: full analysis serialized to a heap-allocated camelCase
    # JSON string. Free *out_json with sonare_free_string.
    if hasattr(lib, "sonare_analyze_json"):
        lib.sonare_analyze_json.restype = ctypes.c_int32
        lib.sonare_analyze_json.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_char_p),
        ]

    # sonare_analyze_json_with_progress: same as sonare_analyze_json but emits
    # progress callbacks. The callback signature is:
    #   void (*)(float progress, const char* stage, void* user_data)
    if hasattr(lib, "sonare_analyze_json_with_progress"):
        lib.sonare_analyze_json_with_progress.restype = ctypes.c_int32
        lib.sonare_analyze_json_with_progress.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            SonareAnalyzeProgressCallback,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char_p),
        ]

    # sonare_analyze_melody_ex: melody contour with selectable tracker (YIN/pYIN)
    # and center-padding option.
    if hasattr(lib, "sonare_analyze_melody_ex"):
        lib.sonare_analyze_melody_ex.restype = ctypes.c_int32
        lib.sonare_analyze_melody_ex.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(SonareMelodyResult),
        ]

    # sonare_free_string: free a heap-allocated C string (e.g. JSON output).
    if hasattr(lib, "sonare_free_string"):
        lib.sonare_free_string.restype = None
        lib.sonare_free_string.argtypes = [ctypes.c_char_p]
