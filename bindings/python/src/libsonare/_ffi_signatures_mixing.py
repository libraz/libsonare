"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_mixing_signatures(lib: ctypes.CDLL) -> None:
    if hasattr(lib, "sonare_mixer_create"):
        lib.sonare_mixer_create.restype = ctypes.c_void_p
        lib.sonare_mixer_create.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.sonare_mixer_add_strip.restype = ctypes.c_void_p
        lib.sonare_mixer_add_strip.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.sonare_strip_set_input_trim_db.restype = ctypes.c_int32
        lib.sonare_strip_set_input_trim_db.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.sonare_strip_set_fader_db.restype = ctypes.c_int32
        lib.sonare_strip_set_fader_db.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.sonare_strip_set_pan.restype = ctypes.c_int32
        lib.sonare_strip_set_pan.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_int]
        lib.sonare_strip_set_dual_pan.restype = ctypes.c_int32
        lib.sonare_strip_set_dual_pan.argtypes = [
            ctypes.c_void_p,
            ctypes.c_float,
            ctypes.c_float,
        ]
        lib.sonare_strip_set_width.restype = ctypes.c_int32
        lib.sonare_strip_set_width.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.sonare_strip_set_muted.restype = ctypes.c_int32
        lib.sonare_strip_set_muted.argtypes = [ctypes.c_void_p, ctypes.c_int]
        if hasattr(lib, "sonare_strip_set_soloed"):
            lib.sonare_strip_set_soloed.restype = ctypes.c_int32
            lib.sonare_strip_set_soloed.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_strip_set_solo_safe.restype = ctypes.c_int32
            lib.sonare_strip_set_solo_safe.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_strip_set_polarity_invert.restype = ctypes.c_int32
            lib.sonare_strip_set_polarity_invert.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_strip_set_pan_law.restype = ctypes.c_int32
            lib.sonare_strip_set_pan_law.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_strip_set_channel_delay_samples.restype = ctypes.c_int32
            lib.sonare_strip_set_channel_delay_samples.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_strip_set_vca_offset_db.restype = ctypes.c_int32
            lib.sonare_strip_set_vca_offset_db.argtypes = [ctypes.c_void_p, ctypes.c_float]
        lib.sonare_strip_add_send.restype = ctypes.c_int32
        lib.sonare_strip_add_send.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.sonare_strip_set_send_db.restype = ctypes.c_int32
        lib.sonare_strip_set_send_db.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_float,
        ]
        if hasattr(lib, "sonare_strip_remove_send"):
            lib.sonare_strip_remove_send.restype = ctypes.c_int32
            lib.sonare_strip_remove_send.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        lib.sonare_strip_meter.restype = ctypes.c_int32
        lib.sonare_strip_meter.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareMixMeterSnapshot),
        ]
        if hasattr(lib, "sonare_strip_meter_tap"):
            lib.sonare_strip_meter_tap.restype = ctypes.c_int32
            lib.sonare_strip_meter_tap.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.POINTER(SonareMixMeterSnapshot),
            ]
        lib.sonare_strip_read_goniometer_latest.restype = ctypes.c_size_t
        lib.sonare_strip_read_goniometer_latest.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(SonareMixGoniometerPoint),
            ctypes.c_size_t,
        ]
        lib.sonare_mixer_from_scene_json.restype = ctypes.c_void_p
        lib.sonare_mixer_from_scene_json.argtypes = [
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int,
        ]
        lib.sonare_mixer_to_scene_json.restype = ctypes.c_int32
        lib.sonare_mixer_to_scene_json.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        if hasattr(lib, "sonare_mixer_compile"):
            lib.sonare_mixer_compile.restype = ctypes.c_int32
            lib.sonare_mixer_compile.argtypes = [ctypes.c_void_p]
        if hasattr(lib, "sonare_mixer_get_strip_count"):
            lib.sonare_mixer_get_strip_count.restype = ctypes.c_int32
            lib.sonare_mixer_get_strip_count.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_size_t),
            ]
        if hasattr(lib, "sonare_mixer_add_bus"):
            lib.sonare_mixer_add_bus.restype = ctypes.c_int32
            lib.sonare_mixer_add_bus.argtypes = [
                ctypes.c_void_p,
                ctypes.c_char_p,
                ctypes.c_char_p,
            ]
            lib.sonare_mixer_remove_bus.restype = ctypes.c_int32
            lib.sonare_mixer_remove_bus.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            lib.sonare_mixer_bus_count.restype = ctypes.c_int32
            lib.sonare_mixer_bus_count.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_size_t),
            ]
            lib.sonare_mixer_add_vca_group.restype = ctypes.c_int32
            lib.sonare_mixer_add_vca_group.argtypes = [
                ctypes.c_void_p,
                ctypes.c_char_p,
                ctypes.c_float,
                ctypes.POINTER(ctypes.c_char_p),
                ctypes.c_size_t,
            ]
            lib.sonare_mixer_remove_vca_group.restype = ctypes.c_int32
            lib.sonare_mixer_remove_vca_group.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            lib.sonare_mixer_vca_group_count.restype = ctypes.c_int32
            lib.sonare_mixer_vca_group_count.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_size_t),
            ]
        if hasattr(lib, "sonare_mixer_strip_count"):
            lib.sonare_mixer_strip_count.restype = ctypes.c_size_t
            lib.sonare_mixer_strip_count.argtypes = [ctypes.c_void_p]
            lib.sonare_mixer_strip_at.restype = ctypes.c_void_p
            lib.sonare_mixer_strip_at.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            lib.sonare_mixer_strip_by_id.restype = ctypes.c_void_p
            lib.sonare_mixer_strip_by_id.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            lib.sonare_strip_schedule_insert_automation.restype = ctypes.c_int32
            lib.sonare_strip_schedule_insert_automation.argtypes = [
                ctypes.c_void_p,
                ctypes.c_uint,
                ctypes.c_uint,
                ctypes.c_int64,
                ctypes.c_float,
                ctypes.c_int,
            ]
        if hasattr(lib, "sonare_strip_schedule_fader_automation"):
            for _name in (
                "sonare_strip_schedule_fader_automation",
                "sonare_strip_schedule_pan_automation",
                "sonare_strip_schedule_width_automation",
            ):
                _fn = getattr(lib, _name)
                _fn.restype = ctypes.c_int32
                _fn.argtypes = [
                    ctypes.c_void_p,
                    ctypes.c_int64,
                    ctypes.c_float,
                    ctypes.c_int,
                ]
            lib.sonare_strip_schedule_send_automation.restype = ctypes.c_int32
            lib.sonare_strip_schedule_send_automation.argtypes = [
                ctypes.c_void_p,
                ctypes.c_size_t,
                ctypes.c_int64,
                ctypes.c_float,
                ctypes.c_int,
            ]
        lib.sonare_mixer_process_stereo.restype = ctypes.c_int32
        lib.sonare_mixer_process_stereo.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
        ]
        if hasattr(lib, "sonare_mixer_tail_samples"):
            lib.sonare_mixer_tail_samples.restype = ctypes.c_int32
            lib.sonare_mixer_tail_samples.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_int),
            ]
        if hasattr(lib, "sonare_mixer_drain_tail_stereo"):
            lib.sonare_mixer_drain_tail_stereo.restype = ctypes.c_int32
            lib.sonare_mixer_drain_tail_stereo.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
            ]
        lib.sonare_mixing_scene_preset_names.restype = ctypes.c_char_p
        lib.sonare_mixing_scene_preset_names.argtypes = []
        lib.sonare_mixing_scene_preset_json.restype = ctypes.c_int32
        lib.sonare_mixing_scene_preset_json.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        lib.sonare_mixer_destroy.restype = None
        lib.sonare_mixer_destroy.argtypes = [ctypes.c_void_p]
