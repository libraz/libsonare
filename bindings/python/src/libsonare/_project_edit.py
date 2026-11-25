# ruff: noqa: F405
"""Project timeline and arrangement editing methods."""

from __future__ import annotations

import ctypes
import json
import re
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING

import numpy as np

from ._errors import _entry, _refuse, _refuse_type
from ._ffi import SONARE_AUTOMATION_TARGET_UNCHANGED
from ._narrowing import _MAX_PUBLIC_PPQ, _Respelling
from ._project_mir import AlignTakeToReferenceResult as AlignTakeToReferenceResult
from ._project_mir import TakeAlignment as TakeAlignment
from ._project_mir import align_take_to_reference as align_take_to_reference
from ._project_mir import chord_symbol_from_analysis as chord_symbol_from_analysis
from ._project_mir import key_mode_from_analysis as key_mode_from_analysis
from ._project_model import *  # noqa: F403
from ._project_model import (
    _AUTOMATION_TARGET_KINDS,
    _STEM_LAYOUTS,
    _automation_lane_desc,
    _automation_target_kind_value,
    _automation_target_param_id_value,
    _fade_curve_value,
    _loop_mode_value,
    _track_kind_value,
)
from ._project_synth import (
    PART_RIG_ALL_PARTS as PART_RIG_ALL_PARTS,
)
from ._project_synth import (
    PART_RIG_MODES as PART_RIG_MODES,
)
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    PartRigMode as PartRigMode,
)
from ._project_synth import (
    _part_rig_inserts_arg as _part_rig_inserts_arg,
)
from ._project_synth import (
    _part_rig_mode_name as _part_rig_mode_name,
)
from ._project_synth import (
    _part_rig_mode_value as _part_rig_mode_value,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    _SIZE_T_MAX,
    _UINT32_MAX,
    SonareExternalStemDesc,
    SonareExternalStemImportRequest,
    SonareExternalStemImportResult,
    SonareProjectClipCompSegment,
    SonareProjectClipDesc,
    SonareProjectClipFade,
    SonareProjectClipTake,
    SonareProjectLoopRecordingDesc,
    SonareProjectSource,
    SonareProjectTrackDesc,
    SonareProjectWarpAnchor,
    SonareProjectWarpMapDesc,
    _as_float32_buffer,
    _check,
    _get_lib,
    _mapping_keys,
    _narrow_double,
    _narrow_float,
    _narrow_int,
    _optional_positive_int,
    _planar_channel_arrays,
    _to_c_float_array,
    _to_c_int,
    _to_c_int64,
    _to_c_size_t,
    _to_c_uint8,
    _to_c_uint32,
    _token_ordinal,
    _utf8_arg,
    _warp_mode_value,
)
from ._types_enums import ProjectWarpMode
from .types import ProjectSource

if TYPE_CHECKING:
    from ._types_engine import AutomationPoint

# Mirrors SONARE_PROJECT_LOOP_RECORDING_DROP_PARTIAL_TAIL.
_LOOP_RECORDING_DROP_PARTIAL_TAIL = 1
_TAKE_KEYS = ("id", "source_id", "source_offset_ppq", "name")
_COMP_SEGMENT_KEYS = ("start_ppq", "end_ppq", "take_id", "crossfade_ppq")


_STEM_KEYS = ("name", "layout", "planar_samples", "role", "start_frame")


def _planar_channels(audio: object) -> list[np.ndarray] | None:
    """Return per-channel float32 buffers when ``audio`` is channels-first, else ``None``."""
    if isinstance(audio, np.ndarray):
        if audio.ndim != 2:
            return None
        return [_as_float32_buffer(row, arg_name="audio") for row in audio]
    if isinstance(audio, Sequence) and len(audio) > 0:
        first = audio[0]
        if isinstance(first, np.ndarray) or (
            isinstance(first, Sequence) and not isinstance(first, (str, bytes))
        ):
            return [_as_float32_buffer(row, arg_name="audio") for row in audio]
    return None


def _automation_lane_target_fields(
    target_param_id: int | None,
    target_kind: ProjectAutomationTargetKind | None,
    send_id: str | None,
    insert_index: int | None,
    processor: str | None,
    param_name: str | None,
    *,
    editing: bool,
) -> tuple[int, int]:
    identities = {
        "send_id": send_id,
        "insert_index": insert_index,
        "processor": processor,
        "param_name": param_name,
    }
    if editing and target_kind is None:
        if target_param_id is not None or any(value is not None for value in identities.values()):
            raise _refuse("target identity fields require target_kind")
        return SONARE_AUTOMATION_TARGET_UNCHANGED, 0

    kind_value = _automation_target_kind_value(
        "opaque" if target_kind is None else target_kind, "target_kind"
    )
    kind = _AUTOMATION_TARGET_KINDS[kind_value]
    if kind == "opaque":
        if any(value is not None for value in identities.values()):
            raise _refuse("typed target identity fields require their target_kind")
        if target_param_id is None:
            raise _refuse_type("target_param_id is required for an opaque target")
        return kind_value, _automation_target_param_id_value(target_param_id)

    if target_param_id is not None:
        raise _refuse("target_param_id must be omitted for a typed target")
    allowed = {
        "trackSendDb": {"send_id"},
        "trackInsertParam": {"insert_index", "processor", "param_name"},
    }
    allowed_fields = allowed.get(kind, set())
    unexpected = next(
        (
            name
            for name, value in identities.items()
            if value is not None and name not in allowed_fields
        ),
        None,
    )
    if unexpected is not None:
        raise _refuse(f"{unexpected} is not valid for target_kind {kind}")
    required_fields = {
        "trackSendDb": ("send_id",),
        "trackInsertParam": ("insert_index", "processor", "param_name"),
    }.get(kind, ())
    missing = next((name for name in required_fields if identities[name] is None), None)
    if missing is not None:
        raise _refuse_type(f"{missing} is required for target_kind {kind}")
    return kind_value, 0


