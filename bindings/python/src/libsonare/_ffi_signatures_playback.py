"""ctypes function signatures for the playback renderer C ABI."""

from __future__ import annotations

import ctypes


def configure_playback_signatures(lib: ctypes.CDLL) -> None:
    # --- Playback renderer (movie audio: upmix, binaural, night mode DRC,
    # dialogue level, speaker calibration, bass management) ---

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
    lib.sonare_playback_renderer_set_config_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

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

    lib.sonare_playback_renderer_input_channel_count.restype = ctypes.c_int32
    lib.sonare_playback_renderer_input_channel_count.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_playback_renderer_output_channel_count.restype = ctypes.c_int32
    lib.sonare_playback_renderer_output_channel_count.argtypes = [
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

    lib.sonare_free_playback_render.restype = None
    lib.sonare_free_playback_render.argtypes = [ctypes.POINTER(ctypes.c_float)]

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
