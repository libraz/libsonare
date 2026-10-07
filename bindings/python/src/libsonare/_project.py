# ruff: noqa: F405
"""Stable public facade for the headless arrangement project API."""

from __future__ import annotations

import contextlib
import ctypes
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING, Any, Self

from ._errors import _invalid_state
from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._project_edit import TakeAlignment as TakeAlignment
from ._project_edit import _ProjectEditMixin
from ._project_edit import align_take_to_reference as align_take_to_reference
from ._project_inspection import _ProjectInspectionMixin
from ._project_midi import _ProjectMidiMixin
from ._project_model import *  # noqa: F403
from ._project_model import (
    _FADE_CURVE_NAMES as _FADE_CURVE_NAMES,
)
from ._project_model import (
    _LOOP_MODE_NAMES as _LOOP_MODE_NAMES,
)
from ._project_model import (
    _SYNTH_WAVEFORM_NAMES as _SYNTH_WAVEFORM_NAMES,
)
from ._project_model import (
    _TRACK_KIND_NAMES as _TRACK_KIND_NAMES,
)
from ._project_model import (
    _automation_lane_desc as _automation_lane_desc,
)
from ._project_model import (
    _cc_binding_from_c as _cc_binding_from_c,
)
from ._project_model import (
    _cc_binding_to_c as _cc_binding_to_c,
)
from ._project_model import _check_project_abi
from ._project_model import (
    _fade_curve_value as _fade_curve_value,
)
from ._project_model import (
    _loop_mode_value as _loop_mode_value,
)
from ._project_model import (
    _make_instrument_callbacks as _make_instrument_callbacks,
)
from ._project_model import (
    _marker_name_bytes as _marker_name_bytes,
)
from ._project_model import (
    _midi_event_tuple as _midi_event_tuple,
)
from ._project_model import _synth_patch_arg as _synth_patch_arg
from ._project_model import (
    _synth_waveform_value as _synth_waveform_value,
)
from ._project_model import (
    _track_kind_value as _track_kind_value,
)
from ._project_model import (
    _validate_midi_event_ppq as _validate_midi_event_ppq,
)
from ._project_model import (
    _validate_midi_event_word as _validate_midi_event_word,
)
from ._project_render import _compile_result_fields, _ProjectRenderMixin
from ._runtime import SonareProjectCompileResult, SonareValueError, _check, _get_lib, _utf8_arg

if TYPE_CHECKING:
    import numpy as np

    from .vocal_edit import VocalStateToken
    from .vocal_project import (
        ProjectVocalEditApplyResult,
        ProjectVocalEditDependency,
        ProjectVocalOriginalSource,
        ProjectVocalRehydrateItem,
    )


class ProjectTimeline:
    """Immutable compiled playback snapshot of a :class:`Project`.

    Built by :meth:`Project.compile_timeline` and installed into a stopped
    engine with :meth:`RealtimeEngine.apply_project_timeline`. It owns
    everything it references, so later project edits do not change it. The
    engine keeps its own share, so :meth:`close` may run right after the apply.
    """

    def __init__(self, handle: ctypes.c_void_p) -> None:
        # Only Project.compile_timeline constructs this, with a live handle.
        self._handle: ctypes.c_void_p | None = handle

    def close(self) -> None:
        """Release the native timeline handle (idempotent)."""
        if self._handle is not None:
            _get_lib().sonare_project_timeline_destroy(self._handle)
            self._handle = None

    # Cross-binding aliases: Node uses destroy(), WASM uses delete().
    def destroy(self) -> None:
        """Alias of :meth:`close` for cross-binding (Node ``destroy``) parity."""
        self.close()

    def delete(self) -> None:
        """Alias of :meth:`close` for cross-binding (WASM ``delete``) parity."""
        self.close()

    def __enter__(self) -> ProjectTimeline:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise _invalid_state("ProjectTimeline is closed")
        return self._handle


