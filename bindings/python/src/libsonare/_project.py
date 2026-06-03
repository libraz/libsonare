"""Headless arrangement / DAW project wrapper for libsonare.

This wraps the curated ``sonare_project_*`` C ABI (``src/sonare_c_project.h``):
an opaque :class:`SonareProject` handle over the arrangement control plane
(Project / EditHistory / EditCompiler / serializer / MIR bridges / SMF).

Every entry point is control-thread-only and performs no file or device I/O:
project JSON and SMF bytes are exchanged through in-memory buffers, so the host
owns storage. Heap buffers returned by the C layer are freed here (no leaks).

Availability: the symbols are always exported, but when libsonare was built
without ``SONARE_WITH_ARRANGEMENT`` every call returns
``SONARE_ERROR_NOT_SUPPORTED`` and :func:`project_abi_version` returns 0. The
:class:`Project` constructor checks the ABI version and raises a clear
``RuntimeError`` in that case.
"""

from __future__ import annotations

import ctypes
import math
import numbers
from collections.abc import Sequence
from typing import SupportsFloat, cast

import numpy as np

from ._runtime import (
    SonareMidiEventPod,
    SonareProjectBounceOptions,
    SonareProjectClipDesc,
    SonareProjectCompileResult,
    SonareProjectTrackDesc,
    _check,
    _from_c_float_array,
    _get_lib,
    _to_c_float_array,
)

# Must match SONARE_PROJECT_ABI_VERSION (src/sonare_c_project.h) and the other
# bindings' expected project ABI constant. A mismatch means the loaded native
# binary lays out the flat project PODs differently than this wrapper expects,
# or the arrangement subsystem was compiled out (runtime version 0).
EXPECTED_PROJECT_ABI_VERSION = 1

# Track kind ordinals (mirror SonareProjectTrackKind).
TRACK_AUDIO = 0
TRACK_MIDI = 1
TRACK_AUX = 2

_TRACK_KIND_NAMES = {
    "audio": TRACK_AUDIO,
    "midi": TRACK_MIDI,
    "aux": TRACK_AUX,
}


