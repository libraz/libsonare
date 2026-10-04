"""Project integration helpers for committed vocal edits."""

from __future__ import annotations

import ctypes
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from enum import IntEnum
from typing import TYPE_CHECKING, Any

import numpy as np
from numpy.typing import NDArray

from ._ffi_types_vocal import (
    VOCAL_MAX_SAMPLE_RATE,
    VOCAL_MIN_SAMPLE_RATE,
    SonareVocalCancelCallback,
    SonareVocalStateToken,
)
from ._ffi_types_vocal_project import (
    SONARE_VOCAL_PROJECT_API_VERSION,
    SONARE_VOCAL_REHYDRATE_ALREADY_READY,
    SONARE_VOCAL_REHYDRATE_REHYDRATED,
    SONARE_VOCAL_REHYDRATE_UNRESOLVED,
    SonareProjectVocalEditApplyDesc,
    SonareProjectVocalEditApplyResult,
    SonareProjectVocalEditDependenciesResult,
    SonareProjectVocalEditDependency,
    SonareProjectVocalOriginalSource,
    SonareProjectVocalRehydrateItem,
    SonareProjectVocalRehydrateResult,
)
from ._narrowing import _narrow_double, _narrow_int
from ._runtime import (
    _INT64_MAX,
    _INT64_MIN,
    _UINT32_MAX,
    SonareValueError,
    _check,
    _get_lib,
    _validate_samples,
)
from .vocal_edit import VocalStateToken

if TYPE_CHECKING:
    from ._project import Project


_UINT64_MAX = (1 << 64) - 1
_MAX_RESULT_ITEMS = 1 << 28
_MAX_U32_ID = _UINT32_MAX - 1
FloatArray = NDArray[np.float32]


class ProjectVocalRehydrateStatus(IntEnum):
    REHYDRATED = SONARE_VOCAL_REHYDRATE_REHYDRATED
    ALREADY_READY = SONARE_VOCAL_REHYDRATE_ALREADY_READY
    UNRESOLVED = SONARE_VOCAL_REHYDRATE_UNRESOLVED


class ProjectVocalReason(IntEnum):
    NONE = 0
    INVALID_INPUT = 1
    REVISION_CONFLICT = 2
    SOURCE_MISMATCH = 3
    UNSUPPORTED = 4
    CANCELLED = 5
    COUNTER_EXHAUSTED = 6
    INVALID_STATE = 7


@dataclass(frozen=True, slots=True)
class ProjectVocalOriginalSource:
    """Original mono PCM supplied to project rehydration."""

    source_id: int
    mono: Sequence[float] | np.ndarray[Any, Any]
    sample_rate: int


@dataclass(frozen=True, slots=True)
class ProjectVocalEditApplyResult:
    clip_id: int
    take_id: int
    original_source_id: int
    derived_source_id: int
    committed_revision: int
    profile_id: int
    derived_source_sha256: bytes
    sidecar_key: str


@dataclass(frozen=True, slots=True)
class ProjectVocalEditDependency:
    clip_id: int
    take_id: int
    original_source_id: int
    derived_source_id: int
    source_sample_rate: int
    profile_id: int
    source_sample_count: int
    committed_revision: int
    original_source_sha256: bytes
    derived_source_sha256: bytes
    original_pcm_available: bool
    derived_pcm_available: bool
    reason: ProjectVocalReason | int
    sidecar_key: str


@dataclass(frozen=True, slots=True)
class ProjectVocalRehydrateItem:
    clip_id: int
    take_id: int
    derived_source_id: int
    status: ProjectVocalRehydrateStatus | int
    reason: ProjectVocalReason | int


def _enum_or_int(enum_type: type[IntEnum], value: int) -> IntEnum | int:
    try:
        return enum_type(value)
    except ValueError:
        return value


def _fixed_text(value: bytes | bytearray) -> str:
    return bytes(value).split(b"\0", 1)[0].decode("utf-8", errors="replace")


def _digest(value: str, name: str) -> bytes:
    if not isinstance(value, str):
        raise TypeError(f"{name} must be a 64-character SHA-256 hexadecimal string")
    if len(value) != 64:
        raise SonareValueError(f"{name} must be a 64-character SHA-256 hexadecimal string")
    try:
        return bytes.fromhex(value)
    except ValueError as exc:
        raise SonareValueError(f"{name} must be a 64-character SHA-256 hexadecimal string") from exc


