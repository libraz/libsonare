"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_extra_signatures(lib: ctypes.CDLL) -> None:
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

    # --- Features - spectral contrast / poly / zero-crossings / tuning ---

    # sonare_spectral_contrast (matrix2d: out_rows/out_cols are int*)
    lib.sonare_spectral_contrast.restype = ctypes.c_int32
    lib.sonare_spectral_contrast.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_poly_features (matrix2d: out_rows/out_cols are int*)
    lib.sonare_poly_features.restype = ctypes.c_int32
    lib.sonare_poly_features.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_zero_crossings (int-vector)
    lib.sonare_zero_crossings.restype = ctypes.c_int32
    lib.sonare_zero_crossings.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_pitch_tuning (scalar)
    lib.sonare_pitch_tuning.restype = ctypes.c_int32
    lib.sonare_pitch_tuning.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
    ]

    # sonare_estimate_tuning (scalar)
    lib.sonare_estimate_tuning.restype = ctypes.c_int32
    lib.sonare_estimate_tuning.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
    ]

    # --- Effects / decomposition - decompose / nn_filter / remix / pv / hpss+res ---

    # sonare_decompose (two flat matrices W, H)
    lib.sonare_decompose.restype = ctypes.c_int32
    lib.sonare_decompose.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_nn_filter (matrix: n_features x n_frames)
    lib.sonare_nn_filter.restype = ctypes.c_int32
    lib.sonare_nn_filter.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_remix (audio)
    lib.sonare_remix.restype = ctypes.c_int32
    lib.sonare_remix.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # sonare_hpss_with_residual (three audio buffers)
    lib.sonare_hpss_with_residual.restype = ctypes.c_int32
    lib.sonare_hpss_with_residual.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_int),
    ]

    # sonare_phase_vocoder (audio)
    lib.sonare_phase_vocoder.restype = ctypes.c_int32
    lib.sonare_phase_vocoder.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_float,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_size_t),
    ]

    # --- Metering - multi-channel / standards-compliant LUFS (offline) ---

    # sonare_lufs_interleaved (LufsResult fields, no heap pointers)
    lib.sonare_lufs_interleaved.restype = ctypes.c_int32
    lib.sonare_lufs_interleaved.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(SonareLufsResult),
    ]

    # sonare_ebur128_loudness_range (scalar)
    lib.sonare_ebur128_loudness_range.restype = ctypes.c_int32
    lib.sonare_ebur128_loudness_range.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_float),
    ]
