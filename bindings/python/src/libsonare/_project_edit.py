# ruff: noqa: F405
"""Project timeline and arrangement editing methods."""

from __future__ import annotations

import ctypes
import math
import operator
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, cast

import numpy as np

from ._project_model import *  # noqa: F403
from ._project_model import (
    _AUTOMATION_TARGET_KIND_UNSET,
    _automation_lane_args,
    _automation_lane_desc,
    _automation_target_kind_value,
    _automation_target_param_id_value,
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
    _C_INT_MAX,
    _C_INT_MIN,
    _SIZE_T_MAX,
    _UINT32_MAX,
    SonareAutomationLaneDescEx,
    SonareExternalStemDesc,
    SonareExternalStemImportRequest,
    SonareExternalStemImportResult,
    SonareProjectClipCompSegment,
    SonareProjectClipDesc,
    SonareProjectClipFade,
    SonareProjectClipTake,
    SonareProjectLoopRecordingDesc,
    SonareProjectTrackDesc,
    SonareProjectWarpAnchor,
    SonareProjectWarpMapDesc,
    SonareTakeAlignConfig,
    SonareTakeAlignment,
    SonareValueError,
    _as_float32_buffer,
    _check,
    _get_lib,
    _guard_buffer,
    _narrow_float,
    _narrow_int,
    _to_c_float_array,
    _to_c_int,
    _to_c_int64,
    _to_c_size_t,
    _to_c_uint32,
    _warp_mode_value,
)


@dataclass(frozen=True)
class TakeAlignment:
    """How well an :func:`align_take_to_reference` alignment was conditioned.

    Every field is descriptive: none of them makes the call fail, and a caller
    deciding what is acceptable supplies its own threshold.

    ``mean_residual_frames`` is the mean absolute frame residual of the alignment
    path around its diagonal trend -- a coarse indicator of how far the alignment
    strayed from a constant rate, not an error bound. ``reference_frames`` /
    ``take_frames`` are the chroma frames each signal produced, named for the
    arguments the caller passed, so their ratio is the overall rate difference
    the anchors encode.
    """

    mean_residual_frames: float
    reference_frames: int
    take_frames: int


