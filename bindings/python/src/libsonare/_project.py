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
from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from typing import SupportsFloat, cast

import numpy as np

from ._runtime import (
    SonareAutomationLaneDesc,
    SonareAutomationPoint,
    SonareBuiltinInstrumentBinding,
    SonareBuiltinSynthConfig,
    SonareMidiEventPod,
    SonareProjectAssistSidecar,
    SonareProjectBounceOptions,
    SonareProjectChordSymbol,
    SonareProjectClipDesc,
    SonareProjectClipFade,
    SonareProjectCompileResult,
    SonareProjectKeySegment,
    SonareProjectTrackDesc,
    _check,
    _curve_value,
    _from_c_float_array,
    _get_lib,
    _to_c_float_array,
)

# Built-in synth waveform ordinals (mirror SonareSynthWaveform).
SYNTH_WAVEFORM_SINE = 0
SYNTH_WAVEFORM_SAW = 1
SYNTH_WAVEFORM_SQUARE = 2
SYNTH_WAVEFORM_TRIANGLE = 3

_SYNTH_WAVEFORM_NAMES = {
    "sine": SYNTH_WAVEFORM_SINE,
    "saw": SYNTH_WAVEFORM_SAW,
    "sawtooth": SYNTH_WAVEFORM_SAW,
    "square": SYNTH_WAVEFORM_SQUARE,
    "triangle": SYNTH_WAVEFORM_TRIANGLE,
}

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

# Clip fade-curve ordinals (mirror SonareProjectFadeCurve).
FADE_CURVE_LINEAR = 0
FADE_CURVE_EQUAL_POWER = 1
FADE_CURVE_EXPONENTIAL = 2
FADE_CURVE_LOGARITHMIC = 3

_FADE_CURVE_NAMES = {
    "linear": FADE_CURVE_LINEAR,
    "equal-power": FADE_CURVE_EQUAL_POWER,
    "equal_power": FADE_CURVE_EQUAL_POWER,
    "exponential": FADE_CURVE_EXPONENTIAL,
    "exp": FADE_CURVE_EXPONENTIAL,
    "logarithmic": FADE_CURVE_LOGARITHMIC,
    "log": FADE_CURVE_LOGARITHMIC,
}

# Clip loop-mode ordinals (mirror SonareProjectLoopMode).
LOOP_MODE_OFF = 0
LOOP_MODE_LOOP = 1

_LOOP_MODE_NAMES = {
    "off": LOOP_MODE_OFF,
    "none": LOOP_MODE_OFF,
    "loop": LOOP_MODE_LOOP,
    "on": LOOP_MODE_LOOP,
}


def _fade_curve_value(curve: str | int) -> int:
    if isinstance(curve, int):
        return curve
    key = curve.lower()
    if key not in _FADE_CURVE_NAMES:
        raise ValueError(f"unknown fade curve: {curve}")
    return _FADE_CURVE_NAMES[key]


def _loop_mode_value(mode: str | int) -> int:
    if isinstance(mode, int):
        return mode
    key = mode.lower()
    if key not in _LOOP_MODE_NAMES:
        raise ValueError(f"unknown loop mode: {mode}")
    return _LOOP_MODE_NAMES[key]


def _automation_lane_desc(
    target_param_id: int,
    points: Sequence[tuple[float, float, int | str]] | Sequence[Sequence[float]] | None,
) -> tuple[SonareAutomationLaneDesc, object]:
    """Marshal ``(ppq, value, curve)`` breakpoints into a SonareAutomationLaneDesc.

    Returns ``(desc, backing)``; the caller must keep ``backing`` (the C point
    array) alive for the duration of the native call.
    """
    rows = list(points) if points is not None else []
    count = len(rows)
    c_points = (SonareAutomationPoint * count)() if count else None
    for i, pt in enumerate(rows):
        seq = tuple(pt)
        if len(seq) < 2:
            raise ValueError(f"points[{i}] must contain at least (ppq, value)")
        ppq = float(seq[0])
        value = float(seq[1])
        curve = _curve_value(seq[2]) if len(seq) >= 3 else 0
        if not math.isfinite(ppq):
            raise ValueError(f"points[{i}].ppq must be a finite number")
        if not math.isfinite(value):
            raise ValueError(f"points[{i}].value must be a finite number")
        c_points[i].ppq = ppq
        c_points[i].value = value
        c_points[i].curve_to_next = int(curve)
    desc = SonareAutomationLaneDesc(
        target_param_id=int(target_param_id),
        points=c_points,
        point_count=ctypes.c_size_t(count),
    )
    return desc, c_points


