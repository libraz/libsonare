"""ctypes function signatures for libsonare."""
# ruff: noqa: F405

from __future__ import annotations

import ctypes

from ._ffi_types import *  # noqa: F403,F405


def configure_repair_dynamics_signatures(lib: ctypes.CDLL) -> None:
    # --- Mastering: offline repair processors ---

    lib.sonare_mastering_repair_declick.restype = ctypes.c_int32
    lib.sonare_mastering_repair_declick.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDeclickConfig),
        ctypes.POINTER(SonareDeclickResult),
    ]

    lib.sonare_mastering_repair_declick_stereo.restype = ctypes.c_int32
    lib.sonare_mastering_repair_declick_stereo.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDeclickConfig),
        ctypes.POINTER(SonareDeclickStereoResult),
    ]

    lib.sonare_mastering_repair_declip_stereo.restype = ctypes.c_int32
    lib.sonare_mastering_repair_declip_stereo.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDeclipConfig),
        ctypes.POINTER(SonareDeclipStereoResult),
    ]

    lib.sonare_mastering_repair_decrackle_stereo.restype = ctypes.c_int32
    lib.sonare_mastering_repair_decrackle_stereo.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDecrackleConfig),
        ctypes.POINTER(SonareDecrackleStereoResult),
    ]

    lib.sonare_mastering_repair_dehum_stereo.restype = ctypes.c_int32
    lib.sonare_mastering_repair_dehum_stereo.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDehumConfig),
        ctypes.POINTER(SonareDehumStereoResult),
    ]

    lib.sonare_mastering_repair_denoise_classical.restype = ctypes.c_int32
    lib.sonare_mastering_repair_denoise_classical.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDenoiseClassicalConfig),
        ctypes.POINTER(SonareDenoiseResult),
    ]

    lib.sonare_mastering_repair_denoise_classical_stereo.restype = ctypes.c_int32
    lib.sonare_mastering_repair_denoise_classical_stereo.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDenoiseClassicalConfig),
        ctypes.POINTER(SonareDenoiseStereoResult),
    ]

    lib.sonare_mastering_repair_dereverb_classical_stereo.restype = ctypes.c_int32
    lib.sonare_mastering_repair_dereverb_classical_stereo.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareDereverbClassicalConfig),
        ctypes.POINTER(SonareDereverbStereoResult),
    ]

    # The N-channel repairs take a pointer table in and a pointer table out, and
    # allocate nothing: the output planes are the caller's, written in place, so
    # the result is a plain report out-param rather than one of the *StereoResult
    # structs the pair entries fill with heap buffers.
    # One analysis and one application over any channel count: planar channels in,
    # JSON in and out, and the application writes caller-owned output planes.
    lib.sonare_mastering_repair_analyze.restype = ctypes.c_int32
    lib.sonare_mastering_repair_analyze.argtypes = [
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_size_t,
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_char_p),
    ]
    lib.sonare_mastering_repair_apply.restype = ctypes.c_int32
    lib.sonare_mastering_repair_apply.argtypes = [
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.c_size_t,
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.c_char_p,
        SonareMasteringProgressCallback,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
        ctypes.POINTER(ctypes.c_char_p),
        SonareCancelCallback,
        ctypes.c_void_p,
    ]

    for _name, _linked_cfg, _linked_report in (
        (
            "sonare_mastering_repair_denoise_classical_linked",
            SonareDenoiseClassicalConfig,
            SonareDenoiseReport,
        ),
        (
            "sonare_mastering_repair_dereverb_classical_linked",
            SonareDereverbClassicalConfig,
            SonareDereverbReport,
        ),
    ):
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(_linked_cfg),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(_linked_report),
        ]

    lib.sonare_mastering_repair_trim_silence_stereo.restype = ctypes.c_int32
    lib.sonare_mastering_repair_trim_silence_stereo.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_int,
        ctypes.POINTER(SonareTrimSilenceConfig),
        ctypes.POINTER(SonareTrimSilenceStereoResult),
    ]

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
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(_detect_cfg),
            ctypes.POINTER(_detection),
        ]

    # The band grid the noise floor is reported on: analysis geometry alone
    # decides it, so it takes neither audio nor a config and is registered
    # outside the detection loop above.
    lib.sonare_mastering_repair_noise_band_bins.restype = ctypes.c_int32
    lib.sonare_mastering_repair_noise_band_bins.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
    ]

    for _name, _repair_cfg, _repair_result in (
        ("sonare_mastering_repair_declip", SonareDeclipConfig, SonareDeclipResult),
        ("sonare_mastering_repair_decrackle", SonareDecrackleConfig, SonareDecrackleResult),
        ("sonare_mastering_repair_dehum", SonareDehumConfig, SonareDehumResult),
        (
            "sonare_mastering_repair_dereverb_classical",
            SonareDereverbClassicalConfig,
            SonareDereverbResult,
        ),
        ("sonare_mastering_repair_trim_silence", SonareTrimSilenceConfig, SonareTrimSilenceResult),
    ):
        _fn = getattr(lib, _name)
        _fn.restype = ctypes.c_int32
        _fn.argtypes = [
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.POINTER(_repair_cfg),
            ctypes.POINTER(_repair_result),
        ]

    # Config-shaping companion to the dereverb processor: it takes a room
    # estimate and an existing config rather than audio, so it is registered
    # outside the repair loop above.
    lib.sonare_mastering_repair_dereverb_apply_room_estimate.restype = ctypes.c_int32
    lib.sonare_mastering_repair_dereverb_apply_room_estimate.argtypes = [
        ctypes.POINTER(SonareRoomEstimate),
        ctypes.POINTER(SonareDereverbClassicalConfig),
    ]

    # The config initialisers: each writes sizeof and the defaults.
    for _name, _init_cfg in (
        ("sonare_declick_config_init", SonareDeclickConfig),
        ("sonare_denoise_classical_config_init", SonareDenoiseClassicalConfig),
        ("sonare_declip_config_init", SonareDeclipConfig),
        ("sonare_decrackle_config_init", SonareDecrackleConfig),
        ("sonare_dehum_config_init", SonareDehumConfig),
        ("sonare_dereverb_classical_config_init", SonareDereverbClassicalConfig),
        ("sonare_trim_silence_config_init", SonareTrimSilenceConfig),
        ("sonare_compressor_config_init", SonareCompressorConfig),
        ("sonare_gate_config_init", SonareGateConfig),
        ("sonare_transient_shaper_config_init", SonareTransientShaperConfig),
    ):
        _fn = getattr(lib, _name)
        _fn.restype = None
        _fn.argtypes = [ctypes.POINTER(_init_cfg)]

    # --- Mastering: offline dynamics processors ---
    # The dynamics signature appends `int* out_latency_samples` to the repair
    # shape, so we register it separately from the repair loop above.
    for _name, _dynamics_cfg in (
        ("sonare_mastering_dynamics_compressor", SonareCompressorConfig),
        ("sonare_mastering_dynamics_gate", SonareGateConfig),
        ("sonare_mastering_dynamics_transient_shaper", SonareTransientShaperConfig),
    ):
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

    # Each mono repair result is released by its own function, which frees the
    # samples and empties the struct.
    for _name, _mono_result in (
        ("declick", SonareDeclickResult),
        ("declip", SonareDeclipResult),
        ("decrackle", SonareDecrackleResult),
        ("dehum", SonareDehumResult),
        ("denoise", SonareDenoiseResult),
        ("dereverb", SonareDereverbResult),
        ("trim_silence", SonareTrimSilenceResult),
    ):
        _free = getattr(lib, f"sonare_free_{_name}_result")
        _free.restype = None
        _free.argtypes = [ctypes.POINTER(_mono_result)]

    # Each stereo repair result is released by its own function, which frees
    # both channels and empties the struct.
    for _name, _result in (
        ("declick", SonareDeclickStereoResult),
        ("declip", SonareDeclipStereoResult),
        ("decrackle", SonareDecrackleStereoResult),
        ("dehum", SonareDehumStereoResult),
        ("denoise", SonareDenoiseStereoResult),
        ("dereverb", SonareDereverbStereoResult),
        ("trim_silence", SonareTrimSilenceStereoResult),
    ):
        _free = getattr(lib, f"sonare_free_{_name}_stereo_result")
        _free.restype = None
        _free.argtypes = [ctypes.POINTER(_result)]
