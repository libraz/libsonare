"""Mastering wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import (
    SonareMasteringResult,
    SonareStreamingPlatform,
)
from ._mastering_offline import _mastering_params
from ._runtime import _check, _get_lib, _to_c_float_array
from .types import (
    MasteringResult,
)


def mastering_pair_process(
    processor_name: str,
    source: Sequence[float] | list[float],
    reference: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> MasteringResult:
    """Apply a named two-input mastering processor."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_apply_pair_processor"):
        raise RuntimeError("libsonare was built without mastering support")
    source_array, source_length = _to_c_float_array(source)
    reference_array, reference_length = _to_c_float_array(reference)
    param_array, param_count = _mastering_params(params)
    out = SonareMasteringResult()
    # Reference masters are commonly a different length than the source; the _ex
    # variant takes independent source/reference lengths (the pair primitives
    # consume each buffer at its own length).
    rc = lib.sonare_mastering_apply_pair_processor_ex(
        processor_name.encode("utf-8"),
        source_array,
        ctypes.c_size_t(source_length),
        reference_array,
        ctypes.c_size_t(reference_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringResult(
            samples=[float(out.samples[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
        )
    finally:
        lib.sonare_free_mastering_result(ctypes.byref(out))


def mastering_pair_analyze(
    analysis_name: str,
    source: Sequence[float] | list[float],
    reference: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> str:
    """Run a named two-input mastering analysis and return shared JSON."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_analyze_pair"):
        raise RuntimeError("libsonare was built without mastering support")
    source_array, source_length = _to_c_float_array(source)
    reference_array, reference_length = _to_c_float_array(reference)
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_char_p()
    # Independent source/reference lengths (see mastering_pair_process).
    rc = lib.sonare_mastering_analyze_pair_ex(
        analysis_name.encode("utf-8"),
        source_array,
        ctypes.c_size_t(source_length),
        reference_array,
        ctypes.c_size_t(reference_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def mastering_stereo_analyze(
    analysis_name: str,
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> str:
    """Run a named stereo mastering analysis and return shared JSON."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_analyze_stereo"):
        raise RuntimeError("libsonare was built without mastering support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_analyze_stereo(
        analysis_name.encode("utf-8"),
        left_array,
        right_array,
        ctypes.c_size_t(left_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def mastering_streaming_preview(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    platforms: Sequence[dict[str, float | str]] | None = None,
) -> str:
    """Preview streaming-platform normalization and ceiling risk as shared JSON."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_streaming_preview"):
        raise RuntimeError("libsonare was built without mastering streaming preview support")
    c_array, length = _to_c_float_array(samples)

    platform_buffers: list[bytes] = []
    platform_array = None
    platform_count = 0
    if platforms:
        platform_count = len(platforms)
        array_type = SonareStreamingPlatform * platform_count
        platform_buffers = [str(platform.get("name", "")).encode("utf-8") for platform in platforms]
        platform_array = array_type(
            *[
                SonareStreamingPlatform(
                    name=platform_buffers[index],
                    target_lufs=float(
                        platform.get("targetLufs", platform.get("target_lufs", -14.0))
                    ),
                    ceiling_db=float(platform.get("ceilingDb", platform.get("ceiling_db", -1.0))),
                )
                for index, platform in enumerate(platforms)
            ]
        )

    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_streaming_preview(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        platform_array,
        ctypes.c_size_t(platform_count),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def mastering_assistant_suggest(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> str:
    """Analyze audio and suggest a mastering chain as shared JSON."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_assistant_suggest"):
        raise RuntimeError("libsonare was built without mastering assistant support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_assistant_suggest(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)


def mastering_audio_profile(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> str:
    """Analyze audio and return the mastering assistant profile as shared JSON."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_audio_profile"):
        raise RuntimeError("libsonare was built without mastering audio profile support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_audio_profile(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(json_ptr),
    )
    _check(rc)
    try:
        return ctypes.string_at(json_ptr).decode("utf-8") if json_ptr.value else ""
    finally:
        if json_ptr.value:
            lib.sonare_free_string(json_ptr)
