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

        if hasattr(lib, "sonare_synth_preset_names"):
            lib.sonare_synth_preset_names.restype = ctypes.c_char_p
            lib.sonare_synth_preset_names.argtypes = []

            if hasattr(lib, "sonare_synth_enum_names"):
                lib.sonare_synth_enum_names.restype = ctypes.c_char_p
                lib.sonare_synth_enum_names.argtypes = [ctypes.c_int]

            lib.sonare_synth_preset_patch.restype = ctypes.c_int32
            lib.sonare_synth_preset_patch.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(SonareSynthPatch),
            ]

            lib.sonare_project_bounce_with_synth_instruments.restype = ctypes.c_int32
            lib.sonare_project_bounce_with_synth_instruments.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareProjectBounceOptions),
                ctypes.POINTER(SonareSynthInstrumentBinding),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.POINTER(ctypes.c_size_t),
            ]

        if hasattr(lib, "sonare_project_load_soundfont"):
            lib.sonare_project_load_soundfont.restype = ctypes.c_int32
            lib.sonare_project_load_soundfont.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_uint8),
                ctypes.c_size_t,
            ]

            lib.sonare_project_clear_soundfont.restype = ctypes.c_int32
            lib.sonare_project_clear_soundfont.argtypes = [ctypes.c_void_p]

            lib.sonare_project_soundfont_preset_count.restype = ctypes.c_int32
            lib.sonare_project_soundfont_preset_count.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_size_t),
            ]

            lib.sonare_project_soundfont_manifest.restype = ctypes.c_int32
            lib.sonare_project_soundfont_manifest.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareSf2ProgramStatus),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_size_t),
            ]

            lib.sonare_project_bounce_with_sf2_instruments.restype = ctypes.c_int32
            lib.sonare_project_bounce_with_sf2_instruments.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareProjectBounceOptions),
                ctypes.POINTER(SonareSf2InstrumentBinding),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.POINTER(ctypes.c_size_t),
            ]

        if hasattr(lib, "sonare_project_bounce_with_instruments"):
            lib.sonare_project_bounce_with_instruments.restype = ctypes.c_int32
            lib.sonare_project_bounce_with_instruments.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareProjectBounceOptions),
                ctypes.POINTER(SonareInstrumentBinding),
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

        lib.sonare_project_set_track_kind.restype = ctypes.c_int32
        lib.sonare_project_set_track_kind.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]

        lib.sonare_project_set_clip_warp_ref.restype = ctypes.c_int32
        lib.sonare_project_set_clip_warp_ref.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
        ]

        lib.sonare_project_set_warp_map.restype = ctypes.c_int32
        lib.sonare_project_set_warp_map.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectWarpMapDesc),
        ]
        lib.sonare_project_remove_warp_map.restype = ctypes.c_int32
        lib.sonare_project_remove_warp_map.argtypes = [
            ctypes.c_void_p,
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

    _configure_midi_naming_signatures(lib)
    _configure_project_extra_signatures(lib)


def _configure_midi_naming_signatures(lib: ctypes.CDLL) -> None:
    # GM / GM2 / CC naming helpers — all static-lifetime, no project handle.
    for _name in (
        "sonare_midi_gm_instrument_name",
        "sonare_midi_gm_family_name",
        "sonare_midi_gm_drum_name",
        "sonare_midi_gm2_drum_set_name",
        "sonare_midi_cc_name",
        "sonare_midi_per_note_controller_name",
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_char_p
            _fn.argtypes = [ctypes.c_int]

    for _name in (
        "sonare_midi_gm2_instrument_name",
        "sonare_midi_gm2_drum_name",
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_char_p
            _fn.argtypes = [ctypes.c_int, ctypes.c_int]

    for _name in (
        "sonare_midi_gm_program_for_name",
        "sonare_midi_gm_drum_note_for_name",
        "sonare_midi_cc_index_for_name",
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_int
            _fn.argtypes = [ctypes.c_char_p]

    if hasattr(lib, "sonare_midi_gm_family_first_program"):
        lib.sonare_midi_gm_family_first_program.restype = ctypes.c_int
        lib.sonare_midi_gm_family_first_program.argtypes = [ctypes.c_int]

    # Pure conversion helpers (no project handle).
    if hasattr(lib, "sonare_midi_bank_program"):
        lib.sonare_midi_bank_program.restype = ctypes.c_int32
        lib.sonare_midi_bank_program.argtypes = [
            ctypes.c_double,
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(SonareMidiEventPod),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]

    if hasattr(lib, "sonare_midi_route_events"):
        lib.sonare_midi_route_events.restype = ctypes.c_int32
        lib.sonare_midi_route_events.argtypes = [
            ctypes.POINTER(SonareMidiEventPod),
            ctypes.c_size_t,
            ctypes.POINTER(SonareMidiRouteConfig),
            ctypes.POINTER(SonareMidiEventPod),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_uint32),
        ]

    if hasattr(lib, "sonare_midi_cc_learn"):
        lib.sonare_midi_cc_learn.restype = ctypes.c_int32
        lib.sonare_midi_cc_learn.argtypes = [
            ctypes.POINTER(SonareMidiEventPod),
            ctypes.c_size_t,
            ctypes.c_uint32,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_uint8,
            ctypes.POINTER(SonareMidiCcBinding),
        ]

    if hasattr(lib, "sonare_midi_cc_to_breakpoint"):
        lib.sonare_midi_cc_to_breakpoint.restype = ctypes.c_int32
        lib.sonare_midi_cc_to_breakpoint.argtypes = [
            ctypes.POINTER(SonareMidiCcBinding),
            ctypes.c_size_t,
            ctypes.POINTER(SonareMidiEventPod),
            ctypes.POINTER(SonareAutomationPoint),
        ]

    if hasattr(lib, "sonare_midi_param_to_cc"):
        lib.sonare_midi_param_to_cc.restype = ctypes.c_int32
        lib.sonare_midi_param_to_cc.argtypes = [
            ctypes.POINTER(SonareMidiCcBinding),
            ctypes.c_size_t,
            ctypes.c_uint32,
            ctypes.c_float,
            ctypes.c_uint8,
            ctypes.c_double,
            ctypes.POINTER(SonareMidiEventPod),
        ]


def _configure_project_extra_signatures(lib: ctypes.CDLL) -> None:
    # Project getters / setters added for full Python parity.
    if hasattr(lib, "sonare_project_bake_midi_fx"):
        lib.sonare_project_bake_midi_fx.restype = ctypes.c_int32
        lib.sonare_project_bake_midi_fx.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_char_p,
        ]

    if hasattr(lib, "sonare_project_get_sample_rate"):
        lib.sonare_project_get_sample_rate.restype = ctypes.c_int32
        lib.sonare_project_get_sample_rate.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_double),
        ]

    if hasattr(lib, "sonare_project_get_overlap_policy"):
        lib.sonare_project_get_overlap_policy.restype = ctypes.c_int32
        lib.sonare_project_get_overlap_policy.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
        ]

    if hasattr(lib, "sonare_project_set_overlap_policy"):
        lib.sonare_project_set_overlap_policy.restype = ctypes.c_int32
        lib.sonare_project_set_overlap_policy.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]

    if hasattr(lib, "sonare_project_set_marker"):
        lib.sonare_project_set_marker.restype = ctypes.c_int32
        lib.sonare_project_set_marker.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_double,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_uint32),
        ]

    if hasattr(lib, "sonare_project_set_mixer_scene_json"):
        lib.sonare_project_set_mixer_scene_json.restype = ctypes.c_int32
        lib.sonare_project_set_mixer_scene_json.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
        ]

    if hasattr(lib, "sonare_project_set_tempo_segments"):
        lib.sonare_project_set_tempo_segments.restype = ctypes.c_int32
        lib.sonare_project_set_tempo_segments.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectTempoSegment),
            ctypes.c_size_t,
        ]

    if hasattr(lib, "sonare_project_set_time_signatures"):
        lib.sonare_project_set_time_signatures.restype = ctypes.c_int32
        lib.sonare_project_set_time_signatures.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectTimeSignatureSegment),
            ctypes.c_size_t,
        ]

    for _name in (
        "sonare_project_source_count",
        "sonare_project_tempo_segment_count",
        "sonare_project_time_signature_count",
        "sonare_project_track_count",
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_int32
            _fn.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]

    if hasattr(lib, "sonare_project_last_bounce_compile_result"):
        lib.sonare_project_last_bounce_compile_result.restype = ctypes.c_int32
        lib.sonare_project_last_bounce_compile_result.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareProjectCompileResult),
        ]
