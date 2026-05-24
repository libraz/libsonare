"""Low-level ctypes wrapper for libsonare."""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
from pathlib import Path

# --- C structures ---


class SonareKey(ctypes.Structure):
    """Maps to SonareKey in sonare_c.h."""

    _fields_ = [
        ("root", ctypes.c_int32),
        ("mode", ctypes.c_int32),
        ("confidence", ctypes.c_float),
    ]


class SonareKeyCandidate(ctypes.Structure):
    """Maps to SonareKeyCandidate in sonare_c.h."""

    _fields_ = [
        ("key", SonareKey),
        ("correlation", ctypes.c_float),
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


class SonareLufsResult(ctypes.Structure):
    """Maps to SonareLufsResult in sonare_c.h."""

    _fields_ = [
        ("integrated_lufs", ctypes.c_float),
        ("momentary_lufs", ctypes.c_float),
        ("short_term_lufs", ctypes.c_float),
        ("loudness_range", ctypes.c_float),
    ]


class SonareBpmCandidate(ctypes.Structure):
    """Maps to SonareBpmCandidate in sonare_c.h."""

    _fields_ = [
        ("bpm", ctypes.c_float),
        ("confidence", ctypes.c_float),
    ]


class SonareBpmAnalysisResult(ctypes.Structure):
    """Maps to SonareBpmAnalysisResult in sonare_c.h."""

    _fields_ = [
        ("bpm", ctypes.c_float),
        ("confidence", ctypes.c_float),
        ("candidates", ctypes.POINTER(SonareBpmCandidate)),
        ("candidate_count", ctypes.c_size_t),
        ("autocorrelation", ctypes.POINTER(ctypes.c_float)),
        ("autocorrelation_count", ctypes.c_size_t),
        ("tempogram", ctypes.POINTER(ctypes.c_float)),
        ("tempogram_count", ctypes.c_size_t),
    ]


class SonareAcousticResult(ctypes.Structure):
    """Maps to SonareAcousticResult in sonare_c.h."""

    _fields_ = [
        ("rt60", ctypes.c_float),
        ("edt", ctypes.c_float),
        ("c50", ctypes.c_float),
        ("c80", ctypes.c_float),
        ("d50", ctypes.c_float),
        ("rt60_bands", ctypes.POINTER(ctypes.c_float)),
        ("edt_bands", ctypes.POINTER(ctypes.c_float)),
        ("c50_bands", ctypes.POINTER(ctypes.c_float)),
        ("c80_bands", ctypes.POINTER(ctypes.c_float)),
        ("band_count", ctypes.c_size_t),
        ("confidence", ctypes.c_float),
        ("is_blind", ctypes.c_int),
    ]


class SonareRhythmResult(ctypes.Structure):
    """Maps to SonareRhythmResult in sonare_c.h."""

    _fields_ = [
        ("bpm", ctypes.c_float),
        ("time_signature", SonareTimeSignature),
        ("groove_type", ctypes.c_int32),
        ("syncopation", ctypes.c_float),
        ("pattern_regularity", ctypes.c_float),
        ("tempo_stability", ctypes.c_float),
        ("beat_intervals", ctypes.POINTER(ctypes.c_float)),
        ("beat_interval_count", ctypes.c_size_t),
    ]


class SonareDynamicsResult(ctypes.Structure):
    """Maps to SonareDynamicsResult in sonare_c.h."""

    _fields_ = [
        ("dynamic_range_db", ctypes.c_float),
        ("peak_db", ctypes.c_float),
        ("rms_db", ctypes.c_float),
        ("crest_factor", ctypes.c_float),
        ("loudness_range_db", ctypes.c_float),
        ("is_compressed", ctypes.c_int),
        ("loudness_times", ctypes.POINTER(ctypes.c_float)),
        ("loudness_rms_db", ctypes.POINTER(ctypes.c_float)),
        ("loudness_count", ctypes.c_size_t),
    ]


class SonareTimbreResult(ctypes.Structure):
    """Maps to SonareTimbreResult in sonare_c.h."""

    _fields_ = [
        ("brightness", ctypes.c_float),
        ("warmth", ctypes.c_float),
        ("density", ctypes.c_float),
        ("roughness", ctypes.c_float),
        ("complexity", ctypes.c_float),
        ("spectral_centroid", ctypes.POINTER(ctypes.c_float)),
        ("spectral_centroid_count", ctypes.c_size_t),
        ("spectral_flatness", ctypes.POINTER(ctypes.c_float)),
        ("spectral_flatness_count", ctypes.c_size_t),
        ("spectral_rolloff", ctypes.POINTER(ctypes.c_float)),
        ("spectral_rolloff_count", ctypes.c_size_t),
    ]


class SonareChord(ctypes.Structure):
    """Maps to SonareChord in sonare_c.h."""

    _fields_ = [
        ("root", ctypes.c_int32),
        ("quality", ctypes.c_int32),
        ("start", ctypes.c_float),
        ("end", ctypes.c_float),
        ("confidence", ctypes.c_float),
        ("bass", ctypes.c_int32),
    ]


class SonareChordAnalysisResult(ctypes.Structure):
    """Maps to SonareChordAnalysisResult in sonare_c.h."""

    _fields_ = [
        ("chords", ctypes.POINTER(SonareChord)),
        ("chord_count", ctypes.c_size_t),
    ]


class SonareChordDetectionOptions(ctypes.Structure):
    """Maps to SonareChordDetectionOptions in sonare_c.h."""

    _fields_ = [
        ("min_duration", ctypes.c_float),
        ("smoothing_window", ctypes.c_float),
        ("threshold", ctypes.c_float),
        ("use_triads_only", ctypes.c_int),
        ("n_fft", ctypes.c_int),
        ("hop_length", ctypes.c_int),
        ("use_beat_sync", ctypes.c_int),
        ("use_hmm", ctypes.c_int),
        ("hmm_beam_width", ctypes.c_int),
        ("use_key_context", ctypes.c_int),
        ("key_root", ctypes.c_int32),
        ("key_mode", ctypes.c_int32),
        ("detect_inversions", ctypes.c_int),
        ("chroma_method", ctypes.c_int),
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


class SonareMasteringConfig(ctypes.Structure):
    """Maps to SonareMasteringConfig in sonare_c.h."""

    _fields_ = [
        ("target_lufs", ctypes.c_float),
        ("ceiling_db", ctypes.c_float),
        ("true_peak_oversample", ctypes.c_int32),
    ]


class SonareMasteringResult(ctypes.Structure):
    """Maps to SonareMasteringResult in sonare_c.h."""

    _fields_ = [
        ("samples", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
        ("input_lufs", ctypes.c_float),
        ("output_lufs", ctypes.c_float),
        ("applied_gain_db", ctypes.c_float),
        ("latency_samples", ctypes.c_int32),
    ]


class SonareMasteringParam(ctypes.Structure):
    """Maps to SonareMasteringParam in sonare_c.h."""

    _fields_ = [
        ("key", ctypes.c_char_p),
        ("value", ctypes.c_double),
    ]


class SonareMasteringStereoResult(ctypes.Structure):
    """Maps to SonareMasteringStereoResult in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
        ("input_lufs", ctypes.c_float),
        ("output_lufs", ctypes.c_float),
        ("applied_gain_db", ctypes.c_float),
        ("latency_samples", ctypes.c_int32),
    ]


class SonareMasteringChainResult(ctypes.Structure):
    """Maps to SonareMasteringChainResult in sonare_c.h."""

    _fields_ = [
        ("samples", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
        ("input_lufs", ctypes.c_float),
        ("output_lufs", ctypes.c_float),
        ("applied_gain_db", ctypes.c_float),
        ("stages", ctypes.POINTER(ctypes.c_char_p)),
        ("stages_count", ctypes.c_size_t),
    ]


class SonareMasteringChainStereoResult(ctypes.Structure):
    """Maps to SonareMasteringChainStereoResult in sonare_c.h."""

    _fields_ = [
        ("left", ctypes.POINTER(ctypes.c_float)),
        ("right", ctypes.POINTER(ctypes.c_float)),
        ("length", ctypes.c_size_t),
        ("sample_rate", ctypes.c_int32),
        ("input_lufs", ctypes.c_float),
        ("output_lufs", ctypes.c_float),
        ("applied_gain_db", ctypes.c_float),
        ("stages", ctypes.POINTER(ctypes.c_char_p)),
        ("stages_count", ctypes.c_size_t),
    ]


# Progress callback: void(float progress, const char* stage, void* user_data).
# Maps to SonareMasteringProgressCallback in sonare_c.h.
SonareMasteringProgressCallback = ctypes.CFUNCTYPE(
    None,
    ctypes.c_float,
    ctypes.c_char_p,
    ctypes.c_void_p,
)


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
    # In editable/source checkouts, prefer the freshly built shared library over
    # any package-adjacent copy that may have been left by an older build.
    project_root = pkg_dir.parent.parent.parent.parent
    lib_name = "libsonare.dylib" if platform.system() == "Darwin" else "libsonare.so"
    build_path = project_root / "build" / "lib" / lib_name
    if build_path.exists():
        return str(build_path)

    for name in ("libsonare.dylib", "libsonare.so", "sonare.dll"):
        candidate = pkg_dir / name
        if candidate.exists():
            return str(candidate)

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

    lib.sonare_audio_detect_bpm.restype = ctypes.c_int32
    lib.sonare_audio_detect_bpm.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]

    lib.sonare_audio_detect_key.restype = ctypes.c_int32
    lib.sonare_audio_detect_key.argtypes = [ctypes.c_void_p, ctypes.POINTER(SonareKey)]

    lib.sonare_audio_detect_beats.restype = ctypes.c_int32
    lib.sonare_audio_detect_beats.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_audio_detect_downbeats.restype = ctypes.c_int32
    lib.sonare_audio_detect_downbeats.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_audio_detect_onsets.restype = ctypes.c_int32
    lib.sonare_audio_detect_onsets.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_audio_analyze.restype = ctypes.c_int32
    lib.sonare_audio_analyze.argtypes = [ctypes.c_void_p, ctypes.POINTER(SonareAnalysisResult)]

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

    # sonare_detect_key_with_options
    lib.sonare_detect_key_with_options.restype = ctypes.c_int32
    lib.sonare_detect_key_with_options.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(SonareKey),
    ]

    lib.sonare_detect_key_with_options_and_modes.restype = ctypes.c_int32
    lib.sonare_detect_key_with_options_and_modes.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.POINTER(SonareKey),
    ]

    lib.sonare_detect_key_with_extended_options.restype = ctypes.c_int32
    lib.sonare_detect_key_with_extended_options.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.c_int32,
        ctypes.c_char_p,
        ctypes.POINTER(SonareKey),
    ]

    lib.sonare_detect_key_candidates.restype = ctypes.c_int32
    lib.sonare_detect_key_candidates.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(SonareKeyCandidate)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_detect_key_candidates_with_modes.restype = ctypes.c_int32
    lib.sonare_detect_key_candidates_with_modes.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(SonareKeyCandidate)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_detect_key_candidates_with_extended_options.restype = ctypes.c_int32
    lib.sonare_detect_key_candidates_with_extended_options.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_size_t,
        ctypes.c_int32,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.POINTER(SonareKeyCandidate)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.sonare_free_key_candidates.restype = None
    lib.sonare_free_key_candidates.argtypes = [ctypes.POINTER(SonareKeyCandidate)]

    # sonare_detect_beats
    lib.sonare_detect_beats.restype = ctypes.c_int32
    lib.sonare_detect_beats.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_detect_downbeats.restype = ctypes.c_int32
    lib.sonare_detect_downbeats.argtypes = [
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

    lib.sonare_analyze_bpm.restype = ctypes.c_int32
    lib.sonare_analyze_bpm.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareBpmAnalysisResult),
    ]

    lib.sonare_analyze_impulse_response.restype = ctypes.c_int32
    lib.sonare_analyze_impulse_response.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareAcousticResult),
    ]

    lib.sonare_detect_acoustic.restype = ctypes.c_int32
    lib.sonare_detect_acoustic.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(SonareAcousticResult),
    ]

    lib.sonare_analyze_rhythm.restype = ctypes.c_int32
    lib.sonare_analyze_rhythm.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareRhythmResult),
    ]

    lib.sonare_analyze_dynamics.restype = ctypes.c_int32
    lib.sonare_analyze_dynamics.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(SonareDynamicsResult),
    ]

    lib.sonare_analyze_timbre.restype = ctypes.c_int32
    lib.sonare_analyze_timbre.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(SonareTimbreResult),
    ]

    lib.sonare_detect_chords.restype = ctypes.c_int32
    lib.sonare_detect_chords.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareChordAnalysisResult),
    ]

    lib.sonare_detect_chords_ex.restype = ctypes.c_int32
    lib.sonare_detect_chords_ex.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareChordDetectionOptions),
        ctypes.POINTER(SonareChordAnalysisResult),
    ]

    # --- Memory management ---

    # sonare_free_floats
    lib.sonare_free_floats.restype = None
    lib.sonare_free_floats.argtypes = [ctypes.POINTER(ctypes.c_float)]

    # sonare_free_result
    lib.sonare_free_result.restype = None
    lib.sonare_free_result.argtypes = [ctypes.POINTER(SonareAnalysisResult)]

    lib.sonare_free_bpm_analysis_result.restype = None
    lib.sonare_free_bpm_analysis_result.argtypes = [ctypes.POINTER(SonareBpmAnalysisResult)]

    lib.sonare_free_acoustic_result.restype = None
    lib.sonare_free_acoustic_result.argtypes = [ctypes.POINTER(SonareAcousticResult)]

    lib.sonare_free_rhythm_result.restype = None
    lib.sonare_free_rhythm_result.argtypes = [ctypes.POINTER(SonareRhythmResult)]

    lib.sonare_free_dynamics_result.restype = None
    lib.sonare_free_dynamics_result.argtypes = [ctypes.POINTER(SonareDynamicsResult)]

    lib.sonare_free_timbre_result.restype = None
    lib.sonare_free_timbre_result.argtypes = [ctypes.POINTER(SonareTimbreResult)]

    lib.sonare_free_chord_analysis_result.restype = None
    lib.sonare_free_chord_analysis_result.argtypes = [ctypes.POINTER(SonareChordAnalysisResult)]

    # --- Error handling ---

    # sonare_error_message
    lib.sonare_error_message.restype = ctypes.c_char_p
    lib.sonare_error_message.argtypes = [ctypes.c_int32]

    # sonare_last_error_message: returns the detailed thread-local message for the
    # most recent error. Returns an empty (but non-NULL) string when nothing has been
    # recorded. Only meaningful when a preceding C API call returned non-OK.
    lib.sonare_last_error_message.restype = ctypes.c_char_p
    lib.sonare_last_error_message.argtypes = []

    # --- Version ---

    # sonare_version
    lib.sonare_version.restype = ctypes.c_char_p
    lib.sonare_version.argtypes = []

    # sonare_has_ffmpeg_support: returns 1 if the shared library was compiled
    # with FFmpeg-backed decoding for M4A/AAC/FLAC/OGG, 0 otherwise.
    lib.sonare_has_ffmpeg_support.restype = ctypes.c_int
    lib.sonare_has_ffmpeg_support.argtypes = []

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

    lib.sonare_frames_to_samples.restype = ctypes.c_int32
    lib.sonare_frames_to_samples.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    lib.sonare_samples_to_frames.restype = ctypes.c_int32
    lib.sonare_samples_to_frames.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]

    for name in (
        "sonare_power_to_db",
        "sonare_amplitude_to_db",
    ):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    for name in (
        "sonare_db_to_power",
        "sonare_db_to_amplitude",
    ):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    for name in ("sonare_preemphasis", "sonare_deemphasis"):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.c_float,
            ctypes.c_int,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    lib.sonare_trim_silence.restype = ctypes.c_int32
    lib.sonare_trim_silence.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_split_silence.restype = ctypes.c_int32
    lib.sonare_split_silence.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_frame_signal.restype = ctypes.c_int32
    lib.sonare_frame_signal.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    for name in ("sonare_pad_center", "sonare_fix_length"):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_float,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    lib.sonare_fix_frames.restype = ctypes.c_int32
    lib.sonare_fix_frames.argtypes = [
        ctypes.POINTER(ctypes.c_int),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_peak_pick.restype = ctypes.c_int32
    lib.sonare_peak_pick.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_vector_normalize.restype = ctypes.c_int32
    lib.sonare_vector_normalize.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_pcen.restype = ctypes.c_int32
    lib.sonare_pcen.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_tonnetz.restype = ctypes.c_int32
    lib.sonare_tonnetz.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    lib.sonare_tempogram.restype = ctypes.c_int32
    lib.sonare_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_cyclic_tempogram.restype = ctypes.c_int32
    lib.sonare_cyclic_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    lib.sonare_plp.restype = ctypes.c_int32
    lib.sonare_plp.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_onset_strength
    lib.sonare_onset_strength.restype = ctypes.c_int32
    lib.sonare_onset_strength.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_fourier_tempogram
    lib.sonare_fourier_tempogram.restype = ctypes.c_int32
    lib.sonare_fourier_tempogram.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_tempogram_ratio
    lib.sonare_tempogram_ratio.restype = ctypes.c_int32
    lib.sonare_tempogram_ratio.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_nnls_chroma
    lib.sonare_nnls_chroma.restype = ctypes.c_int32
    lib.sonare_nnls_chroma.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_lufs
    lib.sonare_lufs.restype = ctypes.c_int32
    lib.sonare_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareLufsResult),
    ]

    # sonare_momentary_lufs
    lib.sonare_momentary_lufs.restype = ctypes.c_int32
    lib.sonare_momentary_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_short_term_lufs
    lib.sonare_short_term_lufs.restype = ctypes.c_int32
    lib.sonare_short_term_lufs.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

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

    if hasattr(lib, "sonare_mastering_process"):
        lib.sonare_mastering_process.restype = ctypes.c_int32
        lib.sonare_mastering_process.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringConfig),
            ctypes.POINTER(SonareMasteringResult),
        ]
        lib.sonare_free_mastering_result.restype = None
        lib.sonare_free_mastering_result.argtypes = [ctypes.POINTER(SonareMasteringResult)]
        lib.sonare_mastering_apply_processor.restype = ctypes.c_int32
        lib.sonare_mastering_apply_processor.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(SonareMasteringResult),
        ]
        lib.sonare_mastering_apply_processor_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_apply_processor_stereo.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(SonareMasteringStereoResult),
        ]
        lib.sonare_mastering_processor_names.restype = ctypes.c_char_p
        lib.sonare_mastering_processor_names.argtypes = []
        lib.sonare_mastering_pair_processor_names.restype = ctypes.c_char_p
        lib.sonare_mastering_pair_processor_names.argtypes = []
        lib.sonare_mastering_pair_analysis_names.restype = ctypes.c_char_p
        lib.sonare_mastering_pair_analysis_names.argtypes = []
        lib.sonare_mastering_stereo_analysis_names.restype = ctypes.c_char_p
        lib.sonare_mastering_stereo_analysis_names.argtypes = []
        lib.sonare_mastering_apply_pair_processor.restype = ctypes.c_int32
        lib.sonare_mastering_apply_pair_processor.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(SonareMasteringResult),
        ]
        lib.sonare_mastering_analyze_pair.restype = ctypes.c_int32
        lib.sonare_mastering_analyze_pair.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        lib.sonare_mastering_analyze_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_analyze_stereo.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareMasteringParam),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        lib.sonare_free_mastering_stereo_result.restype = None
        lib.sonare_free_mastering_stereo_result.argtypes = [
            ctypes.POINTER(SonareMasteringStereoResult)
        ]
        lib.sonare_free_string.restype = None
        lib.sonare_free_string.argtypes = [ctypes.c_void_p]
        if hasattr(lib, "sonare_mastering_chain"):
            lib.sonare_mastering_chain.restype = ctypes.c_int32
            lib.sonare_mastering_chain.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(SonareMasteringChainResult),
            ]
            lib.sonare_mastering_chain_stereo.restype = ctypes.c_int32
            lib.sonare_mastering_chain_stereo.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(SonareMasteringChainStereoResult),
            ]
            lib.sonare_free_mastering_chain_result.restype = None
            lib.sonare_free_mastering_chain_result.argtypes = [
                ctypes.POINTER(SonareMasteringChainResult)
            ]
            lib.sonare_free_mastering_chain_stereo_result.restype = None
            lib.sonare_free_mastering_chain_stereo_result.argtypes = [
                ctypes.POINTER(SonareMasteringChainStereoResult)
            ]
        if hasattr(lib, "sonare_streaming_mastering_chain_create"):
            lib.sonare_streaming_mastering_chain_create.restype = ctypes.c_void_p
            lib.sonare_streaming_mastering_chain_create.argtypes = [
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
            ]
            lib.sonare_streaming_mastering_chain_prepare.restype = ctypes.c_int32
            lib.sonare_streaming_mastering_chain_prepare.argtypes = [
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_streaming_mastering_chain_process_mono.restype = ctypes.c_int32
            lib.sonare_streaming_mastering_chain_process_mono.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
            ]
            lib.sonare_streaming_mastering_chain_process_stereo.restype = ctypes.c_int32
            lib.sonare_streaming_mastering_chain_process_stereo.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
            ]
            lib.sonare_streaming_mastering_chain_reset.restype = ctypes.c_int32
            lib.sonare_streaming_mastering_chain_reset.argtypes = [ctypes.c_void_p]
            lib.sonare_streaming_mastering_chain_latency_samples.restype = ctypes.c_int
            lib.sonare_streaming_mastering_chain_latency_samples.argtypes = [ctypes.c_void_p]
            lib.sonare_streaming_mastering_chain_destroy.restype = None
            lib.sonare_streaming_mastering_chain_destroy.argtypes = [ctypes.c_void_p]
        if hasattr(lib, "sonare_mastering_preset_names"):
            lib.sonare_mastering_preset_names.restype = ctypes.c_char_p
            lib.sonare_mastering_preset_names.argtypes = []
        if hasattr(lib, "sonare_master_audio"):
            lib.sonare_master_audio.restype = ctypes.c_int32
            lib.sonare_master_audio.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(SonareMasteringChainResult),
            ]
            lib.sonare_master_audio_stereo.restype = ctypes.c_int32
            lib.sonare_master_audio_stereo.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(SonareMasteringChainStereoResult),
            ]
        if hasattr(lib, "sonare_mastering_chain_with_progress"):
            lib.sonare_mastering_chain_with_progress.restype = ctypes.c_int32
            lib.sonare_mastering_chain_with_progress.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                SonareMasteringProgressCallback,
                ctypes.c_void_p,
                ctypes.POINTER(SonareMasteringChainResult),
            ]
            lib.sonare_mastering_chain_stereo_with_progress.restype = ctypes.c_int32
            lib.sonare_mastering_chain_stereo_with_progress.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                SonareMasteringProgressCallback,
                ctypes.c_void_p,
                ctypes.POINTER(SonareMasteringChainStereoResult),
            ]
        if hasattr(lib, "sonare_master_audio_with_progress"):
            lib.sonare_master_audio_with_progress.restype = ctypes.c_int32
            lib.sonare_master_audio_with_progress.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                SonareMasteringProgressCallback,
                ctypes.c_void_p,
                ctypes.POINTER(SonareMasteringChainResult),
            ]
            lib.sonare_master_audio_stereo_with_progress.restype = ctypes.c_int32
            lib.sonare_master_audio_stereo_with_progress.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                SonareMasteringProgressCallback,
                ctypes.c_void_p,
                ctypes.POINTER(SonareMasteringChainStereoResult),
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
