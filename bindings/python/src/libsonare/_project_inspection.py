# ruff: noqa: F405
"""Project MIR annotation, metadata, and compilation methods."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, cast, overload

import numpy as np

from ._project_model import *  # noqa: F403
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    SonareProjectAssistSidecar,
    SonareProjectAudioSourceMetadata,
    SonareProjectChordSymbol,
    SonareProjectClip,
    SonareProjectCompileResult,
    SonareProjectKeySegment,
    SonareProjectMarker,
    SonareProjectSource,
    SonareProjectTempoCandidate,
    SonareProjectTempoOptions,
    SonareProjectTempoSegment,
    SonareProjectTimeSignatureSegment,
    SonareProjectTrack,
    SonareValueError,
    _check,
    _get_lib,
    _to_c_float_array,
)
from .types import ProjectClip, ProjectMarker, ProjectSource, ProjectTrack


class _ProjectInspectionMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    @staticmethod
    def _tempo_options(
        adaptive_tempo: bool | None,
        tempo_update_interval_beats: int | None,
        ramp_threshold: float | None,
        include_octave_candidates: bool | None,
    ) -> SonareProjectTempoOptions:
        """Seed the native defaults, then apply whichever were given.

        Seeding rather than zeroing matters: a zeroed ``ramp_threshold`` folds
        the whole take into one tempo segment.
        """
        options: SonareProjectTempoOptions = _get_lib().sonare_project_tempo_options_default()
        if adaptive_tempo is not None:
            options.adaptive_tempo = 1 if adaptive_tempo else 0
        if tempo_update_interval_beats is not None:
            options.tempo_update_interval_beats = int(tempo_update_interval_beats)
        if ramp_threshold is not None:
            options.ramp_threshold = float(ramp_threshold)
        if include_octave_candidates is not None:
            options.include_octave_candidates = 1 if include_octave_candidates else 0
        return options

    def analyze_tempo(
        self,
        audio: Sequence[float] | np.ndarray,
        sample_rate: int,
        adaptive_tempo: bool | None = None,
        tempo_update_interval_beats: int | None = None,
        ramp_threshold: float | None = None,
        include_octave_candidates: bool | None = None,
    ) -> list[dict[str, object]]:
        """Return ranked primary/half/double tempo and meter candidates without editing.

        Args:
            audio: Mono samples.
            sample_rate: Sample rate of ``audio`` in Hz.
            adaptive_tempo: Whether beat tracking may follow a tempo that moves
                during the take. ``None`` keeps the native default, which is
                off: the tracker fits one tempo to the whole take, so on a
                performance that accelerates or breathes every segment the
                bridge emits sits near the take's average. Turn it on for that
                material.
            tempo_update_interval_beats: Beats of context the local tempo
                estimate is read over. Used only when ``adaptive_tempo`` is on.
            ramp_threshold: Relative tempo change at which one tempo segment
                closes and the next opens. Smaller follows the performance more
                closely and emits more segments.
            include_octave_candidates: Whether to rank the half- and
                double-tempo alternatives alongside the primary.

        Returns:
            One dict per ranked candidate, most-supported first.
        """
        c_array, length = _to_c_float_array(audio)
        count = ctypes.c_size_t()
        candidates = (SonareProjectTempoCandidate * 3)()
        options = self._tempo_options(
            adaptive_tempo,
            tempo_update_interval_beats,
            ramp_threshold,
            include_octave_candidates,
        )
        _check(
            _get_lib().sonare_project_analyze_tempo_with_options(
                self._require_handle(),
                c_array,
                ctypes.c_size_t(length),
                int(sample_rate),
                ctypes.byref(options),
                candidates,
                ctypes.c_size_t(len(candidates)),
                ctypes.byref(count),
            )
        )
        labels = ("primary", "half", "double")
        return [
            {
                "bpm": float(candidate.bpm),
                "confidence": float(candidate.confidence),
                "label": labels[candidate.kind] if candidate.kind < len(labels) else "primary",
                "time_signature_count": int(candidate.time_signature_count),
                "time_signature": {
                    "start_ppq": float(candidate.first_time_signature.start_ppq),
                    "numerator": int(candidate.first_time_signature.numerator),
                    "denominator": int(candidate.first_time_signature.denominator),
                },
            }
            for candidate in candidates[: min(count.value, len(candidates))]
        ]

    def auto_tempo(
        self,
        audio: Sequence[float] | np.ndarray,
        sample_rate: int,
        candidate_index: int = 0,
        apply_time_signatures: bool = False,
        adaptive_tempo: bool | None = None,
        tempo_update_interval_beats: int | None = None,
        ramp_threshold: float | None = None,
        include_octave_candidates: bool | None = None,
    ) -> float:
        """Detect tempo from a mono buffer and install it (undoable).

        The scoring arguments mean what they do on :meth:`analyze_tempo`, and
        ``candidate_index`` indexes the ranking that method returns, so pair the
        two on the same values rather than mixing two option sets. Read the
        installed map back with :meth:`tempo_segment_count` and
        :meth:`tempo_segment_by_index`.

        Returns:
            The BPM of the installed map's first segment.
        """
        c_array, length = _to_c_float_array(audio)
        out_bpm = ctypes.c_float()
        options = self._tempo_options(
            adaptive_tempo,
            tempo_update_interval_beats,
            ramp_threshold,
            include_octave_candidates,
        )
        _check(
            _get_lib().sonare_project_auto_tempo_with_options(
                self._require_handle(),
                c_array,
                ctypes.c_size_t(length),
                int(sample_rate),
                ctypes.byref(options),
                ctypes.c_size_t(candidate_index),
                ctypes.c_uint8(apply_time_signatures),
                ctypes.byref(out_bpm),
            )
        )
        return float(out_bpm.value)

    def snap_to_grid(self, ppq: float, strength: float = 1.0, division: int = 1) -> float:
        """Snap a PPQ coordinate to a project-grid line.

        ``division`` is ``0`` for bars, ``1`` for beats, or the number of
        subdivisions per beat (for example, ``4`` for sixteenths). ``strength``
        is in ``[0, 1]`` (0 = no snap, 1 = exact grid line).
        """
        out_ppq = ctypes.c_double()
        _check(
            _get_lib().sonare_project_snap_to_grid_ex(
                self._require_handle(),
                float(ppq),
                float(strength),
                int(division),
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
        c_keys = (SonareProjectKeySegment * count)()
        if c_keys is not None:
            for i, k in enumerate(rows):
                seq = tuple(k)
                if len(seq) < 4:
                    raise SonareValueError(
                        f"keys[{i}] must contain (start_ppq, end_ppq, tonic_pc, mode)"
                    )
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
        c_chords = (SonareProjectChordSymbol * count)()
        # Keep extension arrays and roman-numeral byte strings alive for the call.
        backing: list[object] = []
        if c_chords is not None:
            for i, c in enumerate(rows):
                extension_values = c.get("extensions", []) or []
                if not isinstance(extension_values, Sequence) or isinstance(extension_values, str):
                    raise TypeError("chord extensions must be a sequence")
                ext = [int(cast(int, value)) for value in extension_values]
                ext_count = len(ext)
                c_ext = (
                    (ctypes.c_uint8 * ext_count)(*[int(e) & 0xFF for e in ext])
                    if ext_count
                    else None
                )
                backing.append(c_ext)
                roman = c.get("roman_numeral")
                roman_bytes = roman.encode("utf-8") if isinstance(roman, str) and roman else None
                backing.append(roman_bytes)
                c_chords[i].start_ppq = float(cast(float, c["start_ppq"]))
                c_chords[i].end_ppq = float(cast(float, c["end_ppq"]))
                c_chords[i].root_pc = int(cast(int, c.get("root_pc", 255)))
                c_chords[i].quality = int(cast(int, c.get("quality", 0)))
                c_chords[i].extensions = c_ext
                c_chords[i].extension_count = ctypes.c_size_t(ext_count)
                c_chords[i].slash_bass_pc = int(cast(int, c.get("slash_bass_pc", 255)))
                c_chords[i].roman_numeral = roman_bytes
                c_chords[i].modulation_boundary = 1 if c.get("modulation_boundary") else 0
        _check(
            _get_lib().sonare_project_annotate_chords(
                self._require_handle(), c_chords, ctypes.c_size_t(count)
            )
        )
        del backing

    # -- assist sidecars ----------------------------------------------------

    @overload
    def set_assist_sidecar(
        self,
        module_id: str,
        payload: bytes = b"",
        *,
        schema_version: int = 0,
        target_track_id: int = 0,
        region_start_ppq: float = 0.0,
        region_end_ppq: float = 0.0,
    ) -> None: ...

    @overload
    def set_assist_sidecar(self, module_id: Mapping[str, object]) -> None: ...

    def set_assist_sidecar(
        self,
        module_id: str | Mapping[str, object],
        payload: bytes = b"",
        *,
        schema_version: int = 0,
        target_track_id: int = 0,
        region_start_ppq: float = 0.0,
        region_end_ppq: float = 0.0,
    ) -> None:
        """Add or update an opaque assist sidecar (undoable command).

        Sidecars sharing ``module_id`` + ``target_track_id`` + region scope are
        replaced; otherwise a new one is appended. ``module_id`` must be
        non-empty and ``payload`` is copied opaque bytes. The first argument
        may alternatively be a descriptor mapping with camelCase keys:
        ``moduleId`` (required), ``payload``, ``schemaVersion``,
        ``targetTrackId``, ``regionStartPpq`` and ``regionEndPpq``. Unknown
        descriptor keys are ignored.
        """
        if isinstance(module_id, Mapping):
            if (
                payload != b""
                or schema_version != 0
                or target_track_id != 0
                or region_start_ppq != 0.0
                or region_end_ppq != 0.0
            ):
                raise TypeError("descriptor form cannot be mixed with legacy arguments")
            descriptor = module_id
            resolved_module_id = descriptor.get("moduleId")
            if not isinstance(resolved_module_id, str):
                raise TypeError("moduleId must be a string")
            module_id = resolved_module_id
            payload = cast(bytes, descriptor.get("payload", b""))
            schema_version = cast(int, descriptor.get("schemaVersion", 0))
            target_track_id = cast(int, descriptor.get("targetTrackId", 0))
            region_start_ppq = cast(float, descriptor.get("regionStartPpq", 0.0))
            region_end_ppq = cast(float, descriptor.get("regionEndPpq", 0.0))
        elif not isinstance(module_id, str):
            raise TypeError("module_id must be a string")

        if not module_id:
            raise SonareValueError("module_id must be a non-empty string")
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

    # -- project getters / setters ------------------------------------------

    def get_sample_rate(self) -> float:
        """Return the project sample rate in Hz."""
        out = ctypes.c_double()
        _check(_get_lib().sonare_project_get_sample_rate(self._require_handle(), ctypes.byref(out)))
        return float(out.value)

    def get_overlap_policy(self) -> int:
        """Return the project's clip-overlap policy ordinal."""
        out = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_get_overlap_policy(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def set_overlap_policy(self, policy: int) -> None:
        """Set the project's clip-overlap policy ordinal."""
        _check(_get_lib().sonare_project_set_overlap_policy(self._require_handle(), int(policy)))

    def set_marker(self, marker_id: int, ppq: float, name: str) -> int:
        """Add or replace a marker; return its (possibly newly allocated) id.

        Pass ``marker_id == 0`` to allocate a new marker id.
        """
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_set_marker(
                self._require_handle(),
                int(marker_id),
                float(ppq),
                name.encode("utf-8") if name is not None else None,
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def set_marker_ex(self, marker: ProjectMarker) -> int:
        """Add or replace a marker from a full :class:`ProjectMarker`.

        Carries the marker ``kind`` and (for the key-signature kind) the
        structured key. Pass ``marker.id == 0`` to allocate a new marker id;
        the affected id is returned.
        """
        raw = SonareProjectMarker()
        raw.id = int(marker.id)
        raw.kind = int(marker.kind) & 0xFF
        raw.key_fifths = int(marker.key_fifths)
        raw.key_minor = 1 if marker.key_minor else 0
        raw.ppq = float(marker.ppq)
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_set_marker_ex_name(
                self._require_handle(),
                ctypes.byref(raw),
                (marker.name or "").encode("utf-8"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def marker_by_index(self, index: int) -> ProjectMarker:
        """Read a marker by 0-based stored index, including its kind / key."""
        raw = SonareProjectMarker()
        _check(
            _get_lib().sonare_project_marker_by_index(
                self._require_handle(), ctypes.c_size_t(int(index)), ctypes.byref(raw)
            )
        )
        full_name = ctypes.c_char_p()
        lib = _get_lib()
        _check(
            lib.sonare_project_marker_name_by_index(
                self._require_handle(), ctypes.c_size_t(int(index)), ctypes.byref(full_name)
            )
        )
        try:
            name = full_name.value.decode("utf-8") if full_name.value else ""
        finally:
            lib.sonare_free_string(full_name)
        return ProjectMarker(
            id=int(raw.id),
            ppq=float(raw.ppq),
            name=name,
            kind=int(raw.kind),
            key_fifths=int(raw.key_fifths),
            key_minor=bool(raw.key_minor),
        )

    def track_by_index(self, index: int) -> ProjectTrack:
        """Read a stored project track by 0-based index."""
        raw = SonareProjectTrack()
        _check(
            _get_lib().sonare_project_track_by_index(
                self._require_handle(), int(index), ctypes.byref(raw)
            )
        )
        return ProjectTrack(
            id=int(raw.id),
            kind=int(raw.kind),
            midi_destination_id=int(raw.midi_destination_id),
            gain=float(raw.gain),
            pan=float(raw.pan),
            mute=bool(raw.mute),
            solo=bool(raw.solo),
            name=raw.name.split(b"\0", 1)[0].decode(),
        )

    def clip_by_index(self, index: int) -> ProjectClip:
        """Read a stored project clip by 0-based index."""
        raw = SonareProjectClip()
        _check(
            _get_lib().sonare_project_clip_by_index(
                self._require_handle(), int(index), ctypes.byref(raw)
            )
        )
        return ProjectClip(
            id=int(raw.id),
            track_id=int(raw.track_id),
            source_id=int(raw.source_id),
            source_kind=int(raw.source_kind),
            start_ppq=float(raw.start_ppq),
            length_ppq=float(raw.length_ppq),
            source_offset_ppq=float(raw.source_offset_ppq),
            gain=float(raw.gain),
            loop_mode=int(raw.loop_mode),
            loop_length_ppq=float(raw.loop_length_ppq),
        )

    def source_by_index(self, index: int) -> ProjectSource:
        """Read a stored project source by 0-based index."""
        raw = SonareProjectSource()
        _check(
            _get_lib().sonare_project_source_by_index(
                self._require_handle(), int(index), ctypes.byref(raw)
            )
        )
        content_hash = ""
        external_stem_role = ""
        if int(raw.kind) == 0:  # SONARE_SOURCE_AUDIO; MIDI has no such metadata.
            lib = _get_lib()
            # Metadata is an additive C API.  Keep source inspection usable
            # with an older shared library, where the new fields simply read
            # as their documented empty defaults.
            if hasattr(lib, "sonare_project_get_audio_source_metadata") and hasattr(
                lib, "sonare_project_free_audio_source_metadata"
            ):
                metadata = SonareProjectAudioSourceMetadata()
                try:
                    _check(
                        lib.sonare_project_get_audio_source_metadata(
                            self._require_handle(), int(raw.id), ctypes.byref(metadata)
                        )
                    )
                    content_hash = metadata.content_hash.decode() if metadata.content_hash else ""
                    external_stem_role = (
                        metadata.external_stem_role.decode() if metadata.external_stem_role else ""
                    )
                finally:
                    # The native getter owns both strings even when it reports
                    # a failure after partially filling the descriptor.
                    lib.sonare_project_free_audio_source_metadata(ctypes.byref(metadata))
        return ProjectSource(
            id=int(raw.id),
            kind=int(raw.kind),
            channel_count=int(raw.channel_count),
            storage_handle_id=int(raw.storage_handle_id),
            sample_rate_hint=float(raw.sample_rate_hint),
            name_or_uri=raw.name_or_uri.split(b"\0", 1)[0].decode(),
            content_hash=content_hash,
            external_stem_role=external_stem_role,
        )

    def set_mixer_scene_json(self, scene_json: str) -> None:
        """Replace the project's mixer scene from scene JSON."""
        _check(
            _get_lib().sonare_project_set_mixer_scene_json(
                self._require_handle(),
                scene_json.encode("utf-8"),
            )
        )

    def set_tempo_segments(
        self,
        segments: Sequence[Mapping[str, float] | Sequence[float]],
    ) -> None:
        """Replace the project's tempo map.

        Each segment is a mapping (``start_ppq`` / ``bpm`` / optional
        ``start_sample`` / ``end_bpm``) or a tuple
        ``(start_ppq, bpm, start_sample=ignored, end_bpm=0.0)``. ``start_sample``
        is accepted for ABI/source compatibility but ignored; sample positions
        are derived during normalization. ``end_bpm`` 0 means a constant-tempo
        segment. Pass an empty sequence to clear.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (SonareProjectTempoSegment * count)()
        for i, seg in enumerate(rows):
            if isinstance(seg, Mapping):
                start_ppq = float(seg["start_ppq"])
                bpm = float(seg["bpm"])
                end_bpm = float(seg.get("end_bpm", 0.0))
            else:
                tup = tuple(seg)
                if len(tup) < 2:
                    raise SonareValueError(f"segments[{i}] must contain (start_ppq, bpm)")
                start_ppq = float(tup[0])
                bpm = float(tup[1])
                end_bpm = float(tup[3]) if len(tup) >= 4 else 0.0
            c_segments[i].start_ppq = start_ppq
            c_segments[i].bpm = bpm
            c_segments[i].end_bpm = end_bpm
        _check(
            _get_lib().sonare_project_set_tempo_segments(
                self._require_handle(), c_segments, ctypes.c_size_t(count)
            )
        )

    def set_time_signatures(
        self,
        segments: Sequence[Mapping[str, float] | Sequence[float]],
    ) -> None:
        """Replace the project's time-signature map.

        Each segment is a mapping (``start_ppq`` / ``numerator`` /
        ``denominator``) or a tuple ``(start_ppq, numerator, denominator)``.
        Pass an empty sequence to clear.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (SonareProjectTimeSignatureSegment * count)()
        for i, seg in enumerate(rows):
            if isinstance(seg, Mapping):
                start_ppq = float(seg["start_ppq"])
                numerator = int(seg["numerator"])
                denominator = int(seg["denominator"])
            else:
                tup = tuple(seg)
                if len(tup) < 3:
                    raise SonareValueError(
                        f"segments[{i}] must contain (start_ppq, numerator, denominator)"
                    )
                start_ppq = float(tup[0])
                numerator = int(tup[1])
                denominator = int(tup[2])
            c_segments[i].start_ppq = start_ppq
            c_segments[i].numerator = numerator
            c_segments[i].denominator = denominator
        _check(
            _get_lib().sonare_project_set_time_signatures(
                self._require_handle(), c_segments, ctypes.c_size_t(count)
            )
        )

    def _count(self, fn_name: str) -> int:
        out = ctypes.c_size_t()
        _check(getattr(_get_lib(), fn_name)(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def marker_count(self) -> int:
        """Number of timeline markers in the project."""
        return self._count("sonare_project_marker_count")

    def clip_count(self) -> int:
        """Number of clips in the project."""
        return self._count("sonare_project_clip_count")

    def source_count(self) -> int:
        """Number of registered audio sources in the project."""
        return self._count("sonare_project_source_count")

    def tempo_segment_count(self) -> int:
        """Number of tempo-map segments in the project."""
        return self._count("sonare_project_tempo_segment_count")

    def time_signature_count(self) -> int:
        """Number of time-signature segments in the project."""
        return self._count("sonare_project_time_signature_count")

    def tempo_segment_by_index(self, index: int) -> dict[str, float]:
        """Read a tempo-map segment by 0-based stored index.

        Returns the keys :meth:`set_tempo_segments` accepts, so a segment read
        back can be written straight to another project. ``start_sample`` is not
        among them: a project stores musical positions only, and sample
        positions are derived when it is compiled.

        Raises:
            SonareError: If ``index`` is at or past :meth:`tempo_segment_count`.
        """
        raw = SonareProjectTempoSegment()
        _check(
            _get_lib().sonare_project_tempo_segment_by_index(
                self._require_handle(), ctypes.c_size_t(int(index)), ctypes.byref(raw)
            )
        )
        return {
            "start_ppq": float(raw.start_ppq),
            "bpm": float(raw.bpm),
            "end_bpm": float(raw.end_bpm),
        }

    def time_signature_by_index(self, index: int) -> dict[str, float | int]:
        """Read a time-signature segment by 0-based stored index.

        Raises:
            SonareError: If ``index`` is at or past :meth:`time_signature_count`.
        """
        raw = SonareProjectTimeSignatureSegment()
        _check(
            _get_lib().sonare_project_time_signature_by_index(
                self._require_handle(), ctypes.c_size_t(int(index)), ctypes.byref(raw)
            )
        )
        return {
            "start_ppq": float(raw.start_ppq),
            "numerator": int(raw.numerator),
            "denominator": int(raw.denominator),
        }

    def track_count(self) -> int:
        """Number of tracks in the project."""
        return self._count("sonare_project_track_count")

    # -- compile / render ---------------------------------------------------

    def last_bounce_compile_result(self) -> ProjectCompileResult:
        """Return the compile result captured by the most recent bounce.

        Mirrors :meth:`compile`'s structured :class:`ProjectCompileResult` but
        reads the timeline + diagnostics recorded during the last
        ``bounce*`` call instead of recompiling.

        On a project no bounce has ever run on the result is empty in full:
        ``has_timeline`` is ``False`` and ``diagnostics`` is empty. A failed
        bounce is distinguishable from that state, because a bounce only loses
        its timeline through an error diagnostic and so always reports at least
        one.
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        _check(
            lib.sonare_project_last_bounce_compile_result(
                self._require_handle(), ctypes.byref(result)
            )
        )
        try:
            has_timeline = bool(result.has_timeline)
            messages = result.messages.decode("utf-8") if result.messages else ""
            message_lines = messages.splitlines()
            diagnostics = tuple(
                ProjectDiagnostic(
                    code=int(result.diagnostics[i].code),
                    severity=int(result.diagnostics[i].severity),
                    target_id=int(result.diagnostics[i].target_id),
                    message=message_lines[i] if i < len(message_lines) else "",
                )
                for i in range(int(result.diagnostic_count))
            )
            return ProjectCompileResult(has_timeline, messages, diagnostics)
        finally:
            lib.sonare_project_free_compile_result(ctypes.byref(result))
