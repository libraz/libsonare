"""Low-level ctypes wrapper for libsonare."""

from __future__ import annotations

import ctypes
import ctypes.util
import os
from pathlib import Path


# --- C structures ---


class SonareKey(ctypes.Structure):
    """Maps to SonareKey in sonare_c.h."""

    _fields_ = [
        ("root", ctypes.c_int32),
        ("mode", ctypes.c_int32),
        ("confidence", ctypes.c_float),
    ]


class SonareTimeSignature(ctypes.Structure):
    """Maps to SonareTimeSignature in sonare_c.h."""

    _fields_ = [
        ("numerator", ctypes.c_int32),
        ("denominator", ctypes.c_int32),
        ("confidence", ctypes.c_float),
    ]


class SonareAnalysisResult(ctypes.Structure):
    """Maps to SonareAnalysisResult in sonare_c.h."""

    _fields_ = [
        ("bpm", ctypes.c_float),
        ("bpm_confidence", ctypes.c_float),
        ("key", SonareKey),
        ("time_signature", SonareTimeSignature),
        ("beat_times", ctypes.POINTER(ctypes.c_float)),
        ("beat_count", ctypes.c_size_t),
    ]


# --- Error codes ---

SONARE_OK = 0
SONARE_ERROR_FILE_NOT_FOUND = 1
SONARE_ERROR_INVALID_FORMAT = 2
SONARE_ERROR_DECODE_FAILED = 3
SONARE_ERROR_INVALID_PARAMETER = 4
SONARE_ERROR_OUT_OF_MEMORY = 5
SONARE_ERROR_UNKNOWN = 99


# --- Library discovery ---


def _find_library() -> str:
    """Find the libsonare shared library.

    Search order:
        1. SONARE_LIB_PATH environment variable
        2. Package-adjacent (wheel distribution)
        3. Build directory (development)
        4. System library path
    """
    env_path = os.environ.get("SONARE_LIB_PATH")
    if env_path and Path(env_path).exists():
        return env_path

    pkg_dir = Path(__file__).parent
    for name in ("libsonare.dylib", "libsonare.so", "sonare.dll"):
        candidate = pkg_dir / name
        if candidate.exists():
            return str(candidate)

    # Dev build dir: src/libsonare/ -> bindings/python -> bindings -> project root
    project_root = pkg_dir.parent.parent.parent.parent
    lib_name = "libsonare.dylib" if os.uname().sysname == "Darwin" else "libsonare.so"
    build_path = project_root / "build" / "lib" / lib_name
    if build_path.exists():
        return str(build_path)

    path = ctypes.util.find_library("sonare")
    if path:
        return path

    raise OSError(
        "libsonare shared library not found. "
        "Set SONARE_LIB_PATH or build with: cmake --build build --parallel"
    )


def load_library(lib_path: str | None = None) -> ctypes.CDLL:
    """Load libsonare and configure function signatures.

    Args:
        lib_path: Explicit path to the shared library. If None, searches
            standard locations.

    Returns:
        Loaded ctypes.CDLL with typed function signatures.

    Raises:
        OSError: If the library cannot be found or loaded.
    """
    path = lib_path or _find_library()
    lib = ctypes.CDLL(path)

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

    # sonare_detect_beats
    lib.sonare_detect_beats.restype = ctypes.c_int32
    lib.sonare_detect_beats.argtypes = [
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

    # --- Memory management ---

    # sonare_free_floats
    lib.sonare_free_floats.restype = None
    lib.sonare_free_floats.argtypes = [ctypes.POINTER(ctypes.c_float)]

    # sonare_free_result
    lib.sonare_free_result.restype = None
    lib.sonare_free_result.argtypes = [ctypes.POINTER(SonareAnalysisResult)]

    # --- Error handling ---

    # sonare_error_message
    lib.sonare_error_message.restype = ctypes.c_char_p
    lib.sonare_error_message.argtypes = [ctypes.c_int32]

    # --- Version ---

    # sonare_version
    lib.sonare_version.restype = ctypes.c_char_p
    lib.sonare_version.argtypes = []

    return lib