@dataclass(frozen=True)
class AssistSidecar:
    """One opaque assist sidecar stored on a :class:`Project`.

    ``payload`` is module-owned opaque bytes; ``module_id`` identifies the
    producing module. ``schema_version`` / ``target_track_id`` and the
    ``region_*_ppq`` scope mirror the C ``SonareProjectAssistSidecar``.
    """

    module_id: str
    schema_version: int
    target_track_id: int
    region_start_ppq: float
    region_end_ppq: float
    payload: bytes


@dataclass(frozen=True)
class ProjectDiagnostic:
    """One compile diagnostic surfaced by :meth:`Project.compile`."""

    code: int
    severity: int
    target_id: int


@dataclass(frozen=True)
class BuiltinSynthConfig:
    """Patch for the built-in polyphonic oscillator synth (see
    :meth:`Project.bounce_with_builtin_instrument`).

    Every numeric field uses "0 (or non-positive) => sensible default", so a
    default-constructed config is the default sine patch; override only what you
    need. ``waveform`` may be an ordinal (0=sine, 1=saw, 2=square, 3=triangle)
    or a name (``"sine"`` / ``"saw"`` / ``"square"`` / ``"triangle"``).
    """

    waveform: str | int = SYNTH_WAVEFORM_SINE
    gain: float = 0.0
    attack_ms: float = 0.0
    decay_ms: float = 0.0
    sustain: float = 0.0
    release_ms: float = 0.0
    polyphony: int = 0

    def _to_c(self) -> SonareBuiltinSynthConfig:
        return SonareBuiltinSynthConfig(
            waveform=_synth_waveform_value(self.waveform),
            gain=float(self.gain),
            attack_ms=float(self.attack_ms),
            decay_ms=float(self.decay_ms),
            sustain=float(self.sustain),
            release_ms=float(self.release_ms),
            polyphony=int(self.polyphony),
        )


def _synth_waveform_value(waveform: str | int) -> int:
    if isinstance(waveform, int):
        return waveform
    key = waveform.lower()
    if key not in _SYNTH_WAVEFORM_NAMES:
        raise ValueError(f"unknown synth waveform: {waveform}")
    return _SYNTH_WAVEFORM_NAMES[key]


