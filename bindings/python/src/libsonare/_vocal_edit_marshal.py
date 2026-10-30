"""Conversions between vocal-edit values and the ``sonare_vocal_*`` C structures."""

from __future__ import annotations

import ctypes
import math
from collections.abc import Callable, Sequence
from enum import IntEnum
from typing import Any, SupportsFloat, cast

import numpy as np

from ._errors import SonareError, SonareValueError, _unknown_error
from ._ffi_types_vocal import (
    SONARE_VOCAL_EDIT_API_VERSION,
    VOCAL_MAX_SAMPLE_RATE,
    VOCAL_MIN_SAMPLE_RATE,
    SonareVocalAnalysis,
    SonareVocalAnalysisResult,
    SonareVocalCancelCallback,
    SonareVocalCreateOptions,
    SonareVocalEditResult,
    SonareVocalErrorDetail,
    SonareVocalNote,
    SonareVocalNoteEdit,
    SonareVocalNotesResult,
    SonareVocalOperation,
    SonareVocalPitchPoint,
    SonareVocalPitchResult,
    SonareVocalRange,
    SonareVocalRenderResult,
    SonareVocalRestoreOptions,
    SonareVocalStateToken,
    SonareVocalTransition,
)
from ._runtime import (
    _INT64_MAX,
    _INT64_MIN,
    _UINT32_MAX,
    _check,
    _get_lib,
    _narrow_int,
    _resolve_sample_bound,
    _utf8_arg,
)
from ._vocal_edit_model import (
    VocalAnalysis,
    VocalAnalysisResult,
    VocalCreateOptions,
    VocalEditResult,
    VocalFormantMode,
    VocalIdChange,
    VocalMergeNotes,
    VocalMergePolicy,
    VocalNote,
    VocalNoteEdit,
    VocalOperation,
    VocalOperationKind,
    VocalPitchPoint,
    VocalPitchResult,
    VocalRange,
    VocalRemoveTransition,
    VocalRenderResult,
    VocalResetNotes,
    VocalSetNoteEdit,
    VocalSetNoteSourceSpan,
    VocalSetTransition,
    VocalSplitNote,
    VocalStateToken,
    VocalTargetMode,
    VocalTransition,
)

_UINT64_MAX = (1 << 64) - 1
_MAX_RESULT_ITEMS = 1 << 28


def _finite_float(value: object, name: str) -> float:
    try:
        result = float(cast(SupportsFloat, value))
    except OverflowError as exc:
        raise SonareValueError(f"{name} must be a finite number") from exc
    except (TypeError, ValueError) as exc:
        raise TypeError(f"{name} must be a finite number") from exc
    if not math.isfinite(result):
        raise SonareValueError(f"{name} must be a finite number")
    return result


def _bool_flag(value: object, name: str) -> int:
    if not isinstance(value, (bool, np.bool_)):
        raise TypeError(f"{name} must be a bool")
    return int(value)


def _int64(value: object, name: str) -> int:
    return _narrow_int(value, name, _INT64_MIN, _INT64_MAX)


def _uint32(value: object, name: str, *, allow_zero: bool = True) -> int:
    result = _narrow_int(value, name, 0, _UINT32_MAX)
    if not allow_zero and result == 0:
        raise SonareValueError(f"{name} must be positive")
    return result


def _uint64(value: object, name: str, *, allow_zero: bool = True) -> int:
    result = _narrow_int(value, name, 0, _UINT64_MAX)
    if not allow_zero and result == 0:
        raise SonareValueError(f"{name} must be positive")
    return result


def _enum(value: object, enum_type: type[IntEnum], name: str) -> int:
    try:
        result = _int64(value, name)
    except TypeError as exc:
        raise TypeError(f"{name} must be one of {[e.value for e in enum_type]}") from exc
    except SonareValueError as exc:
        raise SonareValueError(f"{name} must be one of {[e.value for e in enum_type]}") from exc
    try:
        enum_type(result)
    except ValueError as exc:
        raise SonareValueError(f"{name} must be one of {[e.value for e in enum_type]}") from exc
    return result