def _write_digest(destination: Any, digest: bytes) -> None:
    for index, byte in enumerate(digest):
        destination[index] = byte


def _byte_buffer(value: bytes | bytearray | memoryview, name: str) -> bytes:
    if not isinstance(value, (bytes, bytearray, memoryview)):
        raise TypeError(f"{name} must be bytes-like")
    try:
        return bytes(value)
    except (BufferError, TypeError, ValueError) as exc:
        raise TypeError(f"{name} must be bytes-like") from exc


def _project_handle(project: Project) -> ctypes.c_void_p:
    return project._require_handle()


def _id(value: object, name: str, *, allow_zero: bool = False) -> int:
    return _narrow_int(value, name, 0 if allow_zero else 1, _MAX_U32_ID)


def _token_to_c(token: VocalStateToken) -> SonareVocalStateToken:
    if not isinstance(token, VocalStateToken):
        raise TypeError("render_token must be a VocalStateToken")
    return SonareVocalStateToken(
        session_epoch=_narrow_int(
            token.session_epoch, "render_token.session_epoch", 0, _UINT64_MAX
        ),
        revision=_narrow_int(token.revision, "render_token.revision", 0, _UINT64_MAX),
        draft_id=_narrow_int(token.draft_id, "render_token.draft_id", 0, _UINT64_MAX),
        generation=_narrow_int(token.generation, "render_token.generation", 0, _UINT64_MAX),
        request_id=_narrow_int(token.request_id, "render_token.request_id", 0, _UINT64_MAX),
        profile_id=_narrow_int(token.profile_id, "render_token.profile_id", 0, _UINT32_MAX),
    )


def _apply_result(raw: SonareProjectVocalEditApplyResult) -> ProjectVocalEditApplyResult:
    return ProjectVocalEditApplyResult(
        clip_id=int(raw.clip_id),
        take_id=int(raw.take_id),
        original_source_id=int(raw.original_source_id),
        derived_source_id=int(raw.derived_source_id),
        committed_revision=int(raw.committed_revision),
        profile_id=int(raw.profile_id),
        derived_source_sha256=bytes(raw.derived_source_sha256),
        sidecar_key=_fixed_text(raw.sidecar_key),
    )


def _dependency(raw: SonareProjectVocalEditDependency) -> ProjectVocalEditDependency:
    return ProjectVocalEditDependency(
        clip_id=int(raw.clip_id),
        take_id=int(raw.take_id),
        original_source_id=int(raw.original_source_id),
        derived_source_id=int(raw.derived_source_id),
        source_sample_rate=int(raw.source_sample_rate),
        profile_id=int(raw.profile_id),
        source_sample_count=int(raw.source_sample_count),
        committed_revision=int(raw.committed_revision),
        original_source_sha256=bytes(raw.original_source_sha256),
        derived_source_sha256=bytes(raw.derived_source_sha256),
        original_pcm_available=bool(raw.original_pcm_available),
        derived_pcm_available=bool(raw.derived_pcm_available),
        reason=_enum_or_int(ProjectVocalReason, int(raw.reason)),
        sidecar_key=_fixed_text(raw.sidecar_key),
    )


def _rehydrate_item(raw: SonareProjectVocalRehydrateItem) -> ProjectVocalRehydrateItem:
    return ProjectVocalRehydrateItem(
        clip_id=int(raw.clip_id),
        take_id=int(raw.take_id),
        derived_source_id=int(raw.derived_source_id),
        status=_enum_or_int(ProjectVocalRehydrateStatus, int(raw.status)),
        reason=_enum_or_int(ProjectVocalReason, int(raw.reason)),
    )


def _result_count(pointer: Any, count_value: int, name: str) -> int:
    count = int(count_value)
    if count < 0 or count > _MAX_RESULT_ITEMS:
        raise RuntimeError(f"native vocal project {name} count is unreasonable: {count}")
    if count and not pointer:
        raise RuntimeError(f"native vocal project {name} result contains a null array")
    return count


