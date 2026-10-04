"""Melodyne-style monophonic vocal editing.

The classes in this module are a small ownership-safe layer over the versioned
``sonare_vocal_*`` C ABI.  Session, draft, snapshot, and render-job handles are
all context managers.  Returned native arrays are copied before the C result is
released, so callers never observe a borrowed ctypes view after a method
returns.
"""

from __future__ import annotations

import contextlib
import ctypes
import math
import weakref
from collections.abc import Callable, Iterable, Iterator, Sequence
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, Self, SupportsFloat, TypeAlias, cast

import numpy as np
from numpy.typing import NDArray

from ._errors import SonareError, SonareValueError
from ._ffi_types_vocal import (
    SONARE_VOCAL_EDIT_API_VERSION,
    VOCAL_MAX_SAMPLE_RATE,
    VOCAL_MIN_SAMPLE_RATE,
    SonareVocalAnalysis,
    SonareVocalAnalysisResult,
    SonareVocalCancelCallback,
    SonareVocalCapabilities,
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
    SonareVocalStateBytes,
    SonareVocalStateToken,
    SonareVocalTransition,
)
from ._runtime import (
    _INT64_MAX,
    _INT64_MIN,
    _check,
    _get_lib,
    _narrow_int,
    _to_c_int,
    _validate_samples,
)


class VocalTargetMode(IntEnum):
    NONE = 0
    CENTER = 1
    CURVE = 2


class VocalFormantMode(IntEnum):
    PRESERVE = 0
    SHIFT = 1


class VocalOperationKind(IntEnum):
    SET_EDIT = 0
    SET_SOURCE_SPAN = 1
    SPLIT = 2
    MERGE = 3
    SET_TRANSITION = 4
    REMOVE_TRANSITION = 5
    RESET = 6


class VocalMergePolicy(IntEnum):
    PRESERVE = 0
    RESET = 1


_UINT32_MAX = (1 << 32) - 1
_UINT64_MAX = (1 << 64) - 1
_MAX_RESULT_ITEMS = 1 << 28
FloatArray: TypeAlias = NDArray[np.float32]


def _finite_float(value: object, name: str) -> float:
    try:
        result = float(cast(SupportsFloat, value))
    except (TypeError, ValueError, OverflowError) as exc:
        raise SonareValueError(f"{name} must be a finite number") from exc
    if not math.isfinite(result):
        raise SonareValueError(f"{name} must be a finite number")
    return result


def _bool_flag(value: object, name: str) -> int:
    if not isinstance(value, (bool, np.bool_)):
        raise SonareValueError(f"{name} must be a bool")
    return int(value)


def _int64(value: object, name: str) -> int:
    try:
        return _narrow_int(value, name, _INT64_MIN, _INT64_MAX)
    except SonareValueError:
        raise


def _uint32(value: object, name: str, *, allow_zero: bool = True) -> int:
    try:
        result = _narrow_int(value, name, 0, _UINT32_MAX)
    except SonareValueError:
        raise
    if not allow_zero and result == 0:
        raise SonareValueError(f"{name} must be positive")
    return result


def _uint64(value: object, name: str, *, allow_zero: bool = True) -> int:
    try:
        result = _narrow_int(value, name, 0, _UINT64_MAX)
    except SonareValueError:
        raise
    if not allow_zero and result == 0:
        raise SonareValueError(f"{name} must be positive")
    return result


def _enum(value: object, enum_type: type[IntEnum], name: str) -> int:
    try:
        result = _int64(value, name)
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
        raise RuntimeError(f"native vocal result count is unreasonable: {count}")
    if count == 0:
        return np.empty(0, dtype=dtype)
    if not pointer:
        raise RuntimeError("native vocal result contains a null array")
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
        raise RuntimeError(f"native vocal {symbol} returned an invalid output length: {length}")
    return length


_RANGE_BOUND_NAMES = ("range.start_sample", "range.end_sample")


def _range(value: VocalRange | Sequence[int] | None, *, output_length: int) -> SonareVocalRange:
    if value is None:
        return SonareVocalRange(0, output_length)
    if isinstance(value, VocalRange):
        start_i = _int64(value.start_sample, "range.start_sample")
        end_i = _int64(value.end_sample, "range.end_sample")
    else:
        try:
            length = len(value)
        except (TypeError, ValueError) as exc:
            raise SonareValueError(
                "render range must be a (start_sample, end_sample) pair"
            ) from exc
        if length != 2:
            raise SonareValueError("render range must be a (start_sample, end_sample) pair")
        start_i, end_i = (_int64(item, _RANGE_BOUND_NAMES[i]) for i, item in enumerate(value))
    if start_i < 0 or end_i < start_i:
        raise SonareValueError("render range must satisfy 0 <= start_sample <= end_sample")
    return SonareVocalRange(start_i, end_i)