def _copy_pointer(pointer: Any, count: int, dtype: np.dtype[Any]) -> np.ndarray:
    """Copy a native array, rejecting an impossible count before allocation."""
    if count < 0 or count > _MAX_RESULT_ITEMS:
        raise _unknown_error(f"native vocal result count is unreasonable: {count}")
    if count == 0:
        return np.empty(0, dtype=dtype)
    if not pointer:
        raise _unknown_error("native vocal result contains a null array")
    return np.ctypeslib.as_array(pointer, shape=(count,)).copy()


def _text(value: bytes | bytearray | None) -> str:
    if not value:
        return ""
    return bytes(value).split(b"\0", 1)[0].decode("utf-8", errors="replace")


def _state_token(raw: SonareVocalStateToken) -> VocalStateToken:
    return VocalStateToken(
        session_epoch=int(raw.session_epoch),
        revision=int(raw.revision),
        draft_id=int(raw.draft_id),
        generation=int(raw.generation),
        request_id=int(raw.request_id),
        profile_id=int(raw.profile_id),
    )


class VocalEditError(SonareError):
    """A native vocal-edit failure with the C ABI's structured reason detail."""

    def __init__(
        self,
        code: int,
        message: str,
        *,
        reason: int = 0,
        field: str = "",
        expected: int = 0,
        actual: int = 0,
        expected_text: str = "",
        actual_text: str = "",
    ) -> None:
        self.reason = int(reason)
        self.field = field
        self.expected = int(expected)
        self.actual = int(actual)
        self.expected_text = expected_text
        self.actual_text = actual_text
        super().__init__(code, message)


def _vocal_check(rc: int) -> None:
    if int(rc) == 0:
        return
    lib = _get_lib()
    detail = SonareVocalErrorDetail()
    lib.sonare_vocal_error_detail_init(ctypes.byref(detail))
    lib.sonare_vocal_last_error_detail(ctypes.byref(detail))
    try:
        _check(int(rc))
    except SonareError as exc:
        message = str(exc)
        prefix = f"[{exc.code}] "
        if message.startswith(prefix):
            message = message[len(prefix) :]
        raise VocalEditError(
            exc.code,
            message,
            reason=int(detail.reason),
            field=_text(detail.field),
            expected=int(detail.expected),
            actual=int(detail.actual),
            expected_text=_text(detail.expected_text),
            actual_text=_text(detail.actual_text),
        ) from exc


def _new_result(lib: ctypes.CDLL, name: str, cls: type[Any]) -> Any:
    result = cls()
    getattr(lib, name)(ctypes.byref(result))
    return result


def _native_output_length(lib: ctypes.CDLL, symbol: str, handle: ctypes.c_void_p) -> int:
    output_length = ctypes.c_int64()
    _vocal_check(getattr(lib, symbol)(handle, ctypes.byref(output_length)))
    length = int(output_length.value)
    if length <= 0:
        raise _unknown_error(f"native vocal {symbol} returned an invalid output length: {length}")
    return length


_RANGE_BOUND_NAMES = ("range.start_sample", "range.end_sample")
_RANGE_SECOND_NAMES = ("start_sec", "end_sec")


def _range(
    value: VocalRange | Sequence[int | None] | None,
    *,
    output_length: int,
    sample_rate: int,
    start_sec: float | None = None,
    end_sec: float | None = None,
) -> SonareVocalRange:
    """Resolve a render range from a pair of samples or from seconds.

    The range is ``value`` (a :class:`VocalRange` or a ``(start_sample,
    end_sample)`` pair whose entries may be ``None``) or ``start_sec`` /
    ``end_sec``, not both. An omitted start is 0 and an omitted end is the end of
    the output; negative bounds are refused.
    """
    if value is not None and (start_sec is not None or end_sec is not None):
        raise SonareValueError("give range or start_sec/end_sec, not both")
    if value is None:
        start_i = _resolve_sample_bound(
            None, start_sec, sample_rate, _RANGE_BOUND_NAMES[0], "start_sec"
        )
        end_i = _resolve_sample_bound(None, end_sec, sample_rate, _RANGE_BOUND_NAMES[1], "end_sec")
    elif isinstance(value, VocalRange):
        start_i = _resolve_sample_bound(
            value.start_sample, None, sample_rate, _RANGE_BOUND_NAMES[0], "start_sec"
        )
        end_i = _resolve_sample_bound(
            value.end_sample, None, sample_rate, _RANGE_BOUND_NAMES[1], "end_sec"
        )
    else:
        try:
            length = len(value)
        except (TypeError, ValueError) as exc:
            raise SonareValueError(
                "render range must be a (start_sample, end_sample) pair"
            ) from exc
        if length != 2:
            raise SonareValueError("render range must be a (start_sample, end_sample) pair")
        start_i, end_i = (
            _resolve_sample_bound(
                item, None, sample_rate, _RANGE_BOUND_NAMES[i], _RANGE_SECOND_NAMES[i]
            )
            for i, item in enumerate(value)
        )
    start_i = 0 if start_i is None else start_i
    end_i = output_length if end_i is None else end_i
    if end_i < start_i:
        raise SonareValueError("render range must satisfy 0 <= start_sample <= end_sample")
    return SonareVocalRange(start_i, end_i)


