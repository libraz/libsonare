# ruff: noqa: F405
"""Stable public facade for the headless arrangement project API."""

from __future__ import annotations

import contextlib
import ctypes
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING, Any, Self

from ._errors import _entry, _invalid_state, _require_open
from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._project_edit import AlignTakeToReferenceResult as AlignTakeToReferenceResult
from ._project_edit import TakeAlignment as TakeAlignment
from ._project_edit import _ProjectEditMixin
from ._project_edit import align_take_to_reference as align_take_to_reference
from ._project_edit import chord_symbol_from_analysis as chord_symbol_from_analysis
from ._project_edit import key_mode_from_analysis as key_mode_from_analysis
from ._project_inspection import _ProjectInspectionMixin
from ._project_midi import _ProjectMidiMixin
from ._project_model import *  # noqa: F403
from ._project_model import (
    _FADE_CURVES as _FADE_CURVES,
)
from ._project_model import (
    _LOOP_MODES as _LOOP_MODES,
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
    _marker_name_arg as _marker_name_arg,
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
from ._project_render import _compile_diagnostics, _ProjectRenderMixin
from ._runtime import (
    SonareProjectCompileResult,
    _check,
    _coded_error,
    _get_lib,
    _utf8_arg,
)

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

    Built by :meth:`Project.compile` with ``timeline=True`` and installed into a stopped
    engine with :meth:`RealtimeEngine.apply_project_timeline`. It owns
    everything it references, so later project edits do not change it. The
    engine keeps its own share, so :meth:`close` may run right after the apply.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("ProjectTimeline")
    def __init__(self, handle: ctypes.c_void_p) -> None:
        # Only Project.compile constructs this, with a live handle.
        self._handle: ctypes.c_void_p | None = handle

    @_entry("ProjectTimeline.close")
    def close(self) -> None:
        """Release the native timeline handle (idempotent)."""
        if self._handle is not None:
            _get_lib().sonare_project_timeline_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> ProjectTimeline:
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, *_exc: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "ProjectTimeline")


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

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("Project")
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
    @_entry("Project.create")
    def create(cls) -> Self:
        """Create a new empty project."""
        return cls()

    # -- lifecycle ----------------------------------------------------------

    @_entry("Project.close")
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

    def __enter__(self) -> Project:
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, *_exc: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None or getattr(self, "_close_pending", False):
            raise _invalid_state("Project is closed")
        return self._handle

    @_entry("Project.compile")
    def compile(self, *, timeline: bool = False) -> ProjectCompileResult:
        """Compile the project into an RT-readable timeline.

        Returns a :class:`ProjectCompileResult` holding every diagnostic. Never
        throws on bad project content; it surfaces error diagnostics. With
        ``timeline=True`` the result's ``timeline`` is a :class:`ProjectTimeline`
        for an engine when compilation produced one and ``None`` otherwise (the
        error diagnostics say why).
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        handle = ctypes.c_void_p()
        _check(
            lib.sonare_project_compile(
                self._require_handle(),
                ctypes.byref(result),
                ctypes.byref(handle) if timeline else None,
            )
        )
        try:
            return ProjectCompileResult(
                _compile_diagnostics(result),
                ProjectTimeline(handle) if handle.value else None,
            )
        except BaseException:
            if handle.value:
                lib.sonare_project_timeline_destroy(handle)
            raise
        finally:
            lib.sonare_free_project_compile_result(ctypes.byref(result))

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

    @_entry("Project.to_json_bytes")
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

    @_entry("Project.to_json")
    def to_json(self) -> str:
        """Serialize the project to deterministic JSON (UTF-8 decoded)."""
        return self.to_json_bytes().decode("utf-8", errors="replace")

    @classmethod
    @_entry("Project.from_json")
    def from_json(cls, json: str | bytes) -> Project:
        """Deserialize project JSON into a new :class:`Project`.

        Raises a coded :class:`SonareError` (``INVALID_FORMAT`` for a malformed
        document) carrying the joined native diagnostic messages, never crashing.
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
            raise _coded_error(rc, detail or "failed to deserialize project JSON")
        if diag:
            lib.sonare_free_string(diag)
        obj = cls.__new__(cls)
        obj._handle = handle
        return obj

    @classmethod
    @_entry("Project.from_json_with_diagnostics")
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
            raise _coded_error(rc, diagnostics or "failed to deserialize project JSON")
        obj = cls.__new__(cls)
        obj._handle = handle
        return ProjectDeserializeResult(project=obj, diagnostics=diagnostics)

    @_entry("Project.apply_vocal_edit")
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
        take_id: int | None = None,
        rendered_start_sample: int = 0,
    ) -> ProjectVocalEditApplyResult:
        """Commit a full-source vocal render to one clip or take.

        ``take_id`` ``None`` edits the clip's base source; 0 is refused.

        The ``expected_*`` fields must match the clip's current source; a
        ``rendered_mono`` length other than ``expected_source_sample_count``
        raises ``SonareValueError``.
        """
        from .vocal_project import _apply_project_vocal_edit

        return _apply_project_vocal_edit(
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

    @_entry("Project.vocal_edit_dependencies")
    def vocal_edit_dependencies(
        self,
    ) -> tuple[ProjectVocalEditDependency, ...]:
        """Return the sources the project's vocal edits depend on."""
        from .vocal_project import _project_vocal_edit_dependencies

        return _project_vocal_edit_dependencies(self)

    @_entry("Project.rehydrate_vocal_edits")
    def rehydrate_vocal_edits(
        self,
        originals: Sequence[ProjectVocalOriginalSource],
        *,
        cancel: Callable[[], bool] | None = None,
    ) -> tuple[ProjectVocalRehydrateItem, ...]:
        """Rehydrate the project's vocal edits from the given original sources.

        ``cancel`` is polled and returns True to stop early.
        """
        from .vocal_project import _rehydrate_project_vocal_edits

        return _rehydrate_project_vocal_edits(
            self,
            originals=originals,
            cancel=cancel,
        )


_rebind_facade_exports(globals(), "libsonare._project_")
del _rebind_facade_exports
