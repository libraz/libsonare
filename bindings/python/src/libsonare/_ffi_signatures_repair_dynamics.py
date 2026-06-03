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