def _analysis_to_c(
    lib: ctypes.CDLL, analysis: VocalAnalysis
) -> tuple[SonareVocalAnalysis, list[Any]]:
    f0 = np.ascontiguousarray(np.asarray(analysis.f0_hz, dtype=np.float32))
    voiced_input = np.asarray(analysis.voiced)
    if voiced_input.ndim != 1 or not np.isin(voiced_input, (0, 1)).all():
        raise SonareValueError("analysis voiced must be a one-dimensional 0/1 mask")
    voiced = np.ascontiguousarray(voiced_input, dtype=np.uint8)
    if f0.ndim != 1 or f0.size != voiced.size:
        raise SonareValueError("analysis f0_hz and voiced must be one-dimensional and equal length")
    raw = SonareVocalAnalysis()
    lib.sonare_vocal_analysis_init(ctypes.byref(raw))
    raw.frame_origin_sample = _finite_float(analysis.frame_origin_sample, "frame_origin_sample")
    raw.samples_per_frame = _finite_float(analysis.samples_per_frame, "samples_per_frame")
    raw.frame_length_samples = _uint32(
        analysis.frame_length_samples, "frame_length_samples", allow_zero=False
    )
    raw.f0_hz = f0.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    raw.voiced = voiced.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))
    raw.frame_count = int(f0.size)
    name = _utf8_arg(analysis.algorithm_id, "algorithm_id")
    raw.algorithm_id = name
    raw.algorithm_version = _uint32(analysis.algorithm_version, "algorithm_version")
    raw.fmin_hz = _finite_float(analysis.fmin_hz, "fmin_hz")
    raw.fmax_hz = _finite_float(analysis.fmax_hz, "fmax_hz")
    raw.yin_threshold = _finite_float(analysis.yin_threshold, "yin_threshold")
    raw.voiced_threshold = _finite_float(analysis.voiced_threshold, "voiced_threshold")
    raw.centered = _bool_flag(analysis.centered, "centered")
    raw.segmentation_threshold_cents = _finite_float(
        analysis.segmentation_threshold_cents, "segmentation_threshold_cents"
    )
    raw.min_note_ms = _finite_float(analysis.min_note_ms, "min_note_ms")
    raw.reference_hz = _finite_float(analysis.reference_hz, "reference_hz")
    return raw, [f0, voiced, name]


def _vocal_sample_rate(sample_rate: int) -> int:
    return _narrow_int(sample_rate, "sample_rate", VOCAL_MIN_SAMPLE_RATE, VOCAL_MAX_SAMPLE_RATE)


def _restore_options_to_c(
    lib: ctypes.CDLL,
    max_history_bytes: int | None,
    max_cache_bytes: int | None,
    max_undo_depth: int | None,
    max_render_jobs: int | None,
) -> SonareVocalRestoreOptions:
    raw = SonareVocalRestoreOptions()
    lib.sonare_vocal_restore_options_init(ctypes.byref(raw))
    if max_history_bytes is not None:
        raw.max_history_bytes = _uint64(max_history_bytes, "max_history_bytes")
    if max_cache_bytes is not None:
        raw.max_cache_bytes = _uint64(max_cache_bytes, "max_cache_bytes")
    if max_undo_depth is not None:
        raw.max_undo_depth = _uint32(max_undo_depth, "max_undo_depth")
    if max_render_jobs is not None:
        raw.max_render_jobs = _uint32(max_render_jobs, "max_render_jobs")
    return raw


