"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_project_signatures(lib: ctypes.CDLL) -> None:
    # --- Headless arrangement / DAW project (sonare_c_project.h) ---
    if hasattr(lib, "sonare_project_abi_version"):
        lib.sonare_project_abi_version.restype = ctypes.c_uint32
        lib.sonare_project_abi_version.argtypes = []

        # Lifecycle / IO / render.
        lib.sonare_project_create.restype = ctypes.c_int32
        lib.sonare_project_create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]

        lib.sonare_project_destroy.restype = None
        lib.sonare_project_destroy.argtypes = [ctypes.c_void_p]

        lib.sonare_project_serialize.restype = ctypes.c_int32
        lib.sonare_project_serialize.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_size_t),
        ]

        lib.sonare_project_deserialize.restype = ctypes.c_int32
        lib.sonare_project_deserialize.argtypes = [
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_char_p),
        ]

        lib.sonare_project_set_sample_rate.restype = ctypes.c_int32
        lib.sonare_project_set_sample_rate.argtypes = [ctypes.c_void_p, ctypes.c_double]

        lib.sonare_project_compile.restype = ctypes.c_int32
        lib.sonare_project_compile.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectCompileResult),
        ]

        lib.sonare_project_free_compile_result.restype = None
        lib.sonare_project_free_compile_result.argtypes = [
            ctypes.POINTER(SonareProjectCompileResult)
        ]

        lib.sonare_project_bounce.restype = ctypes.c_int32
        lib.sonare_project_bounce.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectBounceOptions),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

        if hasattr(lib, "sonare_project_bounce_with_builtin_instruments"):
            lib.sonare_project_bounce_with_builtin_instruments.restype = ctypes.c_int32
            lib.sonare_project_bounce_with_builtin_instruments.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareProjectBounceOptions),
                ctypes.POINTER(SonareBuiltinInstrumentBinding),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.POINTER(ctypes.c_size_t),
            ]

        # Edit.
        lib.sonare_project_add_track.restype = ctypes.c_int32
        lib.sonare_project_add_track.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectTrackDesc),
            ctypes.POINTER(ctypes.c_uint32),
        ]

        lib.sonare_project_add_clip.restype = ctypes.c_int32
        lib.sonare_project_add_clip.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectClipDesc),
            ctypes.POINTER(ctypes.c_uint32),
        ]

        lib.sonare_project_add_midi_clip.restype = ctypes.c_int32
        lib.sonare_project_add_midi_clip.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
        ]

        lib.sonare_project_split_clip.restype = ctypes.c_int32
        lib.sonare_project_split_clip.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_double,
            ctypes.POINTER(ctypes.c_uint32),
        ]

        lib.sonare_project_trim_clip.restype = ctypes.c_int32
        lib.sonare_project_trim_clip.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_double,
            ctypes.c_double,
        ]

        lib.sonare_project_move_clip.restype = ctypes.c_int32
        lib.sonare_project_move_clip.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_double,
            ctypes.c_uint32,
        ]

        lib.sonare_project_set_clip_warp_ref.restype = ctypes.c_int32
        lib.sonare_project_set_clip_warp_ref.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]

        lib.sonare_project_set_track_midi_destination.restype = ctypes.c_int32
        lib.sonare_project_set_track_midi_destination.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]

        lib.sonare_project_remove_clip.restype = ctypes.c_int32
        lib.sonare_project_remove_clip.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

        lib.sonare_project_set_clip_gain.restype = ctypes.c_int32
        lib.sonare_project_set_clip_gain.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_float,
        ]

        lib.sonare_project_set_clip_fade.restype = ctypes.c_int32
        lib.sonare_project_set_clip_fade.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(SonareProjectClipFade),
            ctypes.POINTER(SonareProjectClipFade),
        ]

        lib.sonare_project_set_clip_loop.restype = ctypes.c_int32
        lib.sonare_project_set_clip_loop.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_int,
            ctypes.c_double,
        ]

        lib.sonare_project_set_clip_source.restype = ctypes.c_int32
        lib.sonare_project_set_clip_source.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]

        lib.sonare_project_duplicate_clip.restype = ctypes.c_int32
        lib.sonare_project_duplicate_clip.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_double,
            ctypes.POINTER(ctypes.c_uint32),
        ]

        lib.sonare_project_remove_track.restype = ctypes.c_int32
        lib.sonare_project_remove_track.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

        lib.sonare_project_rename_track.restype = ctypes.c_int32
        lib.sonare_project_rename_track.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_char_p,
        ]

        lib.sonare_project_set_track_route.restype = ctypes.c_int32
        lib.sonare_project_set_track_route.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_char_p,
            ctypes.c_char_p,
        ]

        lib.sonare_project_add_automation_lane.restype = ctypes.c_int32
        lib.sonare_project_add_automation_lane.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(SonareAutomationLaneDesc),
            ctypes.POINTER(ctypes.c_size_t),
        ]

        lib.sonare_project_edit_automation_lane.restype = ctypes.c_int32
        lib.sonare_project_edit_automation_lane.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_size_t,
            ctypes.POINTER(SonareAutomationLaneDesc),
        ]

        lib.sonare_project_remove_automation_lane.restype = ctypes.c_int32
        lib.sonare_project_remove_automation_lane.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_size_t,
        ]

        lib.sonare_project_undo.restype = ctypes.c_int32
        lib.sonare_project_undo.argtypes = [ctypes.c_void_p]

        lib.sonare_project_redo.restype = ctypes.c_int32
        lib.sonare_project_redo.argtypes = [ctypes.c_void_p]

        # MIDI.
        lib.sonare_project_set_midi_events.restype = ctypes.c_int32
        lib.sonare_project_set_midi_events.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(SonareMidiEventPod),
            ctypes.c_size_t,
        ]

        lib.sonare_project_import_smf.restype = ctypes.c_int32
        lib.sonare_project_import_smf.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint32),
        ]

        lib.sonare_project_export_smf.restype = ctypes.c_int32
        lib.sonare_project_export_smf.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

        lib.sonare_project_import_clip_file.restype = ctypes.c_int32
        lib.sonare_project_import_clip_file.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint32),
        ]

        lib.sonare_project_export_clip_file.restype = ctypes.c_int32
        lib.sonare_project_export_clip_file.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

        lib.sonare_project_set_program.restype = ctypes.c_int32
        lib.sonare_project_set_program.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_int,
            ctypes.c_int,
        ]

        lib.sonare_project_set_program_on_channel.restype = ctypes.c_int32
        lib.sonare_project_set_program_on_channel.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.c_int,
            ctypes.c_int,
        ]

        lib.sonare_project_set_midi_fx.restype = ctypes.c_int32
        lib.sonare_project_set_midi_fx.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_char_p,
        ]

        lib.sonare_project_validate_midi_notes.restype = ctypes.c_int32
        lib.sonare_project_validate_midi_notes.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(SonareNotePairValidation),
        ]

        for _name, _args in {
            "sonare_midi_note_on": [
                ctypes.c_double,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.POINTER(SonareMidiEventPod),
            ],
            "sonare_midi_note_off": [
                ctypes.c_double,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.POINTER(SonareMidiEventPod),
            ],
            "sonare_midi_cc": [
                ctypes.c_double,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.POINTER(SonareMidiEventPod),
            ],
            "sonare_midi_poly_pressure": [
                ctypes.c_double,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.POINTER(SonareMidiEventPod),
            ],
            "sonare_midi_program": [
                ctypes.c_double,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.POINTER(SonareMidiEventPod),
            ],
            "sonare_midi_channel_pressure": [
                ctypes.c_double,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.POINTER(SonareMidiEventPod),
            ],
            "sonare_midi_pitch_bend": [
                ctypes.c_double,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint16,
                ctypes.POINTER(SonareMidiEventPod),
            ],
        }.items():
            if hasattr(lib, _name):
                _fn = getattr(lib, _name)
                _fn.restype = ctypes.c_int32
                _fn.argtypes = _args

        # MIR.
        lib.sonare_project_auto_tempo.restype = ctypes.c_int32
        lib.sonare_project_auto_tempo.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_float),
        ]

        lib.sonare_project_snap_to_grid.restype = ctypes.c_int32
        lib.sonare_project_snap_to_grid.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.POINTER(ctypes.c_double),
        ]

        lib.sonare_project_annotate_keys.restype = ctypes.c_int32
        lib.sonare_project_annotate_keys.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectKeySegment),
            ctypes.c_size_t,
        ]

        lib.sonare_project_annotate_chords.restype = ctypes.c_int32
        lib.sonare_project_annotate_chords.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectChordSymbol),
            ctypes.c_size_t,
        ]

        # Assist sidecars (opaque module state).
        lib.sonare_project_set_assist_sidecar.restype = ctypes.c_int32
        lib.sonare_project_set_assist_sidecar.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]

        lib.sonare_project_assist_sidecar_count.restype = ctypes.c_size_t
        lib.sonare_project_assist_sidecar_count.argtypes = [ctypes.c_void_p]

        lib.sonare_project_get_assist_sidecar.restype = ctypes.c_int32
        lib.sonare_project_get_assist_sidecar.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.POINTER(SonareProjectAssistSidecar),
        ]

        lib.sonare_project_free_assist_sidecar.restype = None
        lib.sonare_project_free_assist_sidecar.argtypes = [
            ctypes.POINTER(SonareProjectAssistSidecar)
        ]

        # Heap byte-buffer free helper (SMF export).
        lib.sonare_free_bytes.restype = None
        lib.sonare_free_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
