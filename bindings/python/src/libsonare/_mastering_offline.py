"""Mastering wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
from collections.abc import Callable, Sequence
from typing import Any, cast

from ._ffi import (
    SonareMasteringChainResult,
    SonareMasteringChainStereoResult,
    SonareMasteringConfig,
    SonareMasteringParam,
    SonareMasteringProgressCallback,
    SonareMasteringResult,
    SonareMasteringStereoResult,
)
from ._runtime import _check, _get_lib, _to_c_float_array
from .types import (
    MasteringChainResult,
    MasteringChainStereoResult,
    MasteringInsertParamInfo,
    MasteringProcessorCatalogEntry,
    MasteringResult,
    MasteringStereoResult,
)


def mastering(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_lufs: float = -14.0,
    ceiling_db: float = -1.0,
    true_peak_oversample: int = 4,
    release_ms: float = 0.0,
    apply_gain_at_input_rate: bool = False,
) -> MasteringResult:
    """Apply mastering loudness normalization with a true-peak ceiling.

    Pass the buffer's actual ``sample_rate``: the default (22050) is non-standard
    for audio, and the LUFS measurement driving normalization is sample-rate
    dependent, so a wrong rate mis-targets the loudness.

    ``release_ms`` tunes the post true-peak limiter release; ``0`` keeps the
    library default (50 ms). ``apply_gain_at_input_rate`` applies the static
    loudness gain at the input (pre-oversample) rate.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_process"):
        raise RuntimeError("libsonare was built without mastering support")

    c_array, length = _to_c_float_array(samples)
    config = SonareMasteringConfig(
        target_lufs=target_lufs,
        ceiling_db=ceiling_db,
        true_peak_oversample=true_peak_oversample,
        release_ms=release_ms,
        apply_gain_at_input_rate=1 if apply_gain_at_input_rate else 0,
    )
    out = SonareMasteringResult()
    rc = lib.sonare_mastering_process(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        processed = [float(out.samples[i]) for i in range(out.length)]
        return MasteringResult(
            samples=processed,
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
        )
    finally:
        lib.sonare_free_mastering_result(ctypes.byref(out))


def _mastering_params(params: dict[str, float | int | bool] | None) -> tuple[Any, int]:
    items = list((params or {}).items())
    array_type = SonareMasteringParam * len(items)
    key_buffers = [str(key).encode("utf-8") for key, _ in items]
    array = array_type(
        *[
            SonareMasteringParam(key=key_buffers[index], value=float(value))
            for index, (_, value) in enumerate(items)
        ]
    )
    return array, len(items)


def mastering_processor_names() -> list[str]:
    """Return supported mastering processor names shared by CLI/Node/WASM/Python."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_processor_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_processor_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_pair_processor_names() -> list[str]:
    """Return supported two-input mastering processor names."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_pair_processor_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_pair_processor_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_pair_analysis_names() -> list[str]:
    """Return supported two-input mastering analysis names."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_pair_analysis_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_pair_analysis_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_stereo_analysis_names() -> list[str]:
    """Return supported stereo mastering analysis names."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_stereo_analysis_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_stereo_analysis_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_insert_names() -> list[str]:
    """Return the mastering insert (FX) names shared by CLI/Node/WASM/Python.

    The native layer returns a lifetime-owned, newline-joined static string the
    caller must NOT free (same convention as the other ``*_names`` getters).
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_insert_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_insert_names()
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_insert_param_names(name: str) -> list[str]:
    """Return the camelCase parameter names a given insert/FX processor reads.

    For tools/UIs that want to validate a scene insert's params before loading
    it: any supplied key NOT in this list is silently ignored by the processor
    (and would be reported via :meth:`Mixer.scene_warnings` on a scene load).
    Band/sub-band processors enumerate their indexed ``band{i}.<field>`` keys.
    Returns an empty list for an unknown ``name`` (or one whose insert needs an
    unavailable build feature, e.g. FX).

    The native layer returns a thread-local, newline-joined string the caller
    must NOT free (same convention as the other ``*_names`` getters).
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_insert_param_names"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_insert_param_names(name.encode("utf-8"))
    return raw.decode("utf-8").splitlines() if raw else []


def mastering_insert_param_info(name: str) -> list[MasteringInsertParamInfo]:
    """Return parameter metadata for a given insert/FX processor.

    Each entry describes one parameter the insert reads, with keys ``name``
    (camelCase parameter name), ``id`` (stable numeric parameter id) and
    ``rtSafe`` (whether the parameter can be changed on the realtime audio
    thread). Returns an empty list for an unknown ``name`` (or one whose
    insert needs an unavailable build feature, e.g. FX).

    The native layer returns a thread-local JSON array string the caller must
    NOT free (same convention as the other mastering getters).
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_insert_param_info"):
        raise RuntimeError("libsonare was built without mastering support")
    raw = lib.sonare_mastering_insert_param_info(name.encode("utf-8"))
    if not raw:
        return []
    parsed = json.loads(raw.decode("utf-8"))
    return cast("list[MasteringInsertParamInfo]", parsed)


def mastering_processor_catalog() -> list[MasteringProcessorCatalogEntry]:
    """Return the full catalog of mastering processors.

    Each entry describes one processor with keys ``id`` (camelCase processor
    id, e.g. ``dynamics.compressor``), ``kind`` (one of ``"realtime"``,
    ``"offline"`` or ``"pair"``), ``realtimeInsertable`` (whether the processor
    can run as a realtime insert), ``stereoOnly`` (whether it requires a
    stereo signal), ``latencySamples`` and ``tailSamples`` (reported processing
    latency and audible decay for one default 48 kHz / 512-sample prepared
    probe; 0 for offline processors and representative values for
    configuration-dependent processors), and ``channelPolicy`` (how the
    mixer wraps the processor on a >2-channel surround bus insert:
    ``"multichannel"`` for one full-buffer call, ``"stereoPairOnly"`` for
    front-L/R-only with surround planes passed through dry). ``kind`` follows
    the precedence pair > realtime > offline:
    a processor exposed as a pair processor is reported as ``"pair"``, an
    otherwise realtime-capable processor as ``"realtime"``, and the remainder
    as ``"offline"``. Returns an empty list when the build lacks mastering
    support.

    The native layer returns a program-lifetime JSON array string the caller
    must NOT free (same convention as the other mastering getters).
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_processor_catalog"):
        return []
    raw = lib.sonare_mastering_processor_catalog()
    if not raw:
        return []
    parsed = json.loads(raw.decode("utf-8"))
    return cast("list[MasteringProcessorCatalogEntry]", parsed)


def mastering_process(
    processor_name: str,
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> MasteringResult:
    """Apply a named mastering processor using the shared cross-language API."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_apply_processor"):
        raise RuntimeError("libsonare was built without mastering support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _mastering_params(params)
    out = SonareMasteringResult()
    rc = lib.sonare_mastering_apply_processor(
        processor_name.encode("utf-8"),
        c_array,
        ctypes.c_size_t(length),
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


def mastering_process_stereo(
    processor_name: str,
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    params: dict[str, float | int | bool] | None = None,
) -> MasteringStereoResult:
    """Apply a named stereo mastering processor using the shared cross-language API."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_apply_processor_stereo"):
        raise RuntimeError("libsonare was built without mastering support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    param_array, param_count = _mastering_params(params)
    out = SonareMasteringStereoResult()
    rc = lib.sonare_mastering_apply_processor_stereo(
        processor_name.encode("utf-8"),
        left_array,
        right_array,
        ctypes.c_size_t(left_length),
        ctypes.c_int(sample_rate),
        param_array,
        ctypes.c_size_t(param_count),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MasteringStereoResult(
            left=[float(out.left[i]) for i in range(out.length)],
            right=[float(out.right[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            latency_samples=int(out.latency_samples),
        )
    finally:
        lib.sonare_free_mastering_stereo_result(ctypes.byref(out))


def _flatten_chain_config(
    config: dict[str, Any] | None,
    prefix: str = "",
) -> dict[str, float]:
    """Flatten a nested chain config dict using dot-notation keys.

    Accepts both nested (``{"dynamics": {"compressor": {"thresholdDb": -24}}}``)
    and flat (``{"dynamics.compressor.thresholdDb": -24}``) representations.
    Booleans are coerced to 0.0/1.0; other values are coerced via ``float``.
    """
    flat: dict[str, float] = {}
    if not config:
        return flat
    for key, value in config.items():
        full_key = f"{prefix}{key}" if not prefix else f"{prefix}.{key}"
        if isinstance(value, dict):
            flat.update(_flatten_chain_config(value, full_key))
        elif isinstance(value, bool):
            flat[full_key] = 1.0 if value else 0.0
        else:
            flat[full_key] = float(value)
    return flat


def _chain_params(config: dict[str, Any] | None) -> tuple[Any, int]:
    flat = _flatten_chain_config(config)
    items = list(flat.items())
    array_type = SonareMasteringParam * len(items)
    key_buffers = [str(key).encode("utf-8") for key, _ in items]
    array = array_type(
        *[
            SonareMasteringParam(key=key_buffers[index], value=float(value))
            for index, (_, value) in enumerate(items)
        ]
    )
    return array, len(items)


def _extract_stages(stages_ptr: object, count: int) -> list[str]:
    if not stages_ptr or count == 0:
        return []
    raw_ptr = cast(Any, stages_ptr)
    result: list[str] = []
    for i in range(count):
        raw = raw_ptr[i]
        result.append(raw.decode("utf-8") if raw else "")
    return result


def _make_progress_trampoline(
    on_progress: Callable[[float, str], None],
) -> Any:
    """Wrap a Python callback for use as a C SonareMasteringProgressCallback.

    The returned object MUST be kept alive across the C call to avoid GC
    collecting the underlying ctypes closure.
    """

    def _trampoline(progress: float, stage_cstr: bytes | None, _user_data: int) -> None:
        try:
            stage = stage_cstr.decode("utf-8") if stage_cstr else ""
            on_progress(float(progress), stage)
        except Exception:  # noqa: BLE001 — never propagate Python exceptions into C
            pass

    return SonareMasteringProgressCallback(_trampoline)


def mastering_chain(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    config: dict[str, Any] | None = None,
    on_progress: Callable[[float, str], None] | None = None,
) -> MasteringChainResult:
    """Apply a configurable mastering chain to mono audio.

    The chain composes (in fixed order) repair, EQ, dynamics, saturation,
    spectral, maximizer, and loudness stages. Each stage is enabled either
    by passing ``"<stage>.enabled": True`` or by setting any field under
    ``"<stage>.*"``. Unknown keys raise ``RuntimeError``.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        config: A nested dict of module -> processor -> parameter, e.g.
            ``{"dynamics": {"compressor": {"thresholdDb": -24}},
            "loudness": {"targetLufs": -14}}``. A boolean toggles a
            module/processor's ``enabled`` flag. (Flat dot-notation keys such
            as ``{"dynamics.compressor.thresholdDb": -24}`` are also accepted
            and may be mixed in.)
        on_progress: Optional callback ``(progress, stage)`` invoked after
            each enabled stage completes. ``progress`` is in ``[0.0, 1.0]``
            and ``stage`` is the stage identifier (e.g.
            ``"dynamics.compressor"``). Exceptions raised inside the
            callback are swallowed.

    Returns:
        :class:`MasteringChainResult` with processed samples, LUFS info,
        and the ordered list of stages that ran.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_chain"):
        raise RuntimeError("libsonare was built without mastering chain support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _chain_params(config)
    out = SonareMasteringChainResult()
    if on_progress is None:
        rc = lib.sonare_mastering_chain(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            param_array,
            ctypes.c_size_t(param_count),
            ctypes.byref(out),
        )
    else:
        if not hasattr(lib, "sonare_mastering_chain_with_progress"):
            raise RuntimeError("libsonare was built without mastering progress support")
        cb = _make_progress_trampoline(on_progress)  # keep alive across the C call
        rc = lib.sonare_mastering_chain_with_progress(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            param_array,
            ctypes.c_size_t(param_count),
            cb,
            None,
            ctypes.byref(out),
        )
    _check(rc)
    try:
        return MasteringChainResult(
            samples=[float(out.samples[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            stages=_extract_stages(out.stages, int(out.stages_count)),
        )
    finally:
        lib.sonare_free_mastering_chain_result(ctypes.byref(out))


def mastering_chain_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    config: dict[str, Any] | None = None,
    on_progress: Callable[[float, str], None] | None = None,
) -> MasteringChainStereoResult:
    """Apply a configurable mastering chain to stereo audio.

    See :func:`mastering_chain` for ``config`` and ``on_progress`` semantics.
    The stereo path also recognises ``stereo.imager.*`` and
    ``stereo.monoMaker.*`` stages.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_chain_stereo"):
        raise RuntimeError("libsonare was built without mastering chain support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    param_array, param_count = _chain_params(config)
    out = SonareMasteringChainStereoResult()
    if on_progress is None:
        rc = lib.sonare_mastering_chain_stereo(
            left_array,
            right_array,
            ctypes.c_size_t(left_length),
            ctypes.c_int(sample_rate),
            param_array,
            ctypes.c_size_t(param_count),
            ctypes.byref(out),
        )
    else:
        if not hasattr(lib, "sonare_mastering_chain_stereo_with_progress"):
            raise RuntimeError("libsonare was built without mastering progress support")
        cb = _make_progress_trampoline(on_progress)  # keep alive across the C call
        rc = lib.sonare_mastering_chain_stereo_with_progress(
            left_array,
            right_array,
            ctypes.c_size_t(left_length),
            ctypes.c_int(sample_rate),
            param_array,
            ctypes.c_size_t(param_count),
            cb,
            None,
            ctypes.byref(out),
        )
    _check(rc)
    try:
        return MasteringChainStereoResult(
            left=[float(out.left[i]) for i in range(out.length)],
            right=[float(out.right[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            stages=_extract_stages(out.stages, int(out.stages_count)),
        )
    finally:
        lib.sonare_free_mastering_chain_stereo_result(ctypes.byref(out))


def mastering_preset_names() -> list[str]:
    """Return built-in mastering preset identifiers (e.g. ``"pop"``, ``"aiMusic"``)."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_preset_names"):
        raise RuntimeError("libsonare was built without mastering preset support")
    raw = lib.sonare_mastering_preset_names()
    return raw.decode("utf-8").splitlines() if raw else []


def master_audio(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    preset_name: str = "pop",
    overrides: dict[str, Any] | None = None,
    on_progress: Callable[[float, str], None] | None = None,
) -> MasteringChainResult:
    """Apply a named mastering preset chain to mono audio.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        preset_name: Preset identifier from :func:`mastering_preset_names`.
        overrides: Optional nested overrides applied on top of the preset
            defaults. Uses the same config shape as :func:`mastering_chain`.
        on_progress: Optional callback ``(progress, stage)`` invoked after each
            enabled stage completes. See :func:`mastering_chain` for details.

    Returns:
        :class:`MasteringChainResult` with processed samples, LUFS info, and the
        ordered list of stages that ran.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_master_audio"):
        raise RuntimeError("libsonare was built without mastering preset support")
    c_array, length = _to_c_float_array(samples)
    param_array, param_count = _chain_params(overrides)
    out = SonareMasteringChainResult()
    if on_progress is None:
        rc = lib.sonare_master_audio(
            preset_name.encode("utf-8"),
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            param_array,
            ctypes.c_size_t(param_count),
            ctypes.byref(out),
        )
    else:
        if not hasattr(lib, "sonare_master_audio_with_progress"):
            raise RuntimeError("libsonare was built without mastering progress support")
        cb = _make_progress_trampoline(on_progress)  # keep alive across the C call
        rc = lib.sonare_master_audio_with_progress(
            preset_name.encode("utf-8"),
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            param_array,
            ctypes.c_size_t(param_count),
            cb,
            None,
            ctypes.byref(out),
        )
    _check(rc)
    try:
        return MasteringChainResult(
            samples=[float(out.samples[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            stages=_extract_stages(out.stages, int(out.stages_count)),
        )
    finally:
        lib.sonare_free_mastering_chain_result(ctypes.byref(out))


def master_audio_stereo(
    left: Sequence[float] | list[float],
    right: Sequence[float] | list[float],
    sample_rate: int = 22050,
    preset_name: str = "pop",
    overrides: dict[str, Any] | None = None,
    on_progress: Callable[[float, str], None] | None = None,
) -> MasteringChainStereoResult:
    """Apply a named mastering preset chain to stereo audio.

    See :func:`master_audio` for the ``preset_name``, ``overrides`` and
    ``on_progress`` semantics.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_master_audio_stereo"):
        raise RuntimeError("libsonare was built without mastering preset support")
    left_array, left_length = _to_c_float_array(left)
    right_array, right_length = _to_c_float_array(right)
    if left_length != right_length:
        raise ValueError("left and right channel lengths must match")
    param_array, param_count = _chain_params(overrides)
    out = SonareMasteringChainStereoResult()
    if on_progress is None:
        rc = lib.sonare_master_audio_stereo(
            preset_name.encode("utf-8"),
            left_array,
            right_array,
            ctypes.c_size_t(left_length),
            ctypes.c_int(sample_rate),
            param_array,
            ctypes.c_size_t(param_count),
            ctypes.byref(out),
        )
    else:
        if not hasattr(lib, "sonare_master_audio_stereo_with_progress"):
            raise RuntimeError("libsonare was built without mastering progress support")
        cb = _make_progress_trampoline(on_progress)  # keep alive across the C call
        rc = lib.sonare_master_audio_stereo_with_progress(
            preset_name.encode("utf-8"),
            left_array,
            right_array,
            ctypes.c_size_t(left_length),
            ctypes.c_int(sample_rate),
            param_array,
            ctypes.c_size_t(param_count),
            cb,
            None,
            ctypes.byref(out),
        )
    _check(rc)
    try:
        return MasteringChainStereoResult(
            left=[float(out.left[i]) for i in range(out.length)],
            right=[float(out.right[i]) for i in range(out.length)],
            sample_rate=int(out.sample_rate),
            input_lufs=float(out.input_lufs),
            output_lufs=float(out.output_lufs),
            applied_gain_db=float(out.applied_gain_db),
            stages=_extract_stages(out.stages, int(out.stages_count)),
        )
    finally:
        lib.sonare_free_mastering_chain_stereo_result(ctypes.byref(out))
