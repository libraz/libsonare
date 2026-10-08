"""One repair analysis and one repair application over any number of channels."""

from __future__ import annotations

import contextlib
import ctypes
import json
import re
from collections.abc import Callable, Mapping, Sequence
from typing import Any, cast

import numpy as np

from ._cancellation import CancellationState, make_cancel_trampoline
from ._effects_repair_common import _linked_channel_planes, _linked_output_planes
from ._ffi import SonareMasteringProgressCallback
from ._runtime import (
    SonareValueError,
    _check,
    _get_lib,
    _planar_channel_arrays,
    _to_c_int,
    _to_c_size_t,
    _unsupported_effect_symbol,
)
from .types import (
    ClickDetection,
    ClipDetection,
    CrackleDetection,
    DeclickReport,
    DeclipReport,
    DecrackleReport,
    DehumReport,
    DenoiseReport,
    DereverbReport,
    HumDetection,
    MasteringRepairAnalysis,
    NoiseDetection,
    RepairApplyResult,
    RepairStageReports,
    ReverbDetection,
)

_REPORT_TYPES: dict[str, tuple[type, type]] = {
    "declip": (DeclipReport, ClipDetection),
    "declick": (DeclickReport, ClickDetection),
    "decrackle": (DecrackleReport, CrackleDetection),
    "dehum": (DehumReport, HumDetection),
    "denoise": (DenoiseReport, NoiseDetection),
    "dereverb": (DereverbReport, ReverbDetection),
}


