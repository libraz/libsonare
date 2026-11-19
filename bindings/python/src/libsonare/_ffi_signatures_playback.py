"""ctypes function signatures for the playback renderer C ABI."""

from __future__ import annotations

import ctypes


def configure_playback_signatures(lib: ctypes.CDLL) -> None:
    # --- Playback renderer (movie audio: upmix, binaural, night mode DRC,
    # dialogue level, speaker calibration, bass management) ---
    #
    # Playback is a real, independently disableable build option
    # (BUILD_PLAYBACK), so an ABI-matched dylib built without it lacks every
    # symbol below. Each assignment is guarded the same way the mastering /
    # mixing / mixing_assistant / repair_dynamics signature modules already
    # guard their own optional symbols, so loading such a dylib does not raise
    # AttributeError here for a command that never touches playback -- only an
    # actual call into a missing symbol fails, from _playback.py's own
    # descriptive check.

    lib.sonare_hrtf_set_create_default.restype = ctypes.c_int32
    lib.sonare_hrtf_set_create_default.argtypes = [ctypes.POINTER(ctypes.c_void_p)]

    lib.sonare_hrtf_set_create_from_memory.restype = ctypes.c_int32
    lib.sonare_hrtf_set_create_from_memory.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    lib.sonare_hrtf_set_destroy.restype = None
    lib.sonare_hrtf_set_destroy.argtypes = [ctypes.c_void_p]

    lib.sonare_playback_renderer_create_json.restype = ctypes.c_int32
    lib.sonare_playback_renderer_create_json.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    lib.sonare_playback_renderer_destroy.restype = None
    lib.sonare_playback_renderer_destroy.argtypes = [ctypes.c_void_p]

    lib.sonare_playback_renderer_reset.restype = ctypes.c_int32
    lib.sonare_playback_renderer_reset.argtypes = [ctypes.c_void_p]

    lib.sonare_playback_renderer_set_config_json.restype = ctypes.c_int32
    lib.sonare_playback_renderer_set_config_json.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
    ]

    lib.sonare_playback_renderer_config_json.restype = ctypes.c_int32
    lib.sonare_playback_renderer_config_json.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_char_p),
    ]

    lib.sonare_playback_renderer_set_head_orientation.restype = ctypes.c_int32
    lib.sonare_playback_renderer_set_head_orientation.argtypes = [
        ctypes.c_void_p,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
    ]

    lib.sonare_playback_renderer_num_input_channels.restype = ctypes.c_int32
    lib.sonare_playback_renderer_num_input_channels.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_playback_renderer_num_output_channels.restype = ctypes.c_int32
    lib.sonare_playback_renderer_num_output_channels.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_playback_renderer_process_planar.restype = ctypes.c_int32
    lib.sonare_playback_renderer_process_planar.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_int,
        ctypes.c_int,
    ]

    lib.sonare_playback_renderer_process_interleaved.restype = ctypes.c_int32
    lib.sonare_playback_renderer_process_interleaved.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
    ]

    lib.sonare_playback_renderer_latency_samples.restype = ctypes.c_int32
    lib.sonare_playback_renderer_latency_samples.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.sonare_playback_renderer_max_block_size.restype = ctypes.c_int32
    lib.sonare_playback_renderer_max_block_size.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_playback_renderer_diagnostics_json.restype = ctypes.c_int32
    lib.sonare_playback_renderer_diagnostics_json.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_char_p),
    ]

    lib.sonare_playback_renderer_non_finite_discard_count.restype = ctypes.c_int32
    lib.sonare_playback_renderer_non_finite_discard_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]

    lib.sonare_playback_render_interleaved.restype = ctypes.c_int32
    lib.sonare_playback_render_interleaved.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_playback_renderer_render_offline.restype = ctypes.c_int32
    lib.sonare_playback_renderer_render_offline.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_playback_loudness_meter_create.restype = ctypes.c_int32
    lib.sonare_playback_loudness_meter_create.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_void_p),
    ]

    lib.sonare_playback_loudness_meter_push_interleaved.restype = ctypes.c_int32
    lib.sonare_playback_loudness_meter_push_interleaved.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
    ]

    lib.sonare_playback_loudness_meter_integrated_lufs.restype = ctypes.c_int32
    lib.sonare_playback_loudness_meter_integrated_lufs.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_float),
    ]

    lib.sonare_playback_loudness_meter_destroy.restype = None
    lib.sonare_playback_loudness_meter_destroy.argtypes = [ctypes.c_void_p]
