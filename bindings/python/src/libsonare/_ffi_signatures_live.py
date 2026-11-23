"""ctypes function signatures for live-device sessions."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403


def configure_live_signatures(lib: ctypes.CDLL) -> None:
    lib.sonare_has_live_devices.restype = ctypes.c_int32
    lib.sonare_has_live_devices.argtypes = []

    lib.sonare_live_audio_devices.restype = ctypes.c_int32
    lib.sonare_live_audio_devices.argtypes = [ctypes.POINTER(SonareLiveAudioDeviceList)]
    lib.sonare_free_live_audio_device_list.restype = None
    lib.sonare_free_live_audio_device_list.argtypes = [ctypes.POINTER(SonareLiveAudioDeviceList)]

    lib.sonare_live_midi_sources.restype = ctypes.c_int32
    lib.sonare_live_midi_sources.argtypes = [ctypes.POINTER(SonareLiveMidiDeviceList)]
    lib.sonare_free_live_midi_device_list.restype = None
    lib.sonare_free_live_midi_device_list.argtypes = [ctypes.POINTER(SonareLiveMidiDeviceList)]

    lib.sonare_live_session_config_init.restype = None
    lib.sonare_live_session_config_init.argtypes = [ctypes.POINTER(SonareLiveSessionConfig)]
    lib.sonare_live_session_create.restype = ctypes.c_int32
    lib.sonare_live_session_create.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareLiveSessionConfig),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.sonare_live_session_destroy.restype = None
    lib.sonare_live_session_destroy.argtypes = [ctypes.c_void_p]
    lib.sonare_live_session_status.restype = ctypes.c_int32
    lib.sonare_live_session_status.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareLiveSessionStatus),
    ]
    lib.sonare_live_session_captured_midi.restype = ctypes.c_int32
    lib.sonare_live_session_captured_midi.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SonareMidiEventList),
    ]