def _options_to_c(
    lib: ctypes.CDLL, options: VocalCreateOptions | None
) -> tuple[SonareVocalCreateOptions, list[Any]]:
    raw = SonareVocalCreateOptions()
    lib.sonare_vocal_create_options_init(ctypes.byref(raw))
    keepalive: list[Any] = []
    if options is None:
        return raw, keepalive
    fields = (
        ("output_length_samples", _int64),
        ("edge_fade_ms", _finite_float),
        ("vibrato_cutoff_hz", _finite_float),
        ("segmentation_threshold_cents", _finite_float),
        ("min_note_ms", _finite_float),
        ("frame_length_samples", lambda v, n: _uint32(v, n, allow_zero=False)),
        ("hop_length_samples", lambda v, n: _uint32(v, n, allow_zero=False)),
        ("fmin_hz", _finite_float),
        ("fmax_hz", _finite_float),
        ("yin_threshold", _finite_float),
        ("voiced_threshold", _finite_float),
        ("centered", _bool_flag),
        ("reference_hz", _finite_float),
        ("max_history_bytes", _uint64),
        ("max_cache_bytes", _uint64),
        ("max_undo_depth", lambda v, n: _uint32(v, n)),
        ("max_render_jobs", lambda v, n: _uint32(v, n)),
    )
    for field_name, converter in fields:
        value = getattr(options, field_name)
        if value is None:
            continue
        converted = converter(value, field_name)
        setattr(raw, field_name, converted)
    if options.analysis is not None:
        analysis, buffers = _analysis_to_c(lib, options.analysis)
        raw.analysis = ctypes.pointer(analysis)
        keepalive.extend((analysis, *buffers))
    return raw, keepalive


def _edit_to_c(lib: ctypes.CDLL, edit: VocalNoteEdit) -> tuple[SonareVocalNoteEdit, list[Any]]:
    raw = SonareVocalNoteEdit()
    lib.sonare_vocal_note_edit_init(ctypes.byref(raw))
    raw.target_mode = _enum(edit.target_mode, VocalTargetMode, "target_mode")
    raw.target_midi = _finite_float(edit.target_midi, "target_midi")
    raw.amount = _finite_float(edit.amount, "amount")
    raw.speed_ms = _finite_float(edit.speed_ms, "speed_ms")
    raw.max_correction_semitones = _finite_float(
        edit.max_correction_semitones, "max_correction_semitones"
    )
    raw.transpose_semitones = _finite_float(edit.transpose_semitones, "transpose_semitones")
    raw.drift_scale = _finite_float(edit.drift_scale, "drift_scale")
    raw.vibrato_scale = _finite_float(edit.vibrato_scale, "vibrato_scale")
    raw.destination_start_sample = _int64(edit.destination_start_sample, "destination_start_sample")
    raw.destination_length_samples = _int64(
        edit.destination_length_samples, "destination_length_samples"
    )
    raw.gain_db = _finite_float(edit.gain_db, "gain_db")
    if not isinstance(edit.muted, (bool, np.bool_)):
        raise TypeError("muted must be a bool")
    raw.muted = int(edit.muted)
    raw.formant_mode = _enum(edit.formant_mode, VocalFormantMode, "formant_mode")
    raw.formant_shift_semitones = _finite_float(
        edit.formant_shift_semitones, "formant_shift_semitones"
    )
    keepalive: list[Any] = []
    if edit.target_points:
        points = (SonareVocalPitchPoint * len(edit.target_points))()
        for i, point in enumerate(edit.target_points):
            points[i].source_sample = _finite_float(
                point.source_sample, "target_points.source_sample"
            )
            points[i].midi = _finite_float(point.target_midi, "target_points.target_midi")
        raw.target_points = ctypes.cast(points, ctypes.POINTER(SonareVocalPitchPoint))
        raw.target_point_count = len(edit.target_points)
        keepalive.append(points)
    if edit.amplitude_envelope:
        envelope = np.ascontiguousarray(np.asarray(edit.amplitude_envelope, dtype=np.float32))
        if envelope.ndim != 1 or not np.isfinite(envelope).all():
            raise SonareValueError("amplitude_envelope must be a finite one-dimensional sequence")
        raw.amplitude_envelope = envelope.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        raw.amplitude_envelope_count = int(envelope.size)
        keepalive.append(envelope)
    return raw, keepalive