@dataclass(frozen=True, slots=True)
class VocalRange:
    start_sample: int
    end_sample: int

    @property
    def length(self) -> int:
        return max(0, self.end_sample - self.start_sample)


@dataclass(frozen=True, slots=True)
class VocalPitchPoint:
    source_sample: float
    target_midi: float

    @property
    def midi(self) -> float:
        return self.target_midi


@dataclass(frozen=True, slots=True)
class VocalNoteEdit:
    target_mode: VocalTargetMode | int = VocalTargetMode.NONE
    target_midi: float = 0.0
    target_points: tuple[VocalPitchPoint, ...] = ()
    amount: float = 0.0
    speed_ms: float = 0.0
    max_correction_semitones: float = 12.0
    transpose_semitones: float = 0.0
    drift_scale: float = 1.0
    vibrato_scale: float = 1.0
    destination_start_sample: int = 0
    destination_length_samples: int = 0
    gain_db: float = 0.0
    muted: bool = False
    formant_mode: VocalFormantMode | int = VocalFormantMode.PRESERVE
    formant_shift_semitones: float = 0.0
    amplitude_envelope: tuple[float, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "target_points", tuple(self.target_points))
        object.__setattr__(self, "amplitude_envelope", tuple(self.amplitude_envelope))


@dataclass(frozen=True, slots=True)
class VocalNote:
    id: int
    source_start_sample: int
    source_end_sample: int
    analysis_frame_start: int
    analysis_frame_end: int
    has_pitch: bool
    median_hz: float
    center_midi: float
    f0_stability: float
    amplitude: FloatArray
    edit: VocalNoteEdit

    @property
    def source_range(self) -> VocalRange:
        return VocalRange(self.source_start_sample, self.source_end_sample)


@dataclass(frozen=True, slots=True)
class VocalTransition:
    left_note_id: int
    right_note_id: int
    left_window_samples: int
    right_window_samples: int
    strength: float


@dataclass(frozen=True, slots=True)
class VocalStateToken:
    session_epoch: int
    revision: int
    draft_id: int
    generation: int
    request_id: int = 0
    profile_id: int = 0

    @property
    def committed_revision(self) -> int:
        return self.revision


@dataclass(frozen=True, slots=True)
class VocalAnalysis:
    frame_origin_sample: float
    samples_per_frame: float
    frame_length_samples: int
    f0_hz: FloatArray
    voiced: NDArray[np.uint8]
    algorithm_id: str
    algorithm_version: int
    fmin_hz: float = 65.0
    fmax_hz: float = 2093.0
    yin_threshold: float = 0.1
    voiced_threshold: float = 0.5
    centered: bool = True
    segmentation_threshold_cents: float = 50.0
    min_note_ms: float = 30.0
    reference_hz: float = 440.0


@dataclass(frozen=True, slots=True)
class VocalAnalysisResult:
    analysis: VocalAnalysis
    source_sha256: str
    analysis_sha256: str
    source_length_samples: int
    sample_rate: int


@dataclass(frozen=True, slots=True)
class VocalCapabilities:
    api_version: int
    profile_id: int
    monophonic_only: bool
    analysis_cancellable: bool
    minimum_formant_shift_semitones: float
    maximum_formant_shift_semitones: float


@dataclass(frozen=True, slots=True)
class VocalIdChange:
    operation_index: int
    retired_id: int
    new_ids: tuple[int, ...]


@dataclass(frozen=True, slots=True)
class VocalEditResult:
    token: VocalStateToken
    dirty_ranges: tuple[VocalRange, ...]
    id_changes: tuple[VocalIdChange, ...] = ()


@dataclass(frozen=True, slots=True)
class VocalPitchResult:
    source_samples: NDArray[np.float64]
    measured_midi: NDArray[np.float64]
    target_midi: NDArray[np.float64]
    effective_midi: NDArray[np.float64]
    voiced: NDArray[np.uint8]
    has_target: NDArray[np.uint8]


