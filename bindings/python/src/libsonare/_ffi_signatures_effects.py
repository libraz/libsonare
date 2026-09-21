"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes
from typing import Any

from ._ffi_types import *  # noqa: F403,F405


def configure_effects_signatures(lib: ctypes.CDLL) -> None:
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
    if hasattr(lib, "sonare_hpss_ex"):
        lib.sonare_hpss_ex.restype = ctypes.c_int32
        lib.sonare_hpss_ex.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(SonareHpssResult),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
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
    if hasattr(lib, "sonare_time_stretch_ex"):
        lib.sonare_time_stretch_ex.restype = ctypes.c_int32
        lib.sonare_time_stretch_ex.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    # sonare_spectral_edit
    lib.sonare_spectral_edit.restype = ctypes.c_int32
    lib.sonare_spectral_edit.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareSpectralEditConfig),
        ctypes.POINTER(SonareSpectralRegionOp),
        ctypes.c_size_t,
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
    if hasattr(lib, "sonare_pitch_shift_ex"):
        lib.sonare_pitch_shift_ex.restype = ctypes.c_int32
        lib.sonare_pitch_shift_ex.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
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

    # sonare_pitch_correct_to_midi_timevarying
    if hasattr(lib, "sonare_pitch_correct_to_midi_timevarying"):
        lib.sonare_pitch_correct_to_midi_timevarying.restype = ctypes.c_int32
        lib.sonare_pitch_correct_to_midi_timevarying.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_int32),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    # sonare_pitch_correction_config_default + sonare_pitch_correct_timevarying
    if hasattr(lib, "sonare_pitch_correct_timevarying"):
        lib.sonare_pitch_correction_config_default.restype = ctypes.c_int32
        lib.sonare_pitch_correction_config_default.argtypes = [
            ctypes.POINTER(SonarePitchCorrectionConfig),
        ]
        lib.sonare_pitch_correct_timevarying.restype = ctypes.c_int32
        lib.sonare_pitch_correct_timevarying.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_int32),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonarePitchCorrectionConfig),
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

    # sonare_note_move
    lib.sonare_note_move.restype = ctypes.c_int32
    lib.sonare_note_move.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_extract_notes + sonare_free_note_objects + sonare_render_notes
    if hasattr(lib, "sonare_extract_notes"):
        lib.sonare_extract_notes.restype = ctypes.c_int32
        lib.sonare_extract_notes.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_int32),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.POINTER(SonareNoteExtractorConfig),
            ctypes.POINTER(SonareNoteObjectsResult),
        ]
        lib.sonare_free_note_objects.restype = None
        lib.sonare_free_note_objects.argtypes = [ctypes.POINTER(SonareNoteObjectsResult)]
        lib.sonare_render_notes.restype = ctypes.c_int32
        lib.sonare_render_notes.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareNoteObject),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.POINTER(SonareNoteRenderConfig),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    # sonare_note_targets_from_smf + sonare_free_note_targets. The SMF reader
    # carries the arrangement gate, not the pitch-editor one the assignment
    # below is behind, so the two are guarded apart.
    if hasattr(lib, "sonare_note_targets_from_smf"):
        lib.sonare_note_targets_from_smf.restype = ctypes.c_int32
        lib.sonare_note_targets_from_smf.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.POINTER(SonareNoteTarget)),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_free_note_targets.restype = None
        lib.sonare_free_note_targets.argtypes = [ctypes.POINTER(SonareNoteTarget)]

    # sonare_note_target_assign_config_default + sonare_assign_note_targets
    if hasattr(lib, "sonare_assign_note_targets"):
        lib.sonare_note_target_assign_config_default.restype = ctypes.c_int32
        lib.sonare_note_target_assign_config_default.argtypes = [
            ctypes.POINTER(SonareNoteTargetAssignConfig)
        ]
        lib.sonare_assign_note_targets.restype = ctypes.c_int32
        lib.sonare_assign_note_targets.argtypes = [
            ctypes.POINTER(SonareNoteObject),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareNoteTarget),
            ctypes.c_size_t,
            ctypes.POINTER(SonareNoteTargetAssignConfig),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    # sonare_polyphonic_* (the handle door onto the same note objects)
    if hasattr(lib, "sonare_polyphonic_analyze"):
        lib.sonare_polyphonic_analyze.restype = ctypes.c_int32
        lib.sonare_polyphonic_analyze.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonarePolyphonicConfig),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        lib.sonare_polyphonic_analysis_destroy.restype = None
        lib.sonare_polyphonic_analysis_destroy.argtypes = [ctypes.c_void_p]
        lib.sonare_polyphonic_note_count.restype = ctypes.c_int32
        lib.sonare_polyphonic_note_count.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_polyphonic_frame_count.restype = ctypes.c_int32
        lib.sonare_polyphonic_frame_count.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int32),
        ]
        lib.sonare_polyphonic_notes.restype = ctypes.c_int32
        lib.sonare_polyphonic_notes.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareNoteObject),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_polyphonic_set_note_edit.restype = ctypes.c_int32
        lib.sonare_polyphonic_set_note_edit.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.POINTER(SonareNoteEdit),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
        ]
        lib.sonare_polyphonic_polyphony.restype = ctypes.c_int32
        lib.sonare_polyphonic_polyphony.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int32),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        for curve in (
            "sonare_polyphonic_note_f0",
            "sonare_polyphonic_note_amplitude",
            "sonare_polyphonic_note_salience",
            "sonare_polyphonic_note_envelope",
        ):
            getattr(lib, curve).restype = ctypes.c_int32
            getattr(lib, curve).argtypes = [
                ctypes.c_void_p,
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_size_t),
            ]
        # Whole-array out like the polyphony above rather than per-note like the
        # curves: one entry per note, so it takes no note index.
        lib.sonare_polyphonic_note_inharmonicity.restype = ctypes.c_int32
        lib.sonare_polyphonic_note_inharmonicity.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_polyphonic_render.restype = ctypes.c_int32
        lib.sonare_polyphonic_render.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareNoteRenderConfig),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    # sonare_transcribe + sonare_free_transcribe_result. The defaults accessor
    # returns the config by value; the facade spells a default as None and
    # leaves the field 0, so it is declared rather than called.
    if hasattr(lib, "sonare_transcribe"):
        lib.sonare_transcribe_config_default.restype = SonareTranscribeConfig
        lib.sonare_transcribe_config_default.argtypes = []
        lib.sonare_transcribe.restype = ctypes.c_int32
        lib.sonare_transcribe.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.POINTER(SonareTranscribeConfig),
            ctypes.POINTER(SonareTranscribeResult),
        ]
        lib.sonare_free_transcribe_result.restype = None
        lib.sonare_free_transcribe_result.argtypes = [ctypes.POINTER(SonareTranscribeResult)]

    # sonare_extract_percussive_events + sonare_free_percussive_events
    # + sonare_render_percussive_events
    if hasattr(lib, "sonare_extract_percussive_events"):
        lib.sonare_extract_percussive_events.restype = ctypes.c_int32
        lib.sonare_extract_percussive_events.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonarePercussiveEventConfig),
            ctypes.POINTER(SonarePercussiveEventsResult),
        ]
        lib.sonare_free_percussive_events.restype = None
        lib.sonare_free_percussive_events.argtypes = [
            ctypes.POINTER(SonarePercussiveEventsResult),
        ]
        lib.sonare_render_percussive_events.restype = ctypes.c_int32
        lib.sonare_render_percussive_events.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonarePercussiveEvent),
            ctypes.c_size_t,
            ctypes.POINTER(SonarePercussiveRenderConfig),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    # sonare_decompose_note_pitch + sonare_free_pitch_decomposition
    if hasattr(lib, "sonare_decompose_note_pitch"):
        lib.sonare_decompose_note_pitch.restype = ctypes.c_int32
        lib.sonare_decompose_note_pitch.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.POINTER(SonarePitchDecompositionResult),
        ]
        lib.sonare_free_pitch_decomposition.restype = None
        lib.sonare_free_pitch_decomposition.argtypes = [
            ctypes.POINTER(SonarePitchDecompositionResult),
        ]

    # sonare_split_note + sonare_merge_notes. The two take the same arguments
    # apart from the cut they describe, so only the tail differs.
    _note_set_edit_argtypes: list[Any] = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.POINTER(SonareNoteExtractorConfig),
        ctypes.POINTER(SonareNoteObject),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
    ]
    if hasattr(lib, "sonare_split_note"):
        lib.sonare_split_note.restype = ctypes.c_int32
        lib.sonare_split_note.argtypes = [
            *_note_set_edit_argtypes,
            ctypes.c_size_t,
            ctypes.c_int32,
            ctypes.POINTER(SonareNoteObjectsResult),
        ]
    if hasattr(lib, "sonare_merge_notes"):
        lib.sonare_merge_notes.restype = ctypes.c_int32
        lib.sonare_merge_notes.argtypes = [
            *_note_set_edit_argtypes,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.POINTER(SonareNoteObjectsResult),
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
    lib.sonare_voice_change_realtime.restype = ctypes.c_int32
    lib.sonare_voice_change_realtime.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
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
    if hasattr(lib, "sonare_streaming_retune_create"):
        lib.sonare_streaming_retune_create.restype = ctypes.c_void_p
        lib.sonare_streaming_retune_create.argtypes = [
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
        ]
        lib.sonare_streaming_retune_destroy.restype = None
        lib.sonare_streaming_retune_destroy.argtypes = [ctypes.c_void_p]
        lib.sonare_streaming_retune_prepare.restype = ctypes.c_int32
        lib.sonare_streaming_retune_prepare.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_int,
        ]
        lib.sonare_streaming_retune_reset.restype = ctypes.c_int32
        lib.sonare_streaming_retune_reset.argtypes = [ctypes.c_void_p]
        lib.sonare_streaming_retune_set_config.restype = ctypes.c_int32
        lib.sonare_streaming_retune_set_config.argtypes = [
            ctypes.c_void_p,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
        ]
        lib.sonare_streaming_retune_config.restype = ctypes.c_int32
        lib.sonare_streaming_retune_config.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.sonare_streaming_retune_process_mono.restype = ctypes.c_int32
        lib.sonare_streaming_retune_process_mono.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
        ]
        lib.sonare_streaming_retune_grain_size.restype = ctypes.c_int32
        lib.sonare_streaming_retune_grain_size.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.sonare_streaming_retune_latency_samples.restype = ctypes.c_int32
        lib.sonare_streaming_retune_latency_samples.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int),
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
    lib.sonare_realtime_voice_changer_process_planar_stereo.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_process_planar_stereo.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
    ]
    lib.sonare_realtime_voice_changer_latency_samples.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_latency_samples.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_realtime_voice_changer_non_finite_discard_count.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_non_finite_discard_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    lib.sonare_realtime_voice_changer_config_json.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_config_json.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_char_p),
    ]
    lib.sonare_realtime_voice_changer_preset_names.restype = ctypes.c_char_p
    lib.sonare_realtime_voice_changer_preset_names.argtypes = []
    lib.sonare_voice_character_preset_id.restype = ctypes.c_char_p
    lib.sonare_voice_character_preset_id.argtypes = [ctypes.c_int]
    lib.sonare_realtime_voice_changer_preset_json.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_preset_json.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_char_p),
    ]
    lib.sonare_realtime_voice_changer_validate_preset_json.restype = ctypes.c_int32
    lib.sonare_realtime_voice_changer_validate_preset_json.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(ctypes.c_char_p),
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
    if hasattr(lib, "sonare_normalize_rms"):
        lib.sonare_normalize_rms.restype = ctypes.c_int32
        lib.sonare_normalize_rms.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    # sonare_normalize_stereo -- one length and one sample rate for the pair, and
    # both channels come back inside the result struct.
    if hasattr(lib, "sonare_normalize_stereo"):
        lib.sonare_normalize_stereo.restype = ctypes.c_int32
        lib.sonare_normalize_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.POINTER(SonareNormalizeStereoResult),
        ]
    if hasattr(lib, "sonare_normalize_rms_stereo"):
        lib.sonare_normalize_rms_stereo.restype = ctypes.c_int32
        lib.sonare_normalize_rms_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.POINTER(SonareNormalizeStereoResult),
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
    if hasattr(lib, "sonare_trim_ex"):
        lib.sonare_trim_ex.restype = ctypes.c_int32
        lib.sonare_trim_ex.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]
