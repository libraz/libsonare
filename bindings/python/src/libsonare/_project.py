# ruff: noqa: F405
"""Stable public facade for the headless arrangement project API."""

from __future__ import annotations

import ctypes
from typing import Self

from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._project_edit import _ProjectEditMixin
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
from ._project_render import _ProjectRenderMixin
from ._runtime import _check, _get_lib


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
        lib = _get_lib()
        _check_project_abi(lib)
        handle = ctypes.c_void_p()
        _check(lib.sonare_project_create(ctypes.byref(handle)))
        self._handle: ctypes.c_void_p | None = handle

    @classmethod
    def create(cls) -> Self:
        """Create a new empty project."""
        return cls()

    # -- lifecycle ----------------------------------------------------------

    def close(self) -> None:
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
        self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise RuntimeError("Project is closed")
        return self._handle

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

        Raises ``ValueError`` on malformed input (with the joined native
        diagnostic messages), never crashing.
        """
        lib = _get_lib()
        _check_project_abi(lib)
        data = json.encode("utf-8") if isinstance(json, str) else bytes(json)
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
            raise ValueError(detail or "failed to deserialize project JSON")
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
        data = json.encode("utf-8") if isinstance(json, str) else bytes(json)
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
            raise ValueError(diagnostics or "failed to deserialize project JSON")
        obj = cls.__new__(cls)
        obj._handle = handle
        return ProjectDeserializeResult(project=obj, diagnostics=diagnostics)


_rebind_facade_exports(globals(), "libsonare._project_")
del _rebind_facade_exports
