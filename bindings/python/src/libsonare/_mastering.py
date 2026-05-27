"""Mastering wrappers for libsonare."""

from __future__ import annotations

import contextlib
import json

from ._ffi import (
    SonareEqSnapshot,
    SonareMasteringChainResult,
    SonareMasteringChainStereoResult,
    SonareMasteringConfig,
    SonareMasteringParam,
    SonareMasteringProgressCallback,
    SonareMasteringResult,
    SonareMasteringStereoResult,
    SonareStreamingPlatform,
)
from ._runtime import *  # noqa: F403
from .types import (
    EqSpectrumSnapshot,
    MasteringChainResult,
    MasteringChainStereoResult,
    MasteringResult,
    MasteringStereoResult,
)


def mastering(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_lufs: float = -14.0,
    ceiling_db: float = -1.0,
    true_peak_oversample: int = 4,
) -> MasteringResult:
    """Apply mastering loudness normalization with a true-peak ceiling."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_mastering_process"):
        raise RuntimeError("libsonare was built without mastering support")

    c_array, length = _to_c_float_array(samples)
    config = SonareMasteringConfig(
        target_lufs=target_lufs,
        ceiling_db=ceiling_db,
        true_peak_oversample=true_peak_oversample,
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


def _mastering_params(params: dict[str, float | int | bool] | None):
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
    config: dict | None,
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


def _chain_params(config: dict | None):
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


def _extract_stages(stages_ptr, count: int) -> list[str]:
    if not stages_ptr or count == 0:
        return []
    result: list[str] = []
    for i in range(count):
        raw = stages_ptr[i]
        result.append(raw.decode("utf-8") if raw else "")
    return result


def _make_progress_trampoline(
    on_progress: Callable[[float, str], None],
) -> SonareMasteringProgressCallback:
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
    config: dict | None = None,
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
        config: Either a flat dict using dot-notation keys (e.g.
            ``{"dynamics.compressor.thresholdDb": -24}``) or a nested dict
            (``{"dynamics": {"compressor": {"thresholdDb": -24}}}``). The
            two forms can be freely mixed.
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
    config: dict | None = None,
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
    preset: str = "pop",
    overrides: dict[str, float | int | bool] | None = None,
    on_progress: Callable[[float, str], None] | None = None,
) -> MasteringChainResult:
    """Apply a named mastering preset chain to mono audio.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        preset: Preset identifier from :func:`mastering_preset_names`.
        overrides: Optional flat / nested overrides applied on top of the preset
            defaults. Keys use the same dot-notation as :func:`mastering_chain`.
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
            preset.encode("utf-8"),
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
            preset.encode("utf-8"),
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
    preset: str = "pop",
    overrides: dict[str, float | int | bool] | None = None,
    on_progress: Callable[[float, str], None] | None = None,
) -> MasteringChainStereoResult:
    """Apply a named mastering preset chain to stereo audio.

    See :func:`master_audio` for the ``preset``, ``overrides`` and
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
            preset.encode("utf-8"),
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
            preset.encode("utf-8"),
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


class StreamingMasteringChain:
    """Block-by-block streaming variant of :func:`mastering_chain`.

    Maintains processor state across :meth:`process_mono`/:meth:`process_stereo`
    calls. Only ProcessorBase-backed stages (eq.tilt, dynamics.compressor,
    saturation.tape, saturation.exciter, spectral.airBand, stereo.imager,
    stereo.monoMaker, maximizer.truePeakLimiter) are supported. Configurations
    that enable ``repair.denoise`` or ``loudness`` raise :class:`RuntimeError`.

    Example::

        chain = StreamingMasteringChain({"eq.tilt.tiltDb": 1.0})
        chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
        out = chain.process_mono([0.1] * 512)
        chain.reset()

    Can also be used as a context manager to ensure the underlying handle is
    released::

        with StreamingMasteringChain({"eq.tilt.tiltDb": 1.0}) as chain:
            chain.prepare(44100, 512, 1)
            ...
    """

    def __init__(self, config: dict | None = None) -> None:
        lib = _get_lib()
        if not hasattr(lib, "sonare_streaming_mastering_chain_create"):
            raise RuntimeError("libsonare was built without streaming mastering chain support")
        param_array, param_count = _chain_params(config)
        handle = lib.sonare_streaming_mastering_chain_create(
            param_array, ctypes.c_size_t(param_count)
        )
        if not handle:
            detail = ""
            if hasattr(lib, "sonare_last_error_message"):
                raw = lib.sonare_last_error_message()
                if raw:
                    detail = raw.decode("utf-8", errors="replace")
            message = "failed to create StreamingMasteringChain"
            if detail:
                message = f"{message}: {detail}"
            raise RuntimeError(message)
        self._lib = lib
        self._handle = ctypes.c_void_p(handle)
        self._prepared_channels = 0

    def prepare(self, sample_rate: int, max_block_size: int, num_channels: int) -> None:
        """Initialize processors for the given sample rate and block layout.

        Args:
            sample_rate: Sample rate in Hz.
            max_block_size: Maximum block size in samples per
                :meth:`process_mono` / :meth:`process_stereo` call.
            num_channels: 1 (mono) or 2 (stereo). Stereo-only stages
                (imager, monoMaker) are skipped when ``num_channels`` is 1.
        """
        self._ensure_open()
        rc = self._lib.sonare_streaming_mastering_chain_prepare(
            self._handle,
            ctypes.c_int(int(sample_rate)),
            ctypes.c_int(int(max_block_size)),
            ctypes.c_int(int(num_channels)),
        )
        _check(rc)
        self._prepared_channels = int(num_channels)

    def process_mono(self, samples: Sequence[float] | list[float]) -> list[float]:
        """Process one mono block, returning the processed samples (length unchanged)."""
        self._ensure_open()
        c_array, length = _to_c_float_array(samples)
        rc = self._lib.sonare_streaming_mastering_chain_process_mono(
            self._handle, c_array, ctypes.c_size_t(length)
        )
        _check(rc)
        return [float(c_array[i]) for i in range(length)]

    def process_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> tuple[list[float], list[float]]:
        """Process one stereo block, returning the processed (left, right) channels."""
        self._ensure_open()
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise ValueError("left and right channel lengths must match")
        rc = self._lib.sonare_streaming_mastering_chain_process_stereo(
            self._handle, left_array, right_array, ctypes.c_size_t(left_length)
        )
        _check(rc)
        return (
            [float(left_array[i]) for i in range(left_length)],
            [float(right_array[i]) for i in range(right_length)],
        )

    def reset(self) -> None:
        """Reset all processor state without rebuilding."""
        self._ensure_open()
        rc = self._lib.sonare_streaming_mastering_chain_reset(self._handle)
        _check(rc)

    @property
    def latency_samples(self) -> int:
        """Total reported latency in samples across all active processors."""
        if self._handle is None or not self._handle:
            return 0
        return int(self._lib.sonare_streaming_mastering_chain_latency_samples(self._handle))

    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle is not None and self._handle:
            self._lib.sonare_streaming_mastering_chain_destroy(self._handle)
            self._handle = ctypes.c_void_p(0)

    def __enter__(self) -> StreamingMasteringChain:
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __del__(self) -> None:
        # Defensive: __del__ must not raise
        with contextlib.suppress(Exception):
            self.close()

    def _ensure_open(self) -> None:
        if self._handle is None or not self._handle:
            raise RuntimeError("StreamingMasteringChain is closed")


class StreamingEqualizer:
    """Block-by-block unified EQ wrapper around the native ``SonareEq`` handle."""

    _PHASES = {
        "zero": 1,
        "zero-latency": 1,
        "zero_latency": 1,
        "natural": 2,
        "natural-phase": 2,
        "natural_phase": 2,
        "linear": 3,
        "linear-phase": 3,
        "linear_phase": 3,
    }

    def __init__(self, sample_rate: int = 48000, max_block_size: int = 512) -> None:
        lib = _get_lib()
        if not hasattr(lib, "sonare_eq_create"):
            raise RuntimeError("libsonare was built without streaming equalizer support")
        handle = lib.sonare_eq_create(float(sample_rate), int(max_block_size))
        if not handle:
            raise RuntimeError("failed to create StreamingEqualizer")
        self._lib = lib
        self._handle = ctypes.c_void_p(handle)
        self.sample_rate = int(sample_rate)
        self.max_block_size = int(max_block_size)
        self._sidechain_refs: object | None = None

    def set_band(self, index: int, band: dict | str) -> None:
        """Set one EQ band from a JSON string or a Python dict."""
        self._ensure_open()
        payload = band if isinstance(band, str) else json.dumps(band, separators=(",", ":"))
        rc = self._lib.sonare_eq_set_band(
            self._handle, ctypes.c_int(int(index)), payload.encode("utf-8")
        )
        _check(rc)

    def clear(self) -> None:
        """Clear all EQ bands."""
        self._ensure_open()
        self._lib.sonare_eq_clear(self._handle)

    def set_phase_mode(self, mode: int | str) -> None:
        """Set the global phase mode: zero/natural/linear or 0/1/2."""
        self._ensure_open()
        if isinstance(mode, str):
            key = mode.lower()
            if key not in self._PHASES:
                raise ValueError(f"unknown EQ phase mode: {mode}")
            value = self._PHASES[key]
        else:
            value = int(mode)
        _check(self._lib.sonare_eq_set_phase_mode(self._handle, ctypes.c_int(value)))

    def set_auto_gain(self, enabled: bool) -> None:
        """Enable or disable auto-gain compensation."""
        self._ensure_open()
        self._lib.sonare_eq_set_auto_gain(self._handle, ctypes.c_int(1 if enabled else 0))

    def set_gain_scale(self, scale: float) -> None:
        """Set all-band EQ gain scale as a 0.0..2.0 multiplier."""
        self._ensure_open()
        _check(self._lib.sonare_eq_set_gain_scale(self._handle, ctypes.c_float(float(scale))))

    def set_output_gain_db(self, gain_db: float) -> None:
        """Set post-EQ output gain in dB."""
        self._ensure_open()
        _check(self._lib.sonare_eq_set_output_gain_db(self._handle, ctypes.c_float(float(gain_db))))

    def set_output_pan(self, pan: float) -> None:
        """Set post-EQ stereo balance in -1.0..1.0; mono input ignores pan."""
        self._ensure_open()
        _check(self._lib.sonare_eq_set_output_pan(self._handle, ctypes.c_float(float(pan))))

    def set_sidechain_mono(self, samples: Sequence[float] | list[float]) -> None:
        """Set a mono external key for dynamic bands with ``externalSidechain`` enabled."""
        self._ensure_open()
        c_array, length = _to_c_float_array(samples)
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 1
        channels = channel_array_type(c_array)
        _check(self._lib.sonare_eq_set_sidechain(self._handle, channels, ctypes.c_int(1), length))
        self._sidechain_refs = (c_array, channels)

    def set_sidechain_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> None:
        """Set a stereo external key for dynamic bands with ``externalSidechain`` enabled."""
        self._ensure_open()
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise ValueError("left and right sidechain lengths must match")
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 2
        channels = channel_array_type(left_array, right_array)
        _check(
            self._lib.sonare_eq_set_sidechain(
                self._handle, channels, ctypes.c_int(2), ctypes.c_int(left_length)
            )
        )
        self._sidechain_refs = (left_array, right_array, channels)

    def clear_sidechain(self) -> None:
        """Clear any pending external key buffer."""
        self._ensure_open()
        self._lib.sonare_eq_clear_sidechain(self._handle)
        self._sidechain_refs = None

    def match(
        self,
        source: Sequence[float] | list[float],
        reference: Sequence[float] | list[float],
        max_bands: int = 8,
    ) -> None:
        """Configure live EQ bands by matching ``source`` to ``reference``."""
        self._ensure_open()
        source_array, source_length = _to_c_float_array(source)
        reference_array, reference_length = _to_c_float_array(reference)
        if source_length != reference_length:
            raise ValueError("source and reference lengths must match")
        rc = self._lib.sonare_eq_match(
            self._handle,
            source_array,
            reference_array,
            ctypes.c_size_t(source_length),
            ctypes.c_int(self.sample_rate),
            ctypes.c_int(int(max_bands)),
        )
        _check(rc)

    def process_mono(self, samples: Sequence[float] | list[float]) -> list[float]:
        """Process one mono block, returning processed samples."""
        self._ensure_open()
        c_array, length = _to_c_float_array(samples)
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 1
        channels = channel_array_type(c_array)
        _check(self._lib.sonare_eq_process(self._handle, channels, ctypes.c_int(1), length))
        self._sidechain_refs = None
        return [float(c_array[i]) for i in range(length)]

    def process_stereo(
        self,
        left: Sequence[float] | list[float],
        right: Sequence[float] | list[float],
    ) -> tuple[list[float], list[float]]:
        """Process one stereo block, returning the processed (left, right) channels."""
        self._ensure_open()
        left_array, left_length = _to_c_float_array(left)
        right_array, right_length = _to_c_float_array(right)
        if left_length != right_length:
            raise ValueError("left and right channel lengths must match")
        channel_array_type = ctypes.POINTER(ctypes.c_float) * 2
        channels = channel_array_type(left_array, right_array)
        _check(
            self._lib.sonare_eq_process(
                self._handle, channels, ctypes.c_int(2), ctypes.c_int(left_length)
            )
        )
        self._sidechain_refs = None
        return (
            [float(left_array[i]) for i in range(left_length)],
            [float(right_array[i]) for i in range(right_length)],
        )

    def spectrum(self) -> EqSpectrumSnapshot:
        """Return the latest pre/post sample stream and band-gain snapshot."""
        self._ensure_open()
        out = SonareEqSnapshot()
        _check(self._lib.sonare_eq_spectrum(self._handle, ctypes.byref(out)))
        pre_count = int(out.pre_count)
        post_count = int(out.post_count)
        return EqSpectrumSnapshot(
            pre_left=[float(out.pre_left[i]) for i in range(pre_count)],
            pre_right=[float(out.pre_right[i]) for i in range(pre_count)],
            post_left=[float(out.post_left[i]) for i in range(post_count)],
            post_right=[float(out.post_right[i]) for i in range(post_count)],
            band_gain_db=[float(out.band_gain_db[i]) for i in range(24)],
            profile_db=[float(out.profile_db[i]) for i in range(16)],
            last_auto_gain_db=float(out.last_auto_gain_db),
            seq=int(out.seq),
        )

    @property
    def latency_samples(self) -> int:
        self._ensure_open()
        return int(self._lib.sonare_eq_latency_samples(self._handle))

    @property
    def last_auto_gain_db(self) -> float:
        self._ensure_open()
        return float(self._lib.sonare_eq_last_auto_gain_db(self._handle))

    def close(self) -> None:
        """Release the underlying C handle. Safe to call multiple times."""
        if self._handle is not None and self._handle:
            self._lib.sonare_eq_destroy(self._handle)
            self._handle = ctypes.c_void_p(0)

    def __enter__(self) -> StreamingEqualizer:
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _ensure_open(self) -> None:
        if self._handle is None or not self._handle:
            raise RuntimeError("StreamingEqualizer is closed")


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
    if source_length != reference_length:
        raise ValueError("source and reference lengths must match")
    param_array, param_count = _mastering_params(params)
    out = SonareMasteringResult()
    rc = lib.sonare_mastering_apply_pair_processor(
        processor_name.encode("utf-8"),
        source_array,
        reference_array,
        ctypes.c_size_t(source_length),
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
    if source_length != reference_length:
        raise ValueError("source and reference lengths must match")
    param_array, param_count = _mastering_params(params)
    json_ptr = ctypes.c_void_p()
    rc = lib.sonare_mastering_analyze_pair(
        analysis_name.encode("utf-8"),
        source_array,
        reference_array,
        ctypes.c_size_t(source_length),
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
    json_ptr = ctypes.c_void_p()
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

    json_ptr = ctypes.c_void_p()
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
    json_ptr = ctypes.c_void_p()
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
    json_ptr = ctypes.c_void_p()
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