def apply_project_vocal_edit(
    project: Project,
    *,
    clip_id: int,
    expected_source_id: int,
    expected_source_sample_rate: int,
    expected_source_sample_count: int,
    expected_source_sha256: str,
    expected_clip_length_ppq: float,
    expected_source_offset_ppq: float,
    rendered_mono: Sequence[float] | np.ndarray[Any, Any],
    rendered_sample_rate: int,
    render_token: VocalStateToken,
    sve1: bytes | bytearray | memoryview,
    take_id: int = 0,
    rendered_start_sample: int = 0,
) -> ProjectVocalEditApplyResult:
    """Commit a full-source vocal render to one Project clip or take."""

    handle = _project_handle(project)
    source = _validate_samples("apply_project_vocal_edit", rendered_mono, arg_name="rendered_mono")
    expected_count = _narrow_int(
        expected_source_sample_count,
        "expected_source_sample_count",
        1,
        _INT64_MAX,
    )
    if source.size != expected_count:
        raise SonareValueError(
            "rendered_mono length must equal expected_source_sample_count "
            f"({source.size} != {expected_count})"
        )
    expected_rate = _narrow_int(
        expected_source_sample_rate,
        "expected_source_sample_rate",
        VOCAL_MIN_SAMPLE_RATE,
        VOCAL_MAX_SAMPLE_RATE,
    )
    rendered_rate = _narrow_int(
        rendered_sample_rate,
        "rendered_sample_rate",
        VOCAL_MIN_SAMPLE_RATE,
        VOCAL_MAX_SAMPLE_RATE,
    )
    if rendered_rate != expected_rate:
        raise SonareValueError(
            "rendered_sample_rate must equal expected_source_sample_rate "
            f"({rendered_rate} != {expected_rate})"
        )
    start = _narrow_int(rendered_start_sample, "rendered_start_sample", _INT64_MIN, _INT64_MAX)
    if start != 0:
        raise SonareValueError("rendered_start_sample must be zero for the v1 Project vocal API")
    sve1_bytes = _byte_buffer(sve1, "sve1")
    if not sve1_bytes:
        raise SonareValueError("sve1 must not be empty")
    _narrow_int(len(sve1_bytes), "sve1_size", 1, _UINT64_MAX)
    token = _token_to_c(render_token)
    if token.draft_id != 0 or token.generation != 0:
        raise SonareValueError("render_token must be a committed session token")
    desc = SonareProjectVocalEditApplyDesc()
    lib = _get_lib()
    lib.sonare_project_vocal_edit_apply_desc_init(ctypes.byref(desc))
    desc.clip_id = _id(clip_id, "clip_id")
    desc.take_id = _id(take_id, "take_id", allow_zero=True)
    desc.expected_source_id = _id(expected_source_id, "expected_source_id")
    desc.expected_source_sample_rate = expected_rate
    desc.expected_source_sample_count = expected_count
    _write_digest(
        desc.expected_source_sha256, _digest(expected_source_sha256, "expected_source_sha256")
    )
    desc.expected_clip_length_ppq = _narrow_double(
        expected_clip_length_ppq, "expected_clip_length_ppq"
    )
    desc.expected_source_offset_ppq = _narrow_double(
        expected_source_offset_ppq, "expected_source_offset_ppq"
    )
    desc.rendered_mono = source.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    desc.rendered_sample_count = int(source.size)
    desc.rendered_sample_rate = rendered_rate
    desc.rendered_start_sample = start
    desc.render_token = token
    sve1_buffer = (ctypes.c_uint8 * len(sve1_bytes)).from_buffer_copy(sve1_bytes)
    desc.sve1 = ctypes.cast(sve1_buffer, ctypes.POINTER(ctypes.c_uint8))
    desc.sve1_size = len(sve1_bytes)
    result = SonareProjectVocalEditApplyResult()
    lib.sonare_project_vocal_edit_apply_result_init(ctypes.byref(result))
    _check(lib.sonare_project_apply_vocal_edit(handle, ctypes.byref(desc), ctypes.byref(result)))
    return _apply_result(result)


def get_project_vocal_edit_dependencies(
    project: Project,
) -> tuple[ProjectVocalEditDependency, ...]:
    """Return copied vocal sidecar dependencies in native storage order."""

    handle = _project_handle(project)
    lib = _get_lib()
    result = SonareProjectVocalEditDependenciesResult()
    lib.sonare_project_vocal_edit_dependencies_result_init(ctypes.byref(result))
    try:
        _check(lib.sonare_project_get_vocal_edit_dependencies(handle, ctypes.byref(result)))
        count = _result_count(result.dependencies, result.dependency_count, "dependency")
        return tuple(_dependency(result.dependencies[index]) for index in range(count))
    finally:
        lib.sonare_project_free_vocal_edit_dependencies(ctypes.byref(result))


