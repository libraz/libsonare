# ruff: noqa: F405
"""Project timeline and arrangement editing methods."""

from __future__ import annotations

import ctypes
import math
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING

import numpy as np

from ._project_model import *  # noqa: F403
from ._project_model import (
    _automation_lane_desc,
    _fade_curve_value,
    _loop_mode_value,
    _track_kind_value,
)
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    SonareProjectClipCompSegment,
    SonareProjectClipDesc,
    SonareProjectClipFade,
    SonareProjectClipTake,
    SonareProjectLoopRecordingDesc,
    SonareProjectTrackDesc,
    SonareProjectWarpAnchor,
    SonareProjectWarpMapDesc,
    _check,
    _get_lib,
    _to_c_float_array,
    _warp_mode_value,
)


class _ProjectEditMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

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
            channels = int(audio_channels)
            if channels <= 0 or total % channels != 0:
                raise ValueError("audio length must be a multiple of audio_channels")
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

    def add_loop_recording_takes(
        self,
        track_id: int,
        start_ppq: float,
        loop_length_ppq: float,
        audio: Sequence[float] | np.ndarray,
        *,
        audio_channels: int = 1,
        audio_sample_rate: int = 48000,
    ) -> tuple[int, int]:
        """Split a captured loop recording into takes and add one clip.

        Returns ``(clip_id, take_count)``. ``audio`` is interleaved float32
        capture data; each loop-length span becomes a separate take and the
        newest take is made active.
        """
        channels = int(audio_channels)
        backing, total = _to_c_float_array(audio)
        if channels <= 0 or total % channels != 0:
            raise ValueError("audio length must be a multiple of audio_channels")
        frames = total // channels
        desc = SonareProjectLoopRecordingDesc(
            track_id=int(track_id),
            reserved=0,
            start_ppq=float(start_ppq),
            loop_length_ppq=float(loop_length_ppq),
            audio_interleaved=backing,
            audio_frames=int(frames),
            audio_channels=channels,
            audio_sample_rate=int(audio_sample_rate),
        )
        out_clip = ctypes.c_uint32()
        out_take_count = ctypes.c_size_t()
        _check(
            _get_lib().sonare_project_add_loop_recording_takes(
                self._require_handle(),
                ctypes.byref(desc),
                ctypes.byref(out_clip),
                ctypes.byref(out_take_count),
            )
        )
        del backing
        return int(out_clip.value), int(out_take_count.value)

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

    def set_track_kind(self, track_id: int, kind: str | int) -> None:
        """Change a track kind via an undoable edit command."""
        _check(
            _get_lib().sonare_project_set_track_kind(
                self._require_handle(),
                int(track_id),
                _track_kind_value(kind),
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

    def set_clip_warp_mode(self, clip_id: int, mode: str | int) -> None:
        """Set a clip's warp playback mode."""
        _check(
            _get_lib().sonare_project_set_clip_warp_mode(
                self._require_handle(),
                int(clip_id),
                _warp_mode_value(mode),
            )
        )

    def set_clip_takes(
        self,
        clip_id: int,
        takes: Sequence[Mapping[str, object]] | Sequence[tuple[int, int, float, str]] | None,
        active_take_id: int = 0,
    ) -> None:
        """Replace a clip's take list and active take via an undoable edit."""
        take_items = list(takes or [])
        c_takes = (SonareProjectClipTake * len(take_items))()
        name_backing: list[bytes] = []
        for i, item in enumerate(take_items):
            if isinstance(item, Mapping):
                take_id = int(item.get("id", 0))
                source_id = int(item.get("source_id", item.get("sourceId", 0)))
                source_offset = float(
                    item.get("source_offset_ppq", item.get("sourceOffsetPpq", 0.0))
                )
                name = item.get("name", "")
            else:
                take_id, source_id, source_offset, name = item
            c_takes[i].id = int(take_id)
            c_takes[i].source_id = int(source_id)
            c_takes[i].source_offset_ppq = float(source_offset)
            if name:
                encoded = str(name).encode("utf-8")
                name_backing.append(encoded)
                c_takes[i].name = encoded
        _check(
            _get_lib().sonare_project_set_clip_takes(
                self._require_handle(),
                int(clip_id),
                c_takes if take_items else None,
                len(take_items),
                int(active_take_id),
            )
        )
        del name_backing

    def set_clip_comp_segments(
        self,
        clip_id: int,
        segments: Sequence[Mapping[str, object]] | Sequence[tuple[float, float, int]] | None,
    ) -> None:
        """Replace a clip's comp lane via an undoable edit."""
        segment_items = list(segments or [])
        c_segments = (SonareProjectClipCompSegment * len(segment_items))()
        for i, item in enumerate(segment_items):
            if isinstance(item, Mapping):
                start_ppq = float(item.get("start_ppq", item.get("startPpq", 0.0)))
                end_ppq = float(item.get("end_ppq", item.get("endPpq", 0.0)))
                take_id = int(item.get("take_id", item.get("takeId", 0)))
            else:
                start_ppq, end_ppq, take_id = item
            c_segments[i].start_ppq = float(start_ppq)
            c_segments[i].end_ppq = float(end_ppq)
            c_segments[i].take_id = int(take_id)
        _check(
            _get_lib().sonare_project_set_clip_comp_segments(
                self._require_handle(),
                int(clip_id),
                c_segments if segment_items else None,
                len(segment_items),
            )
        )

    def set_warp_map(
        self,
        warp_ref_id: int,
        anchors: Sequence[tuple[float, float]],
        name: str | None = None,
    ) -> None:
        """Add or replace a first-class project warp map."""
        count = len(anchors)
        c_anchors = (SonareProjectWarpAnchor * count)()
        for i, (warp_sample, source_sample) in enumerate(anchors):
            c_anchors[i].warp_sample = float(warp_sample)
            c_anchors[i].source_sample = float(source_sample)
        encoded_name = name.encode("utf-8") if name else None
        desc = SonareProjectWarpMapDesc(
            id=int(warp_ref_id),
            name=encoded_name,
            anchors=c_anchors,
            anchor_count=count,
        )
        _check(_get_lib().sonare_project_set_warp_map(self._require_handle(), ctypes.byref(desc)))

    def remove_warp_map(self, warp_ref_id: int) -> None:
        """Remove a first-class project warp map by id."""
        _check(
            _get_lib().sonare_project_remove_warp_map(
                self._require_handle(),
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

    def set_track_gain(self, track_id: int, gain: float) -> None:
        """Set a track's linear playback gain (1.0 = unity; >= 0) via an undoable edit.

        The compiler folds the track's gain/mute/solo/pan into its channel strip,
        so the value applies uniformly to the track's audio and MIDI.
        """
        g = float(gain)
        if not math.isfinite(g) or g < 0.0:
            raise ValueError("gain must be a finite number >= 0")
        _check(_get_lib().sonare_project_set_track_gain(self._require_handle(), int(track_id), g))

    def set_track_mute(self, track_id: int, mute: bool) -> None:
        """Set a track's mute flag via an undoable edit (a muted track is silent)."""
        _check(
            _get_lib().sonare_project_set_track_mute(
                self._require_handle(), int(track_id), 1 if mute else 0
            )
        )

    def set_track_solo(self, track_id: int, solo: bool) -> None:
        """Set a track's solo flag via an undoable edit.

        When any track is soloed, only soloed tracks sound.
        """
        _check(
            _get_lib().sonare_project_set_track_solo(
                self._require_handle(), int(track_id), 1 if solo else 0
            )
        )

    def set_track_pan(self, track_id: int, pan: float) -> None:
        """Set a track's stereo balance in [-1, +1] (0 = center) via an undoable edit.

        ``pan`` is clamped to the valid range by the core.
        """
        p = float(pan)
        if not math.isfinite(p):
            raise ValueError("pan must be a finite number")
        _check(_get_lib().sonare_project_set_track_pan(self._require_handle(), int(track_id), p))

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
        self,
        clip_id: int,
        loop_mode: str | int = LOOP_MODE_OFF,
        loop_length_ppq: float = 0.0,
        loop_crossfade_ppq: float = 0.0,
    ) -> None:
        """Set a clip's loop mode + loop length (PPQ) via an undoable edit.

        ``loop_mode`` is a :data:`LOOP_MODE_*` ordinal or name. When looping,
        ``loop_length_ppq`` must be finite and >= 0, where ``0`` means "loop the
        entire clip" (the effective length is resolved from the clip's own
        duration). ``loop_crossfade_ppq`` is an optional equal-power crossfade at
        the loop seam (PPQ, finite and >= 0; 0 = hard loop); the engine clamps it
        to the clip's pre-roll and half the loop.
        """
        mode = _loop_mode_value(loop_mode)
        length = float(loop_length_ppq)
        crossfade = float(loop_crossfade_ppq)
        if not math.isfinite(length) or length < 0.0:
            raise ValueError("loop_length_ppq must be a finite number >= 0")
        if not math.isfinite(crossfade) or crossfade < 0.0:
            raise ValueError("loop_crossfade_ppq must be a finite number >= 0")
        _check(
            _get_lib().sonare_project_set_clip_loop(
                self._require_handle(), int(clip_id), int(mode), length, crossfade
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

    def clear_history(self) -> None:
        """Empty the undo and redo stacks without altering project state."""
        _check(_get_lib().sonare_project_clear_history(self._require_handle()))

    def set_max_undo_depth(self, depth: int) -> None:
        """Clamp the undo history to ``depth`` entries (minimum 1), evicting the oldest."""
        _check(
            _get_lib().sonare_project_set_max_undo_depth(
                self._require_handle(),
                ctypes.c_size_t(int(depth)),
            )
        )

    # -- MIDI ---------------------------------------------------------------