def project_abi_version() -> int:
    """Return the runtime project ABI version of the loaded native library.

    Equals :data:`EXPECTED_PROJECT_ABI_VERSION` when the arrangement subsystem
    is compiled in, ``0`` when libsonare was built without it.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_project_abi_version"):
        return 0
    return int(lib.sonare_project_abi_version())


def _track_kind_value(kind: str | int) -> int:
    if isinstance(kind, int):
        return kind
    key = kind.lower()
    if key not in _TRACK_KIND_NAMES:
        raise ValueError(f"unknown track kind: {kind}")
    return _TRACK_KIND_NAMES[key]


def _midi_event_tuple(name: str, *args: float | int) -> tuple[float, int, int]:
    lib = _get_lib()
    event = SonareMidiEventPod()
    fn = getattr(lib, name)
    _check(fn(*args, ctypes.byref(event)))
    return (float(event.ppq), int(event.data0), int(event.data1))


def _validate_midi_event_word(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise ValueError(f"{label} must be an integer in [0, 4294967295]")
    word = float(cast(SupportsFloat, value))
    if not math.isfinite(word) or not word.is_integer() or word < 0.0 or word > 0xFFFFFFFF:
        raise ValueError(f"{label} must be an integer in [0, 4294967295]")
    return int(word)


def _validate_midi_event_ppq(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise ValueError(f"{label} must be a non-negative finite number")
    ppq = float(cast(SupportsFloat, value))
    if not math.isfinite(ppq) or ppq < 0.0:
        raise ValueError(f"{label} must be a non-negative finite number")
    return ppq


class Project:
    """Pythonic wrapper around the native headless-project handle.

    All mutation routes through the native ``EditHistory`` (so :meth:`undo` /
    :meth:`redo` work), musical positions are PPQ (quarter notes), and
    serialization is deterministic (``to_json`` is byte-stable for a given
    project state within one build).
    """

    def __init__(self) -> None:
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_abi_version"):
            raise RuntimeError("libsonare was built without arrangement support")
        abi = int(lib.sonare_project_abi_version())
        if abi != EXPECTED_PROJECT_ABI_VERSION:
            raise RuntimeError(
                f"libsonare project ABI mismatch: native binary reports {abi}, "
                f"expected {EXPECTED_PROJECT_ABI_VERSION}. The installed shared "
                "library is incompatible with this Python binding (0 = arrangement "
                "support not compiled in)."
            )
        handle = ctypes.c_void_p()
        _check(lib.sonare_project_create(ctypes.byref(handle)))
        self._handle: ctypes.c_void_p | None = handle

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
        return self.to_json_bytes().decode("utf-8")

    @classmethod
    def from_json(cls, json: str | bytes) -> Project:
        """Deserialize project JSON into a new :class:`Project`.

        Raises ``ValueError`` on malformed input (with the joined native
        diagnostic messages), never crashing.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_abi_version"):
            raise RuntimeError("libsonare was built without arrangement support")
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

    def set_sample_rate(self, sample_rate: float) -> None:
        """Set the project sample rate in Hz (must be > 0)."""
        _check(
            _get_lib().sonare_project_set_sample_rate(self._require_handle(), float(sample_rate))
        )

    # -- edit ---------------------------------------------------------------

    def add_track(self, kind: str | int = TRACK_AUDIO, name: str | None = None) -> int:
        """Add a track and return its allocated stable id.

        Args:
            kind: ``"audio"`` / ``"midi"`` / ``"aux"`` (or the integer ordinal).
            name: Optional track name.
        """
        desc = SonareProjectTrackDesc(
            kind=_track_kind_value(kind),
            name=name.encode("utf-8") if name is not None else None,
        )
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_track(
                self._require_handle(), ctypes.byref(desc), ctypes.byref(out_id)
            )
        )
        return int(out_id.value)

    def add_clip(
        self,
        track_id: int,
        start_ppq: float,
        length_ppq: float,
        *,
        is_midi: bool = False,
        source_offset_ppq: float = 0.0,
        gain: float = 1.0,
        audio: Sequence[float] | np.ndarray | None = None,
        audio_channels: int = 1,
        audio_sample_rate: int = 0,
        source_uri: str | None = None,
    ) -> int:
        """Add an audio or MIDI clip and return its allocated clip id.

        For an audio clip, supply decoded interleaved ``audio`` to make it
        renderable by :meth:`bounce`; omit it for a metadata-only source
        (optionally referenced by ``source_uri``). For a MIDI clip pass
        ``is_midi=True`` and set events later via :meth:`set_midi_events`.
        """
        c_audio = None
        audio_frames = 0
        backing = None
        if audio is not None:
            backing, total = _to_c_float_array(audio)
            c_audio = backing
            channels = max(1, int(audio_channels))
            audio_frames = total // channels
        desc = SonareProjectClipDesc(
            track_id=int(track_id),
            is_midi=1 if is_midi else 0,
            start_ppq=float(start_ppq),
            length_ppq=float(length_ppq),
            source_offset_ppq=float(source_offset_ppq),
            gain=float(gain),
            audio_interleaved=c_audio,
            audio_frames=int(audio_frames),
            audio_channels=int(audio_channels),
            audio_sample_rate=int(audio_sample_rate),
            source_uri=source_uri.encode("utf-8") if source_uri is not None else None,
        )
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_clip(
                self._require_handle(), ctypes.byref(desc), ctypes.byref(out_id)
            )
        )
        # `backing` is kept alive until the call returns above.
        del backing
        return int(out_id.value)

    def add_midi_clip(self, start_ppq: float, length_ppq: float) -> tuple[int, int]:
        """Create a MIDI track + clip; return ``(track_id, clip_id)``."""
        out_track = ctypes.c_uint32()
        out_clip = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_midi_clip(
                self._require_handle(),
                float(start_ppq),
                float(length_ppq),
                ctypes.byref(out_track),
                ctypes.byref(out_clip),
            )
        )
        return int(out_track.value), int(out_clip.value)

    def split_clip(self, clip_id: int, split_ppq: float) -> int:
        """Split a clip at ``split_ppq`` (absolute PPQ); return the new clip id."""
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_split_clip(
                self._require_handle(),
                int(clip_id),
                float(split_ppq),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def trim_clip(self, clip_id: int, new_start_ppq: float, new_length_ppq: float) -> None:
        """Trim a clip's start / length (PPQ)."""
        _check(
            _get_lib().sonare_project_trim_clip(
                self._require_handle(),
                int(clip_id),
                float(new_start_ppq),
                float(new_length_ppq),
            )
        )

    def move_clip(self, clip_id: int, new_start_ppq: float, new_track_id: int = 0) -> None:
        """Move a clip to ``new_start_ppq`` (and optionally ``new_track_id``)."""
        _check(
            _get_lib().sonare_project_move_clip(
                self._require_handle(),
                int(clip_id),
                float(new_start_ppq),
                int(new_track_id),
            )
        )

    def undo(self) -> None:
        """Undo the most recent edit (raises when the undo stack is empty)."""
        _check(_get_lib().sonare_project_undo(self._require_handle()))

    def redo(self) -> None:
        """Redo the most recently undone edit (raises when the redo stack is empty)."""
        _check(_get_lib().sonare_project_redo(self._require_handle()))

    # -- MIDI ---------------------------------------------------------------

    def set_midi_events(
        self,
        clip_id: int,
        events: Sequence[tuple[float, int, int]] | Sequence[Sequence[float]] | np.ndarray,
    ) -> None:
        """Replace a MIDI clip's entire event list.

        Each event is ``(ppq, data0, data1)`` (the first two UMP-1.0 words of a
        channel-voice message; stored opaquely). Pass an empty sequence to clear.
        """
        rows = list(events)
        count = len(rows)
        c_events = (SonareMidiEventPod * count)()
        for i, ev in enumerate(rows):
            if len(ev) < 3:
                raise ValueError(f"events[{i}] must contain (ppq, data0, data1)")
            ppq, data0, data1 = ev[0], ev[1], ev[2]
            c_events[i].ppq = _validate_midi_event_ppq(ppq, f"events[{i}].ppq")
            c_events[i].data0 = _validate_midi_event_word(data0, f"events[{i}].data0")
            c_events[i].data1 = _validate_midi_event_word(data1, f"events[{i}].data1")
        _check(
            _get_lib().sonare_project_set_midi_events(
                self._require_handle(),
                int(clip_id),
                c_events if count else None,
                ctypes.c_size_t(count),
            )
        )

    def import_smf(self, data: bytes) -> int:
        """Import an in-memory SMF buffer; return the first added clip id.

        Raises ``ValueError`` (via the error code) on malformed input, never
        crashing.
        """
        lib = _get_lib()
        raw = bytes(data)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        out_id = ctypes.c_uint32()
        _check(
            lib.sonare_project_import_smf(
                self._require_handle(),
                buf,
                ctypes.c_size_t(len(raw)),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def export_smf(self) -> bytes:
        """Export the project's tempo map + MIDI clips to an SMF byte buffer."""
        lib = _get_lib()
        out = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_export_smf(
                self._require_handle(), ctypes.byref(out), ctypes.byref(out_len)
            )
        )
        try:
            if not out or out_len.value == 0:
                return b""
            return ctypes.string_at(out, out_len.value)
        finally:
            if out:
                lib.sonare_free_bytes(out)

    def set_program(self, clip_id: int, program: int, bank: int = 0) -> None:
        """Set a MIDI clip's channel-0 program / bank at source PPQ 0."""
        _check(
            _get_lib().sonare_project_set_program(
                self._require_handle(), int(clip_id), int(program), int(bank)
            )
        )

    def set_program_on_channel(
        self, clip_id: int, group: int, channel: int, program: int, bank: int = -1
    ) -> None:
        """Set a MIDI clip's program / bank for one UMP group and channel."""
        _check(
            _get_lib().sonare_project_set_program_on_channel(
                self._require_handle(),
                int(clip_id),
                int(group),
                int(channel),
                int(program),
                int(bank),
            )
        )

    def set_midi_fx(self, clip_id: int, config_json: str) -> None:
        """Configure and apply a clip's MIDI-FX chain from JSON."""
        _check(
            _get_lib().sonare_project_set_midi_fx(
                self._require_handle(),
                int(clip_id),
                config_json.encode("utf-8"),
            )
        )

    @staticmethod
    def midi_note_on(
        ppq: float, group: int, channel: int, note: int, velocity: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 note-on event tuple accepted by :meth:`set_midi_events`."""
        return _midi_event_tuple("sonare_midi_note_on", ppq, group, channel, note, velocity)

    @staticmethod
    def midi_note_off(
        ppq: float, group: int, channel: int, note: int, velocity: int = 0
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 note-off event tuple accepted by :meth:`set_midi_events`."""
        return _midi_event_tuple("sonare_midi_note_off", ppq, group, channel, note, velocity)

    @staticmethod
    def midi_cc(
        ppq: float, group: int, channel: int, controller: int, value: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 control-change event tuple."""
        return _midi_event_tuple("sonare_midi_cc", ppq, group, channel, controller, value)

    @staticmethod
    def midi_poly_pressure(
        ppq: float, group: int, channel: int, note: int, pressure: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 poly-pressure event tuple."""
        return _midi_event_tuple("sonare_midi_poly_pressure", ppq, group, channel, note, pressure)

    @staticmethod
    def midi_program(ppq: float, group: int, channel: int, program: int) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 program-change event tuple."""
        return _midi_event_tuple("sonare_midi_program", ppq, group, channel, program)

    @staticmethod
    def midi_channel_pressure(
        ppq: float, group: int, channel: int, pressure: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 channel-pressure event tuple."""
        return _midi_event_tuple("sonare_midi_channel_pressure", ppq, group, channel, pressure)

    @staticmethod
    def midi_pitch_bend(ppq: float, group: int, channel: int, bend: int) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 pitch-bend event tuple (`bend` is unsigned 14-bit)."""
        return _midi_event_tuple("sonare_midi_pitch_bend", ppq, group, channel, bend)

    # -- MIR ----------------------------------------------------------------

    def auto_tempo(self, audio: Sequence[float] | np.ndarray, sample_rate: int) -> float:
        """Detect tempo from a mono buffer and install it (undoable).

        Returns the primary BPM estimate.
        """
        c_array, length = _to_c_float_array(audio)
        out_bpm = ctypes.c_float()
        _check(
            _get_lib().sonare_project_auto_tempo(
                self._require_handle(),
                c_array,
                ctypes.c_size_t(length),
                int(sample_rate),
                ctypes.byref(out_bpm),
            )
        )
        return float(out_bpm.value)

    def snap_to_grid(self, ppq: float, strength: float = 1.0) -> float:
        """Snap a PPQ coordinate to the nearest beat of the project grid.

        ``strength`` in ``[0, 1]`` (0 = no snap, 1 = exact grid line).
        """
        out_ppq = ctypes.c_double()
        _check(
            _get_lib().sonare_project_snap_to_grid(
                self._require_handle(),
                float(ppq),
                float(strength),
                ctypes.byref(out_ppq),
            )
        )
        return float(out_ppq.value)

    # -- compile / render ---------------------------------------------------

    def compile(self) -> tuple[bool, str]:
        """Compile the project into an RT-readable timeline.

        Returns ``(has_timeline, messages)``: ``has_timeline`` is True when
        compilation produced a renderable timeline (no error diagnostics), and
        ``messages`` is the newline-joined human-readable diagnostic detail.
        Never throws on bad project content; it surfaces error diagnostics.
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        _check(lib.sonare_project_compile(self._require_handle(), ctypes.byref(result)))
        try:
            has_timeline = bool(result.has_timeline)
            messages = result.messages.decode("utf-8") if result.messages else ""
            return has_timeline, messages
        finally:
            lib.sonare_project_free_compile_result(ctypes.byref(result))

    def bounce(
        self,
        *,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project offline to interleaved float audio.

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic: the
        same project + options yields a bit-identical array within one build.
        Zero-valued options take native defaults (project sample rate, 2
        channels, block 128).
        """
        lib = _get_lib()
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        out = ctypes.POINTER(ctypes.c_float)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_bounce(
                self._require_handle(),
                ctypes.byref(options),
                ctypes.byref(out),
                ctypes.byref(out_len),
            )
        )
        try:
            interleaved = _from_c_float_array(out, int(out_len.value))
        finally:
            if out and out_len.value > 0:
                lib.sonare_free_floats(out)
        channels = num_channels if num_channels > 0 else 2
        if channels > 0 and interleaved.size % channels == 0:
            return interleaved.reshape(-1, channels)
        return interleaved.reshape(-1, 1)