@dataclass(frozen=True, slots=True)
class VocalRenderResult:
    samples: FloatArray
    start_sample: int
    token: VocalStateToken
    processed_ranges: tuple[VocalRange, ...]
    cache_hit_units: int
    dry_passed_frames: int
    limited_correction_frames: int


@dataclass(frozen=True, slots=True)
class VocalCreateOptions:
    output_length_samples: int | None = None
    edge_fade_ms: float | None = None
    vibrato_cutoff_hz: float | None = None
    segmentation_threshold_cents: float | None = None
    min_note_ms: float | None = None
    frame_length_samples: int | None = None
    hop_length_samples: int | None = None
    fmin_hz: float | None = None
    fmax_hz: float | None = None
    max_history_bytes: int | None = None
    max_cache_bytes: int | None = None
    max_undo_depth: int | None = None
    max_render_jobs: int | None = None
    analysis: VocalAnalysis | None = None
    yin_threshold: float | None = None
    voiced_threshold: float | None = None
    centered: bool | None = None
    reference_hz: float | None = None


@dataclass(frozen=True, slots=True)
class VocalSetNoteEdit:
    note_id: int
    edit: VocalNoteEdit


@dataclass(frozen=True, slots=True)
class VocalSetNoteSourceSpan:
    note_id: int
    source_start_sample: int
    source_end_sample: int
    destination_start_sample: int = 0
    destination_length_samples: int = 0


@dataclass(frozen=True, slots=True)
class VocalSplitNote:
    note_id: int
    cut_source_sample: int


@dataclass(frozen=True, slots=True)
class VocalMergeNotes:
    note_ids: tuple[int, ...]
    merge_policy: VocalMergePolicy | int = VocalMergePolicy.PRESERVE

    def __post_init__(self) -> None:
        object.__setattr__(self, "note_ids", tuple(self.note_ids))


@dataclass(frozen=True, slots=True)
class VocalSetTransition:
    transition: VocalTransition


@dataclass(frozen=True, slots=True)
class VocalRemoveTransition:
    left_note_id: int
    right_note_id: int


@dataclass(frozen=True, slots=True)
class VocalResetNotes:
    note_ids: tuple[int, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "note_ids", tuple(self.note_ids))


VocalOperation: TypeAlias = (
    VocalSetNoteEdit
    | VocalSetNoteSourceSpan
    | VocalSplitNote
    | VocalMergeNotes
    | VocalSetTransition
    | VocalRemoveTransition
    | VocalResetNotes
)


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
    name = analysis.algorithm_id.encode("utf-8")
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
        raise SonareValueError("muted must be a bool")
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