class _ProjectEditMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...
        def _source_from_raw(
            self, raw: SonareProjectSource, name_or_uri: str | None = None
        ) -> ProjectSource: ...

    @_entry("Project.set_sample_rate")
    def set_sample_rate(self, sample_rate: int) -> None:
        """Set the project sample rate in Hz.

        Must be an integer number of hertz within ``[8000, 384000]``; anything else
        raises.
        The change is applied through the edit history, so it is undoable.
        """
        _check(
            _get_lib().sonare_project_set_sample_rate(
                self._require_handle(), _to_c_int(sample_rate, "sample_rate")
            )
        )

    @_entry("Project.unresolved_audio_source_ids")
    def unresolved_audio_source_ids(self) -> np.ndarray:
        """Return the ``uint32`` ids of audio sources that need PCM after deserialization."""
        count = ctypes.c_size_t()
        lib = _get_lib()
        _check(
            lib.sonare_project_unresolved_audio_source_count(
                self._require_handle(), ctypes.byref(count)
            )
        )
        ids: list[int] = []
        for index in range(count.value):
            source_id = ctypes.c_uint32()
            _check(
                lib.sonare_project_unresolved_audio_source_id_by_index(
                    self._require_handle(),
                    _to_c_size_t(index, "index"),
                    ctypes.byref(source_id),
                )
            )
            ids.append(int(source_id.value))
        return np.asarray(ids, dtype=np.uint32)

    @_entry("Project.unresolved_audio_sources")
    def unresolved_audio_sources(self) -> list[ProjectSource]:
        """Return descriptors of audio sources that need PCM, with the full URI."""
        count = ctypes.c_size_t()
        lib = _get_lib()
        _check(
            lib.sonare_project_unresolved_audio_source_count(
                self._require_handle(), ctypes.byref(count)
            )
        )
        sources: list[ProjectSource] = []
        for index in range(count.value):
            raw = SonareProjectSource()
            _check(
                lib.sonare_project_unresolved_audio_source_by_index(
                    self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
                )
            )
            sources.append(self._source_from_raw(raw))
        return sources

    @_entry("Project.set_source_audio")
    def set_source_audio(
        self,
        source_id: int,
        channels: Sequence[Sequence[float] | np.ndarray] | np.ndarray,
        sample_rate: int,
    ) -> None:
        """Register decoded, channels-first PCM for an existing audio source."""
        planes, _, frames = _planar_channel_arrays(channels, subject="channels")
        channel_count = _narrow_int(len(planes), "channels", 1, _C_INT_MAX)
        interleaved = np.stack([np.ctypeslib.as_array(plane) for plane in planes], axis=1)
        backing, _ = _to_c_float_array(interleaved.reshape(-1), arg_name="channels")
        channels_c = _to_c_int(channel_count, "channels")

        def respell_source_audio_subject(subject: str) -> _Respelling | None:
            match = re.fullmatch(r"interleaved\[(\d+)\]", subject)
            if match is None:
                return None
            index = int(match.group(1))
            return _Respelling(f"channels[{index % channel_count}][{index // channel_count}]")

        _check(
            _get_lib().sonare_project_set_source_audio(
                self._require_handle(),
                _to_c_uint32(source_id, "source_id"),
                backing,
                _to_c_int64(frames, "frames"),
                channels_c,
                _to_c_int(sample_rate, "sample_rate"),
            ),
            respelled=respell_source_audio_subject,
        )

    @_entry("Project.add_audio_source")
    def add_audio_source(self, uri: str) -> int:
        """Add an unresolved audio source and return its new stable id."""
        encoded_uri = _utf8_arg(uri, "uri")
        if not encoded_uri:
            raise _refuse("uri must not be empty")
        source_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_audio_source(
                self._require_handle(), encoded_uri, ctypes.byref(source_id)
            )
        )
        return int(source_id.value)

    @_entry("Project.relink_source")
    def relink_source(self, source_id: int, uri: str) -> int:
        """Relink an audio source to ``uri`` and return its unchanged id."""
        native_id = _to_c_uint32(source_id, "source_id")
        encoded_uri = _utf8_arg(uri, "uri")
        if not encoded_uri:
            raise _refuse("uri must not be empty")
        _check(
            _get_lib().sonare_project_relink_source(self._require_handle(), native_id, encoded_uri)
        )
        return int(native_id.value)

    @_entry("Project.set_audio_source_metadata")
    def set_audio_source_metadata(
        self, source_id: int, content_hash: str | None, external_stem_role: str | None
    ) -> None:
        """Replace an audio source's metadata strings as one undoable edit.

        Both values are required. ``None`` clears that field and an empty
        string is refused; the source URI, audio content, and other source
        state are kept.
        """
        for arg_name, value in (
            ("content_hash", content_hash),
            ("external_stem_role", external_stem_role),
        ):
            if value is not None and not isinstance(value, str):
                raise _refuse_type(f"{arg_name} must be a string or None")
            if value == "":
                raise _refuse(f"{arg_name} must not be empty; pass None to clear it")
        lib = _get_lib()
        _check(
            lib.sonare_project_set_audio_source_metadata(
                self._require_handle(),
                _to_c_uint32(source_id, "source_id"),
                # The C call reads an empty string as "clear".
                _utf8_arg(content_hash or "", "content_hash"),
                _utf8_arg(external_stem_role or "", "external_stem_role"),
            )
        )

    # -- edit ---------------------------------------------------------------

    @_entry("Project.add_track")
    def add_track(self, kind: ProjectTrackKind = "audio", name: str | None = None) -> int:
        """Add a track and return its allocated stable id.

        Args:
            kind: ``"audio"`` (default), ``"midi"`` or ``"aux"``.
            name: Optional track name.
        """
        if name == "":
            raise _refuse("name must not be empty; omit it for no name")
        desc = SonareProjectTrackDesc(
            kind=_track_kind_value(kind, "kind"),
            name=_utf8_arg(name, "name") if name is not None else None,
        )
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_track(
                self._require_handle(), ctypes.byref(desc), ctypes.byref(out_id)
            ),
            flattened="desc",
        )
        return int(out_id.value)

    @_entry("Project.add_clip")
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
        audio_channels: int | None = None,
        audio_sample_rate: int | None = None,
        source_uri: str | None = None,
    ) -> int:
        """Add an audio or MIDI clip and return its allocated clip id.

        For an audio clip, supply decoded interleaved ``audio`` to make it
        renderable by :meth:`bounce`; omit it for a metadata-only source
        (optionally referenced by ``source_uri``). For a MIDI clip pass
        ``is_midi=True`` and set events later via :meth:`set_midi_events`.

        ``audio_channels`` and ``audio_sample_rate`` (in ``[8000, 384000]``)
        are required whenever ``audio`` is supplied and refused without it: the
        library has no default for the format of audio the caller decoded.
        """
        c_audio = None
        audio_frames = 0
        backing = None
        channels = 0
        rate = 0
        if audio is None:
            if audio_channels is not None or audio_sample_rate is not None:
                raise _refuse(
                    "audio_channels and audio_sample_rate describe audio; omit them without audio"
                )
        else:
            if audio_channels is None or audio_sample_rate is None:
                raise _refuse("audio requires audio_channels and audio_sample_rate")
            rate = _narrow_int(audio_sample_rate, "audio_sample_rate", _C_INT_MIN, _C_INT_MAX)
            backing, total = _to_c_float_array(audio, arg_name="audio")
            c_audio = backing
            # Narrowed before the frame count is derived from it, so a fractional
            # channel count is refused rather than silently becoming its floor.
            channels = _narrow_int(audio_channels, "audio_channels", _C_INT_MIN, _C_INT_MAX)
            if channels <= 0 or total % channels != 0:
                raise _refuse("audio length must be a multiple of audio_channels")
            audio_frames = total // channels
        desc = SonareProjectClipDesc(
            track_id=track_id,
            is_midi=1 if is_midi else 0,
            start_ppq=_narrow_double(start_ppq, "start_ppq"),
            length_ppq=_narrow_double(length_ppq, "length_ppq"),
            source_offset_ppq=_narrow_double(source_offset_ppq, "source_offset_ppq"),
            gain=_narrow_double(gain, "gain"),
            audio_interleaved=c_audio,
            audio_frames=audio_frames,
            audio_channels=channels,
            audio_sample_rate=rate,
            source_uri=_utf8_arg(source_uri, "source_uri") if source_uri is not None else None,
        )
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_clip(
                self._require_handle(), ctypes.byref(desc), ctypes.byref(out_id)
            ),
            flattened="desc",
        )
        # `backing` is kept alive until the call returns above.
        del backing
        return int(out_id.value)

    @_entry("Project.import_external_stems")
    def import_external_stems(
        self,
        sample_rate: int,
        stems: Sequence[Mapping[str, object]],
    ) -> ExternalSeparatedStemImportResult:
        """Import host-separated planar PCM as normal audio tracks and clips.

        Each mapping supplies ``name``, ``layout`` (a :data:`ProjectStemLayout`
        token), ``planar_samples`` (one float array per channel), and optional
        ``role`` / ``start_frame``; any other key is refused by name. Input is
        copied by the native project; it is never resampled, retimed,
        phase-aligned, or gain-compensated. Returns the created track and clip ids
        as ``uint32`` arrays.
        """
        descs: list[SonareExternalStemDesc] = []
        names: list[bytes] = []
        roles: list[bytes | None] = []
        sample_arrays: list[list[np.ndarray]] = []
        plane_arrays: list[ctypes.Array[ctypes._Pointer[ctypes.c_float]]] = []
        for i, raw_stem in enumerate(stems):
            stem = _mapping_keys(raw_stem, _STEM_KEYS, f"stems[{i}]")
            name = stem.get("name")
            planes = stem.get("planar_samples")
            if not isinstance(name, str) or not isinstance(planes, Sequence):
                raise _refuse_type("each stem needs string name and planar_samples")
            layout_value = (
                _token_ordinal(stem.get("layout"), _STEM_LAYOUTS, f"stems[{i}].layout") + 1
            )
            if len(planes) != layout_value:
                raise _refuse("planar_samples must match mono or stereo layout")
            arrays = [_as_float32_buffer(plane, arg_name="planar_samples") for plane in planes]
            if not arrays or any(array.size != arrays[0].size for array in arrays):
                raise _refuse("all planar_samples entries must have equal length")
            names.append(_utf8_arg(name, "name"))
            role = stem.get("role")
            if role is not None and not isinstance(role, str):
                raise _refuse_type("stem role must be a string when supplied")
            roles.append(None if role is None else _utf8_arg(role, "role"))
            sample_arrays.append(arrays)
            c_planes = (ctypes.POINTER(ctypes.c_float) * layout_value)(
                *(array.ctypes.data_as(ctypes.POINTER(ctypes.c_float)) for array in arrays)
            )
            plane_arrays.append(c_planes)
            descs.append(
                SonareExternalStemDesc(
                    name=names[-1],
                    role=roles[-1],
                    layout=layout_value,
                    planar_samples=c_planes,
                    frame_count=int(arrays[0].size),
                    # Passed unconverted so the struct's own narrowing sees the
                    # caller's value; the core refuses a negative start itself.
                    start_frame=stem.get("start_frame", 0),
                )
            )
        c_descs = (SonareExternalStemDesc * len(descs))(*descs)
        request = SonareExternalStemImportRequest(
            sample_rate=sample_rate,
            stems=c_descs,
            stem_count=len(descs),
        )
        result = SonareExternalStemImportResult()
        lib = _get_lib()
        _check(
            lib.sonare_project_import_external_stems(
                self._require_handle(), ctypes.byref(request), ctypes.byref(result)
            ),
            flattened="request",
        )
        try:
            return ExternalSeparatedStemImportResult(
                track_ids=np.array(
                    [result.track_ids[index] for index in range(result.count)], dtype=np.uint32
                ),
                clip_ids=np.array(
                    [result.clip_ids[index] for index in range(result.count)], dtype=np.uint32
                ),
            )
        finally:
            lib.sonare_free_external_stem_import_result(ctypes.byref(result))

    @_entry("Project.add_loop_recording_takes")
    def add_loop_recording_takes(
        self,
        track_id: int,
        start_ppq: float,
        loop_length_ppq: float,
        audio: Sequence[float] | Sequence[Sequence[float]] | np.ndarray,
        *,
        audio_channels: int | None = None,
        audio_sample_rate: int,
        partial_tail: str = "keep",
    ) -> ProjectLoopRecordingResult:
        """Split a captured loop recording into takes and add one clip.

        ``audio`` is either interleaved float32 capture data, which requires
        ``audio_channels``, or planar: a channels-first 2-D array or a sequence
        of equal-length per-channel sequences, whose channel count is the number
        of rows (``audio_channels`` is then refused). The capture's
        ``audio_sample_rate`` is required: the library has no default for the
        format of audio the caller recorded. Each loop-length
        span becomes a separate take and the last take is made active, except
        that a partial last take (the capture does not fill the last loop) is
        kept but not made active when an earlier complete take exists. A capture
        shorter than one loop has only the partial take, which stays active.

        ``partial_tail`` is ``"keep"`` (default) to keep the partial take, or
        ``"drop"`` to not create it. Returns a :class:`ProjectLoopRecordingResult`; a partial
        take is marked ``"partial": true`` in its ``takes`` entry of
        :meth:`to_json`.
        """
        if partial_tail == "keep":
            flags = 0
        elif partial_tail == "drop":
            flags = _LOOP_RECORDING_DROP_PARTIAL_TAIL
        else:
            raise _refuse("partial_tail must be one of keep, drop")
        planar = _planar_channels(audio)
        if planar is not None:
            if not planar:
                raise _refuse("audio must not be empty")
            if any(plane.shape[0] != planar[0].shape[0] for plane in planar):
                raise _refuse("planar audio channels must have equal lengths")
            if audio_channels is not None:
                raise _refuse(
                    "audio_channels must be omitted for planar audio, whose shape carries it"
                )
            channels = len(planar)
            audio = np.stack(planar, axis=1).reshape(-1)
        else:
            if audio_channels is None:
                raise _refuse("audio_channels must be given for interleaved audio")
            # Narrowed before the frame count is derived from it, so a fractional
            # channel count is refused rather than silently becoming its floor.
            channels = _narrow_int(audio_channels, "audio_channels", _C_INT_MIN, _C_INT_MAX)
        sample_rate = _narrow_int(audio_sample_rate, "audio_sample_rate", _C_INT_MIN, _C_INT_MAX)
        backing, total = _to_c_float_array(audio, arg_name="audio")
        if channels <= 0 or total % channels != 0:
            raise _refuse("audio length must be a multiple of audio_channels")
        frames = total // channels
        desc = SonareProjectLoopRecordingDesc(
            track_id=track_id,
            flags=flags,
            start_ppq=_narrow_double(start_ppq, "start_ppq"),
            loop_length_ppq=_narrow_double(loop_length_ppq, "loop_length_ppq"),
            audio_interleaved=backing,
            audio_frames=frames,
            audio_channels=channels,
            audio_sample_rate=sample_rate,
        )
        out_clip = ctypes.c_uint32()
        out_take_count = ctypes.c_size_t()
        _check(
            _get_lib().sonare_project_add_loop_recording_takes(
                self._require_handle(),
                ctypes.byref(desc),
                ctypes.byref(out_clip),
                ctypes.byref(out_take_count),
            ),
            flattened="desc",
        )
        del backing
        return ProjectLoopRecordingResult(
            clip_id=int(out_clip.value), take_count=int(out_take_count.value)
        )

    @_entry("Project.add_midi_clip")
    def add_midi_clip(self, start_ppq: float, length_ppq: float) -> ProjectMidiClipResult:
        """Create a MIDI track + clip; return a :class:`ProjectMidiClipResult`.

        The clip window is half-open, ``[0, length_ppq)``: an event at exactly
        ``length_ppq`` is outside it. Export and validation close a note
        still sounding at the clip end with a note-off there.
        """
        out_track = ctypes.c_uint32()
        out_clip = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_midi_clip(
                self._require_handle(),
                _narrow_double(start_ppq, "start_ppq"),
                _narrow_double(length_ppq, "length_ppq"),
                ctypes.byref(out_track),
                ctypes.byref(out_clip),
            )
        )
        return ProjectMidiClipResult(track_id=int(out_track.value), clip_id=int(out_clip.value))

    @_entry("Project.split_clip")
    def split_clip(self, clip_id: int, split_ppq: float) -> int:
        """Split a clip at ``split_ppq`` (absolute PPQ); return the new clip id."""
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_split_clip(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _narrow_double(split_ppq, "split_ppq"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    @_entry("Project.trim_clip")
    def trim_clip(self, clip_id: int, new_start_ppq: float, new_length_ppq: float) -> None:
        """Trim a clip's start / length (PPQ)."""
        _check(
            _get_lib().sonare_project_trim_clip(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _narrow_double(new_start_ppq, "new_start_ppq"),
                _narrow_double(new_length_ppq, "new_length_ppq"),
            )
        )

    @_entry("Project.move_clip")
    def move_clip(
        self, clip_id: int, new_start_ppq: float, new_track_id: int | None = None
    ) -> None:
        """Move a clip to ``new_start_ppq``, onto ``new_track_id`` when given.

        ``None`` keeps the clip on its track; an explicit 0 is refused, since no
        track takes that id.
        """
        _check(
            _get_lib().sonare_project_move_clip(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _narrow_double(new_start_ppq, "new_start_ppq"),
                _to_c_uint32(
                    _optional_positive_int(new_track_id, "new_track_id", _UINT32_MAX),
                    "new_track_id",
                ),
            )
        )

    @_entry("Project.set_track_kind")
    def set_track_kind(self, track_id: int, kind: ProjectTrackKind) -> None:
        """Change a track kind via an undoable edit command."""
        _check(
            _get_lib().sonare_project_set_track_kind(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint32(_track_kind_value(kind, "kind"), "kind"),
            )
        )

    @_entry("Project.set_clip_warp_ref")
    def set_clip_warp_ref(self, clip_id: int, warp_ref_id: int) -> None:
        """Set a clip's warp reference id (``0`` clears it)."""
        _check(
            _get_lib().sonare_project_set_clip_warp_ref(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_uint32(warp_ref_id, "warp_ref_id"),
            )
        )

    @_entry("Project.set_clip_warp_mode")
    def set_clip_warp_mode(self, clip_id: int, mode: ProjectWarpMode) -> None:
        """Set a clip's warp playback mode."""
        _check(
            _get_lib().sonare_project_set_clip_warp_mode(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_uint32(_warp_mode_value(mode, "mode"), "mode"),
            )
        )

    @_entry("Project.set_clip_takes")
    def set_clip_takes(
        self,
        clip_id: int,
        takes: Sequence[Mapping[str, object]],
        active_take_id: int | None = None,
    ) -> None:
        """Replace a clip's take list and active take via an undoable edit; ``[]`` clears it.

        Each take mapping needs a non-zero ``id``; ``source_id`` absent or ``None`` uses the
        clip's current source, and ``name`` absent or ``None`` leaves the take unnamed.
        ``active_take_id`` ``None`` plays the clip's base source.
        """
        if takes is None:
            raise _refuse_type("takes must be a sequence; pass [] to clear the takes")
        take_items = list(takes)
        c_takes = (SonareProjectClipTake * len(take_items))()
        name_backing: list[bytes] = []
        for i, item in enumerate(take_items):
            where = f"takes[{i}]"
            take = _mapping_keys(item, _TAKE_KEYS, where)
            if take.get("id") is None:
                raise _refuse(f"{where}.id must be given")
            c_takes[i].id = _optional_positive_int(take["id"], f"{where}.id", _UINT32_MAX)
            c_takes[i].source_id = _optional_positive_int(
                take.get("source_id"), f"{where}.source_id", _UINT32_MAX
            )
            c_takes[i].source_offset_ppq = _narrow_double(
                take.get("source_offset_ppq", 0.0), f"{where}.source_offset_ppq"
            )
            name = take.get("name")
            if name is not None:
                if not isinstance(name, str):
                    raise _refuse_type(f"{where}.name must be a string or None")
                if not name:
                    raise _refuse(f"{where}.name must not be empty; omit it for an unnamed take")
                encoded = _utf8_arg(name, "name")
                name_backing.append(encoded)
                c_takes[i].name = encoded
        _check(
            _get_lib().sonare_project_set_clip_takes(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                c_takes if take_items else None,
                len(take_items),
                _to_c_uint32(
                    _optional_positive_int(active_take_id, "active_take_id", _UINT32_MAX),
                    "active_take_id",
                ),
            )
        )
        del name_backing

    @_entry("Project.set_clip_comp_segments")
    def set_clip_comp_segments(
        self,
        clip_id: int,
        segments: Sequence[Mapping[str, object]],
    ) -> None:
        """Replace a clip's comp lane via an undoable edit; ``[]`` clears it.

        A segment's ``take_id`` absent or ``None`` plays the base/active take; 0 is refused.
        """
        if segments is None:
            raise _refuse_type("segments must be a sequence; pass [] to clear the lane")
        segment_items = list(segments)
        c_segments = (SonareProjectClipCompSegment * len(segment_items))()
        for i, item in enumerate(segment_items):
            segment = _mapping_keys(item, _COMP_SEGMENT_KEYS, f"segments[{i}]")
            start_ppq = segment.get("start_ppq")
            end_ppq = segment.get("end_ppq", 0.0)
            crossfade_ppq = segment.get("crossfade_ppq", 0.0)
            c_segments[i].start_ppq = _narrow_double(start_ppq, f"segments[{i}].start_ppq")
            c_segments[i].end_ppq = _narrow_double(end_ppq, f"segments[{i}].end_ppq")
            # Absent or None falls back to the base/active take.
            c_segments[i].take_id = _optional_positive_int(
                segment.get("take_id"),
                f"segments[{i}].take_id",
                _UINT32_MAX,
            )
            c_segments[i].crossfade_ppq = _narrow_double(
                crossfade_ppq, f"segments[{i}].crossfade_ppq"
            )
        _check(
            _get_lib().sonare_project_set_clip_comp_segments(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                c_segments if segment_items else None,
                len(segment_items),
            )
        )

    @_entry("Project.set_warp_map")
    def set_warp_map(
        self,
        warp_ref_id: int,
        anchors: Sequence[tuple[float, float]],
        name: str | None = None,
    ) -> None:
        """Add or replace a first-class project warp map.

        ``name`` labels the map; ``None`` leaves it unnamed and an empty string is refused.
        """
        count = len(anchors)
        c_anchors = (SonareProjectWarpAnchor * count)()
        for i, (warp_sample, source_sample) in enumerate(anchors):
            c_anchors[i].warp_sample = _narrow_double(warp_sample, f"anchors[{i}].warp_sample")
            c_anchors[i].source_sample = _narrow_double(
                source_sample, f"anchors[{i}].source_sample"
            )
        encoded_name = None if name is None else _utf8_arg(name, "name")
        if encoded_name == b"":
            raise _refuse("name must not be empty; omit it for no name")
        desc = SonareProjectWarpMapDesc(
            id=_narrow_int(warp_ref_id, "warp_ref_id", 0, _UINT32_MAX),
            name=encoded_name,
            anchors=c_anchors,
            anchor_count=count,
        )
        _check(
            _get_lib().sonare_project_set_warp_map(self._require_handle(), ctypes.byref(desc)),
            flattened="desc",
        )

    @_entry("Project.remove_warp_map")
    def remove_warp_map(self, warp_ref_id: int) -> None:
        """Remove a first-class project warp map by id."""
        _check(
            _get_lib().sonare_project_remove_warp_map(
                self._require_handle(),
                _to_c_uint32(warp_ref_id, "warp_ref_id"),
            )
        )

    @_entry("Project.set_track_midi_destination")
    def set_track_midi_destination(self, track_id: int, destination_id: int) -> None:
        """Route a track's MIDI to host-instrument ``destination_id``.

        Destination ``0``, which every track starts on, is a real id like any other.

        The compiler stamps every MIDI clip on the track with this id so the
        engine dispatches the clip's events to the instrument registered for that
        destination. Routes through an undoable edit command.

        Builtin, NativeSynth, and SF2 instruments retain source-track
        provenance in a shared destination voice pool. With only zero-latency
        instruments bound, live lanes and channel-strip bounces remain aligned.
        Configure one live lane per source track that needs strip processing.
        A shared opaque or latency-bearing instrument cannot be separated back
        into strip inputs; its channel-strip bounce raises ``SonareError`` with
        ``NOT_SUPPORTED``.
        """
        _check(
            _get_lib().sonare_project_set_track_midi_destination(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint32(destination_id, "destination_id"),
            )
        )

    @_entry("Project.set_part_rig")
    def set_part_rig(
        self,
        destination_id: int,
        part: int,
        *,
        mode: PartRigMode,
        inserts: Sequence[Mapping[str, object]] | None = None,
    ) -> None:
        """Pick the rig for one part of a destination's instrument via an undoable edit.

        ``mode`` is ``"bank"`` (the instrument's own rig), ``"none"`` (dry) or
        ``"chain"``. A chain takes ``inserts``: 1-8 mappings of
        ``{"processor": name, "params": dict}``. ``part`` is
        0..15, or :data:`PART_RIG_ALL_PARTS` for the destination default; a
        ``"bank"`` entry is kept so it can override that default.

        The entry is stored data: bounces apply it to a NativeSynth or SF2
        instrument bound to the destination and ignore it on a builtin or
        callback instrument. Raises :class:`SonareValueError` for malformed
        input and for an out-of-range part or an invalid chain, and
        :class:`SonareError` with ``NOT_SUPPORTED`` for a chain in a build
        without mastering.
        """
        mode_value = _part_rig_mode_value(mode, "mode")
        json_arg = _part_rig_inserts_arg(mode_value, inserts)
        lib = _get_lib()
        _check(
            lib.sonare_project_set_part_rig(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_uint8(part, "part"),
                _to_c_int(mode_value, "mode"),
                json_arg,
            )
        )

    @_entry("Project.part_rig")
    def part_rig(self, destination_id: int, part: int) -> PartRigEntry | None:
        """Read back :meth:`set_part_rig` as a :class:`PartRigEntry`, or ``None`` if unset.

        ``mode`` is the :data:`PartRigMode` token. ``inserts`` is a tuple of
        ``{"processor", "params"}`` dicts for a ``"chain"`` entry
        (``params`` is a dict) and ``None`` otherwise.
        """
        lib = _get_lib()
        mode = ctypes.c_int()
        out_json = ctypes.c_char_p()
        present = ctypes.c_int()
        _check(
            lib.sonare_project_get_part_rig(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_uint8(part, "part"),
                ctypes.byref(mode),
                ctypes.byref(out_json),
                ctypes.byref(present),
            )
        )
        try:
            raw = out_json.value
        finally:
            if out_json.value:
                lib.sonare_free_string(out_json)
        if not present.value:
            return None
        mode_out = _part_rig_mode_name(int(mode.value))
        if raw is None:
            return PartRigEntry(mode=mode_out, inserts=None)
        inserts = tuple(
            {"processor": entry["processor"], "params": entry["params"]}
            for entry in json.loads(raw.decode("utf-8"))
        )
        return PartRigEntry(mode=mode_out, inserts=inserts)

    @_entry("Project.clear_part_rig")
    def clear_part_rig(self, destination_id: int, part: int) -> None:
        """Remove one part-rig entry via an undoable edit (absent entries are a no-op)."""
        lib = _get_lib()
        _check(
            lib.sonare_project_clear_part_rig(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_uint8(part, "part"),
            )
        )

    @_entry("Project.set_track_gain")
    def set_track_gain(self, track_id: int, gain: float) -> None:
        """Set a track's linear playback gain (1.0 = unity; >= 0) via an undoable edit.

        The value reaches the track's audio and MIDI alike, but the stage it lands
        on follows the track's channel strip. A strip bound by this track alone
        (including one synthesized for an unbound track) carries the controls on
        its own fader and panner. A strip several tracks share processes their sum
        and carries none of them; each track applies its controls upstream instead
        -- on its own clip schedules for audio, on its track lane for MIDI.

        A MIDI track's gain/pan on a shared strip ride the track lane, which is
        fed per source track only by an instrument that preserves source-track
        identity (see :meth:`set_track_midi_destination`). An opaque host-callback
        instrument, or one reporting non-zero latency, renders one buffer per
        destination and has no per-track stage on a shared strip, so its gain/pan
        do not reach the bounce there; bind such an instrument to a track with an
        exclusive strip. Mute and solo are unaffected: a silenced MIDI track
        schedules no events at all.
        """
        # Type half shared, semantic half local: the lower bound is this field's own rule.
        g = _narrow_float(gain, "gain")
        if g < 0.0:
            raise _refuse("gain must be at least 0")
        _check(
            _get_lib().sonare_project_set_track_gain(
                self._require_handle(), _to_c_uint32(track_id, "track_id"), g
            )
        )

    @_entry("Project.set_track_mute")
    def set_track_mute(self, track_id: int, mute: bool) -> None:
        """Set a track's mute flag via an undoable edit (a muted track is silent)."""
        _check(
            _get_lib().sonare_project_set_track_mute(
                self._require_handle(), _to_c_uint32(track_id, "track_id"), 1 if mute else 0
            )
        )

    @_entry("Project.set_track_solo")
    def set_track_solo(self, track_id: int, solo: bool) -> None:
        """Set a track's solo flag via an undoable edit.

        When any track is soloed, only soloed tracks sound.
        """
        _check(
            _get_lib().sonare_project_set_track_solo(
                self._require_handle(), _to_c_uint32(track_id, "track_id"), 1 if solo else 0
            )
        )

    @_entry("Project.set_track_pan")
    def set_track_pan(self, track_id: int, pan: float) -> None:
        """Set a track's stereo balance in [-1, +1] (0 = center) via an undoable edit.

        A ``pan`` outside [-1, +1] or non-finite raises :class:`SonareValueError`
        and leaves the project unchanged. See
        :meth:`set_track_gain` for which stage a track's controls land on. The pan
        law that shapes the balance belongs to that stage: the strip's configured
        law on a channel strip and on the clips of an audio track sharing a strip,
        and the track lane's law for a MIDI track sharing a strip (the law of
        whatever strip the host bound to that lane, or a linear balance when none
        is bound). Every law is normalized so a centered track stays at unity and
        only the away channel is attenuated, so the difference is a taper, not a
        level offset.
        """
        p = _narrow_float(pan, "pan")
        _check(
            _get_lib().sonare_project_set_track_pan(
                self._require_handle(), _to_c_uint32(track_id, "track_id"), p
            )
        )

    @_entry("Project.remove_clip")
    def remove_clip(self, clip_id: int) -> None:
        """Remove a clip via an undoable edit command (undo restores it)."""
        _check(
            _get_lib().sonare_project_remove_clip(
                self._require_handle(), _to_c_uint32(clip_id, "clip_id")
            )
        )

    @_entry("Project.set_clip_gain")
    def set_clip_gain(self, clip_id: int, gain: float) -> None:
        """Set a clip's linear playback gain (at least 0; 0 = muted) via an undoable edit."""
        # Type half shared, semantic half local: the lower bound is this field's own rule.
        g = _narrow_float(gain, "gain")
        if g < 0.0:
            raise _refuse("gain must be at least 0")
        _check(
            _get_lib().sonare_project_set_clip_gain(
                self._require_handle(), _to_c_uint32(clip_id, "clip_id"), g
            )
        )

    @_entry("Project.set_clip_fade")
    def set_clip_fade(
        self,
        clip_id: int,
        fade_in_ppq: float = 0.0,
        fade_out_ppq: float = 0.0,
        *,
        fade_in_curve: ProjectFadeCurve = "linear",
        fade_out_curve: ProjectFadeCurve = "linear",
    ) -> None:
        """Set a clip's fade-in and fade-out regions via an undoable edit.

        Each fade length (PPQ) must be finite and in [0, 1e12] (0 = no fade); each curve
        is a token (``"linear"`` / ``"equalPower"`` / ``"exponential"`` /
        ``"logarithmic"``).
        """
        fin = _narrow_double(fade_in_ppq, "fade_in_ppq")
        fout = _narrow_double(fade_out_ppq, "fade_out_ppq")
        if not 0.0 <= fin <= _MAX_PUBLIC_PPQ:
            raise _refuse("fade_in_ppq must be finite and in [0, 1e12]")
        if not 0.0 <= fout <= _MAX_PUBLIC_PPQ:
            raise _refuse("fade_out_ppq must be finite and in [0, 1e12]")
        c_in = SonareProjectClipFade(
            length_ppq=fin, curve=_fade_curve_value(fade_in_curve, "fade_in_curve")
        )
        c_out = SonareProjectClipFade(
            length_ppq=fout, curve=_fade_curve_value(fade_out_curve, "fade_out_curve")
        )
        _check(
            _get_lib().sonare_project_set_clip_fade(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                ctypes.byref(c_in),
                ctypes.byref(c_out),
            )
        )

    @_entry("Project.set_clip_loop")
    def set_clip_loop(
        self,
        clip_id: int,
        loop_mode: ProjectLoopMode = "off",
        loop_length_ppq: float | None = None,
        loop_crossfade_ppq: float = 0.0,
    ) -> None:
        """Set a clip's loop mode + loop length (PPQ) via an undoable edit.

        ``loop_mode`` is ``"off"`` or ``"loop"``. An omitted ``loop_length_ppq``
        loops the entire clip (the length is resolved from the clip's own
        duration); a given one must be in (0, 1e12]. ``loop_crossfade_ppq`` is
        an optional equal-power crossfade at the loop seam (PPQ, in [0, 1e12];
        0 = hard loop); the engine clamps it to the clip's pre-roll and half the
        loop.
        """
        mode = _loop_mode_value(loop_mode, "loop_mode")
        length = 0.0
        if loop_length_ppq is not None:
            length = _narrow_double(loop_length_ppq, "loop_length_ppq")
            if length == 0.0:
                raise _refuse("loop_length_ppq must not be 0; omit it to loop the whole clip")
            if not 0.0 < length <= _MAX_PUBLIC_PPQ:
                raise _refuse("loop_length_ppq must be finite and in (0, 1e12]")
        crossfade = _narrow_double(loop_crossfade_ppq, "loop_crossfade_ppq")
        if not 0.0 <= crossfade <= _MAX_PUBLIC_PPQ:
            raise _refuse("loop_crossfade_ppq must be finite and in [0, 1e12]")
        _check(
            _get_lib().sonare_project_set_clip_loop(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_int(mode, "loop_mode"),
                length,
                crossfade,
            )
        )

    @_entry("Project.set_clip_source")
    def set_clip_source(self, clip_id: int, source_id: int) -> None:
        """Rebind a clip to a different (already-registered) source via an undoable edit."""
        _check(
            _get_lib().sonare_project_set_clip_source(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_uint32(source_id, "source_id"),
            )
        )

    @_entry("Project.duplicate_clip")
    def duplicate_clip(self, clip_id: int, new_start_ppq: float) -> int:
        """Duplicate a clip at ``new_start_ppq`` (same track); return the new clip id."""
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_duplicate_clip(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _narrow_double(new_start_ppq, "new_start_ppq"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    @_entry("Project.remove_track")
    def remove_track(self, track_id: int) -> None:
        """Remove a track (and its clips) via an undoable edit command."""
        _check(
            _get_lib().sonare_project_remove_track(
                self._require_handle(), _to_c_uint32(track_id, "track_id")
            )
        )

    @_entry("Project.rename_track")
    def rename_track(self, track_id: int, name: str | None = None) -> None:
        """Rename a track via an undoable edit command (``None`` = empty name)."""
        _check(
            _get_lib().sonare_project_rename_track(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _utf8_arg(name, "name") if name is not None else None,
            )
        )

    @_entry("Project.set_track_route")
    def set_track_route(
        self, track_id: int, channel_strip_ref: str | None = None, output_target: str | None = None
    ) -> None:
        """Set a track's mixer-strip binding + output target via an undoable edit.

        Pass ``None`` for either field to clear it.
        """
        if channel_strip_ref == "":
            raise _refuse("channel_strip_ref must not be empty")
        if output_target == "":
            raise _refuse("output_target must not be empty")
        _check(
            _get_lib().sonare_project_set_track_route(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _utf8_arg(channel_strip_ref, "channel_strip_ref")
                if channel_strip_ref is not None
                else None,
                _utf8_arg(output_target, "output_target") if output_target is not None else None,
            )
        )

    @_entry("Project.add_automation_lane")
    def add_automation_lane(
        self,
        track_id: int,
        points: Sequence[AutomationPoint] | None = None,
        *,
        target_kind: ProjectAutomationTargetKind | None = None,
        target_param_id: int | None = None,
        send_id: str | None = None,
        insert_index: int | None = None,
        processor: str | None = None,
        param_name: str | None = None,
    ) -> int:
        """Append a project-owned automation lane and return its lane id.

        ``points`` is an optional sequence of :class:`AutomationPoint` records. The
        default target is opaque and requires ``target_param_id``. Typed targets
        take their identity through ``send_id`` or ``insert_index``, ``processor``
        and ``param_name``; they do not take a parameter id.
        """
        kind_value, target_id = _automation_lane_target_fields(
            target_param_id,
            target_kind,
            send_id,
            insert_index,
            processor,
            param_name,
            editing=False,
        )
        desc, _backing = _automation_lane_desc(
            target_id,
            points,
            kind_value,
            send_id=send_id,
            insert_index=insert_index,
            processor=processor,
            param_name=param_name,
        )
        out_lane_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_add_automation_lane(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                ctypes.byref(desc),
                ctypes.byref(out_lane_id),
            ),
            flattened="desc",
        )
        del _backing
        return int(out_lane_id.value)

    @_entry("Project.edit_automation_lane")
    def edit_automation_lane(
        self,
        track_id: int,
        lane_id: int,
        points: Sequence[AutomationPoint] | None = None,
        *,
        target_kind: ProjectAutomationTargetKind | None = None,
        target_param_id: int | None = None,
        send_id: str | None = None,
        insert_index: int | None = None,
        processor: str | None = None,
        param_name: str | None = None,
    ) -> None:
        """Replace a lane by its project-owned id via an undoable edit.

        Omitting ``target_kind`` preserves the lane identity and permits a points-only
        edit. When it is supplied, target fields follow :meth:`add_automation_lane`.
        """
        kind_value, target_id = _automation_lane_target_fields(
            target_param_id,
            target_kind,
            send_id,
            insert_index,
            processor,
            param_name,
            editing=True,
        )
        desc, _backing = _automation_lane_desc(
            target_id,
            points,
            kind_value,
            send_id=send_id,
            insert_index=insert_index,
            processor=processor,
            param_name=param_name,
        )
        _check(
            _get_lib().sonare_project_edit_automation_lane(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint32(lane_id, "lane_id"),
                ctypes.byref(desc),
            ),
            flattened="desc",
        )
        del _backing

    @_entry("Project.remove_automation_lane")
    def remove_automation_lane(self, track_id: int, lane_id: int) -> None:
        """Remove the lane identified by ``lane_id`` via an undoable edit."""
        _check(
            _get_lib().sonare_project_remove_automation_lane(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint32(lane_id, "lane_id"),
            )
        )

    @_entry("Project.undo")
    def undo(self) -> None:
        """Undo the most recent edit (raises when the undo stack is empty)."""
        _check(_get_lib().sonare_project_undo(self._require_handle()))

    @_entry("Project.redo")
    def redo(self) -> None:
        """Redo the most recently undone edit (raises when the redo stack is empty)."""
        _check(_get_lib().sonare_project_redo(self._require_handle()))

    @_entry("Project.clear_history")
    def clear_history(self) -> None:
        """Empty the undo and redo stacks without altering project state."""
        _check(_get_lib().sonare_project_clear_history(self._require_handle()))

    @_entry("Project.set_max_undo_depth")
    def set_max_undo_depth(self, depth: int) -> None:
        """Set undo depth in [1, 500000000] and evict the oldest excess entries.

        A depth outside this interval raises SonareValueError.
        """
        _check(
            _get_lib().sonare_project_set_max_undo_depth(
                self._require_handle(),
                _to_c_size_t(depth, "depth"),
            )
        )

    @_entry("Project.set_max_history_bytes")
    def set_max_history_bytes(self, bytes: int) -> None:
        """Set the combined retained undo/redo history byte cap.

        ``0`` is valid and makes successful edits non-undoable. The value is
        checked against the platform's ``size_t`` range before calling the
        optional additive C ABI symbol.
        """
        value = _narrow_int(bytes, "bytes", 0, _SIZE_T_MAX)

        lib = _get_lib()
        _check(
            lib.sonare_project_set_max_history_bytes(
                self._require_handle(), _to_c_size_t(value, "value")
            )
        )

    # -- MIDI ---------------------------------------------------------------
