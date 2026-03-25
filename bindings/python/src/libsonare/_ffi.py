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


class SonareStftResult(ctypes.Structure):
    """Maps to SonareStftResult in sonare_c.h."""

    _fields_ = [
        ("n_bins", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("n_fft", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("magnitude", ctypes.POINTER(ctypes.c_float)),
        ("power", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareMelResult(ctypes.Structure):
    """Maps to SonareMelResult in sonare_c.h."""

    _fields_ = [
        ("n_mels", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("power", ctypes.POINTER(ctypes.c_float)),
        ("db", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareMfccResult(ctypes.Structure):
    """Maps to SonareMfccResult in sonare_c.h."""

    _fields_ = [
        ("n_mfcc", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("coefficients", ctypes.POINTER(ctypes.c_float)),
    ]


class SonareChromaResult(ctypes.Structure):
    """Maps to SonareChromaResult in sonare_c.h."""

    _fields_ = [
        ("n_chroma", ctypes.c_int32),
        ("n_frames", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("hop_length", ctypes.c_int32),
        ("features", ctypes.POINTER(ctypes.c_float)),
        ("mean_energy", ctypes.POINTER(ctypes.c_float)),
    ]


class SonarePitchResult(ctypes.Structure):
    """Maps to SonarePitchResult in sonare_c.h."""

    _fields_ = [
        ("n_frames", ctypes.c_int32),
        ("f0", ctypes.POINTER(ctypes.c_float)),
        ("voiced_prob", ctypes.POINTER(ctypes.c_float)),
        ("voiced_flag", ctypes.POINTER(ctypes.c_int32)),
        ("median_f0", ctypes.c_float),
        ("mean_f0", ctypes.c_float),
    ]


class SonareHpssResult(ctypes.Structure):
    """Maps to SonareHpssResult in sonare_c.h."""

    _fields_ = [
        ("harmonic", ctypes.POINTER(ctypes.c_float)),
        ("percussive", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
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

    # --- Features - Spectrogram ---

    # sonare_stft
    lib.sonare_stft.restype = ctypes.c_int32
    lib.sonare_stft.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareStftResult),
    ]

    # sonare_stft_db
    lib.sonare_stft_db.restype = ctypes.c_int32
    lib.sonare_stft_db.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
    ]

    # --- Features - Mel ---

    # sonare_mel_spectrogram
    lib.sonare_mel_spectrogram.restype = ctypes.c_int32
    lib.sonare_mel_spectrogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareMelResult),
    ]

    # sonare_mfcc
    lib.sonare_mfcc.restype = ctypes.c_int32
    lib.sonare_mfcc.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareMfccResult),
    ]

    # --- Features - Chroma ---

    # sonare_chroma
    lib.sonare_chroma.restype = ctypes.c_int32
    lib.sonare_chroma.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareChromaResult),
    ]

    # --- Features - Spectral ---

    # sonare_spectral_centroid
    lib.sonare_spectral_centroid.restype = ctypes.c_int32
    lib.sonare_spectral_centroid.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_bandwidth
    lib.sonare_spectral_bandwidth.restype = ctypes.c_int32
    lib.sonare_spectral_bandwidth.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_rolloff
    lib.sonare_spectral_rolloff.restype = ctypes.c_int32
    lib.sonare_spectral_rolloff.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_spectral_flatness
    lib.sonare_spectral_flatness.restype = ctypes.c_int32
    lib.sonare_spectral_flatness.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_zero_crossing_rate
    lib.sonare_zero_crossing_rate.restype = ctypes.c_int32
    lib.sonare_zero_crossing_rate.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_rms_energy
    lib.sonare_rms_energy.restype = ctypes.c_int32
    lib.sonare_rms_energy.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Features - Pitch ---

    # sonare_pitch_yin
    lib.sonare_pitch_yin.restype = ctypes.c_int32
    lib.sonare_pitch_yin.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonarePitchResult),
    ]

    # sonare_pitch_pyin
    lib.sonare_pitch_pyin.restype = ctypes.c_int32
    lib.sonare_pitch_pyin.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonarePitchResult),
    ]

    # --- Core - Conversion ---

    lib.sonare_hz_to_mel.restype = ctypes.c_float
    lib.sonare_hz_to_mel.argtypes = [ctypes.c_float]

    lib.sonare_mel_to_hz.restype = ctypes.c_float
    lib.sonare_mel_to_hz.argtypes = [ctypes.c_float]

    lib.sonare_hz_to_midi.restype = ctypes.c_float
    lib.sonare_hz_to_midi.argtypes = [ctypes.c_float]

    lib.sonare_midi_to_hz.restype = ctypes.c_float
    lib.sonare_midi_to_hz.argtypes = [ctypes.c_float]

    lib.sonare_hz_to_note.restype = ctypes.c_char_p
    lib.sonare_hz_to_note.argtypes = [ctypes.c_float]

    lib.sonare_note_to_hz.restype = ctypes.c_float
    lib.sonare_note_to_hz.argtypes = [ctypes.c_char_p]

    lib.sonare_frames_to_time.restype = ctypes.c_float
    lib.sonare_frames_to_time.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    lib.sonare_time_to_frames.restype = ctypes.c_int32
    lib.sonare_time_to_frames.argtypes = [ctypes.c_float, ctypes.c_int, ctypes.c_int]

    # --- Core - Resample ---

    lib.sonare_resample.restype = ctypes.c_int32
    lib.sonare_resample.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Free functions for result types ---

    lib.sonare_free_stft_result.restype = None
    lib.sonare_free_stft_result.argtypes = [ctypes.POINTER(SonareStftResult)]

    lib.sonare_free_mel_result.restype = None
    lib.sonare_free_mel_result.argtypes = [ctypes.POINTER(SonareMelResult)]

    lib.sonare_free_mfcc_result.restype = None
    lib.sonare_free_mfcc_result.argtypes = [ctypes.POINTER(SonareMfccResult)]

    lib.sonare_free_chroma_result.restype = None
    lib.sonare_free_chroma_result.argtypes = [ctypes.POINTER(SonareChromaResult)]

    lib.sonare_free_pitch_result.restype = None
    lib.sonare_free_pitch_result.argtypes = [ctypes.POINTER(SonarePitchResult)]

    lib.sonare_free_hpss_result.restype = None
    lib.sonare_free_hpss_result.argtypes = [ctypes.POINTER(SonareHpssResult)]

    lib.sonare_free_ints.restype = None
    lib.sonare_free_ints.argtypes = [ctypes.POINTER(ctypes.c_int32)]

    return lib