def _transition_to_c(lib: ctypes.CDLL, transition: VocalTransition) -> SonareVocalTransition:
    raw = SonareVocalTransition()
    raw.struct_size = ctypes.sizeof(SonareVocalTransition)
    raw.schema_version = SONARE_VOCAL_EDIT_API_VERSION
    raw.left_note_id = _uint32(transition.left_note_id, "left_note_id", allow_zero=False)
    raw.right_note_id = _uint32(transition.right_note_id, "right_note_id", allow_zero=False)
    raw.left_window_samples = _narrow_int(
        transition.left_window_samples, "left_window_samples", 0, _INT64_MAX
    )
    raw.right_window_samples = _narrow_int(
        transition.right_window_samples, "right_window_samples", 0, _INT64_MAX
    )
    raw.strength = _finite_float(transition.strength, "strength")
    if not 0.0 <= raw.strength <= 1.0:
        raise SonareValueError("strength must be in [0, 1]")
    return raw


def _operation_position(
    operation_name: str,
    sample_value: int | None,
    sec_value: float | None,
    sample_rate: int,
    sample_name: str,
    sec_name: str,
) -> int:
    """Resolve one required operation position from samples or seconds.

    Absence is ``None``; a real 0 is the sample position 0.
    """
    resolved = _resolve_sample_bound(sample_value, sec_value, sample_rate, sample_name, sec_name)
    if resolved is None:
        raise SonareValueError(f"{operation_name} needs {sample_name} or {sec_name}")
    return resolved


def _operation_to_c(
    lib: ctypes.CDLL, operation: VocalOperation, sample_rate: int
) -> tuple[SonareVocalOperation, list[Any]]:
    raw = SonareVocalOperation()
    lib.sonare_vocal_operation_init(ctypes.byref(raw))
    keepalive: list[Any] = []
    if isinstance(operation, VocalSetNoteEdit):
        raw.kind = int(VocalOperationKind.SET_EDIT)
        raw.note_id = _uint32(operation.note_id, "note_id", allow_zero=False)
        raw.edit, buffers = _edit_to_c(lib, operation.edit)
        keepalive.extend(buffers)
    elif isinstance(operation, VocalSetNoteSourceSpan):
        raw.kind = int(VocalOperationKind.SET_SOURCE_SPAN)
        raw.note_id = _uint32(operation.note_id, "note_id", allow_zero=False)
        raw.source_start_sample = _operation_position(
            "VocalSetNoteSourceSpan",
            operation.source_start_sample,
            operation.source_start_sec,
            sample_rate,
            "source_start_sample",
            "source_start_sec",
        )
        raw.source_end_sample = _operation_position(
            "VocalSetNoteSourceSpan",
            operation.source_end_sample,
            operation.source_end_sec,
            sample_rate,
            "source_end_sample",
            "source_end_sec",
        )
        raw.destination_start_sample = _operation_position(
            "VocalSetNoteSourceSpan",
            operation.destination_start_sample,
            operation.destination_start_sec,
            sample_rate,
            "destination_start_sample",
            "destination_start_sec",
        )
        raw.destination_length_samples = _operation_position(
            "VocalSetNoteSourceSpan",
            operation.destination_length_samples,
            operation.destination_length_sec,
            sample_rate,
            "destination_length_samples",
            "destination_length_sec",
        )
    elif isinstance(operation, VocalSplitNote):
        raw.kind = int(VocalOperationKind.SPLIT)
        raw.note_id = _uint32(operation.note_id, "note_id", allow_zero=False)
        raw.cut_source_sample = _operation_position(
            "VocalSplitNote",
            operation.cut_source_sample,
            operation.cut_source_sec,
            sample_rate,
            "cut_source_sample",
            "cut_source_sec",
        )
    elif isinstance(operation, VocalMergeNotes):
        raw.kind = int(VocalOperationKind.MERGE)
        raw.merge_policy = _enum(operation.merge_policy, VocalMergePolicy, "merge_policy")
        ids = (ctypes.c_uint32 * len(operation.note_ids))()
        for i, value in enumerate(operation.note_ids):
            ids[i] = _uint32(value, "note_ids", allow_zero=False)
        raw.note_ids = ctypes.cast(ids, ctypes.POINTER(ctypes.c_uint32))
        raw.note_id_count = len(operation.note_ids)
        keepalive.append(ids)
    elif isinstance(operation, VocalSetTransition):
        raw.kind = int(VocalOperationKind.SET_TRANSITION)
        raw.transition = _transition_to_c(lib, operation.transition)
    elif isinstance(operation, VocalRemoveTransition):
        raw.kind = int(VocalOperationKind.REMOVE_TRANSITION)
        raw.transition.left_note_id = _uint32(
            operation.left_note_id, "left_note_id", allow_zero=False
        )
        raw.transition.right_note_id = _uint32(
            operation.right_note_id, "right_note_id", allow_zero=False
        )
    elif isinstance(operation, VocalResetNotes):
        raw.kind = int(VocalOperationKind.RESET)
        ids = (ctypes.c_uint32 * len(operation.note_ids))()
        for i, value in enumerate(operation.note_ids):
            ids[i] = _uint32(value, "note_ids", allow_zero=False)
        raw.note_ids = ctypes.cast(ids, ctypes.POINTER(ctypes.c_uint32))
        raw.note_id_count = len(operation.note_ids)
        keepalive.append(ids)
    else:
        raise SonareValueError(f"unsupported vocal operation: {type(operation).__name__}")
    return raw, keepalive


