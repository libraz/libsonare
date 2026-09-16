"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_repair_dynamics_signatures(lib: ctypes.CDLL) -> None:
    # --- Mastering: offline repair processors ---

    if hasattr(lib, "sonare_mastering_repair_declick"):
        lib.sonare_mastering_repair_declick.restype = ctypes.c_int32
        lib.sonare_mastering_repair_declick.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDeclickConfig),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    if hasattr(lib, "sonare_mastering_repair_declick_stereo"):
        lib.sonare_mastering_repair_declick_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_repair_declick_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDeclickConfig),
            ctypes.POINTER(SonareDeclickStereoResult),
        ]

    if hasattr(lib, "sonare_mastering_repair_declip_stereo"):
        lib.sonare_mastering_repair_declip_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_repair_declip_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDeclipConfig),
            ctypes.POINTER(SonareDeclipStereoResult),
        ]

    if hasattr(lib, "sonare_mastering_repair_decrackle_stereo"):
        lib.sonare_mastering_repair_decrackle_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_repair_decrackle_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDecrackleConfig),
            ctypes.POINTER(SonareDecrackleStereoResult),
        ]

    if hasattr(lib, "sonare_mastering_repair_dehum_stereo"):
        lib.sonare_mastering_repair_dehum_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_repair_dehum_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDehumConfig),
            ctypes.POINTER(SonareDehumStereoResult),
        ]

    if hasattr(lib, "sonare_mastering_repair_denoise_classical"):
        lib.sonare_mastering_repair_denoise_classical.restype = ctypes.c_int32
        lib.sonare_mastering_repair_denoise_classical.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDenoiseClassicalConfig),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    if hasattr(lib, "sonare_mastering_repair_denoise_classical_stereo"):
        lib.sonare_mastering_repair_denoise_classical_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_repair_denoise_classical_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDenoiseClassicalConfig),
            ctypes.POINTER(SonareDenoiseStereoResult),
        ]

    if hasattr(lib, "sonare_mastering_repair_dereverb_classical_stereo"):
        lib.sonare_mastering_repair_dereverb_classical_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_repair_dereverb_classical_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareDereverbClassicalConfig),
            ctypes.POINTER(SonareDereverbStereoResult),
        ]

    if hasattr(lib, "sonare_mastering_repair_trim_silence_stereo"):
        lib.sonare_mastering_repair_trim_silence_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_repair_trim_silence_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareTrimSilenceConfig),
            ctypes.POINTER(SonareTrimSilenceStereoResult),
        ]

    if hasattr(lib, "sonare_mastering_repair_detect_trim_range_stereo"):
        lib.sonare_mastering_repair_detect_trim_range_stereo.restype = ctypes.c_int32
        lib.sonare_mastering_repair_detect_trim_range_stereo.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(SonareTrimSilenceConfig),
            ctypes.POINTER(SonareTrimRange),
        ]

    # The measure-only entry points: one config type and one POD result each,
    # and nothing allocated, so they share a shape the repairs do not.
    for _name, _detect_cfg, _detection in (
        ("sonare_mastering_repair_detect_clicks", SonareDeclickConfig, SonareClickDetection),
        (
            "sonare_mastering_repair_detect_noise_floor",
            SonareDenoiseClassicalConfig,
            SonareNoiseDetection,
        ),
        ("sonare_mastering_repair_detect_clipping", SonareDeclipConfig, SonareClipDetection),
        ("sonare_mastering_repair_detect_crackle", SonareDecrackleConfig, SonareCrackleDetection),
        ("sonare_mastering_repair_detect_hum", SonareDehumConfig, SonareHumDetection),
        (
            "sonare_mastering_repair_detect_reverb",
            SonareDereverbClassicalConfig,
            SonareReverbDetection,
        ),
        ("sonare_mastering_repair_detect_trim_range", SonareTrimSilenceConfig, SonareTrimRange),
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_int32
            _fn.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(_detect_cfg),
                ctypes.POINTER(_detection),
            ]

    for _name, _repair_cfg in (
        ("sonare_mastering_repair_declip", SonareDeclipConfig),
        ("sonare_mastering_repair_decrackle", SonareDecrackleConfig),
        ("sonare_mastering_repair_dehum", SonareDehumConfig),
        ("sonare_mastering_repair_dereverb_classical", SonareDereverbClassicalConfig),
        ("sonare_mastering_repair_trim_silence", SonareTrimSilenceConfig),
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_int32
            _fn.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(_repair_cfg),
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.POINTER(ctypes.c_size_t),
            ]

    # Config-shaping companion to the dereverb processor: it takes a room
    # estimate and an existing config rather than audio, so it is registered
    # outside the repair loop above.
    if hasattr(lib, "sonare_mastering_repair_dereverb_apply_room_estimate"):
        lib.sonare_mastering_repair_dereverb_apply_room_estimate.restype = ctypes.c_int32
        lib.sonare_mastering_repair_dereverb_apply_room_estimate.argtypes = [
            ctypes.POINTER(SonareRoomEstimate),
            ctypes.POINTER(SonareDereverbClassicalConfig),
        ]

    # --- Mastering: offline dynamics processors ---
    # The dynamics signature appends `int* out_latency_samples` to the repair
    # shape, so we register it separately from the repair loop above.
    for _name, _dynamics_cfg in (
        ("sonare_mastering_dynamics_compressor", SonareCompressorConfig),
        ("sonare_mastering_dynamics_gate", SonareGateConfig),
        ("sonare_mastering_dynamics_transient_shaper", SonareTransientShaperConfig),
    ):
        if hasattr(lib, _name):
            _fn = getattr(lib, _name)
            _fn.restype = ctypes.c_int32
            _fn.argtypes = [
                ctypes.POINTER(ctypes.c_float),
                ctypes.c_size_t,
                ctypes.c_int,
                ctypes.POINTER(_dynamics_cfg),
                ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
                ctypes.POINTER(ctypes.c_size_t),
                ctypes.POINTER(ctypes.c_int),
            ]
