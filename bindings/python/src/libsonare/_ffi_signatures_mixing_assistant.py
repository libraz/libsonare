"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes
from typing import Any

from ._ffi_types import *  # noqa: F403,F405

# The two suggest entry points share one signature: parallel per-track channel
# pointer arrays, per-track ids/names/lengths, a flat param list, and a
# caller-freed JSON string. Declared once so the pair cannot drift apart.
_SUGGEST_ARGTYPES: list[Any] = [
    ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
    ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.POINTER(ctypes.c_char_p),
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_size_t,
    ctypes.c_int,
    ctypes.POINTER(SonareMasteringParam),
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_char_p),
]


def configure_mixing_assistant_signatures(lib: ctypes.CDLL) -> None:
    if hasattr(lib, "sonare_mixing_assistant_suggest"):
        lib.sonare_mixing_assistant_suggest.restype = ctypes.c_int32
        lib.sonare_mixing_assistant_suggest.argtypes = list(_SUGGEST_ARGTYPES)
        lib.sonare_mixing_assistant_suggest_scene_json.restype = ctypes.c_int32
        lib.sonare_mixing_assistant_suggest_scene_json.argtypes = list(_SUGGEST_ARGTYPES)
        lib.sonare_mixing_assistant_source_class_names.restype = ctypes.c_char_p
        lib.sonare_mixing_assistant_source_class_names.argtypes = []
        lib.sonare_mixing_assistant_source_class_from_name.restype = ctypes.c_int
        lib.sonare_mixing_assistant_source_class_from_name.argtypes = [ctypes.c_char_p]
