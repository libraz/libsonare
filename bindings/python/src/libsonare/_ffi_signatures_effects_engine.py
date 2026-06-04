"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_effects_engine_signatures(lib: ctypes.CDLL) -> None:
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

    # Realtime engine / offline DAW engine
    lib.sonare_engine_create.restype = ctypes.c_int32
    lib.sonare_engine_create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    lib.sonare_engine_destroy.restype = None
    lib.sonare_engine_destroy.argtypes = [ctypes.c_void_p]
    lib.sonare_engine_prepare.restype = ctypes.c_int32
    lib.sonare_engine_prepare.argtypes = [
        ctypes.c_void_p,
        ctypes.c_double,
        ctypes.c_int,
        ctypes.c_size_t,
        ctypes.c_size_t,
    ]
    lib.sonare_engine_play.restype = ctypes.c_int32
    lib.sonare_engine_play.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.sonare_engine_stop.restype = ctypes.c_int32
    lib.sonare_engine_stop.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.sonare_engine_seek_sample.restype = ctypes.c_int32
    lib.sonare_engine_seek_sample.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64]
    lib.sonare_engine_seek_ppq.restype = ctypes.c_int32
    lib.sonare_engine_seek_ppq.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_int64]
    lib.sonare_engine_set_tempo.restype = ctypes.c_int32
    lib.sonare_engine_set_tempo.argtypes = [ctypes.c_void_p, ctypes.c_double]
    lib.sonare_engine_set_time_signature.restype = ctypes.c_int32
    lib.sonare_engine_set_time_signature.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.sonare_engine_set_loop.restype = ctypes.c_int32
    lib.sonare_engine_set_loop.argtypes = [
        ctypes.c_void_p,
        ctypes.c_double,
        ctypes.c_double,
        ctypes.c_int,
    ]
    lib.sonare_engine_add_parameter.restype = ctypes.c_int32
    lib.sonare_engine_add_parameter.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareParameterInfo),
    ]
    if hasattr(lib, "sonare_engine_clear_parameters"):
        lib.sonare_engine_clear_parameters.restype = ctypes.c_int32
        lib.sonare_engine_clear_parameters.argtypes = [ctypes.c_void_p]
    lib.sonare_engine_parameter_count.restype = ctypes.c_int32
    lib.sonare_engine_parameter_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_parameter_info_by_index.restype = ctypes.c_int32
    lib.sonare_engine_parameter_info_by_index.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(SonareParameterInfo),
    ]
    lib.sonare_engine_parameter_info.restype = ctypes.c_int32
    lib.sonare_engine_parameter_info.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(SonareParameterInfo),
    ]
    lib.sonare_engine_set_automation_lane.restype = ctypes.c_int32
    lib.sonare_engine_set_automation_lane.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(SonareAutomationPoint),
        ctypes.c_size_t,
    ]
    lib.sonare_engine_automation_lane_count.restype = ctypes.c_int32
    lib.sonare_engine_automation_lane_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_set_markers.restype = ctypes.c_int32
    lib.sonare_engine_set_markers.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineMarker),
        ctypes.c_size_t,
    ]
    lib.sonare_engine_marker_count.restype = ctypes.c_int32
    lib.sonare_engine_marker_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_marker_by_index.restype = ctypes.c_int32
    lib.sonare_engine_marker_by_index.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(SonareEngineMarker),
    ]
    lib.sonare_engine_marker.restype = ctypes.c_int32
    lib.sonare_engine_marker.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(SonareEngineMarker),
    ]
    lib.sonare_engine_seek_marker.restype = ctypes.c_int32
    lib.sonare_engine_seek_marker.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int64]
    lib.sonare_engine_set_loop_from_markers.restype = ctypes.c_int32
    lib.sonare_engine_set_loop_from_markers.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
    ]
    lib.sonare_engine_set_metronome.restype = ctypes.c_int32
    lib.sonare_engine_set_metronome.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineMetronomeConfig),
    ]
    lib.sonare_engine_metronome.restype = ctypes.c_int32
    lib.sonare_engine_metronome.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineMetronomeConfig),
    ]
    lib.sonare_engine_count_in_end_sample.restype = ctypes.c_int32
    lib.sonare_engine_count_in_end_sample.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int64,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int64),
    ]
    lib.sonare_engine_set_clips.restype = ctypes.c_int32
    lib.sonare_engine_set_clips.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineClip),
        ctypes.c_size_t,
    ]
    lib.sonare_engine_clip_count.restype = ctypes.c_int32
    lib.sonare_engine_clip_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_set_capture_buffer.restype = ctypes.c_int32
    lib.sonare_engine_set_capture_buffer.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineCaptureBuffer),
    ]
    lib.sonare_engine_arm_capture.restype = ctypes.c_int32
    lib.sonare_engine_arm_capture.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    lib.sonare_engine_set_capture_punch.restype = ctypes.c_int32
    lib.sonare_engine_set_capture_punch.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int64,
        ctypes.c_int64,
        ctypes.c_int,
    ]
    lib.sonare_engine_reset_capture.restype = ctypes.c_int32
    lib.sonare_engine_reset_capture.argtypes = [ctypes.c_void_p]
    lib.sonare_engine_capture_status.restype = ctypes.c_int32
    lib.sonare_engine_capture_status.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineCaptureStatus),
    ]
    lib.sonare_engine_set_graph.restype = ctypes.c_int32
    lib.sonare_engine_set_graph.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineGraphSpec),
    ]
    lib.sonare_engine_graph_node_count.restype = ctypes.c_int32
    lib.sonare_engine_graph_node_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_graph_connection_count.restype = ctypes.c_int32
    lib.sonare_engine_graph_connection_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_engine_process.restype = ctypes.c_int32
    lib.sonare_engine_process.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_int,
        ctypes.c_int,
    ]
    lib.sonare_engine_process_with_monitor.restype = ctypes.c_int32
    lib.sonare_engine_process_with_monitor.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_int,
        ctypes.c_int,
    ]
    lib.sonare_engine_render_offline.restype = ctypes.c_int32
    lib.sonare_engine_render_offline.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_int,
        ctypes.c_int64,
        ctypes.c_int,
    ]
    lib.sonare_engine_bounce_options_default.restype = ctypes.c_int32
    lib.sonare_engine_bounce_options_default.argtypes = [
        ctypes.POINTER(SonareEngineBounceOptions),
    ]
    lib.sonare_engine_bounce_offline.restype = ctypes.c_int32
    lib.sonare_engine_bounce_offline.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineBounceOptions),
        ctypes.POINTER(SonareEngineBounceResult),
    ]
    lib.sonare_free_bounce_result.restype = None
    lib.sonare_free_bounce_result.argtypes = [ctypes.POINTER(SonareEngineBounceResult)]
    lib.sonare_engine_freeze_offline.restype = ctypes.c_int32
    lib.sonare_engine_freeze_offline.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineFreezeOptions),
        ctypes.POINTER(SonareEngineFreezeResult),
    ]
    lib.sonare_engine_drain_telemetry.restype = ctypes.c_int32
    lib.sonare_engine_drain_telemetry.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareEngineTelemetry),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    if hasattr(lib, "sonare_engine_drain_meter_telemetry"):
        lib.sonare_engine_drain_meter_telemetry.restype = ctypes.c_int32
        lib.sonare_engine_drain_meter_telemetry.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareMeterTelemetryRecord),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
    if hasattr(lib, "sonare_engine_set_parameter"):
        lib.sonare_engine_set_parameter.restype = ctypes.c_int32
        lib.sonare_engine_set_parameter.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_float,
            ctypes.c_int64,
        ]
        lib.sonare_engine_set_parameter_smoothed.restype = ctypes.c_int32
        lib.sonare_engine_set_parameter_smoothed.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_float,
            ctypes.c_int64,
        ]
    if hasattr(lib, "sonare_engine_push_midi_cc"):
        lib.sonare_engine_push_midi_cc.restype = ctypes.c_int32
        lib.sonare_engine_push_midi_cc.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.c_int64,
        ]
    if hasattr(lib, "sonare_engine_push_midi_panic"):
        lib.sonare_engine_push_midi_panic.restype = ctypes.c_int32
        lib.sonare_engine_push_midi_panic.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int64,
        ]
    if hasattr(lib, "sonare_engine_set_builtin_instrument"):
        lib.sonare_engine_set_builtin_instrument.restype = ctypes.c_int32
        lib.sonare_engine_set_builtin_instrument.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(SonareBuiltinSynthConfig),
        ]
    if hasattr(lib, "sonare_engine_load_soundfont"):
        lib.sonare_engine_load_soundfont.restype = ctypes.c_int32
        lib.sonare_engine_load_soundfont.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
        ]
    if hasattr(lib, "sonare_engine_set_sf2_instrument"):
        lib.sonare_engine_set_sf2_instrument.restype = ctypes.c_int32
        lib.sonare_engine_set_sf2_instrument.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(SonareSf2InstrumentConfig),
        ]
    if hasattr(lib, "sonare_engine_clear_midi_instrument"):
        lib.sonare_engine_clear_midi_instrument.restype = ctypes.c_int32
        lib.sonare_engine_clear_midi_instrument.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
    if hasattr(lib, "sonare_engine_midi_instrument_count"):
        lib.sonare_engine_midi_instrument_count.restype = ctypes.c_int32
        lib.sonare_engine_midi_instrument_count.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_size_t),
        ]
    if hasattr(lib, "sonare_engine_bind_midi_cc"):
        lib.sonare_engine_bind_midi_cc.restype = ctypes.c_int32
        lib.sonare_engine_bind_midi_cc.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint8,
            ctypes.c_uint8,
            ctypes.c_uint32,
            ctypes.c_float,
            ctypes.c_float,
        ]
    if hasattr(lib, "sonare_engine_clear_midi_cc_bindings"):
        lib.sonare_engine_clear_midi_cc_bindings.restype = ctypes.c_int32
        lib.sonare_engine_clear_midi_cc_bindings.argtypes = [ctypes.c_void_p]
    if hasattr(lib, "sonare_engine_midi_cc_binding_count"):
        lib.sonare_engine_midi_cc_binding_count.restype = ctypes.c_int32
        lib.sonare_engine_midi_cc_binding_count.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_size_t),
        ]
    if hasattr(lib, "sonare_engine_clear_midi_fx"):
        lib.sonare_engine_clear_midi_fx.restype = ctypes.c_int32
        lib.sonare_engine_clear_midi_fx.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
    if hasattr(lib, "sonare_engine_set_midi_input_source"):
        lib.sonare_engine_set_midi_input_source.restype = ctypes.c_int32
        lib.sonare_engine_set_midi_input_source.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
    if hasattr(lib, "sonare_engine_clear_midi_input_source"):
        lib.sonare_engine_clear_midi_input_source.restype = ctypes.c_int32
        lib.sonare_engine_clear_midi_input_source.argtypes = [ctypes.c_void_p]
    if hasattr(lib, "sonare_engine_midi_input_pending_count"):
        lib.sonare_engine_midi_input_pending_count.restype = ctypes.c_int32
        lib.sonare_engine_midi_input_pending_count.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_size_t),
        ]
    for _name in (
        "sonare_engine_push_midi_input_note_on",
        "sonare_engine_push_midi_input_note_off",
        "sonare_engine_push_midi_input_cc",
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_int32
            _fn.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_int64,
            ]
    for _name in (
        "sonare_engine_push_midi_note_on",
        "sonare_engine_push_midi_note_off",
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_int32
            _fn.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint32,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_uint8,
                ctypes.c_int64,
            ]
    if hasattr(lib, "sonare_engine_get_transport_state"):
        lib.sonare_engine_get_transport_state.restype = ctypes.c_int32
        lib.sonare_engine_get_transport_state.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareTransportState),
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