class Project(
    _ProjectEditMixin,
    _ProjectMidiMixin,
    _ProjectInspectionMixin,
    _ProjectRenderMixin,
):
    """Pythonic wrapper around the native headless-project handle.

    All mutation routes through the native ``EditHistory`` (so :meth:`undo` /
    :meth:`redo` work), musical positions are PPQ (quarter notes), and
    serialization is deterministic (``to_json`` is byte-stable for a given
    project state within one build).
    """

    def __init__(self) -> None:
        # Set first so a failed create (e.g. an ABI mismatch) leaves a valid
        # attribute for __del__/close() instead of raising AttributeError.
        self._handle: ctypes.c_void_p | None = None
        lib = _get_lib()
        _check_project_abi(lib)
        handle = ctypes.c_void_p()
        _check(lib.sonare_project_create(ctypes.byref(handle)))
        self._handle = handle

    @classmethod
    def create(cls) -> Self:
        """Create a new empty project."""
        return cls()

    # -- lifecycle ----------------------------------------------------------

    def close(self) -> None:
        """Destroy the native project; deferred to the end of an active native call."""
        if getattr(self, "_active_native_calls", 0):
            self._close_pending = True
            return
        self._destroy_handle()

    def _destroy_handle(self) -> None:
        self._close_pending = False
        if self._handle is not None:
            _get_lib().sonare_project_destroy(self._handle)
            self._handle = None

    # Cross-binding aliases: Node uses destroy(), WASM uses delete().
    def destroy(self) -> None:
        """Alias of :meth:`close` for cross-binding (Node ``destroy``) parity."""
        self.close()

    def delete(self) -> None:
        """Alias of :meth:`close` for cross-binding (WASM ``delete``) parity."""
        self.close()

    def __enter__(self) -> Project:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None or getattr(self, "_close_pending", False):
            raise _invalid_state("Project is closed")
        return self._handle

    def compile_timeline(self) -> ProjectCompileResult:
        """Compile the project and keep the timeline for an engine.

        Returns the :class:`ProjectCompileResult` :meth:`compile` returns, with
        ``timeline`` set to a :class:`ProjectTimeline` when compilation produced
        one and ``None`` otherwise (the error diagnostics say why). Never throws
        on bad project content.
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        timeline = ctypes.c_void_p()
        _check(
            lib.sonare_project_compile_timeline(
                self._require_handle(), ctypes.byref(result), ctypes.byref(timeline)
            )
        )
        try:
            return ProjectCompileResult(
                *_compile_result_fields(result),
                ProjectTimeline(timeline) if timeline.value else None,
            )
        except BaseException:
            lib.sonare_project_timeline_destroy(timeline)
            raise
        finally:
            lib.sonare_project_free_compile_result(ctypes.byref(result))

    def _enter_native_call(self) -> ctypes.c_void_p:
        """Pin the Project handle while a callback-capable native call runs."""
        handle = self._require_handle()
        self._active_native_calls = getattr(self, "_active_native_calls", 0) + 1
        return handle

    def _exit_native_call(self) -> None:
        active = getattr(self, "_active_native_calls", 0)
        self._active_native_calls = max(0, active - 1)
        if self._active_native_calls == 0 and getattr(self, "_close_pending", False):
            self._destroy_handle()

    # -- serialization ------------------------------------------------------

    def to_json_bytes(self) -> bytes:
        """Serialize the project to deterministic JSON as raw UTF-8 bytes."""
        lib = _get_lib()
        out = ctypes.c_char_p()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_serialize(
                self._require_handle(), ctypes.byref(out), ctypes.byref(out_len)
            )
        )
        try:
            if not out.value:
                return b""
            # `out.value` is a fresh Python bytes copy of the C string; keep the
            # original pointer (`out`) for the free call below.
            return ctypes.string_at(out, out_len.value)
        finally:
            if out:
                lib.sonare_free_string(out)

    def to_json(self) -> str:
        """Serialize the project to deterministic JSON (UTF-8 decoded)."""
        return self.to_json_bytes().decode("utf-8", errors="replace")

    @classmethod
    def from_json(cls, json: str | bytes) -> Project:
        """Deserialize project JSON into a new :class:`Project`.

        Raises :class:`SonareValueError` on malformed input (with the joined
        native diagnostic messages), never crashing.
        """
        lib = _get_lib()
        _check_project_abi(lib)
        data = _utf8_arg(json, "json") if isinstance(json, str) else bytes(json)
        handle = ctypes.c_void_p()
        diag = ctypes.c_char_p()
        rc = lib.sonare_project_deserialize(
            data, ctypes.c_size_t(len(data)), ctypes.byref(handle), ctypes.byref(diag)
        )
        if rc != 0:
            try:
                detail = diag.value.decode("utf-8") if diag.value else ""
            finally:
                if diag:
                    lib.sonare_free_string(diag)
            raise SonareValueError(detail or "failed to deserialize project JSON")
        if diag:
            lib.sonare_free_string(diag)
        obj = cls.__new__(cls)
        obj._handle = handle
        return obj

    @classmethod
    def from_json_with_diagnostics(cls, json: str | bytes) -> ProjectDeserializeResult:
        """Deserialize project JSON and return warnings from successful loads."""
        lib = _get_lib()
        _check_project_abi(lib)
        data = _utf8_arg(json, "json") if isinstance(json, str) else bytes(json)
        handle = ctypes.c_void_p()
        diag = ctypes.c_char_p()
        rc = lib.sonare_project_deserialize(
            data, ctypes.c_size_t(len(data)), ctypes.byref(handle), ctypes.byref(diag)
        )
        try:
            diagnostics = diag.value.decode("utf-8") if diag.value else ""
        finally:
            if diag:
                lib.sonare_free_string(diag)
        if rc != 0:
            raise SonareValueError(diagnostics or "failed to deserialize project JSON")
        obj = cls.__new__(cls)
        obj._handle = handle
        return ProjectDeserializeResult(project=obj, diagnostics=diagnostics)

    def apply_vocal_edit(
        self,
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
        from .vocal_project import apply_project_vocal_edit

        return apply_project_vocal_edit(
            self,
            clip_id=clip_id,
            expected_source_id=expected_source_id,
            expected_source_sample_rate=expected_source_sample_rate,
            expected_source_sample_count=expected_source_sample_count,
            expected_source_sha256=expected_source_sha256,
            expected_clip_length_ppq=expected_clip_length_ppq,
            expected_source_offset_ppq=expected_source_offset_ppq,
            rendered_mono=rendered_mono,
            rendered_sample_rate=rendered_sample_rate,
            render_token=render_token,
            sve1=sve1,
            take_id=take_id,
            rendered_start_sample=rendered_start_sample,
        )

    def get_vocal_edit_dependencies(
        self,
    ) -> tuple[ProjectVocalEditDependency, ...]:
        from .vocal_project import get_project_vocal_edit_dependencies

        return get_project_vocal_edit_dependencies(self)

    def rehydrate_vocal_edits(
        self,
        original_sources: Sequence[ProjectVocalOriginalSource],
        *,
        cancel: Callable[[], bool] | None = None,
    ) -> tuple[ProjectVocalRehydrateItem, ...]:
        from .vocal_project import rehydrate_project_vocal_edits

        return rehydrate_project_vocal_edits(
            self,
            original_sources=original_sources,
            cancel=cancel,
        )


_rebind_facade_exports(globals(), "libsonare._project_")
del _rebind_facade_exports