def _note_edit_from_c(raw: SonareVocalNoteEdit) -> VocalNoteEdit:
    points = (
        tuple(
            VocalPitchPoint(
                float(raw.target_points[i].source_sample), float(raw.target_points[i].midi)
            )
            for i in range(int(raw.target_point_count))
        )
        if raw.target_points
        else ()
    )
    envelope = tuple(
        float(v)
        for v in _copy_pointer(
            raw.amplitude_envelope, int(raw.amplitude_envelope_count), np.dtype(np.float32)
        )
    )
    return VocalNoteEdit(
        target_mode=VocalTargetMode(int(raw.target_mode)),
        target_midi=float(raw.target_midi),
        target_points=points,
        amount=float(raw.amount),
        speed_ms=float(raw.speed_ms),
        max_correction_semitones=float(raw.max_correction_semitones),
        transpose_semitones=float(raw.transpose_semitones),
        drift_scale=float(raw.drift_scale),
        vibrato_scale=float(raw.vibrato_scale),
        destination_start_sample=int(raw.destination_start_sample),
        destination_length_samples=int(raw.destination_length_samples),
        gain_db=float(raw.gain_db),
        muted=bool(raw.muted),
        formant_mode=VocalFormantMode(int(raw.formant_mode)),
        formant_shift_semitones=float(raw.formant_shift_semitones),
        amplitude_envelope=envelope,
    )


def _note_from_c(raw: SonareVocalNote) -> VocalNote:
    amplitude = _copy_pointer(raw.amplitude, int(raw.amplitude_count), np.dtype(np.float32))
    return VocalNote(
        id=int(raw.id),
        source_start_sample=int(raw.source_start_sample),
        source_end_sample=int(raw.source_end_sample),
        analysis_frame_start=int(raw.analysis_frame_start),
        analysis_frame_end=int(raw.analysis_frame_end),
        has_pitch=bool(raw.has_pitch),
        median_hz=float(raw.median_hz),
        center_midi=float(raw.center_midi),
        f0_stability=float(raw.f0_stability),
        amplitude=amplitude,
        edit=_note_edit_from_c(raw.edit),
    )


def _notes_from_result(
    raw: SonareVocalNotesResult,
) -> tuple[tuple[VocalNote, ...], tuple[VocalTransition, ...]]:
    notes = (
        tuple(_note_from_c(raw.notes[i]) for i in range(int(raw.note_count))) if raw.notes else ()
    )
    transitions = (
        tuple(
            VocalTransition(
                left_note_id=int(raw.transitions[i].left_note_id),
                right_note_id=int(raw.transitions[i].right_note_id),
                left_window_samples=int(raw.transitions[i].left_window_samples),
                right_window_samples=int(raw.transitions[i].right_window_samples),
                strength=float(raw.transitions[i].strength),
            )
            for i in range(int(raw.transition_count))
        )
        if raw.transitions
        else ()
    )
    return notes, transitions