def _snake(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def _fields(values: Mapping[str, Any]) -> dict[str, Any]:
    return {_snake(key): value for key, value in values.items()}


def _stage_reports(entry: Mapping[str, Any]) -> RepairStageReports:
    report_type, detection_type = _REPORT_TYPES[entry["stage"]]
    reports = []
    for report in entry["reports"]:
        fields = _fields(report)
        fields["detected"] = detection_type(**_fields(report["detected"]))
        reports.append(report_type(**fields))
    return RepairStageReports(stage=entry["stage"], scope=entry["scope"], reports=reports)


def _progress_trampoline(
    on_progress: Callable[[float, str], None] | None,
) -> Any:
    if on_progress is None:
        return SonareMasteringProgressCallback(0)

    def _trampoline(progress: float, stage_cstr: bytes | None, _user_data: int) -> None:
        # Never propagate a Python exception into C.
        with contextlib.suppress(Exception):
            on_progress(float(progress), stage_cstr.decode("utf-8") if stage_cstr else "")

    return SonareMasteringProgressCallback(_trampoline)


def _take_string(lib: ctypes.CDLL, pointer: ctypes.c_char_p) -> str:
    try:
        return ctypes.string_at(pointer).decode("utf-8") if pointer.value else ""
    finally:
        if pointer.value:
            lib.sonare_free_string(pointer)


def mastering_repair_analyze(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    sample_rate: int = 22050,
    *,
    prefer_streaming_safe: bool = True,
) -> MasteringRepairAnalysis:
    """Measure every channel's repair defects and recommend stages.

    The six detectors run on each channel and aggregate the way the stereo
    audio profile aggregates them; the recommendation is the mastering
    assistant's own repair selection, with its thresholds and the settings it
    derives (the declip threshold at the measured flat level, the dehum
    fundamental the search found). Dereverb is never recommended: its statistic
    does not separate a sustaining dry signal from a reverberant one.

    Example::

        analysis = libsonare.mastering_repair_analyze([left, right], 48000)
        result = libsonare.mastering_repair_apply(
            [left, right], 48000, stages=analysis["recommended"]
        )

    Args:
        channels: One buffer per channel, all of the same length, or a 2-D
            ``(channels, frames)`` array. At least one.
        sample_rate: Sample rate in Hz (default 22050).
        prefer_streaming_safe: When a denoise is recommended, track the noise
            frame by frame (default True).

    Returns:
        :class:`~libsonare.types.MasteringRepairAnalysis`: the aggregate and
        per-channel defects in the audio profile's shape, whether a declip at
        the flat level leaves louder audio alone, the programme loudness, the
        recommended stages and why each was or was not chosen.

    Raises:
        SonareValueError: If ``channels`` is empty, the channels disagree in
            length, or any channel is empty or carries a non-finite sample.
        SonareError: If the C call rejects the request.
    """
    lib = _get_lib()
    symbol = "sonare_mastering_repair_analyze"
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)
    planes = _linked_channel_planes("mastering_repair_analyze", channels)
    arrays, in_ptrs, frame_count = _planar_channel_arrays(planes, subject="channels")
    request = json.dumps({"preferStreamingSafe": bool(prefer_streaming_safe)})
    json_ptr = ctypes.c_char_p()
    _check(
        lib.sonare_mastering_repair_analyze(
            ctypes.cast(in_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
            _to_c_size_t(len(arrays), "channel_count"),
            _to_c_size_t(frame_count, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            request.encode("utf-8"),
            ctypes.byref(json_ptr),
        )
    )
    return cast(MasteringRepairAnalysis, json.loads(_take_string(lib, json_ptr)))


def mastering_repair_apply(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    sample_rate: int = 22050,
    *,
    stages: Sequence[Mapping[str, Any]],
    on_progress: Callable[[float, str], None] | None = None,
    cancel: Callable[[], bool] | None = None,
) -> RepairApplyResult:
    """Apply repair stages to every channel in the chain's fixed order.

    The stages run as declip, declick, decrackle, dehum, denoise, dereverb
    whatever order ``stages`` gives. Declip, declick and dehum decide over the
    whole channel set (a defect found in one channel is repaired in all of
    them, and dehum tracks one fundamental) and report per channel; decrackle
    runs each channel alone; denoise and dereverb apply one linked mask and
    report once for the set.

    Example::

        result = libsonare.mastering_repair_apply(
            [left, right],
            48000,
            stages=[{"stage": "denoise", "mode": "logMmse"}, {"stage": "declick"}],
        )
        print(result.reports[0].stage)  # "declick"

    Args:
        channels: One buffer per channel, all of the same length, or a 2-D
            ``(channels, frames)`` array. At least one.
        sample_rate: Sample rate in Hz (default 22050).
        stages: ``{"stage": name, <setting>: value}`` dicts, each stage at most
            once; ``name`` is one of ``declip``, ``declick``, ``decrackle``,
            ``dehum``, ``denoise``, ``dereverb``. The settings are the stage's
            catalog parameters in their camelCase spelling (``repair.declip``,
            ``repair.declick``, ``repair.decrackle``, ``repair.dehum``,
            ``repair.denoiseClassical``, ``repair.dereverbClassical``); an enum
            takes its choice name or wire value. An unknown one is refused.
        on_progress: Optional callback ``(progress, stage)`` invoked after each
            stage with the fraction done and ``"repair.<stage>"``. Exceptions
            raised inside it are swallowed.
        cancel: Optional zero-argument callable polled after each progress
            report. A true return value cancels with ``SonareError``.

    Returns:
        :class:`~libsonare.types.RepairApplyResult` with one repaired buffer per
        input channel and one :class:`~libsonare.types.RepairStageReports` per
        applied stage, in application order.

    Raises:
        SonareValueError: If ``channels`` is empty, the channels disagree in
            length, any channel is empty or carries a non-finite sample, or
            ``stages`` is not a sequence of mappings.
        SonareError: If the C call rejects the request (a repeated or unknown
            stage, an unknown or out-of-range setting) or it was cancelled.
    """
    lib = _get_lib()
    symbol = "sonare_mastering_repair_apply"
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)
    if isinstance(stages, (str, bytes, Mapping)) or not all(
        isinstance(stage, Mapping) for stage in stages
    ):
        raise SonareValueError("mastering_repair_apply: stages must be a sequence of mappings")
    planes = _linked_channel_planes("mastering_repair_apply", channels)
    arrays, in_ptrs, frame_count = _planar_channel_arrays(planes, subject="channels")
    stages_json = json.dumps([dict(stage) for stage in stages])
    out_buffers, out_ptrs = _linked_output_planes(len(arrays), frame_count)
    progress_cb = _progress_trampoline(on_progress)
    cancel_cb = make_cancel_trampoline(CancellationState(cancel))
    reports_ptr = ctypes.c_char_p()
    _check(
        lib.sonare_mastering_repair_apply(
            ctypes.cast(in_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
            _to_c_size_t(len(arrays), "channel_count"),
            _to_c_size_t(frame_count, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            stages_json.encode("utf-8"),
            progress_cb,
            None,
            ctypes.cast(out_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
            ctypes.byref(reports_ptr),
            cancel_cb,
            None,
        )
    )
    entries = json.loads(_take_string(lib, reports_ptr))
    return RepairApplyResult(
        channels=out_buffers, reports=[_stage_reports(entry) for entry in entries]
    )