@_guard_buffer("reference", "take")
def align_take_to_reference(
    reference: Sequence[float] | list[float] | np.ndarray,
    take: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    hop_length: int | None = None,
    bins_per_octave: int | None = None,
    validate: bool = True,
) -> tuple[list[tuple[float, float]], TakeAlignment]:
    """Align a take to a reference timeline and return anchors placing it under it.

    A chromagram is measured for each signal and the two are aligned, then the
    alignment is reduced to anchors :meth:`Project.set_warp_map` accepts: at
    least two finite, strictly increasing pairs. The reduction is needed rather
    than decorative -- an alignment path advances one axis at a time, so the raw
    correspondence repeats a coordinate wherever one signal carries more frames
    than the other, and those pairs are refused as a warp map.

    **The anchors are oriented for the take's own clip.** The first element of
    each pair is a position on the REFERENCE timeline and the second the
    corresponding position in the TAKE, which is the direction a clip whose
    source is that take needs. Pass them straight to
    :meth:`Project.set_warp_map` for the take's clip, and bind the clip to the
    resulting warp map with :meth:`Project.set_clip_warp_ref`.

    Both signals are read at ``sample_rate``; resample first if they differ,
    since the alignment does no I/O and no rate conversion.

    Args:
        reference: The reference timeline -- the guide take, or the backing track
            the takes were sung against.
        take: The signal to be placed under it. Its length is independent of
            ``reference``'s; a take running at a different rate is the case this
            exists for.
        sample_rate: Sample rate of both buffers in Hz. It has to carry the whole
            chroma grid, whose top bin must sit under Nyquist: at the default
            resolution 8 kHz is enough, and a finer ``bins_per_octave`` raises
            that bin and the rate it needs -- 24 bins per octave already refuses
            8 kHz.
        hop_length: Chroma hop in samples, which sets the time resolution of the
            anchors. ``None`` keeps the library value, and so does ``0``: the
            field has no meaning at 0, so there is no separate default to fill
            in first.
        bins_per_octave: Chroma bins per octave, ``None`` / ``0`` as above. Must
            be a positive **multiple of 12**, because the chroma folds the CQT
            grid onto twelve pitch classes: 12, 24 and 36 are in domain and 13 or
            18 are refused.
        validate: Reject empty / NaN / Inf input (default True).

    Returns:
        ``(anchors, alignment)`` -- the ``(warp_sample, source_sample)`` pairs in
        increasing order, and a :class:`TakeAlignment` describing how well the
        alignment was conditioned. The metadata is how a caller tells a take the
        reference genuinely fits from one it does not.

    Raises:
        SonareValueError: If either buffer is empty or carries a non-finite
            sample, or if a configuration value does not fit its C field.
        SonareError: ``INVALID_PARAMETER`` for a non-positive ``sample_rate``, a
            rate too low to carry the chroma grid, a ``bins_per_octave`` that is
            not a positive multiple of 12, or a pair that yields fewer than two
            distinct anchors -- which is what an unalignable pair looks like, and
            is reported rather than answered with a map a caller cannot use.
            ``NOT_SUPPORTED`` when the library was built without the arrangement
            subsystem.

    Example:
        >>> anchors, alignment = libsonare.align_take_to_reference(guide, take, 44100)
        >>> project.set_warp_map(1, anchors)
        >>> project.set_clip_warp_ref(take_clip, 1)
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_align_take_to_reference"):
        raise RuntimeError(
            "loaded libsonare does not export sonare_align_take_to_reference; "
            "rebuild or upgrade the shared library before calling align_take_to_reference"
        )
    # Left at 0 when the caller supplied nothing, which is what the C entry reads
    # as "library value"; neither field has a meaning at 0, so there is no
    # default-filling call to make first. Assigned unconverted so the struct's
    # own narrowing sees the caller's value rather than a truncation of it.
    config = SonareTakeAlignConfig()
    if hop_length is not None:
        config.hop_length = hop_length
    if bins_per_octave is not None:
        config.bins_per_octave = bins_per_octave

    reference_array, reference_len = _to_c_float_array(
        reference, fn_name="align_take_to_reference", arg_name="reference"
    )
    take_array, take_len = _to_c_float_array(
        take, fn_name="align_take_to_reference", arg_name="take"
    )
    out_anchors = ctypes.POINTER(SonareProjectWarpAnchor)()
    out_count = ctypes.c_size_t()
    out_alignment = SonareTakeAlignment()
    rc = lib.sonare_align_take_to_reference(
        reference_array,
        _to_c_size_t(reference_len, "reference_len"),
        take_array,
        _to_c_size_t(take_len, "take_len"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out_anchors),
        ctypes.byref(out_count),
        ctypes.byref(out_alignment),
    )
    try:
        _check(rc)
        anchors = [
            (float(out_anchors[i].warp_sample), float(out_anchors[i].source_sample))
            for i in range(out_count.value)
        ]
    finally:
        # The array is heap-owned, so it is released whether the rows were read
        # or the read raised part-way. A refused call leaves the pointer NULL.
        if out_anchors:
            lib.sonare_free_warp_anchors(out_anchors)
    return anchors, TakeAlignment(
        mean_residual_frames=float(out_alignment.mean_residual_frames),
        reference_frames=int(out_alignment.reference_frames),
        take_frames=int(out_alignment.take_frames),
    )


class _ProjectEditMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    def set_sample_rate(self, sample_rate: float) -> None:
        """Set the project sample rate in Hz.

        Must be in ``[8000, 384000]``; anything outside that range raises.
        The change is applied through the edit history, so it is undoable.
        """
        _check(
            _get_lib().sonare_project_set_sample_rate(self._require_handle(), float(sample_rate))
        )

    def unresolved_audio_source_ids(self) -> list[int]:
        """Return audio source ids that need PCM after deserialization."""
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
        return ids

    def set_source_audio(
        self, source_id: int, audio: Sequence[float] | np.ndarray, channels: int, sample_rate: int
    ) -> None:
        """Register decoded interleaved PCM for an existing audio source (undoable)."""
        backing, total = _to_c_float_array(audio, arg_name="audio")
        if channels <= 0 or total % channels != 0:
            raise SonareValueError("audio length must be a multiple of channels")
        # Narrowed before the frame count is derived from it, so a `channels`
        # the C type cannot hold is refused under its own name.
        channel_count = _to_c_int(channels, "channels")
        _check(
            _get_lib().sonare_project_set_source_audio(
                self._require_handle(),
                _to_c_uint32(source_id, "source_id"),
                backing,
                _to_c_int64(total // channel_count.value, "frames"),
                channel_count,
                _to_c_int(sample_rate, "sample_rate"),
            )
        )

    def set_audio_source_metadata(
        self, source_id: int, content_hash: str, external_stem_role: str
    ) -> None:
        """Replace an audio source's metadata strings as one undoable edit.

        Both values are required strings. Passing an empty string clears that
        field; the source URI, audio content, and other source state are kept.
        """
        if not isinstance(content_hash, str) or not isinstance(external_stem_role, str):
            raise TypeError("content_hash and external_stem_role must be strings")
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_set_audio_source_metadata"):
            raise RuntimeError("loaded libsonare does not support audio-source metadata")
        _check(
            lib.sonare_project_set_audio_source_metadata(
                self._require_handle(),
                _to_c_uint32(source_id, "source_id"),
                content_hash.encode("utf-8"),
                external_stem_role.encode("utf-8"),
            )
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

        ``audio_sample_rate`` is required whenever ``audio`` is supplied and
        must be in ``[8000, 384000]``: its 0 default is not a "use the project
        rate" sentinel, and leaving it at 0 alongside ``audio`` is rejected.
        """
        c_audio = None
        audio_frames = 0
        backing = None
        if audio is not None:
            backing, total = _to_c_float_array(audio, arg_name="audio")
            c_audio = backing
            # Narrowed before the frame count is derived from it, so a fractional
            # channel count is refused rather than silently becoming its floor.
            channels = _narrow_int(audio_channels, "audio_channels", _C_INT_MIN, _C_INT_MAX)
            if channels <= 0 or total % channels != 0:
                raise SonareValueError("audio length must be a multiple of audio_channels")
            audio_frames = total // channels
        desc = SonareProjectClipDesc(
            track_id=track_id,
            is_midi=1 if is_midi else 0,
            start_ppq=float(start_ppq),
            length_ppq=float(length_ppq),
            source_offset_ppq=float(source_offset_ppq),
            gain=float(gain),
            audio_interleaved=c_audio,
            audio_frames=audio_frames,
            audio_channels=channels if audio is not None else 0,
            audio_sample_rate=audio_sample_rate if audio is not None else 0,
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

    def import_external_stems(
        self,
        sample_rate: int,
        stems: Sequence[Mapping[str, object]],
    ) -> tuple[list[int], list[int]]:
        """Import host-separated planar PCM as normal audio tracks and clips.

        Each mapping supplies ``name``, ``layout`` (``"mono"``/``"stereo"``
        or 1/2), ``planar_samples`` (one float array per channel), and optional
        ``role`` / ``start_frame``. Input is copied by the native project; it
        is never resampled, retimed, phase-aligned, or gain-compensated.
        """
        descs: list[SonareExternalStemDesc] = []
        names: list[bytes] = []
        roles: list[bytes | None] = []
        sample_arrays: list[list[np.ndarray]] = []
        plane_arrays: list[ctypes.Array[ctypes._Pointer[ctypes.c_float]]] = []
        for stem in stems:
            name = stem.get("name")
            layout = stem.get("layout")
            planes = stem.get("planar_samples")
            if not isinstance(name, str) or not isinstance(planes, Sequence):
                raise TypeError("each stem needs string name and planar_samples")
            layout_value: object = 1 if layout == "mono" else 2 if layout == "stereo" else layout
            if layout_value not in (1, 2) or len(planes) != layout_value:
                raise SonareValueError("planar_samples must match mono or stereo layout")
            arrays = [_as_float32_buffer(plane, arg_name="planar_samples") for plane in planes]
            if not arrays or any(array.size != arrays[0].size for array in arrays):
                raise SonareValueError("all planar_samples entries must have equal length")
            names.append(name.encode("utf-8"))
            role = stem.get("role")
            if role is not None and not isinstance(role, str):
                raise TypeError("stem role must be a string when supplied")
            roles.append(None if role is None else role.encode("utf-8"))
            sample_arrays.append(arrays)
            c_planes = (ctypes.POINTER(ctypes.c_float) * int(cast(int, layout_value)))(
                *(array.ctypes.data_as(ctypes.POINTER(ctypes.c_float)) for array in arrays)
            )
            plane_arrays.append(c_planes)
            descs.append(
                SonareExternalStemDesc(
                    name=names[-1],
                    role=roles[-1],
                    layout=int(cast(int, layout_value)),
                    planar_samples=c_planes,
                    frame_count=int(arrays[0].size),
                    # Passed unconverted so the struct's own narrowing sees the
                    # caller's value; the core refuses a negative start itself.
                    start_frame=stem.get("start_frame", 0),
                )
            )
        c_descs = (SonareExternalStemDesc * len(descs))(*descs)
        request = SonareExternalStemImportRequest(
            struct_version=0,
            sample_rate=sample_rate,
            stems=c_descs,
            stem_count=len(descs),
        )
        result = SonareExternalStemImportResult()
        lib = _get_lib()
        _check(
            lib.sonare_project_import_external_stems(
                self._require_handle(), ctypes.byref(request), ctypes.byref(result)
            )
        )
        try:
            return (
                [int(result.track_ids[index]) for index in range(result.count)],
                [int(result.clip_ids[index]) for index in range(result.count)],
            )
        finally:
            lib.sonare_free_external_stem_import_result(ctypes.byref(result))

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
        # Narrowed before the frame count is derived from it, so a fractional
        # channel count is refused rather than silently becoming its floor.
        channels = _narrow_int(audio_channels, "audio_channels", _C_INT_MIN, _C_INT_MAX)
        backing, total = _to_c_float_array(audio, arg_name="audio")
        if channels <= 0 or total % channels != 0:
            raise SonareValueError("audio length must be a multiple of audio_channels")
        frames = total // channels
        desc = SonareProjectLoopRecordingDesc(
            track_id=track_id,
            reserved=0,
            start_ppq=float(start_ppq),
            loop_length_ppq=float(loop_length_ppq),
            audio_interleaved=backing,
            audio_frames=frames,
            audio_channels=channels,
            audio_sample_rate=audio_sample_rate,
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
                _to_c_uint32(clip_id, "clip_id"),
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
                _to_c_uint32(clip_id, "clip_id"),
                float(new_start_ppq),
                float(new_length_ppq),
            )
        )

    def move_clip(self, clip_id: int, new_start_ppq: float, new_track_id: int = 0) -> None:
        """Move a clip to ``new_start_ppq`` (and optionally ``new_track_id``)."""
        _check(
            _get_lib().sonare_project_move_clip(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                float(new_start_ppq),
                _to_c_uint32(new_track_id, "new_track_id"),
            )
        )

    def set_track_kind(self, track_id: int, kind: str | int) -> None:
        """Change a track kind via an undoable edit command."""
        _check(
            _get_lib().sonare_project_set_track_kind(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint32(_track_kind_value(kind), "kind"),
            )
        )

    def set_clip_warp_ref(self, clip_id: int, warp_ref_id: int) -> None:
        """Set a clip's warp reference id (``0`` clears it)."""
        _check(
            _get_lib().sonare_project_set_clip_warp_ref(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_uint32(warp_ref_id, "warp_ref_id"),
            )
        )

    def set_clip_warp_mode(self, clip_id: int, mode: str | int) -> None:
        """Set a clip's warp playback mode."""
        _check(
            _get_lib().sonare_project_set_clip_warp_mode(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_uint32(_warp_mode_value(mode), "mode"),
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
                take_id = item.get("id", 0)
                source_id = item.get("source_id", item.get("sourceId", 0))
                source_offset = float(
                    item.get("source_offset_ppq", item.get("sourceOffsetPpq", 0.0))
                )
                name = item.get("name", "")
            else:
                take_id, source_id, source_offset, name = cast(tuple[int, int, float, str], item)
            # Narrowed rather than coerced, for the reason source_id gives below:
            # int(0.5) is a 0 the take list then carries as a real take id.
            c_takes[i].id = _narrow_int(take_id, f"set_clip_takes: takes[{i}].id", 0, _UINT32_MAX)
            # Narrowed rather than coerced: int(0.5) is the 0 this field reads as
            # "use the clip's current source", so a fractional source id would
            # attach the clip's own source and report success.
            c_takes[i].source_id = _narrow_int(
                source_id, f"set_clip_takes: takes[{i}].source_id", 0, _UINT32_MAX
            )
            c_takes[i].source_offset_ppq = float(source_offset)
            if name:
                encoded = str(name).encode("utf-8")
                name_backing.append(encoded)
                c_takes[i].name = encoded
        _check(
            _get_lib().sonare_project_set_clip_takes(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                c_takes if take_items else None,
                len(take_items),
                # Narrowed rather than coerced, like takes[].source_id above:
                # int(0.5) is the 0 this argument reads as "use the clip's base
                # source", so a fractional take id would select that and report
                # success.
                _to_c_uint32(active_take_id, "set_clip_takes: active_take_id"),
            )
        )
        del name_backing

    def set_clip_comp_segments(
        self,
        clip_id: int,
        segments: Sequence[Mapping[str, object]]
        | Sequence[tuple[float, float, int]]
        | Sequence[tuple[float, float, int, float]]
        | None,
    ) -> None:
        """Replace a clip's comp lane via an undoable edit."""
        segment_items = list(segments or [])
        c_segments = (SonareProjectClipCompSegment * len(segment_items))()
        for i, item in enumerate(segment_items):
            if isinstance(item, Mapping):
                start_ppq = float(item.get("start_ppq", item.get("startPpq", 0.0)))
                end_ppq = float(item.get("end_ppq", item.get("endPpq", 0.0)))
                take_id = item.get("take_id", item.get("takeId", 0))
                crossfade_ppq = float(item.get("crossfade_ppq", item.get("crossfadePpq", 0.0)))
            elif len(item) == 4:
                start_ppq, end_ppq, take_id, crossfade_ppq = cast(
                    tuple[float, float, int, float], item
                )
            else:
                start_ppq, end_ppq, take_id = cast(tuple[float, float, int], item)
                crossfade_ppq = 0.0
            c_segments[i].start_ppq = float(start_ppq)
            c_segments[i].end_ppq = float(end_ppq)
            # Narrowed rather than coerced: int(0.5) is the 0 this field reads as
            # "fall back to the base/active take", so a fractional take id would
            # select that fallback and report success.
            c_segments[i].take_id = _narrow_int(
                take_id, f"set_clip_comp_segments: segments[{i}].take_id", 0, _UINT32_MAX
            )
            c_segments[i].crossfade_ppq = float(crossfade_ppq)
        _check(
            _get_lib().sonare_project_set_clip_comp_segments(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
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
            # Passed unconverted so the struct's own narrowing sees the caller's
            # value; int() would truncate a fraction past it.
            id=warp_ref_id,
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
                _to_c_uint32(warp_ref_id, "warp_ref_id"),
            )
        )

    def set_track_midi_destination(self, track_id: int, destination_id: int) -> None:
        """Route a track's MIDI to host-instrument ``destination_id`` (0 = default).

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
        # Type half shared, semantic half local: ">= 0" is this field's own rule.
        g = _narrow_float(gain, "gain")
        if g < 0.0:
            raise SonareValueError("gain must be a finite number >= 0")
        _check(
            _get_lib().sonare_project_set_track_gain(
                self._require_handle(), _to_c_uint32(track_id, "track_id"), g
            )
        )

    def set_track_mute(self, track_id: int, mute: bool) -> None:
        """Set a track's mute flag via an undoable edit (a muted track is silent)."""
        _check(
            _get_lib().sonare_project_set_track_mute(
                self._require_handle(), _to_c_uint32(track_id, "track_id"), 1 if mute else 0
            )
        )

    def set_track_solo(self, track_id: int, solo: bool) -> None:
        """Set a track's solo flag via an undoable edit.

        When any track is soloed, only soloed tracks sound.
        """
        _check(
            _get_lib().sonare_project_set_track_solo(
                self._require_handle(), _to_c_uint32(track_id, "track_id"), 1 if solo else 0
            )
        )

    def set_track_pan(self, track_id: int, pan: float) -> None:
        """Set a track's stereo balance in [-1, +1] (0 = center) via an undoable edit.

        ``pan`` is clamped to the valid range by the core. See
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

    def remove_clip(self, clip_id: int) -> None:
        """Remove a clip via an undoable edit command (undo restores it)."""
        _check(
            _get_lib().sonare_project_remove_clip(
                self._require_handle(), _to_c_uint32(clip_id, "clip_id")
            )
        )

    def set_clip_gain(self, clip_id: int, gain: float) -> None:
        """Set a clip's linear playback gain (>= 0; 0 = muted) via an undoable edit."""
        # Type half shared, semantic half local: ">= 0" is this field's own rule.
        g = _narrow_float(gain, "gain")
        if g < 0.0:
            raise SonareValueError("gain must be a finite number >= 0")
        _check(
            _get_lib().sonare_project_set_clip_gain(
                self._require_handle(), _to_c_uint32(clip_id, "clip_id"), g
            )
        )

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
            raise SonareValueError("fade_in_length_ppq must be a finite number >= 0")
        if not math.isfinite(fout) or fout < 0.0:
            raise SonareValueError("fade_out_length_ppq must be a finite number >= 0")
        c_in = SonareProjectClipFade(length_ppq=fin, curve=_fade_curve_value(fade_in_curve))
        c_out = SonareProjectClipFade(length_ppq=fout, curve=_fade_curve_value(fade_out_curve))
        _check(
            _get_lib().sonare_project_set_clip_fade(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
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
            raise SonareValueError("loop_length_ppq must be a finite number >= 0")
        if not math.isfinite(crossfade) or crossfade < 0.0:
            raise SonareValueError("loop_crossfade_ppq must be a finite number >= 0")
        _check(
            _get_lib().sonare_project_set_clip_loop(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_int(mode, "loop_mode"),
                length,
                crossfade,
            )
        )

    def set_clip_source(self, clip_id: int, source_id: int) -> None:
        """Rebind a clip to a different (already-registered) source via an undoable edit."""
        _check(
            _get_lib().sonare_project_set_clip_source(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_uint32(source_id, "source_id"),
            )
        )

    def duplicate_clip(self, clip_id: int, new_start_ppq: float) -> int:
        """Duplicate a clip at ``new_start_ppq`` (same track); return the new clip id."""
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_duplicate_clip(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                float(new_start_ppq),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def remove_track(self, track_id: int) -> None:
        """Remove a track (and its clips) via an undoable edit command."""
        _check(
            _get_lib().sonare_project_remove_track(
                self._require_handle(), _to_c_uint32(track_id, "track_id")
            )
        )

    def rename_track(self, track_id: int, name: str | None = None) -> None:
        """Rename a track via an undoable edit command (``None`` = empty name)."""
        _check(
            _get_lib().sonare_project_rename_track(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
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
                _to_c_uint32(track_id, "track_id"),
                channel_strip_ref.encode("utf-8") if channel_strip_ref is not None else None,
                output_target.encode("utf-8") if output_target is not None else None,
            )
        )

    def add_automation_lane(
        self,
        track_id: int,
        target_param_id: int | Mapping[str, object],
        points: (
            Sequence[tuple[float, float, int | str]]
            | Sequence[Sequence[float]]
            | Sequence[Mapping[str, object]]
            | Mapping[str, object]
            | None
        ) = None,
        target_kind: ProjectAutomationTargetKind | None = _AUTOMATION_TARGET_KIND_UNSET,
    ) -> int:
        """Append an automation lane and return its stable target parameter id.

        ``points`` is an optional list of ``(ppq, value, curve)`` breakpoints
        (``curve`` is an :class:`AutomationCurve` ordinal / name; default linear).
        ``target_param_id`` must be non-zero; zero is reserved as the unset id.
        Omit ``target_kind`` to use the legacy opaque-lane ABI. Supplying
        ``"opaque"``/``0``, ``"track-fader-db"``/``1``, or
        ``"track-pan"``/``2`` uses the typed extended ABI.
        A mapping descriptor with ``target_param_id``/``targetParamId``,
        ``points``, and optional ``target_kind``/``targetKind`` is also
        accepted for cross-binding parity.
        """
        lane_target, lane_points, lane_kind = _automation_lane_args(
            target_param_id, points, target_kind
        )
        desc, _backing = _automation_lane_desc(lane_target, lane_points)
        kind = (
            None
            if lane_kind is _AUTOMATION_TARGET_KIND_UNSET
            else _automation_target_kind_value(lane_kind)
        )
        lib = _get_lib()
        out_target_param_id = ctypes.c_uint32()
        if kind is None:
            # Keep the legacy call shape and symbol selection exact when the
            # optional kind is absent. Native legacy edits preserve an
            # existing lane's typed kind by contract.
            _check(
                lib.sonare_project_add_automation_lane(
                    self._require_handle(),
                    _to_c_uint32(track_id, "track_id"),
                    ctypes.byref(desc),
                    ctypes.byref(out_target_param_id),
                )
            )
        else:
            if not hasattr(lib, "sonare_project_add_automation_lane_ex"):
                raise RuntimeError("loaded libsonare does not support typed automation lanes")
            desc_ex = SonareAutomationLaneDescEx(
                target_param_id=desc.target_param_id,
                target_kind=kind,
                points=desc.points,
                point_count=desc.point_count,
            )
            _check(
                lib.sonare_project_add_automation_lane_ex(
                    self._require_handle(),
                    _to_c_uint32(track_id, "track_id"),
                    ctypes.byref(desc_ex),
                    ctypes.byref(out_target_param_id),
                )
            )
        del _backing
        return int(out_target_param_id.value)

    def edit_automation_lane(
        self,
        track_id: int,
        target_param_id: int | Mapping[str, object],
        points: (
            Sequence[tuple[float, float, int | str]]
            | Sequence[Sequence[float]]
            | Sequence[Mapping[str, object]]
            | Mapping[str, object]
            | None
        ) = None,
        target_kind: ProjectAutomationTargetKind | None = _AUTOMATION_TARGET_KIND_UNSET,
    ) -> None:
        """Replace an automation lane via an undoable edit.

        Omit ``target_kind`` to use the legacy edit ABI, which preserves the
        existing lane kind. Supplying a kind applies it through the typed
        extended ABI and rejects same-track typed-target conflicts atomically.
        A mapping descriptor is accepted in the same form as
        :meth:`add_automation_lane`; the edit target id remains the second
        positional argument when using the historical form.
        """
        lane_target, lane_points, lane_kind = _automation_lane_args(
            target_param_id, points, target_kind
        )
        desc, _backing = _automation_lane_desc(lane_target, lane_points)
        target_id = _automation_target_param_id_value(lane_target)
        kind = (
            None
            if lane_kind is _AUTOMATION_TARGET_KIND_UNSET
            else _automation_target_kind_value(lane_kind)
        )
        lib = _get_lib()
        if kind is None:
            _check(
                lib.sonare_project_edit_automation_lane(
                    self._require_handle(),
                    _to_c_uint32(track_id, "track_id"),
                    _to_c_uint32(target_id, "target_id"),
                    ctypes.byref(desc),
                )
            )
        else:
            if not hasattr(lib, "sonare_project_edit_automation_lane_ex"):
                raise RuntimeError("loaded libsonare does not support typed automation lanes")
            desc_ex = SonareAutomationLaneDescEx(
                target_param_id=desc.target_param_id,
                target_kind=kind,
                points=desc.points,
                point_count=desc.point_count,
            )
            _check(
                lib.sonare_project_edit_automation_lane_ex(
                    self._require_handle(),
                    _to_c_uint32(track_id, "track_id"),
                    _to_c_uint32(target_id, "target_id"),
                    ctypes.byref(desc_ex),
                )
            )
        del _backing

    def remove_automation_lane(self, track_id: int, target_param_id: int) -> None:
        """Remove the lane identified by ``target_param_id`` via an undoable edit."""
        target_id = _automation_target_param_id_value(target_param_id)
        _check(
            _get_lib().sonare_project_remove_automation_lane(
                self._require_handle(),
                _to_c_uint32(track_id, "track_id"),
                _to_c_uint32(target_id, "target_id"),
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
                _to_c_size_t(depth, "depth"),
            )
        )

    def set_max_history_bytes(self, bytes: int) -> None:
        """Set the combined retained undo/redo history byte cap.

        ``0`` is valid and makes successful edits non-undoable. The value is
        checked against the platform's ``size_t`` range before calling the
        optional additive C ABI symbol.
        """
        if isinstance(bytes, bool):
            raise SonareValueError("bytes must be a non-negative integer within size_t range")
        try:
            value = operator.index(bytes)
        except TypeError:
            raise SonareValueError(
                "bytes must be a non-negative integer within size_t range"
            ) from None
        if value < 0 or value > _SIZE_T_MAX:
            raise SonareValueError("bytes must be a non-negative integer within size_t range")

        lib = _get_lib()
        if not hasattr(lib, "sonare_project_set_max_history_bytes"):
            raise RuntimeError(
                "loaded libsonare does not export sonare_project_set_max_history_bytes; "
                "rebuild or upgrade the shared library before calling "
                "Project.set_max_history_bytes"
            )
        _check(
            lib.sonare_project_set_max_history_bytes(
                self._require_handle(), _to_c_size_t(value, "value")
            )
        )

    # -- MIDI ---------------------------------------------------------------