def _original_sources_to_c(
    originals: Sequence[ProjectVocalOriginalSource],
) -> tuple[ctypes.Array[SonareProjectVocalOriginalSource] | None, list[np.ndarray]]:
    if not originals:
        return None, []
    rows = (SonareProjectVocalOriginalSource * len(originals))()
    buffers: list[np.ndarray] = []
    seen: set[int] = set()
    for index, original in enumerate(originals):
        source_id = _id(original.source_id, f"original_sources[{index}].source_id")
        if source_id in seen:
            raise SonareValueError(f"original_sources contains duplicate source_id {source_id}")
        seen.add(source_id)
        sample_rate = _narrow_int(
            original.sample_rate,
            f"original_sources[{index}].sample_rate",
            VOCAL_MIN_SAMPLE_RATE,
            VOCAL_MAX_SAMPLE_RATE,
        )
        buffer = _validate_samples(
            "rehydrate_project_vocal_edits",
            original.mono,
            arg_name=f"original_sources[{index}].mono",
        )
        buffers.append(buffer)
        rows[index].struct_size = ctypes.sizeof(SonareProjectVocalOriginalSource)
        rows[index].schema_version = SONARE_VOCAL_PROJECT_API_VERSION
        rows[index].source_id = source_id
        rows[index].mono = buffer.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        rows[index].sample_count = int(buffer.size)
        rows[index].sample_rate = sample_rate
    return rows, buffers


def _project_cancel_callback(
    cancel: Callable[[], bool] | None,
) -> tuple[Any, list[BaseException]]:
    """Create a call-scoped callback and retain probe failures for Python.

    A ctypes callback cannot safely let a Python exception escape into C. It
    therefore records the exception, requests cancellation, and lets the
    caller re-raise it after the native call has unwound.
    """
    errors: list[BaseException] = []
    if cancel is not None and not callable(cancel):
        raise TypeError("cancel must be callable or None")

    def probe(_: ctypes.c_void_p) -> int:
        if errors:
            return 1
        if cancel is None:
            return 0
        try:
            return int(bool(cancel()))
        except BaseException as exc:
            errors.append(exc)
            return 1

    callback = SonareVocalCancelCallback(probe)
    return callback, errors


def rehydrate_project_vocal_edits(
    project: Project,
    original_sources: Sequence[ProjectVocalOriginalSource],
    *,
    cancel: Callable[[], bool] | None = None,
) -> tuple[ProjectVocalRehydrateItem, ...]:
    """Restore unresolved vocal PCM without creating an edit-history entry.

    Any truthy return from ``cancel`` cancels the call.
    """

    originals = tuple(original_sources)
    original_count = _narrow_int(
        len(originals), "original_count", 0, min(_UINT64_MAX, _MAX_RESULT_ITEMS)
    )
    rows, buffers = _original_sources_to_c(originals)
    callback, callback_errors = _project_cancel_callback(cancel)
    result = SonareProjectVocalRehydrateResult()
    lib = _get_lib()
    handle = project._enter_native_call()
    try:
        lib.sonare_project_vocal_rehydrate_result_init(ctypes.byref(result))
        error = lib.sonare_project_rehydrate_vocal_edits(
            handle,
            rows,
            original_count,
            callback,
            None,
            ctypes.byref(result),
        )
        if callback_errors:
            raise callback_errors[0]
        _check(error)
        count = _result_count(result.items, result.item_count, "rehydrate")
        return tuple(_rehydrate_item(result.items[index]) for index in range(count))
    finally:
        lib.sonare_project_free_vocal_rehydrate_result(ctypes.byref(result))
        project._exit_native_call()
        del buffers


__all__ = [
    "ProjectVocalRehydrateStatus",
    "ProjectVocalReason",
    "ProjectVocalOriginalSource",
    "ProjectVocalEditApplyResult",
    "ProjectVocalEditDependency",
    "ProjectVocalRehydrateItem",
    "apply_project_vocal_edit",
    "get_project_vocal_edit_dependencies",
    "rehydrate_project_vocal_edits",
]