@dataclass(frozen=True)
class ProjectCompileResult:
    """Structured compile result matching the C/Node/WASM project surface.

    Iteration preserves the legacy ``has_timeline, messages = project.compile()``
    pattern while exposing structured diagnostics.
    """

    has_timeline: bool
    messages: str
    diagnostics: tuple[ProjectDiagnostic, ...]

    @property
    def diagnostic_count(self) -> int:
        return len(self.diagnostics)

    def __iter__(self) -> Iterator[object]:
        yield self.has_timeline
        yield self.messages


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

    def set_clip_warp_ref(self, clip_id: int, warp_ref_id: int) -> None:
        """Set a clip's warp reference id (``0`` clears it)."""
        _check(
            _get_lib().sonare_project_set_clip_warp_ref(
                self._require_handle(),
                int(clip_id),
                int(warp_ref_id),
            )
        )

    def set_track_midi_destination(self, track_id: int, destination_id: int) -> None:
        """Route a track's MIDI to host-instrument ``destination_id`` (0 = default).

        The compiler stamps every MIDI clip on the track with this id so the
        engine dispatches the clip's events to the instrument registered for that
        destination. Routes through an undoable edit command.
        """
        _check(
            _get_lib().sonare_project_set_track_midi_destination(
                self._require_handle(),
                int(track_id),
                int(destination_id),
            )
        )

    def remove_clip(self, clip_id: int) -> None:
        """Remove a clip via an undoable edit command (undo restores it)."""
        _check(_get_lib().sonare_project_remove_clip(self._require_handle(), int(clip_id)))

    def set_clip_gain(self, clip_id: int, gain: float) -> None:
        """Set a clip's linear playback gain (>= 0; 0 = muted) via an undoable edit."""
        g = float(gain)
        if not math.isfinite(g) or g < 0.0:
            raise ValueError("gain must be a finite number >= 0")
        _check(_get_lib().sonare_project_set_clip_gain(self._require_handle(), int(clip_id), g))

    def set_clip_fade(
        self,
        clip_id: int,
        fade_in_length_ppq: float = 0.0,
        fade_out_length_ppq: float = 0.0,
        *,
        fade_in_curve: str | int = FADE_CURVE_LINEAR,
        fade_out_curve: str | int = FADE_CURVE_LINEAR,
    ) -> None:
        """Set a clip's fade-in and fade-out regions via an undoable edit.

        Each fade length (PPQ) must be finite and >= 0 (0 = no fade); each curve
        is a :data:`FADE_CURVE_*` ordinal or name (``"linear"`` / ``"equal-power"``
        / ``"exponential"`` / ``"logarithmic"``).
        """
        fin = float(fade_in_length_ppq)
        fout = float(fade_out_length_ppq)
        if not math.isfinite(fin) or fin < 0.0:
            raise ValueError("fade_in_length_ppq must be a finite number >= 0")
        if not math.isfinite(fout) or fout < 0.0:
            raise ValueError("fade_out_length_ppq must be a finite number >= 0")
        c_in = SonareProjectClipFade(length_ppq=fin, curve=_fade_curve_value(fade_in_curve))
        c_out = SonareProjectClipFade(length_ppq=fout, curve=_fade_curve_value(fade_out_curve))
        _check(
            _get_lib().sonare_project_set_clip_fade(
                self._require_handle(),
                int(clip_id),
                ctypes.byref(c_in),
                ctypes.byref(c_out),
            )
        )

    def set_clip_loop(
        self, clip_id: int, loop_mode: str | int = LOOP_MODE_OFF, loop_length_ppq: float = 0.0
    ) -> None:
        """Set a clip's loop mode + loop length (PPQ) via an undoable edit.

        ``loop_mode`` is a :data:`LOOP_MODE_*` ordinal or name. When looping,
        ``loop_length_ppq`` must be finite and > 0; otherwise finite and >= 0.
        """
        mode = _loop_mode_value(loop_mode)
        length = float(loop_length_ppq)
        if not math.isfinite(length) or length < 0.0:
            raise ValueError("loop_length_ppq must be a finite number >= 0")
        if mode == LOOP_MODE_LOOP and length <= 0.0:
            raise ValueError("loop_length_ppq must be > 0 when looping")
        _check(
            _get_lib().sonare_project_set_clip_loop(
                self._require_handle(), int(clip_id), int(mode), length
            )
        )

    def set_clip_source(self, clip_id: int, source_id: int) -> None:
        """Rebind a clip to a different (already-registered) source via an undoable edit."""
        _check(
            _get_lib().sonare_project_set_clip_source(
                self._require_handle(), int(clip_id), int(source_id)
            )
        )

    def duplicate_clip(self, clip_id: int, new_start_ppq: float) -> int:
        """Duplicate a clip at ``new_start_ppq`` (same track); return the new clip id."""
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_duplicate_clip(
                self._require_handle(),
                int(clip_id),
                float(new_start_ppq),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def remove_track(self, track_id: int) -> None:
        """Remove a track (and its clips) via an undoable edit command."""
        _check(_get_lib().sonare_project_remove_track(self._require_handle(), int(track_id)))

    def rename_track(self, track_id: int, name: str | None = None) -> None:
        """Rename a track via an undoable edit command (``None`` = empty name)."""
        _check(
            _get_lib().sonare_project_rename_track(
                self._require_handle(),
                int(track_id),
                name.encode("utf-8") if name is not None else None,
            )
        )

    def set_track_route(
        self, track_id: int, channel_strip_ref: str | None = None, output_target: str | None = None
    ) -> None:
        """Set a track's mixer-strip binding + output target via an undoable edit.

        Pass ``None`` (or ``""``) for either field to clear it.
        """
        _check(
            _get_lib().sonare_project_set_track_route(
                self._require_handle(),
                int(track_id),
                channel_strip_ref.encode("utf-8") if channel_strip_ref is not None else None,
                output_target.encode("utf-8") if output_target is not None else None,
            )
        )

    def add_automation_lane(
        self,
        track_id: int,
        target_param_id: int,
        points: Sequence[tuple[float, float, int | str]] | Sequence[Sequence[float]] | None = None,
    ) -> int:
        """Append an automation lane to a track; return its index within the track.

        ``points`` is an optional list of ``(ppq, value, curve)`` breakpoints
        (``curve`` is an :class:`AutomationCurve` ordinal / name; default linear).
        """
        desc, _backing = _automation_lane_desc(target_param_id, points)
        out_index = ctypes.c_size_t()
        _check(
            _get_lib().sonare_project_add_automation_lane(
                self._require_handle(),
                int(track_id),
                ctypes.byref(desc),
                ctypes.byref(out_index),
            )
        )
        del _backing
        return int(out_index.value)

    def edit_automation_lane(
        self,
        track_id: int,
        lane_index: int,
        target_param_id: int,
        points: Sequence[tuple[float, float, int | str]] | Sequence[Sequence[float]] | None = None,
    ) -> None:
        """Replace an existing automation lane in place via an undoable edit."""
        desc, _backing = _automation_lane_desc(target_param_id, points)
        _check(
            _get_lib().sonare_project_edit_automation_lane(
                self._require_handle(),
                int(track_id),
                ctypes.c_size_t(int(lane_index)),
                ctypes.byref(desc),
            )
        )
        del _backing

    def remove_automation_lane(self, track_id: int, lane_index: int) -> None:
        """Remove an automation lane from a track via an undoable edit command."""
        _check(
            _get_lib().sonare_project_remove_automation_lane(
                self._require_handle(),
                int(track_id),
                ctypes.c_size_t(int(lane_index)),
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

    def import_clip_file(self, data: bytes) -> int:
        """Import an in-memory MIDI 2.0 Clip File (``SMF2CLIP``); return the
        first added clip id.

        Unlike :meth:`import_smf`, the UMP-based container preserves MIDI 2.0
        channel-voice messages (16-bit velocity, 32-bit CC, per-note /
        registered controllers, bank-valid Program Change) without loss. Raises
        ``ValueError`` on malformed input, never crashing.
        """
        lib = _get_lib()
        raw = bytes(data)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        out_id = ctypes.c_uint32()
        _check(
            lib.sonare_project_import_clip_file(
                self._require_handle(),
                buf,
                ctypes.c_size_t(len(raw)),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def export_clip_file(self) -> bytes:
        """Export the project's tempo map + MIDI clips to a MIDI 2.0 Clip File
        (``SMF2CLIP``) byte buffer.

        MIDI 2.0-only events are written WITHOUT loss — prefer this over
        :meth:`export_smf` when MIDI 2.0 fidelity matters.
        """
        lib = _get_lib()
        out = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_export_clip_file(
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

    def set_program(self, clip_id: int, program: int, bank: int = -1) -> None:
        """Set a MIDI clip's channel-0 program / bank at source PPQ 0.

        ``bank`` defaults to ``-1`` (no Bank Select emitted), matching
        :meth:`set_program_on_channel`; pass ``>= 0`` to emit a Bank Select.
        """
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

    def annotate_keys(
        self, keys: Sequence[tuple[float, float, int, int]] | Sequence[Sequence[float]]
    ) -> None:
        """Replace the project's key annotation stream via an undoable command.

        Each key is ``(start_ppq, end_ppq, tonic_pc, mode)`` where ``tonic_pc``
        is 0..11 (C=0) or 255 unknown and ``mode`` is the KeyMode ordinal
        (0 unknown, 1 major, 2 minor, 3 dorian, 4 phrygian, 5 lydian,
        6 mixolydian, 7 locrian). Pass an empty sequence to clear.
        """
        rows = list(keys)
        count = len(rows)
        c_keys = (SonareProjectKeySegment * count)() if count else None
        for i, k in enumerate(rows):
            seq = tuple(k)
            if len(seq) < 4:
                raise ValueError(f"keys[{i}] must contain (start_ppq, end_ppq, tonic_pc, mode)")
            c_keys[i].start_ppq = float(seq[0])
            c_keys[i].end_ppq = float(seq[1])
            c_keys[i].tonic_pc = int(seq[2])
            c_keys[i].mode = int(seq[3])
        _check(
            _get_lib().sonare_project_annotate_keys(
                self._require_handle(), c_keys, ctypes.c_size_t(count)
            )
        )

    def annotate_chords(self, chords: Sequence[dict[str, object]]) -> None:
        """Replace the project's chord-symbol annotation stream (undoable command).

        Each chord is a mapping with keys ``start_ppq``, ``end_ppq``, ``root_pc``
        (0..11 / 255), ``quality`` (ChordQuality ordinal), optional
        ``extensions`` (iterable of semitone ints, up to 8), ``slash_bass_pc``
        (default 255), ``roman_numeral`` (optional str) and ``modulation_boundary``
        (bool). Pass an empty sequence to clear.
        """
        rows = list(chords)
        count = len(rows)
        c_chords = (SonareProjectChordSymbol * count)() if count else None
        # Keep extension arrays and roman-numeral byte strings alive for the call.
        backing: list[object] = []
        for i, c in enumerate(rows):
            ext = list(c.get("extensions", []) or [])
            ext_count = len(ext)
            c_ext = (
                (ctypes.c_uint8 * ext_count)(*[int(e) & 0xFF for e in ext]) if ext_count else None
            )
            backing.append(c_ext)
            roman = c.get("roman_numeral")
            roman_bytes = roman.encode("utf-8") if isinstance(roman, str) and roman else None
            backing.append(roman_bytes)
            c_chords[i].start_ppq = float(c["start_ppq"])
            c_chords[i].end_ppq = float(c["end_ppq"])
            c_chords[i].root_pc = int(c.get("root_pc", 255))
            c_chords[i].quality = int(c.get("quality", 0))
            c_chords[i].extensions = c_ext
            c_chords[i].extension_count = ctypes.c_size_t(ext_count)
            c_chords[i].slash_bass_pc = int(c.get("slash_bass_pc", 255))
            c_chords[i].roman_numeral = roman_bytes
            c_chords[i].modulation_boundary = 1 if c.get("modulation_boundary") else 0
        _check(
            _get_lib().sonare_project_annotate_chords(
                self._require_handle(), c_chords, ctypes.c_size_t(count)
            )
        )
        del backing

    # -- assist sidecars ----------------------------------------------------

    def set_assist_sidecar(
        self,
        module_id: str,
        payload: bytes,
        *,
        schema_version: int = 0,
        target_track_id: int = 0,
        region_start_ppq: float = 0.0,
        region_end_ppq: float = 0.0,
    ) -> None:
        """Add or update an opaque assist sidecar (undoable command).

        Sidecars sharing ``module_id`` + ``target_track_id`` + region scope are
        replaced; otherwise a new one is appended. ``module_id`` must be
        non-empty and ``payload`` is copied opaque bytes.
        """
        if not module_id:
            raise ValueError("module_id must be a non-empty string")
        raw = bytes(payload)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        _check(
            _get_lib().sonare_project_set_assist_sidecar(
                self._require_handle(),
                module_id.encode("utf-8"),
                int(schema_version),
                int(target_track_id),
                float(region_start_ppq),
                float(region_end_ppq),
                buf,
                ctypes.c_size_t(len(raw)),
            )
        )

    def assist_sidecar_count(self) -> int:
        """Number of assist sidecars currently stored on the project."""
        return int(_get_lib().sonare_project_assist_sidecar_count(self._require_handle()))

    def get_assist_sidecar(self, index: int) -> AssistSidecar:
        """Read one assist sidecar by stable project order as an :class:`AssistSidecar`."""
        lib = _get_lib()
        raw = SonareProjectAssistSidecar()
        _check(
            lib.sonare_project_get_assist_sidecar(
                self._require_handle(), int(index), ctypes.byref(raw)
            )
        )
        try:
            module_id = raw.module_id.decode("utf-8") if raw.module_id else ""
            payload_len = int(raw.payload_len)
            payload = (
                ctypes.string_at(raw.payload, payload_len) if raw.payload and payload_len else b""
            )
            return AssistSidecar(
                module_id=module_id,
                schema_version=int(raw.schema_version),
                target_track_id=int(raw.target_track_id),
                region_start_ppq=float(raw.region_start_ppq),
                region_end_ppq=float(raw.region_end_ppq),
                payload=payload,
            )
        finally:
            lib.sonare_project_free_assist_sidecar(ctypes.byref(raw))

    def assist_sidecars(self) -> list[AssistSidecar]:
        """Return all stored assist sidecars as a list of :class:`AssistSidecar`."""
        return [self.get_assist_sidecar(i) for i in range(self.assist_sidecar_count())]

    # -- compile / render ---------------------------------------------------

    def compile(self) -> ProjectCompileResult:
        """Compile the project into an RT-readable timeline.

        Returns a :class:`ProjectCompileResult` with ``has_timeline``,
        ``messages``, and structured ``diagnostics``. The result remains
        iterable as ``(has_timeline, messages)`` for legacy callers. Never
        throws on bad project content; it surfaces error diagnostics.
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        _check(lib.sonare_project_compile(self._require_handle(), ctypes.byref(result)))
        try:
            has_timeline = bool(result.has_timeline)
            messages = result.messages.decode("utf-8") if result.messages else ""
            diagnostics = tuple(
                ProjectDiagnostic(
                    code=int(result.diagnostics[i].code),
                    severity=int(result.diagnostics[i].severity),
                    target_id=int(result.diagnostics[i].target_id),
                )
                for i in range(int(result.diagnostic_count))
            )
            return ProjectCompileResult(has_timeline, messages, diagnostics)
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
        channels, block 128). Omitting ``total_frames`` (or passing <= 0) makes
        the native layer auto-derive the render length from the arrangement.

        Note: MIDI tracks render to silence here because no instrument is bound;
        use :meth:`bounce_with_builtin_instrument` to render MIDI through the
        built-in synth.
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
            # The C ABI returns a non-null buffer even when out_len == 0 (a
            # sentinel `new float[1]`); free it unconditionally to avoid leaking
            # on empty bounces. _from_c_float_array already copied the samples.
            if out:
                lib.sonare_free_floats(out)
        channels = num_channels if num_channels > 0 else 2
        if channels > 0 and interleaved.size % channels == 0:
            return interleaved.reshape(-1, channels)
        return interleaved.reshape(-1, 1)

    def bounce_with_builtin_instrument(
        self,
        instrument: BuiltinSynthConfig | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, BuiltinSynthConfig]] | None = None,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through the built-in synth.

        Unlike :meth:`bounce`, MIDI tracks routed to a bound destination render
        through the built-in polyphonic oscillator synth, so a MIDI-only
        arrangement produces audible (non-silent) output without the caller
        supplying its own instrument callbacks.

        Args:
            instrument: Patch bound to ``destination_id`` (default 0). Pass
                ``None`` for the default sine patch. Ignored when ``instruments``
                is given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0).
            instruments: Optional explicit list of ``(destination_id, patch)``
                bindings, overriding ``instrument`` / ``destination_id``.
            total_frames: Render length in frames; <= 0 auto-derives the length
                from the arrangement (musical end + the synth's release tail).
            block_size / num_channels / sample_rate / instrument_latency_samples:
                As :meth:`bounce` (0 takes native defaults).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + patch.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_builtin_instruments"):
            raise RuntimeError("libsonare was built without the built-in instrument bounce ABI")
        if instruments is None:
            patch = instrument if instrument is not None else BuiltinSynthConfig()
            bindings = [(int(destination_id), patch)]
        else:
            bindings = [(int(dst), patch) for dst, patch in instruments]
        count = len(bindings)
        c_bindings = (SonareBuiltinInstrumentBinding * count)()
        for i, (dst, patch) in enumerate(bindings):
            c_bindings[i].destination_id = dst
            c_bindings[i].config = patch._to_c()
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
            lib.sonare_project_bounce_with_builtin_instruments(
                self._require_handle(),
                ctypes.byref(options),
                c_bindings if count else None,
                ctypes.c_size_t(count),
                ctypes.byref(out),
                ctypes.byref(out_len),
            )
        )
        try:
            interleaved = _from_c_float_array(out, int(out_len.value))
        finally:
            # Free unconditionally: the C ABI returns a non-null sentinel buffer
            # even when out_len == 0, so a `> 0` guard would leak it.
            if out:
                lib.sonare_free_floats(out)
        channels = num_channels if num_channels > 0 else 2
        if channels > 0 and interleaved.size % channels == 0:
            return interleaved.reshape(-1, channels)
        return interleaved.reshape(-1, 1)
