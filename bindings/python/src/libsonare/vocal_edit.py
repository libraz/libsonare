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
import weakref
from collections.abc import Callable, Iterable, Iterator, Sequence
from typing import Any, Self

import numpy as np

from ._errors import SonareValueError, _invalid_state, _unknown_error
from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._ffi_types_vocal import (
    SonareVocalAnalysisResult,
    SonareVocalCapabilities,
    SonareVocalEditResult,
    SonareVocalNotesResult,
    SonareVocalOperation,
    SonareVocalPitchResult,
    SonareVocalRenderResult,
    SonareVocalStateBytes,
    SonareVocalStateToken,
)
from ._runtime import (
    _get_lib,
    _to_c_int,
    _validate_samples,
)
from ._vocal_edit_marshal import (
    _MAX_RESULT_ITEMS as _MAX_RESULT_ITEMS,
)
from ._vocal_edit_marshal import (
    _RANGE_BOUND_NAMES as _RANGE_BOUND_NAMES,
)
from ._vocal_edit_marshal import (
    _RANGE_SECOND_NAMES as _RANGE_SECOND_NAMES,
)
from ._vocal_edit_marshal import (
    _UINT32_MAX as _UINT32_MAX,
)
from ._vocal_edit_marshal import (
    _UINT64_MAX as _UINT64_MAX,
)
from ._vocal_edit_marshal import (
    VocalEditError as VocalEditError,
)
from ._vocal_edit_marshal import (
    _analysis_from_result as _analysis_from_result,
)
from ._vocal_edit_marshal import (
    _analysis_to_c as _analysis_to_c,
)
from ._vocal_edit_marshal import (
    _bool_flag as _bool_flag,
)
from ._vocal_edit_marshal import (
    _callback as _callback,
)
from ._vocal_edit_marshal import (
    _copy_pointer as _copy_pointer,
)
from ._vocal_edit_marshal import (
    _edit_result_from_c as _edit_result_from_c,
)
from ._vocal_edit_marshal import (
    _edit_to_c as _edit_to_c,
)
from ._vocal_edit_marshal import (
    _enum as _enum,
)
from ._vocal_edit_marshal import (
    _finite_float as _finite_float,
)
from ._vocal_edit_marshal import (
    _int64 as _int64,
)
from ._vocal_edit_marshal import (
    _native_output_length as _native_output_length,
)
from ._vocal_edit_marshal import (
    _new_result as _new_result,
)
from ._vocal_edit_marshal import (
    _note_edit_from_c as _note_edit_from_c,
)
from ._vocal_edit_marshal import (
    _note_from_c as _note_from_c,
)
from ._vocal_edit_marshal import (
    _notes_from_result as _notes_from_result,
)
from ._vocal_edit_marshal import (
    _operation_position as _operation_position,
)
from ._vocal_edit_marshal import (
    _operation_to_c as _operation_to_c,
)
from ._vocal_edit_marshal import (
    _options_to_c as _options_to_c,
)
from ._vocal_edit_marshal import (
    _pitch_result_from_c as _pitch_result_from_c,
)
from ._vocal_edit_marshal import (
    _probe_check as _probe_check,
)
from ._vocal_edit_marshal import (
    _range as _range,
)
from ._vocal_edit_marshal import (
    _render_result_from_c as _render_result_from_c,
)
from ._vocal_edit_marshal import (
    _restore_options_to_c as _restore_options_to_c,
)
from ._vocal_edit_marshal import (
    _state_token as _state_token,
)
from ._vocal_edit_marshal import (
    _text as _text,
)
from ._vocal_edit_marshal import (
    _transition_to_c as _transition_to_c,
)
from ._vocal_edit_marshal import (
    _uint32 as _uint32,
)
from ._vocal_edit_marshal import (
    _uint64 as _uint64,
)
from ._vocal_edit_marshal import (
    _vocal_check as _vocal_check,
)
from ._vocal_edit_marshal import (
    _vocal_sample_rate as _vocal_sample_rate,
)
from ._vocal_edit_model import (
    FloatArray as FloatArray,
)
from ._vocal_edit_model import (
    VocalAnalysis as VocalAnalysis,
)
from ._vocal_edit_model import (
    VocalAnalysisResult as VocalAnalysisResult,
)
from ._vocal_edit_model import (
    VocalCapabilities as VocalCapabilities,
)
from ._vocal_edit_model import (
    VocalCreateOptions as VocalCreateOptions,
)
from ._vocal_edit_model import (
    VocalEditResult as VocalEditResult,
)
from ._vocal_edit_model import (
    VocalFormantMode as VocalFormantMode,
)
from ._vocal_edit_model import (
    VocalIdChange as VocalIdChange,
)
from ._vocal_edit_model import (
    VocalMergeNotes as VocalMergeNotes,
)
from ._vocal_edit_model import (
    VocalMergePolicy as VocalMergePolicy,
)
from ._vocal_edit_model import (
    VocalNote as VocalNote,
)
from ._vocal_edit_model import (
    VocalNoteEdit as VocalNoteEdit,
)
from ._vocal_edit_model import (
    VocalOperation as VocalOperation,
)
from ._vocal_edit_model import (
    VocalOperationKind as VocalOperationKind,
)
from ._vocal_edit_model import (
    VocalPitchPoint as VocalPitchPoint,
)
from ._vocal_edit_model import (
    VocalPitchResult as VocalPitchResult,
)
from ._vocal_edit_model import (
    VocalRange as VocalRange,
)
from ._vocal_edit_model import (
    VocalRemoveTransition as VocalRemoveTransition,
)
from ._vocal_edit_model import (
    VocalRenderResult as VocalRenderResult,
)
from ._vocal_edit_model import (
    VocalResetNotes as VocalResetNotes,
)
from ._vocal_edit_model import (
    VocalSetNoteEdit as VocalSetNoteEdit,
)
from ._vocal_edit_model import (
    VocalSetNoteSourceSpan as VocalSetNoteSourceSpan,
)
from ._vocal_edit_model import (
    VocalSetTransition as VocalSetTransition,
)
from ._vocal_edit_model import (
    VocalSplitNote as VocalSplitNote,
)
from ._vocal_edit_model import (
    VocalStateToken as VocalStateToken,
)
from ._vocal_edit_model import (
    VocalTargetMode as VocalTargetMode,
)
from ._vocal_edit_model import (
    VocalTransition as VocalTransition,
)