def _analysis_from_result(raw: SonareVocalAnalysisResult) -> VocalAnalysisResult:
    a = raw.analysis
    f0 = _copy_pointer(a.f0_hz, int(a.frame_count), np.dtype(np.float32))
    voiced = _copy_pointer(a.voiced, int(a.frame_count), np.dtype(np.uint8))
    analysis = VocalAnalysis(
        frame_origin_sample=float(a.frame_origin_sample),
        samples_per_frame=float(a.samples_per_frame),
        frame_length_samples=int(a.frame_length_samples),
        f0_hz=f0,
        voiced=voiced,
        algorithm_id=_text(a.algorithm_id),
        algorithm_version=int(a.algorithm_version),
        fmin_hz=float(a.fmin_hz),
        fmax_hz=float(a.fmax_hz),
        yin_threshold=float(a.yin_threshold),
        voiced_threshold=float(a.voiced_threshold),
        centered=bool(a.centered),
        segmentation_threshold_cents=float(a.segmentation_threshold_cents),
        min_note_ms=float(a.min_note_ms),
        reference_hz=float(a.reference_hz),
    )
    return VocalAnalysisResult(
        analysis=analysis,
        source_sha256=_text(raw.source_sha256),
        analysis_sha256=_text(raw.analysis_sha256),
        source_length_samples=int(raw.source_length_samples),
        sample_rate=int(raw.sample_rate),
    )


def _edit_result_from_c(raw: SonareVocalEditResult) -> VocalEditResult:
    ranges = (
        tuple(
            VocalRange(int(raw.dirty_ranges[i].start_sample), int(raw.dirty_ranges[i].end_sample))
            for i in range(int(raw.dirty_range_count))
        )
        if raw.dirty_ranges
        else ()
    )
    changes = []
    if raw.id_changes:
        for i in range(int(raw.id_change_count)):
            change = raw.id_changes[i]
            new_ids = tuple(v for v in (int(change.first_new_id), int(change.second_new_id)) if v)
            changes.append(
                VocalIdChange(int(change.operation_index), int(change.retired_id), new_ids)
            )
    return VocalEditResult(_state_token(raw.token), ranges, tuple(changes))


def _pitch_result_from_c(raw: SonareVocalPitchResult) -> VocalPitchResult:
    count = int(raw.frame_count)
    return VocalPitchResult(
        source_samples=_copy_pointer(raw.source_samples, count, np.dtype(np.float64)),
        measured_midi=_copy_pointer(raw.measured_midi, count, np.dtype(np.float64)),
        target_midi=_copy_pointer(raw.target_midi, count, np.dtype(np.float64)),
        effective_midi=_copy_pointer(raw.effective_midi, count, np.dtype(np.float64)),
        voiced=_copy_pointer(raw.voiced, count, np.dtype(np.uint8)),
        has_target=_copy_pointer(raw.has_target, count, np.dtype(np.uint8)),
    )


def _render_result_from_c(raw: SonareVocalRenderResult) -> VocalRenderResult:
    ranges = (
        tuple(
            VocalRange(
                int(raw.processed_ranges[i].start_sample), int(raw.processed_ranges[i].end_sample)
            )
            for i in range(int(raw.processed_range_count))
        )
        if raw.processed_ranges
        else ()
    )
    return VocalRenderResult(
        samples=_copy_pointer(raw.samples, int(raw.sample_count), np.dtype(np.float32)),
        start_sample=int(raw.start_sample),
        token=_state_token(raw.token),
        processed_ranges=ranges,
        cache_hit_units=int(raw.cache_hit_units),
        dry_passed_frames=int(raw.dry_passed_frames),
        limited_correction_frames=int(raw.limited_correction_frames),
    )


def _callback(cancel: Callable[[], bool] | None) -> tuple[Any, list[BaseException]]:
    errors: list[BaseException] = []
    if cancel is None:
        return SonareVocalCancelCallback(), errors

    def probe(_: ctypes.c_void_p) -> int:
        if errors:
            return 1
        try:
            return 1 if bool(cancel()) else 0
        except BaseException as exc:
            errors.append(exc)
            return 1

    return SonareVocalCancelCallback(probe), errors


def _probe_check(errors: list[BaseException], error: int) -> None:
    if errors:
        raise errors[0]
    _vocal_check(error)
