"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_mastering_signatures(lib: ctypes.CDLL) -> None:
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
        if hasattr(lib, "sonare_mastering_insert_names"):
            lib.sonare_mastering_insert_names.restype = ctypes.c_char_p
            lib.sonare_mastering_insert_names.argtypes = []
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
        if hasattr(lib, "sonare_mastering_apply_pair_processor_ex"):
            lib.sonare_mastering_apply_pair_processor_ex.restype = ctypes.c_int32
            lib.sonare_mastering_apply_pair_processor_ex.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
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
            ctypes.POINTER(ctypes.c_char_p),
        ]
        if hasattr(lib, "sonare_mastering_analyze_pair_ex"):
            lib.sonare_mastering_analyze_pair_ex.restype = ctypes.c_int32
            lib.sonare_mastering_analyze_pair_ex.argtypes = [
                ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_char_p),
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
            ctypes.POINTER(ctypes.c_char_p),
        ]
        if hasattr(lib, "sonare_mastering_streaming_preview"):
            lib.sonare_mastering_streaming_preview.restype = ctypes.c_int32
            lib.sonare_mastering_streaming_preview.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareStreamingPlatform),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_char_p),
            ]
        if hasattr(lib, "sonare_mastering_assistant_suggest"):
            lib.sonare_mastering_assistant_suggest.restype = ctypes.c_int32
            lib.sonare_mastering_assistant_suggest.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_char_p),
            ]
        if hasattr(lib, "sonare_mastering_audio_profile"):
            lib.sonare_mastering_audio_profile.restype = ctypes.c_int32
            lib.sonare_mastering_audio_profile.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(SonareMasteringParam),
                ctypes.c_size_t,
                ctypes.POINTER(ctypes.c_char_p),
            ]
        lib.sonare_free_mastering_stereo_result.restype = None
        lib.sonare_free_mastering_stereo_result.argtypes = [
            ctypes.POINTER(SonareMasteringStereoResult)
        ]
        lib.sonare_free_string.restype = None
        lib.sonare_free_string.argtypes = [ctypes.c_char_p]
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
            if hasattr(lib, "sonare_streaming_mastering_chain_create_ex"):
                lib.sonare_streaming_mastering_chain_create_ex.restype = ctypes.c_void_p
                lib.sonare_streaming_mastering_chain_create_ex.argtypes = [
                    ctypes.POINTER(SonareMasteringParam),
                    ctypes.c_size_t,
                    ctypes.c_float,
                    ctypes.c_float,
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
        if hasattr(lib, "sonare_eq_create"):
            lib.sonare_eq_create.restype = ctypes.c_void_p
            lib.sonare_eq_create.argtypes = [ctypes.c_double, ctypes.c_int]
            lib.sonare_eq_destroy.restype = None
            lib.sonare_eq_destroy.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_set_band.restype = ctypes.c_int32
            lib.sonare_eq_set_band.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
            lib.sonare_eq_clear.restype = None
            lib.sonare_eq_clear.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_set_phase_mode.restype = ctypes.c_int32
            lib.sonare_eq_set_phase_mode.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_eq_match.restype = ctypes.c_int32
            lib.sonare_eq_match.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_float),
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_eq_set_auto_gain.restype = None
            lib.sonare_eq_set_auto_gain.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.sonare_eq_last_auto_gain_db.restype = ctypes.c_float
            lib.sonare_eq_last_auto_gain_db.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_set_gain_scale.restype = ctypes.c_int32
            lib.sonare_eq_set_gain_scale.argtypes = [ctypes.c_void_p, ctypes.c_float]
            lib.sonare_eq_set_output_gain_db.restype = ctypes.c_int32
            lib.sonare_eq_set_output_gain_db.argtypes = [ctypes.c_void_p, ctypes.c_float]
            lib.sonare_eq_set_output_pan.restype = ctypes.c_int32
            lib.sonare_eq_set_output_pan.argtypes = [ctypes.c_void_p, ctypes.c_float]
            lib.sonare_eq_latency_samples.restype = ctypes.c_int
            lib.sonare_eq_latency_samples.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_set_sidechain.restype = ctypes.c_int32
            lib.sonare_eq_set_sidechain.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_eq_clear_sidechain.restype = None
            lib.sonare_eq_clear_sidechain.argtypes = [ctypes.c_void_p]
            lib.sonare_eq_process.restype = ctypes.c_int32
            lib.sonare_eq_process.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.c_int,
                ctypes.c_int,
            ]
            lib.sonare_eq_spectrum.restype = ctypes.c_int32
            lib.sonare_eq_spectrum.argtypes = [
                ctypes.c_void_p,
                ctypes.POINTER(SonareEqSnapshot),
            ]
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