class _HandleOwner:
    _handle: ctypes.c_void_p
    _destroy_symbol: str
    _active_native_calls: int
    _close_pending: bool

    def _require_handle(self) -> ctypes.c_void_p:
        if not self._handle or not self._handle.value or getattr(self, "_close_pending", False):
            raise _invalid_state(f"{type(self).__name__} is closed")
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
        self._sample_rate = rate
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
        obj._sample_rate = rate
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
        return VocalRenderSnapshot(handle, self._sample_rate)

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
        range: VocalRange | Sequence[int | None] | None = None,
        *,
        start_sec: float | None = None,
        end_sec: float | None = None,
        request_id: int = 0,
        cancel: Callable[[], bool] | None = None,
    ) -> VocalRenderResult:
        """Render a destination range given in samples (``range``) or seconds.

        ``range`` is a :class:`VocalRange` or a ``(start_sample, end_sample)``
        pair; ``start_sec`` / ``end_sec`` spell the same bounds in seconds,
        rounded to the nearest sample at the session's rate. Give one form, not
        both. An omitted start is 0 and an omitted end is the end of the output.
        """
        request_id = _uint64(request_id, "request_id")
        _range(
            range,
            output_length=self.output_length_samples,
            sample_rate=self._sample_rate,
            start_sec=start_sec,
            end_sec=end_sec,
        )
        with self._native_call(), self.capture_render_snapshot() as snapshot:
            return snapshot.render(
                range, start_sec=start_sec, end_sec=end_sec, request_id=request_id, cancel=cancel
            )


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
            raw_ops[i], buffers = _operation_to_c(lib, operation, self._session._sample_rate)
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
        return VocalRenderSnapshot(handle, self._session._sample_rate)


class VocalRenderSnapshot(_HandleOwner):
    _destroy_symbol = "sonare_vocal_snapshot_destroy"

    def __init__(self, handle: ctypes.c_void_p, sample_rate: int) -> None:
        self._handle = handle
        self._sample_rate = sample_rate
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
        range: VocalRange | Sequence[int | None] | None = None,
        *,
        start_sec: float | None = None,
        end_sec: float | None = None,
        request_id: int = 0,
        cancel: Callable[[], bool] | None = None,
    ) -> VocalRenderResult:
        """Render a destination range in samples or seconds, as :meth:`VocalEditSession.render`."""
        with self._native_call():
            lib = _get_lib()
            raw = _new_result(lib, "sonare_vocal_render_result_init", SonareVocalRenderResult)
            callback, errors = _callback(cancel)
            try:
                _probe_check(
                    errors,
                    lib.sonare_vocal_snapshot_render(
                        self._require_handle(),
                        _range(
                            range,
                            output_length=self._output_length,
                            sample_rate=self._sample_rate,
                            start_sec=start_sec,
                            end_sec=end_sec,
                        ),
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
        range: VocalRange | Sequence[int | None] | None = None,
        *,
        start_sec: float | None = None,
        end_sec: float | None = None,
        request_id: int = 0,
    ) -> VocalRenderJob:
        lib = _get_lib()
        handle = ctypes.c_void_p()
        _vocal_check(
            lib.sonare_vocal_render_job_begin(
                self._require_handle(),
                _range(
                    range,
                    output_length=self._output_length,
                    sample_rate=self._sample_rate,
                    start_sec=start_sec,
                    end_sec=end_sec,
                ),
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
            raise _unknown_error("native vocal render finalize produced no result")
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

_rebind_facade_exports(globals(), "libsonare._vocal_edit_")
del _rebind_facade_exports
