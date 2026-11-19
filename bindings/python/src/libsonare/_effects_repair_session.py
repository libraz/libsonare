"""One repair analysis and one repair application over any number of channels."""

from __future__ import annotations

import ctypes
import dataclasses
import json
from collections.abc import Callable, Mapping, Sequence
from typing import Any, cast

import numpy as np

from ._cancellation import (
    CancellationState,
    make_cancel_trampoline,
    make_progress_trampoline,
    raise_callback_error,
)
from ._effects_repair_common import _linked_channel_planes, _linked_output_planes
from ._errors import _entry, _refuse_type
from ._ffi import SonareMasteringProgressCallback
from ._result_records import _camel, _fields, _snake
from ._runtime import (
    _WITHOUT_MASTERING,
    _check,
    _get_lib,
    _planar_channel_arrays,
    _require_export,
    _to_c_int,
    _to_c_size_t,
    _utf8_arg,
)
from .types import (
    AudioDefects,
    ClickDetection,
    ClipDetection,
    CrackleDetection,
    DeclickReport,
    DeclickStage,
    DeclipReport,
    DeclipStage,
    DecrackleReport,
    DecrackleStage,
    DehumReport,
    DehumStage,
    DenoiseReport,
    DenoiseStage,
    DereverbReport,
    DereverbStage,
    HumDetection,
    MasteringRepairAnalysis,
    NoiseDetection,
    RepairApplyResult,
    RepairStage,
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


_STAGE_TYPES: dict[str, type[RepairStage]] = {
    "declip": DeclipStage,
    "declick": DeclickStage,
    "decrackle": DecrackleStage,
    "dehum": DehumStage,
    "denoise": DenoiseStage,
    "dereverb": DereverbStage,
}


def _detection(detection_type: type, values: Mapping[str, Any]) -> Any:
    detected = _fields(values)
    if detection_type is ClipDetection and not detected["flat_run_count"]:
        # With no flat run there is no level to report; the document still carries one.
        detected["flat_level"] = None
    if detection_type is ReverbDetection:
        # The document omits the key when the WPE stage did not run.
        detected.setdefault("late_predictability", None)
    return detection_type(**detected)


def _audio_defects_from_wire(values: Mapping[str, Any]) -> AudioDefects:
    return AudioDefects(
        click=_detection(ClickDetection, values["click"]),
        clip=_detection(ClipDetection, values["clip"]),
        crackle=_detection(CrackleDetection, values["crackle"]),
        noise=_detection(NoiseDetection, values["noise"]),
        hum=_detection(HumDetection, values["hum"]),
        reverb=_detection(ReverbDetection, values["reverb"]),
    )


def _stage_from_wire(values: Mapping[str, Any]) -> RepairStage:
    settings = {_snake(key): value for key, value in values.items() if key != "stage"}
    return _STAGE_TYPES[values["stage"]](**settings)


def _stage_to_wire(stage: RepairStage) -> dict[str, Any]:
    wire: dict[str, Any] = {"stage": stage.stage}
    for item in dataclasses.fields(cast(Any, stage)):
        value = getattr(stage, item.name)
        if item.name == "stage" or value is None:
            continue
        wire[_camel(item.name)] = value
    return wire


def _repair_analysis_from_wire(values: Mapping[str, Any]) -> MasteringRepairAnalysis:
    defects = values.get("defects")
    channels = values.get("channels")
    return MasteringRepairAnalysis(
        defects=None if defects is None else _audio_defects_from_wire(defects),
        channels=None if channels is None else [_audio_defects_from_wire(c) for c in channels],
        declip_threshold_safe=bool(values["declipThresholdSafe"]),
        integrated_lufs=float(values["integratedLufs"]),
        recommended=[_stage_from_wire(stage) for stage in values["recommended"]],
        explanation=list(values["explanation"]),
    )


def _stage_reports(entry: Mapping[str, Any]) -> RepairStageReports:
    report_type, detection_type = _REPORT_TYPES[entry["stage"]]
    reports = []
    for report in entry["reports"]:
        fields = _fields(report)
        fields["detected"] = _detection(detection_type, report["detected"])
        reports.append(report_type(**fields))
    return RepairStageReports(stage=entry["stage"], scope=entry["scope"], reports=reports)


def _progress_trampoline(
    on_progress: Callable[[float, str], None] | None, state: CancellationState
) -> Any:
    if on_progress is None:
        return SonareMasteringProgressCallback(0)
    return make_progress_trampoline(SonareMasteringProgressCallback, on_progress, state)


def _take_string(lib: ctypes.CDLL, pointer: ctypes.c_char_p) -> str:
    try:
        return ctypes.string_at(pointer).decode("utf-8") if pointer.value else ""
    finally:
        if pointer.value:
            lib.sonare_free_string(pointer)


@_entry("mastering_repair_analyze")
def mastering_repair_analyze(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    sample_rate: int,
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
            [left, right], 48000, stages=analysis.recommended
        )

    Args:
        channels: One buffer per channel, all of the same length, or a 2-D
            ``(channels, frames)`` array. At least one.
        sample_rate: Sample rate in Hz.
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
    _require_export(lib, "sonare_mastering_repair_analyze", _WITHOUT_MASTERING)
    planes = _linked_channel_planes(channels)
    arrays, in_ptrs, frame_count = _planar_channel_arrays(planes, subject="channels")
    request = json.dumps({"preferStreamingSafe": bool(prefer_streaming_safe)})
    json_ptr = ctypes.c_char_p()
    _check(
        lib.sonare_mastering_repair_analyze(
            ctypes.cast(in_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
            _to_c_size_t(len(arrays), "channel_count"),
            _to_c_size_t(frame_count, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            _utf8_arg(request, "request"),
            ctypes.byref(json_ptr),
        )
    )
    return _repair_analysis_from_wire(json.loads(_take_string(lib, json_ptr)))


@_entry("mastering_repair_apply")
def mastering_repair_apply(
    channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
    sample_rate: int,
    *,
    stages: Sequence[RepairStage],
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
            stages=[libsonare.DenoiseStage(mode="logMmse"), libsonare.DeclickStage()],
        )
        print(result.reports[0].stage)  # "declick"

    Args:
        channels: One buffer per channel, all of the same length, or a 2-D
            ``(channels, frames)`` array. At least one.
        sample_rate: Sample rate in Hz.
        stages: Stage records (:class:`DeclipStage`, :class:`DeclickStage`,
            :class:`DecrackleStage`, :class:`DehumStage`, :class:`DenoiseStage`,
            :class:`DereverbStage`), each stage at most once. A setting left as
            None takes the stage's default.
        on_progress: Optional callback ``(progress, stage)`` invoked after each
            stage with the fraction done and ``"repair.<stage>"``. An exception
            raised inside it stops the repair at its next cancellation point
            and is raised from this call.
        cancel: Optional zero-argument callable polled after each progress
            report. A true return value cancels with ``SonareError``.

    Returns:
        :class:`~libsonare.types.RepairApplyResult` with one repaired buffer per
        input channel and one :class:`~libsonare.types.RepairStageReports` per
        applied stage, in application order.

    Raises:
        TypeError: If ``stages`` is not a sequence of stage records.
        SonareValueError: If ``channels`` is empty, the channels disagree in
            length, or any channel is empty or carries a non-finite sample.
        SonareError: If the C call rejects the request (a repeated or unknown
            stage, an unknown or out-of-range setting) or it was cancelled.
    """
    lib = _get_lib()
    _require_export(lib, "sonare_mastering_repair_apply", _WITHOUT_MASTERING)
    stage_types = tuple(_STAGE_TYPES.values())
    if isinstance(stages, (str, bytes, Mapping)) or not all(
        isinstance(stage, stage_types) for stage in stages
    ):
        raise _refuse_type("stages must be a sequence of stage records")
    planes = _linked_channel_planes(channels)
    arrays, in_ptrs, frame_count = _planar_channel_arrays(planes, subject="channels")
    stages_json = json.dumps([_stage_to_wire(stage) for stage in stages])
    out_buffers, out_ptrs = _linked_output_planes(len(arrays), frame_count)
    state = CancellationState(cancel)
    progress_cb = _progress_trampoline(on_progress, state)
    cancel_cb = make_cancel_trampoline(state)
    reports_ptr = ctypes.c_char_p()
    rc = lib.sonare_mastering_repair_apply(
        ctypes.cast(in_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
        _to_c_size_t(len(arrays), "channel_count"),
        _to_c_size_t(frame_count, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _utf8_arg(stages_json, "stages"),
        progress_cb,
        None,
        ctypes.cast(out_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_float))),
        ctypes.byref(reports_ptr),
        cancel_cb,
        None,
    )
    raise_callback_error(state, rc, lambda: _take_string(lib, reports_ptr))
    _check(rc)
    entries = json.loads(_take_string(lib, reports_ptr))
    return RepairApplyResult(
        channels=out_buffers, reports=[_stage_reports(entry) for entry in entries]
    )