def _operation_to_c(
    lib: ctypes.CDLL, operation: VocalOperation
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
        raw.source_start_sample = _int64(operation.source_start_sample, "source_start_sample")
        raw.source_end_sample = _int64(operation.source_end_sample, "source_end_sample")
        raw.destination_start_sample = _int64(
            operation.destination_start_sample, "destination_start_sample"
        )
        raw.destination_length_samples = _int64(
            operation.destination_length_samples, "destination_length_samples"
        )
    elif isinstance(operation, VocalSplitNote):
        raw.kind = int(VocalOperationKind.SPLIT)
        raw.note_id = _uint32(operation.note_id, "note_id", allow_zero=False)
        raw.cut_source_sample = _int64(operation.cut_source_sample, "cut_source_sample")
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


class _HandleOwner:
    _handle: ctypes.c_void_p
    _destroy_symbol: str
    _active_native_calls: int
    _close_pending: bool

    def _require_handle(self) -> ctypes.c_void_p:
        if not self._handle or not self._handle.value or getattr(self, "_close_pending", False):
            raise SonareError(7, f"{type(self).__name__} is closed")
        return self._handle

    def close(self) -> None:
        handle = getattr(self, "_handle", None)
        if handle is None or not handle.value:
            return
        if getattr(self, "_active_native_calls", 0):
            self._close_pending = True
            return
        self._destroy_handle()

    def _destroy_handle(self) -> None:
        self._close_pending = False
        try:
            getattr(_get_lib(), self._destroy_symbol)(self._handle)
        finally:
            self._handle = ctypes.c_void_p()

    @contextlib.contextmanager
    def _native_call(self) -> Iterator[None]:
        self._require_handle()
        self._active_native_calls = getattr(self, "_active_native_calls", 0) + 1
        try:
            yield
        finally:
            self._active_native_calls = max(0, self._active_native_calls - 1)
            if self._active_native_calls == 0 and getattr(self, "_close_pending", False):
                self._destroy_handle()

    def __enter__(self) -> Self:
        self._require_handle()
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()


class VocalEditSession(_HandleOwner):
    """A control-thread-owned, immutable-source vocal editing session."""

    _destroy_symbol = "sonare_vocal_session_destroy"

    def _destroy_handle(self) -> None:
        # Drafts reference the session, so closing it disposes every live draft first.
        for draft in tuple(self._live_drafts()):
            draft.close()
        super()._destroy_handle()

    def _live_drafts(self) -> weakref.WeakSet[VocalEditDraft]:
        drafts: weakref.WeakSet[VocalEditDraft] | None = getattr(self, "_drafts", None)
        if drafts is None:
            drafts = weakref.WeakSet()
            self._drafts = drafts
        return drafts

    def __init__(
        self,
        samples: Sequence[float] | np.ndarray[Any, Any],
        sample_rate: int,
        *,
        options: VocalCreateOptions | None = None,
        analysis: VocalAnalysis | None = None,
        output_length_samples: int | None = None,
        **option_overrides: Any,
    ) -> None:
        lib = _get_lib()
        if not int(lib.sonare_vocal_available()):
            raise VocalEditError(6, "libsonare was built without vocal-edit support")
        source = _validate_samples("create_vocal_edit_session", samples)
        rate = _vocal_sample_rate(sample_rate)
        if output_length_samples is not None:
            output_length_samples = _int64(output_length_samples, "output_length_samples")
        if options is not None and (
            analysis is not None or output_length_samples is not None or option_overrides
        ):
            raise SonareValueError("options cannot be combined with individual vocal options")
        if options is None:
            options = VocalCreateOptions(
                output_length_samples=output_length_samples,
                analysis=analysis,
                **option_overrides,
            )
        elif analysis is not None or output_length_samples is not None:
            raise SonareValueError("analysis/output_length_samples cannot override options")
        raw_options, keepalive = _options_to_c(lib, options)
        handle = ctypes.c_void_p()
        _vocal_check(
            lib.sonare_vocal_session_create(
                source.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                _int64(source.size, "frames"),
                1,
                _to_c_int(rate, "sample_rate"),
                ctypes.byref(raw_options),
                ctypes.byref(handle),
            )
        )
        self._handle = handle
        self._source_length = int(source.size)
        self._keepalive: tuple[Any, ...] = (source, raw_options, keepalive)
        try:
            self._output_length = _native_output_length(
                lib, "sonare_vocal_session_output_length", handle
            )
        except Exception:
            self.close()
            raise

    @classmethod
    def restore(
        cls,
        samples: Sequence[float] | np.ndarray[Any, Any],
        sample_rate: int,
        state: bytes | bytearray | memoryview,
        *,
        max_history_bytes: int | None = None,
        max_cache_bytes: int | None = None,
        max_undo_depth: int | None = None,
        max_render_jobs: int | None = None,
    ) -> VocalEditSession:
        """Restore a session; the limits default to those of session creation."""
        obj = cls.__new__(cls)
        lib = _get_lib()
        source = _validate_samples("restore_vocal_edit_session", samples)
        rate = _vocal_sample_rate(sample_rate)
        restore_options = _restore_options_to_c(
            lib, max_history_bytes, max_cache_bytes, max_undo_depth, max_render_jobs
        )
        state_bytes = bytes(state)
        if not state_bytes:
            raise SonareValueError("state must not be empty")
        payload = (ctypes.c_uint8 * len(state_bytes)).from_buffer_copy(state_bytes)
        handle = ctypes.c_void_p()
        _vocal_check(
            lib.sonare_vocal_session_restore(
                source.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                _int64(source.size, "frames"),
                1,
                _to_c_int(rate, "sample_rate"),
                ctypes.cast(payload, ctypes.POINTER(ctypes.c_uint8)),
                len(state_bytes),
                ctypes.byref(restore_options),
                ctypes.byref(handle),
            )
        )
        obj._handle = handle
        obj._source_length = int(source.size)
        obj._keepalive = (source, payload)
        try:
            obj._output_length = _native_output_length(
                lib, "sonare_vocal_session_output_length", handle
            )
        except Exception:
            obj.close()
            raise
        return obj

    def notes(self) -> tuple[tuple[VocalNote, ...], tuple[VocalTransition, ...]]:
        lib = _get_lib()
        raw = _new_result(lib, "sonare_vocal_notes_result_init", SonareVocalNotesResult)
        try:
            _vocal_check(lib.sonare_vocal_session_notes(self._require_handle(), ctypes.byref(raw)))
            return _notes_from_result(raw)
        finally:
            lib.sonare_vocal_free_notes(ctypes.byref(raw))

    def analysis(self) -> VocalAnalysisResult:
        lib = _get_lib()
        raw = _new_result(lib, "sonare_vocal_analysis_result_init", SonareVocalAnalysisResult)
        try:
            _vocal_check(
                lib.sonare_vocal_session_analysis(self._require_handle(), ctypes.byref(raw))
            )
            return _analysis_from_result(raw)
        finally:
            lib.sonare_vocal_free_analysis(ctypes.byref(raw))

    def capabilities(self) -> VocalCapabilities:
        lib = _get_lib()
        raw = _new_result(lib, "sonare_vocal_capabilities_init", SonareVocalCapabilities)
        _vocal_check(
            lib.sonare_vocal_session_capabilities(self._require_handle(), ctypes.byref(raw))
        )
        return VocalCapabilities(
            api_version=int(raw.api_version),
            profile_id=int(raw.profile_id),
            monophonic_only=bool(raw.monophonic_only),
            analysis_cancellable=bool(raw.analysis_cancellable),
            minimum_formant_shift_semitones=float(raw.minimum_formant_shift_semitones),
            maximum_formant_shift_semitones=float(raw.maximum_formant_shift_semitones),
        )

    def token(self) -> VocalStateToken:
        lib = _get_lib()
        raw = SonareVocalStateToken()
        _vocal_check(lib.sonare_vocal_session_token(self._require_handle(), ctypes.byref(raw)))
        return _state_token(raw)

    @property
    def revision(self) -> int:
        return self.token().revision

    @property
    def output_length_samples(self) -> int:
        return self._output_length

    def history(self) -> tuple[bool, bool]:
        lib = _get_lib()
        can_undo = ctypes.c_int()
        can_redo = ctypes.c_int()
        _vocal_check(
            lib.sonare_vocal_session_history(
                self._require_handle(), ctypes.byref(can_undo), ctypes.byref(can_redo)
            )
        )
        return bool(can_undo.value), bool(can_redo.value)

    @property
    def can_undo(self) -> bool:
        return self.history()[0]

    @property
    def can_redo(self) -> bool:
        return self.history()[1]

    def begin_edit(self, expected_revision: int | None = None) -> VocalEditDraft:
        lib = _get_lib()
        revision = (
            self.revision
            if expected_revision is None
            else _uint64(expected_revision, "expected_revision")
        )
        handle = ctypes.c_void_p()
        _vocal_check(
            lib.sonare_vocal_session_begin_edit(
                self._require_handle(), int(revision), ctypes.byref(handle)
            )
        )
        draft = VocalEditDraft(self, handle)
        self._live_drafts().add(draft)
        return draft

    def undo(self, expected_revision: int | None = None) -> VocalEditResult:
        return self._history_call("sonare_vocal_session_undo", expected_revision)

    def redo(self, expected_revision: int | None = None) -> VocalEditResult:
        return self._history_call("sonare_vocal_session_redo", expected_revision)

    def _history_call(self, symbol: str, expected_revision: int | None) -> VocalEditResult:
        lib = _get_lib()
        revision = (
            self.revision
            if expected_revision is None
            else _uint64(expected_revision, "expected_revision")
        )
        raw = _new_result(lib, "sonare_vocal_edit_result_init", SonareVocalEditResult)
        try:
            _vocal_check(
                getattr(lib, symbol)(self._require_handle(), int(revision), ctypes.byref(raw))
            )
            return _edit_result_from_c(raw)
        finally:
            lib.sonare_vocal_free_edit_result(ctypes.byref(raw))

    def evaluate_pitch(self, note_id: int) -> VocalPitchResult:
        return _evaluate_pitch(
            _get_lib(), "sonare_vocal_session_evaluate_pitch", self._require_handle(), note_id
        )

    def source_sample_to_destination_sample(self, note_id: int, source_sample: float) -> float:
        return _coordinate(
            _get_lib(),
            "sonare_vocal_session_source_to_destination",
            self._require_handle(),
            note_id,
            source_sample,
        )

    def destination_sample_to_source_sample(self, note_id: int, destination_sample: float) -> float:
        return _coordinate(
            _get_lib(),
            "sonare_vocal_session_destination_to_source",
            self._require_handle(),
            note_id,
            destination_sample,
        )

    def capture_render_snapshot(self) -> VocalRenderSnapshot:
        lib = _get_lib()
        handle = ctypes.c_void_p()
        _vocal_check(
            lib.sonare_vocal_session_capture_snapshot(self._require_handle(), ctypes.byref(handle))
        )
        return VocalRenderSnapshot(handle)

    def export_state(self) -> bytes:
        lib = _get_lib()
        raw = _new_result(lib, "sonare_vocal_state_bytes_init", SonareVocalStateBytes)
        try:
            _vocal_check(
                lib.sonare_vocal_session_export_state(self._require_handle(), ctypes.byref(raw))
            )
            return bytes(_copy_pointer(raw.data, int(raw.size), np.dtype(np.uint8)))
        finally:
            lib.sonare_vocal_free_state_bytes(ctypes.byref(raw))

    def render(
        self,
        range: VocalRange | Sequence[int] | None = None,
        *,
        request_id: int = 0,
        cancel: Callable[[], bool] | None = None,
    ) -> VocalRenderResult:
        request_id = _uint64(request_id, "request_id")
        _range(range, output_length=self.output_length_samples)
        with self._native_call(), self.capture_render_snapshot() as snapshot:
            return snapshot.render(range, request_id=request_id, cancel=cancel)


class VocalEditDraft(_HandleOwner):
    _destroy_symbol = "sonare_vocal_draft_destroy"

    def __init__(self, session: VocalEditSession, handle: ctypes.c_void_p) -> None:
        self._session = session
        self._handle = handle
        self._output_length = session.output_length_samples

    def notes(self) -> tuple[tuple[VocalNote, ...], tuple[VocalTransition, ...]]:
        lib = _get_lib()
        raw = _new_result(lib, "sonare_vocal_notes_result_init", SonareVocalNotesResult)
        try:
            _vocal_check(lib.sonare_vocal_draft_notes(self._require_handle(), ctypes.byref(raw)))
            return _notes_from_result(raw)
        finally:
            lib.sonare_vocal_free_notes(ctypes.byref(raw))

    def token(self) -> VocalStateToken:
        lib = _get_lib()
        raw = SonareVocalStateToken()
        _vocal_check(lib.sonare_vocal_draft_token(self._require_handle(), ctypes.byref(raw)))
        return _state_token(raw)

    def apply(
        self,
        operations: Iterable[VocalOperation],
        expected_generation: int,
    ) -> VocalEditResult:
        lib = _get_lib()
        ops = tuple(operations)
        generation = _uint64(expected_generation, "expected_generation")
        raw_ops = (SonareVocalOperation * len(ops))()
        keepalive: list[Any] = []
        for i, operation in enumerate(ops):
            raw_ops[i], buffers = _operation_to_c(lib, operation)
            keepalive.extend(buffers)
        raw = _new_result(lib, "sonare_vocal_edit_result_init", SonareVocalEditResult)
        try:
            pointer = ctypes.cast(raw_ops, ctypes.POINTER(SonareVocalOperation)) if ops else None
            _vocal_check(
                lib.sonare_vocal_draft_apply(
                    self._require_handle(), int(generation), pointer, len(ops), ctypes.byref(raw)
                )
            )
            return _edit_result_from_c(raw)
        finally:
            lib.sonare_vocal_free_edit_result(ctypes.byref(raw))

    def commit(self, expected_revision: int | None = None) -> VocalEditResult:
        lib = _get_lib()
        revision = (
            self._session.revision
            if expected_revision is None
            else _uint64(expected_revision, "expected_revision")
        )
        raw = _new_result(lib, "sonare_vocal_edit_result_init", SonareVocalEditResult)
        try:
            _vocal_check(
                lib.sonare_vocal_draft_commit(
                    self._require_handle(), int(revision), ctypes.byref(raw)
                )
            )
            return _edit_result_from_c(raw)
        finally:
            lib.sonare_vocal_free_edit_result(ctypes.byref(raw))

    def cancel(self) -> None:
        lib = _get_lib()
        _vocal_check(lib.sonare_vocal_draft_cancel(self._require_handle()))

    def evaluate_pitch(self, note_id: int) -> VocalPitchResult:
        return _evaluate_pitch(
            _get_lib(), "sonare_vocal_draft_evaluate_pitch", self._require_handle(), note_id
        )

    def source_sample_to_destination_sample(self, note_id: int, source_sample: float) -> float:
        return _coordinate(
            _get_lib(),
            "sonare_vocal_draft_source_to_destination",
            self._require_handle(),
            note_id,
            source_sample,
        )

    def destination_sample_to_source_sample(self, note_id: int, destination_sample: float) -> float:
        return _coordinate(
            _get_lib(),
            "sonare_vocal_draft_destination_to_source",
            self._require_handle(),
            note_id,
            destination_sample,
        )

    def capture_render_snapshot(self) -> VocalRenderSnapshot:
        lib = _get_lib()
        handle = ctypes.c_void_p()
        _vocal_check(
            lib.sonare_vocal_draft_capture_snapshot(self._require_handle(), ctypes.byref(handle))
        )
        return VocalRenderSnapshot(handle)


class VocalRenderSnapshot(_HandleOwner):
    _destroy_symbol = "sonare_vocal_snapshot_destroy"

    def __init__(self, handle: ctypes.c_void_p) -> None:
        self._handle = handle
        try:
            self._output_length = _native_output_length(
                _get_lib(), "sonare_vocal_snapshot_output_length", handle
            )
        except Exception:
            self.close()
            raise

    @property
    def output_length_samples(self) -> int:
        return self._output_length

    def render(
        self,
        range: VocalRange | Sequence[int] | None = None,
        *,
        request_id: int = 0,
        cancel: Callable[[], bool] | None = None,
    ) -> VocalRenderResult:
        with self._native_call():
            lib = _get_lib()
            raw = _new_result(lib, "sonare_vocal_render_result_init", SonareVocalRenderResult)
            callback, errors = _callback(cancel)
            try:
                _probe_check(
                    errors,
                    lib.sonare_vocal_snapshot_render(
                        self._require_handle(),
                        _range(range, output_length=self._output_length),
                        _uint64(request_id, "request_id"),
                        callback,
                        None,
                        ctypes.byref(raw),
                    ),
                )
                return _render_result_from_c(raw)
            finally:
                lib.sonare_vocal_free_render_result(ctypes.byref(raw))

    def begin_render_job(
        self,
        range: VocalRange | Sequence[int] | None = None,
        *,
        request_id: int = 0,
    ) -> VocalRenderJob:
        lib = _get_lib()
        handle = ctypes.c_void_p()
        _vocal_check(
            lib.sonare_vocal_render_job_begin(
                self._require_handle(),
                _range(range, output_length=self._output_length),
                _uint64(request_id, "request_id"),
                ctypes.byref(handle),
            )
        )
        return VocalRenderJob(self, handle)


class VocalRenderJob(_HandleOwner):
    _destroy_symbol = "sonare_vocal_render_job_destroy"

    def __init__(self, snapshot: VocalRenderSnapshot, handle: ctypes.c_void_p) -> None:
        self._snapshot = snapshot
        self._handle = handle

    def next(self, cancel: Callable[[], bool] | None = None) -> bool:
        with self._native_call(), self._snapshot._native_call():
            lib = _get_lib()
            complete = ctypes.c_int(0)
            callback, errors = _callback(cancel)
            _probe_check(
                errors,
                lib.sonare_vocal_render_job_next(
                    self._require_handle(), callback, None, ctypes.byref(complete)
                ),
            )
            return bool(complete.value)

    def finalize(self, cancel: Callable[[], bool] | None = None) -> VocalRenderResult:
        native_succeeded = False
        result: VocalRenderResult | None = None
        try:
            with self._native_call(), self._snapshot._native_call():
                lib = _get_lib()
                raw = _new_result(lib, "sonare_vocal_render_result_init", SonareVocalRenderResult)
                callback, errors = _callback(cancel)
                try:
                    _probe_check(
                        errors,
                        lib.sonare_vocal_render_job_finalize(
                            self._require_handle(), callback, None, ctypes.byref(raw)
                        ),
                    )
                    native_succeeded = True
                    result = _render_result_from_c(raw)
                finally:
                    lib.sonare_vocal_free_render_result(ctypes.byref(raw))
        finally:
            if native_succeeded:
                self.close()
        if result is None:
            raise RuntimeError("native vocal render finalize produced no result")
        return result

    def abort(self) -> None:
        if self._handle and self._handle.value:
            _get_lib().sonare_vocal_render_job_abort(self._handle)


def _evaluate_pitch(
    lib: ctypes.CDLL, symbol: str, handle: ctypes.c_void_p, note_id: int
) -> VocalPitchResult:
    raw = _new_result(lib, "sonare_vocal_pitch_result_init", SonareVocalPitchResult)
    try:
        _vocal_check(
            getattr(lib, symbol)(
                handle, _uint32(note_id, "note_id", allow_zero=False), ctypes.byref(raw)
            )
        )
        return _pitch_result_from_c(raw)
    finally:
        lib.sonare_vocal_free_pitch_result(ctypes.byref(raw))


def _coordinate(
    lib: ctypes.CDLL, symbol: str, handle: ctypes.c_void_p, note_id: int, sample: float
) -> float:
    out = ctypes.c_double()
    _vocal_check(
        getattr(lib, symbol)(
            handle,
            _uint32(note_id, "note_id", allow_zero=False),
            _finite_float(sample, "sample"),
            ctypes.byref(out),
        )
    )
    return float(out.value)


def vocal_edit_available() -> bool:
    """Return whether the loaded native library includes vocal-edit support."""
    return bool(_get_lib().sonare_vocal_available())


def vocal_edit_api_version() -> int:
    """Return the vocal-edit C API version reported by the loaded native library."""
    return int(_get_lib().sonare_vocal_edit_api_version())


def create_vocal_edit_session(
    samples: Sequence[float] | np.ndarray[Any, Any],
    sample_rate: int,
    *,
    options: VocalCreateOptions | None = None,
    analysis: VocalAnalysis | None = None,
    output_length_samples: int | None = None,
    **option_overrides: Any,
) -> VocalEditSession:
    """Create a copied-source vocal-edit session."""
    return VocalEditSession(
        samples,
        _vocal_sample_rate(sample_rate),
        options=options,
        analysis=analysis,
        output_length_samples=(
            None
            if output_length_samples is None
            else _int64(output_length_samples, "output_length_samples")
        ),
        **option_overrides,
    )


def restore_vocal_edit_session(
    samples: Sequence[float] | np.ndarray[Any, Any],
    sample_rate: int,
    state: bytes | bytearray | memoryview,
    *,
    max_history_bytes: int | None = None,
    max_cache_bytes: int | None = None,
    max_undo_depth: int | None = None,
    max_render_jobs: int | None = None,
) -> VocalEditSession:
    """Restore an SVE1 state after strict source matching in the native core."""
    return VocalEditSession.restore(
        samples,
        _vocal_sample_rate(sample_rate),
        state,
        max_history_bytes=(
            None if max_history_bytes is None else _uint64(max_history_bytes, "max_history_bytes")
        ),
        max_cache_bytes=(
            None if max_cache_bytes is None else _uint64(max_cache_bytes, "max_cache_bytes")
        ),
        max_undo_depth=(
            None if max_undo_depth is None else _uint32(max_undo_depth, "max_undo_depth")
        ),
        max_render_jobs=(
            None if max_render_jobs is None else _uint32(max_render_jobs, "max_render_jobs")
        ),
    )


__all__ = [
    "VocalTargetMode",
    "VocalFormantMode",
    "VocalOperationKind",
    "VocalMergePolicy",
    "VocalRange",
    "VocalPitchPoint",
    "VocalNoteEdit",
    "VocalNote",
    "VocalTransition",
    "VocalStateToken",
    "VocalAnalysis",
    "VocalAnalysisResult",
    "VocalCapabilities",
    "VocalIdChange",
    "VocalEditResult",
    "VocalPitchResult",
    "VocalRenderResult",
    "VocalCreateOptions",
    "VocalSetNoteEdit",
    "VocalSetNoteSourceSpan",
    "VocalSplitNote",
    "VocalMergeNotes",
    "VocalSetTransition",
    "VocalRemoveTransition",
    "VocalResetNotes",
    "VocalOperation",
    "VocalEditError",
    "VocalEditSession",
    "VocalEditDraft",
    "VocalRenderSnapshot",
    "VocalRenderJob",
    "create_vocal_edit_session",
    "restore_vocal_edit_session",
    "vocal_edit_api_version",
    "vocal_edit_available",
]
