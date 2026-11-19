# ruff: noqa: F405
"""Project MIR annotation, metadata, and compilation methods."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, cast

import numpy as np

from ._engine_conversions import _marker_kind_name, _marker_kind_value
from ._errors import _entry, _refuse, _refuse_type
from ._narrowing import _narrow_double
from ._project_model import *  # noqa: F403
from ._project_model import (
    _LOOP_MODES,
    _NO_PITCH_CLASS,
    _OVERLAP_POLICIES,
    _PROJECT_CHORD_QUALITIES,
    _PROJECT_KEY_MODES,
    _SOURCE_KINDS,
    _TRACK_KINDS,
    ProjectLoopMode,
    ProjectSourceKind,
    ProjectTrackKind,
    _absent_or_in_range,
    _marker_name_arg,
)
from ._project_render import _compile_diagnostics
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    _METER_SEGMENT_KEYS,
    _TEMPO_SEGMENT_KEYS,
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
    _check,
    _get_lib,
    _guard_buffer,
    _mapping_keys,
    _narrow_int,
    _ordinal_token,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _to_c_uint8,
    _to_c_uint32,
    _token_ordinal,
    _utf8_arg,
)
from ._types_engine import MarkerKind
from .types import ProjectClip, ProjectMarker, ProjectSource, ProjectTrack

_KEY_SEGMENT_KEYS = ("start_ppq", "end_ppq", "tonic_pc", "mode")
_CHORD_KEYS = (
    "start_ppq",
    "end_ppq",
    "root_pc",
    "quality",
    "extensions",
    "slash_bass_pc",
    "roman_numeral",
    "modulation_boundary",
)


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
            # Assigned unconverted so the struct's own narrowing sees the
            # caller's value; int() would truncate a fraction past it.
            options.tempo_update_interval_beats = tempo_update_interval_beats
        if ramp_threshold is not None:
            options.ramp_threshold = _narrow_double(ramp_threshold, "ramp_threshold")
        if include_octave_candidates is not None:
            options.include_octave_candidates = 1 if include_octave_candidates else 0
        return options

    @_entry("Project.analyze_tempo")
    @_guard_buffer("audio")
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

        Raises:
            SonareValueError: If ``audio`` is empty or holds a NaN or Inf sample.
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
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
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

    @_entry("Project.auto_tempo")
    @_guard_buffer("audio")
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
        installed map back with :meth:`tempo_segments`.

        Returns:
            The BPM of the installed map's first segment.

        Raises:
            SonareValueError: If ``audio`` is empty or holds a NaN or Inf sample.
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
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(options),
                _to_c_size_t(candidate_index, "candidate_index"),
                _to_c_uint8(1 if apply_time_signatures else 0, "apply_time_signatures"),
                ctypes.byref(out_bpm),
            )
        )
        return float(out_bpm.value)

    @_entry("Project.snap_to_grid")
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
                _narrow_double(ppq, "ppq"),
                _narrow_double(strength, "strength"),
                _to_c_int(division, "division"),
                ctypes.byref(out_ppq),
            )
        )
        return float(out_ppq.value)

    @_entry("Project.annotate_keys")
    def annotate_keys(self, keys: Sequence[Mapping[str, object]]) -> None:
        """Replace the project's key annotation stream via an undoable command.

        Each key is a mapping with ``start_ppq``, ``end_ppq``, an optional
        ``tonic_pc`` (0..11, C=0; absent or ``None`` when the tonic is unknown)
        and an optional ``mode`` (a :data:`ProjectKeyMode` token; absent or
        ``None`` is ``"unknown"``). Pass an empty sequence to clear.
        """
        rows = list(keys)
        count = len(rows)
        c_keys = (SonareProjectKeySegment * count)()
        for i, key in enumerate(rows):
            k = _mapping_keys(key, _KEY_SEGMENT_KEYS, f"keys[{i}]")
            c_keys[i].start_ppq = _narrow_double(k["start_ppq"], f"keys[{i}].start_ppq")
            c_keys[i].end_ppq = _narrow_double(k["end_ppq"], f"keys[{i}].end_ppq")
            c_keys[i].tonic_pc = _absent_or_in_range(
                k.get("tonic_pc"), f"keys[{i}].tonic_pc", 11, _NO_PITCH_CLASS
            )
            mode = k.get("mode")
            if mode is not None:
                c_keys[i].mode = _token_ordinal(mode, _PROJECT_KEY_MODES, f"keys[{i}].mode")
        _check(
            _get_lib().sonare_project_annotate_keys(
                self._require_handle(), c_keys, _to_c_size_t(count, "count")
            )
        )

    @_entry("Project.annotate_chords")
    def annotate_chords(self, chords: Sequence[dict[str, object]]) -> None:
        """Replace the project's chord-symbol annotation stream (undoable command).

        Each chord is a mapping with keys ``start_ppq``, ``end_ppq``, ``root_pc``
        (0..11, absent or ``None`` when the root is unknown), ``quality``
        (a :data:`ProjectChordQuality` token; absent or ``None`` is
        ``"unknown"``), optional ``extensions`` (iterable of scale degrees
        such as 7, 9, 13, up to 32), ``slash_bass_pc`` (0..11, absent or ``None``
        for a chord that is not a slash chord), ``roman_numeral`` (optional str) and
        ``modulation_boundary`` (bool). Pass an empty sequence to clear.
        """
        rows = list(chords)
        count = len(rows)
        c_chords = (SonareProjectChordSymbol * count)()
        # Keep extension arrays and roman-numeral byte strings alive for the call.
        backing: list[object] = []
        if c_chords is not None:
            for i, chord in enumerate(rows):
                c = _mapping_keys(chord, _CHORD_KEYS, f"chords[{i}]")
                extension_values = c.get("extensions", []) or []
                if not isinstance(extension_values, Sequence) or isinstance(extension_values, str):
                    raise _refuse_type("chord extensions must be a sequence")
                # Left unconverted so the per-element narrowing below sees the
                # caller's value; int() would truncate a fraction past it.
                ext = list(extension_values)
                ext_count = len(ext)
                c_ext = (
                    (ctypes.c_uint8 * ext_count)(
                        *[
                            _narrow_int(e, f"chords[{i}].extensions[{j}]", 0, 255)
                            for j, e in enumerate(ext)
                        ]
                    )
                    if ext_count
                    else None
                )
                backing.append(c_ext)
                roman = c.get("roman_numeral")
                roman_bytes = (
                    _utf8_arg(roman, "roman") if isinstance(roman, str) and roman else None
                )
                backing.append(roman_bytes)
                c_chords[i].start_ppq = _narrow_double(c["start_ppq"], "start_ppq")
                c_chords[i].end_ppq = _narrow_double(c["end_ppq"], "end_ppq")
                # Assigned unconverted so the struct's own narrowing sees each
                # caller value; int() would truncate a fraction past it.
                c_chords[i].root_pc = _absent_or_in_range(
                    c.get("root_pc"), f"chords[{i}].root_pc", 11, _NO_PITCH_CLASS
                )
                quality = c.get("quality")
                if quality is not None:
                    c_chords[i].quality = _token_ordinal(
                        quality, _PROJECT_CHORD_QUALITIES, f"chords[{i}].quality"
                    )
                c_chords[i].extensions = c_ext
                c_chords[i].extension_count = _to_c_size_t(ext_count, "ext_count")
                c_chords[i].slash_bass_pc = _absent_or_in_range(
                    c.get("slash_bass_pc"), f"chords[{i}].slash_bass_pc", 11, _NO_PITCH_CLASS
                )
                c_chords[i].roman_numeral = roman_bytes
                c_chords[i].modulation_boundary = 1 if c.get("modulation_boundary") else 0
        _check(
            _get_lib().sonare_project_annotate_chords(
                self._require_handle(), c_chords, _to_c_size_t(count, "count")
            )
        )
        del backing

    # -- assist sidecars ----------------------------------------------------

    @_entry("Project.set_assist_sidecar")
    def set_assist_sidecar(
        self,
        module_id: str,
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
        non-empty and ``payload`` is copied opaque bytes.
        """
        if not isinstance(module_id, str):
            raise _refuse_type("module_id must be a string")
        if not module_id:
            raise _refuse("module_id must be a non-empty string")
        raw = bytes(payload)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        _check(
            _get_lib().sonare_project_set_assist_sidecar(
                self._require_handle(),
                _utf8_arg(module_id, "module_id"),
                _to_c_uint32(schema_version, "schema_version"),
                _to_c_uint32(target_track_id, "target_track_id"),
                _narrow_double(region_start_ppq, "region_start_ppq"),
                _narrow_double(region_end_ppq, "region_end_ppq"),
                buf,
                ctypes.c_size_t(len(raw)),
            )
        )

    def _assist_sidecar(self, index: int) -> AssistSidecar:
        lib = _get_lib()
        raw = SonareProjectAssistSidecar()
        _check(
            lib.sonare_project_get_assist_sidecar(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
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
            lib.sonare_free_project_assist_sidecar(ctypes.byref(raw))

    @_entry("Project.assist_sidecars")
    def assist_sidecars(self) -> list[AssistSidecar]:
        """Return all stored assist sidecars as a list of :class:`AssistSidecar`."""
        count = int(_get_lib().sonare_project_assist_sidecar_count(self._require_handle()))
        return [self._assist_sidecar(i) for i in range(count)]

    # -- project getters / setters ------------------------------------------

    @property
    def sample_rate(self) -> float:
        """The project sample rate in Hz."""
        out = ctypes.c_double()
        _check(_get_lib().sonare_project_get_sample_rate(self._require_handle(), ctypes.byref(out)))
        return float(out.value)

    @property
    def overlap_policy(self) -> ProjectOverlapPolicy:
        """The project's clip-overlap policy."""
        out = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_get_overlap_policy(self._require_handle(), ctypes.byref(out))
        )
        return cast(
            ProjectOverlapPolicy,
            _ordinal_token(int(out.value), _OVERLAP_POLICIES, "overlap policy"),
        )

    @_entry("Project.set_overlap_policy")
    def set_overlap_policy(self, policy: ProjectOverlapPolicy) -> None:
        """Set the project's clip-overlap policy.

        Raises:
            TypeError: If ``policy`` is not a string token.
            SonareValueError: If ``policy`` is not ``"disallow"`` or ``"allow"``.
        """
        ordinal = _token_ordinal(policy, _OVERLAP_POLICIES, "policy")
        _check(
            _get_lib().sonare_project_set_overlap_policy(
                self._require_handle(), _to_c_uint32(ordinal, "policy")
            )
        )

    @_entry("Project.set_marker")
    def set_marker(
        self,
        ppq: float,
        *,
        marker_id: int | None = None,
        name: str | None = None,
        kind: MarkerKind | None = None,
        key_fifths: int | None = None,
        key_minor: bool | None = None,
    ) -> int:
        """Add or replace a marker; return its id.

        ``marker_id`` names the marker to replace; omitted, a new id is
        allocated. A field left as ``None`` takes the library default.
        ``key_fifths`` (-7..7) and ``key_minor`` describe the key-signature kind.

        Raises:
            SonareValueError: If ``marker_id`` is 0 (omit it to allocate) or a
                field is out of range.
            TypeError: If ``kind`` is not a string token.
        """
        raw = SonareProjectMarker()
        if marker_id is not None:
            if marker_id == 0:
                raise _refuse("marker_id must not be 0 (omit it to allocate a new id)")
            raw.id = _to_c_uint32(marker_id, "marker_id")
        raw.ppq = _narrow_double(ppq, "ppq")
        if kind is not None:
            raw.kind = _marker_kind_value(kind)
        if key_fifths is not None:
            # Assigned unconverted so the struct's own narrowing sees the value.
            raw.key_fifths = key_fifths
        if key_minor is not None:
            raw.key_minor = 1 if key_minor else 0
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_set_marker_ex_name(
                self._require_handle(),
                ctypes.byref(raw),
                _utf8_arg(_marker_name_arg(name), "name"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    def _marker_by_index(self, index: int) -> ProjectMarker:
        """Read a marker by 0-based stored index, including its kind / key."""
        raw = SonareProjectMarker()
        _check(
            _get_lib().sonare_project_marker_by_index(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
            )
        )
        name = self._string_by_index("sonare_project_marker_name_by_index", index) or None
        return ProjectMarker(
            id=int(raw.id),
            ppq=float(raw.ppq),
            name=name,
            kind=_marker_kind_name(raw.kind),
            key_fifths=int(raw.key_fifths),
            key_minor=bool(raw.key_minor),
        )

    def _string_by_index(self, entry: str, index: int) -> str:
        """Read one complete owned UTF-8 string through a ``*_by_index`` getter."""
        out = ctypes.c_char_p()
        lib = _get_lib()
        _check(
            getattr(lib, entry)(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(out)
            )
        )
        try:
            return out.value.decode("utf-8") if out.value else ""
        finally:
            lib.sonare_free_string(out)

    def _track_by_index(self, index: int) -> ProjectTrack:
        """Read a stored project track by 0-based index."""
        raw = SonareProjectTrack()
        _check(
            _get_lib().sonare_project_track_by_index(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
            )
        )
        return ProjectTrack(
            id=int(raw.id),
            kind=cast(ProjectTrackKind, _TRACK_KINDS[int(raw.kind)]),
            midi_destination_id=int(raw.midi_destination_id),
            gain=float(raw.gain),
            pan=float(raw.pan),
            mute=bool(raw.mute),
            solo=bool(raw.solo),
            name=self._string_by_index("sonare_project_track_name_by_index", index) or None,
        )

    def _clip_by_index(self, index: int) -> ProjectClip:
        """Read a stored project clip by 0-based index."""
        raw = SonareProjectClip()
        _check(
            _get_lib().sonare_project_clip_by_index(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
            )
        )
        return ProjectClip(
            id=int(raw.id),
            track_id=int(raw.track_id),
            source_id=int(raw.source_id),
            source_kind=cast(ProjectSourceKind, _SOURCE_KINDS[int(raw.source_kind)]),
            start_ppq=float(raw.start_ppq),
            length_ppq=float(raw.length_ppq),
            source_offset_ppq=float(raw.source_offset_ppq),
            gain=float(raw.gain),
            loop_mode=cast(ProjectLoopMode, _LOOP_MODES[int(raw.loop_mode)]),
            loop_length_ppq=float(raw.loop_length_ppq),
        )

    def _source_by_index(self, index: int) -> ProjectSource:
        """Read a stored project source by 0-based index."""
        raw = SonareProjectSource()
        _check(
            _get_lib().sonare_project_source_by_index(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
            )
        )
        name_or_uri = self._string_by_index("sonare_project_source_name_or_uri_by_index", index)
        return self._source_from_raw(raw, name_or_uri)

    def _source_from_raw(
        self, raw: SonareProjectSource, name_or_uri: str | None = None
    ) -> ProjectSource:
        """Build a ProjectSource carrying the full, untruncated name or URI.

        ``name_or_uri`` is the complete string when the caller read it by index;
        otherwise an audio source's URI is read by id.
        """
        content_hash: str | None = None
        external_stem_role: str | None = None
        if int(raw.kind) == 0:  # SONARE_SOURCE_AUDIO; MIDI has no such metadata.
            lib = _get_lib()
            # Metadata is an additive C API.  Keep source inspection usable
            # with an older shared library, where the new fields simply read
            # as their documented empty defaults.
            if hasattr(lib, "sonare_project_get_audio_source_metadata") and hasattr(
                lib, "sonare_free_project_audio_source_metadata"
            ):
                metadata = SonareProjectAudioSourceMetadata()
                try:
                    _check(
                        lib.sonare_project_get_audio_source_metadata(
                            self._require_handle(),
                            _to_c_uint32(raw.id, "source_id"),
                            ctypes.byref(metadata),
                        )
                    )
                    # The core reports an unset field as NULL or as an empty string.
                    content_hash = (metadata.content_hash or b"").decode() or None
                    external_stem_role = (metadata.external_stem_role or b"").decode() or None
                finally:
                    # The native getter owns both strings even when it reports
                    # a failure after partially filling the descriptor.
                    lib.sonare_free_project_audio_source_metadata(ctypes.byref(metadata))
        if name_or_uri is None:
            name_or_uri = self._audio_source_uri(raw)
        return ProjectSource(
            id=int(raw.id),
            kind=cast(ProjectSourceKind, _SOURCE_KINDS[int(raw.kind)]),
            num_channels=int(raw.num_channels),
            storage_handle_id=int(raw.storage_handle_id),
            sample_rate_hint=float(raw.sample_rate_hint),
            name_or_uri=name_or_uri,
            content_hash=content_hash,
            external_stem_role=external_stem_role,
        )

    def _audio_source_uri(self, raw: SonareProjectSource) -> str:
        """Read a source's name or URI by id, the full URI for an audio source."""
        name_or_uri: str = raw.name_or_uri.split(b"\0", 1)[0].decode()
        lib = _get_lib()
        if int(raw.kind) == 0 and hasattr(lib, "sonare_project_get_audio_source_uri"):
            uri = ctypes.c_char_p()
            try:
                _check(
                    lib.sonare_project_get_audio_source_uri(
                        self._require_handle(),
                        _to_c_uint32(raw.id, "source_id"),
                        ctypes.byref(uri),
                    )
                )
                if uri.value:
                    name_or_uri = uri.value.decode()
            finally:
                if uri.value:
                    lib.sonare_free_string(uri)
        return name_or_uri

    @_entry("Project.set_mixer_scene_json")
    def set_mixer_scene_json(self, scene_json: str) -> None:
        """Replace the project's mixer scene from scene JSON.

        A scene key the reader does not recognise raises ``SonareError``
        (``INVALID_FORMAT``) naming its path, e.g. ``strips[2].faderDB``; keys starting
        with ``$`` or ``x-`` are exempt. The scene is left unchanged on a refusal.
        """
        lib = _get_lib()
        _check(
            lib.sonare_project_set_mixer_scene_json(
                self._require_handle(),
                _utf8_arg(scene_json, "scene_json"),
            )
        )

    @_entry("Project.set_tempo_segments")
    def set_tempo_segments(
        self,
        segments: Sequence[Mapping[str, float]],
    ) -> None:
        """Replace the project's tempo map.

        Each segment is a mapping with ``start_ppq``, ``bpm`` and an optional
        ``end_bpm``; sample positions are derived during normalization.
        ``end_bpm`` 0 means a constant-tempo segment. Pass an empty sequence to
        clear.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (SonareProjectTempoSegment * count)()
        for i, seg in enumerate(rows):
            fields = _mapping_keys(seg, _TEMPO_SEGMENT_KEYS, f"segments[{i}]")
            start_ppq = _narrow_double(fields["start_ppq"], "start_ppq")
            bpm = _narrow_double(fields["bpm"], "bpm")
            end_bpm = _narrow_double(fields.get("end_bpm", 0.0), "end_bpm")
            c_segments[i].start_ppq = start_ppq
            c_segments[i].bpm = bpm
            c_segments[i].end_bpm = end_bpm
        _check(
            _get_lib().sonare_project_set_tempo_segments(
                self._require_handle(), c_segments, _to_c_size_t(count, "count")
            )
        )

    @_entry("Project.set_time_signatures")
    def set_time_signatures(
        self,
        segments: Sequence[Mapping[str, float]],
    ) -> None:
        """Replace the project's time-signature map.

        Each segment is a mapping with ``start_ppq``, ``numerator`` and
        ``denominator``. Pass an empty sequence to clear.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (SonareProjectTimeSignatureSegment * count)()
        for i, seg in enumerate(rows):
            fields = _mapping_keys(seg, _METER_SEGMENT_KEYS, f"segments[{i}]")
            c_segments[i].start_ppq = _narrow_double(fields["start_ppq"], "start_ppq")
            # Left unconverted so the struct's own narrowing sees each caller
            # value; int() would truncate a fraction past it.
            c_segments[i].numerator = fields["numerator"]
            c_segments[i].denominator = fields["denominator"]
        _check(
            _get_lib().sonare_project_set_time_signatures(
                self._require_handle(), c_segments, _to_c_size_t(count, "count")
            )
        )

    def _count(self, fn_name: str) -> int:
        out = ctypes.c_size_t()
        _check(getattr(_get_lib(), fn_name)(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    @_entry("Project.markers")
    def markers(self) -> list[ProjectMarker]:
        """Every marker, in stored order, including its kind and key."""
        return [self._marker_by_index(i) for i in range(self._count("sonare_project_marker_count"))]

    @_entry("Project.tracks")
    def tracks(self) -> list[ProjectTrack]:
        """Every track, in stored order."""
        return [self._track_by_index(i) for i in range(self._count("sonare_project_track_count"))]

    @_entry("Project.clips")
    def clips(self) -> list[ProjectClip]:
        """Every clip, in stored order."""
        return [self._clip_by_index(i) for i in range(self._count("sonare_project_clip_count"))]

    @_entry("Project.sources")
    def sources(self) -> list[ProjectSource]:
        """Every registered source, in stored order."""
        return [self._source_by_index(i) for i in range(self._count("sonare_project_source_count"))]

    @_entry("Project.tempo_segments")
    def tempo_segments(self) -> list[dict[str, float]]:
        """Every tempo-map segment in stored order, as :meth:`set_tempo_segments` takes it."""
        count = self._count("sonare_project_tempo_segment_count")
        return [self._tempo_segment_by_index(i) for i in range(count)]

    @_entry("Project.time_signatures")
    def time_signatures(self) -> list[dict[str, float | int]]:
        """Every time-signature segment in stored order, as :meth:`set_time_signatures` takes it."""
        count = self._count("sonare_project_time_signature_count")
        return [self._time_signature_by_index(i) for i in range(count)]

    def _tempo_segment_by_index(self, index: int) -> dict[str, float]:
        """Read a tempo-map segment by 0-based stored index.

        Returns the keys :meth:`set_tempo_segments` accepts, so a segment read
        back can be written straight to another project. ``start_sample`` is not
        among them: a project stores musical positions only, and sample
        positions are derived when it is compiled.

        Raises:
            SonareValueError: If ``index`` is past the stored segments.
        """
        raw = SonareProjectTempoSegment()
        _check(
            _get_lib().sonare_project_tempo_segment_by_index(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
            )
        )
        return {
            "start_ppq": float(raw.start_ppq),
            "bpm": float(raw.bpm),
            "end_bpm": float(raw.end_bpm),
        }

    def _time_signature_by_index(self, index: int) -> dict[str, float | int]:
        """Read a time-signature segment by 0-based stored index.

        Raises:
            SonareValueError: If ``index`` is past the stored segments.
        """
        raw = SonareProjectTimeSignatureSegment()
        _check(
            _get_lib().sonare_project_time_signature_by_index(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
            )
        )
        return {
            "start_ppq": float(raw.start_ppq),
            "numerator": int(raw.numerator),
            "denominator": int(raw.denominator),
        }

    # -- compile / render ---------------------------------------------------

    @_entry("Project.last_bounce_compile_result")
    def last_bounce_compile_result(self) -> ProjectCompileResult | None:
        """Return the compile result captured by the most recent bounce.

        Mirrors :meth:`compile`'s structured :class:`ProjectCompileResult` but
        reads the diagnostics recorded during the last ``bounce*`` call instead
        of recompiling. ``None`` when no bounce has run on this project, or the
        last one was refused for its arguments before it compiled; an empty
        ``diagnostics`` means the last bounce compiled clean.
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        _check(
            lib.sonare_project_last_bounce_compile_result(
                self._require_handle(), ctypes.byref(result)
            )
        )
        try:
            # A bounce only loses its timeline through an error diagnostic.
            if not result.has_timeline and result.diagnostic_count == 0:
                return None
            return ProjectCompileResult(_compile_diagnostics(result))
        finally:
            lib.sonare_free_project_compile_result(ctypes.byref(result))
