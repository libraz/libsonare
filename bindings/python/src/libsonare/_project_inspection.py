# ruff: noqa: F405
"""Project MIR annotation, metadata, and compilation methods."""

from __future__ import annotations

import ctypes
import json
import numbers
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, cast

import numpy as np

from ._engine_conversions import _marker_kind_name, _marker_kind_value
from ._errors import SonareValueError, _entry, _malformed_output, _refuse, _refuse_type
from ._integer_domains import DOMAINS as _INTEGER_DOMAINS
from ._narrowing import _domain_double, _narrow_double
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
    _automation_target_kind_names,
    _automation_target_kind_token,
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
    SONARE_AUTOMATION_INSERT_INDEX_NONE,
    SonareProjectAssistSidecar,
    SonareProjectAudioSourceMetadata,
    SonareProjectAutomationLaneList,
    SonareProjectAutoTempoResult,
    SonareProjectChordSymbol,
    SonareProjectChordSymbolList,
    SonareProjectClip,
    SonareProjectCompileResult,
    SonareProjectKeySegment,
    SonareProjectKeySegmentList,
    SonareProjectMarker,
    SonareProjectPosition,
    SonareProjectSection,
    SonareProjectSectionList,
    SonareProjectSnapToGridOptions,
    SonareProjectSource,
    SonareProjectTempoCandidate,
    SonareProjectTempoOptions,
    SonareProjectTempoSegment,
    SonareProjectTempoSegmentList,
    SonareProjectTimeSignatureList,
    SonareProjectTimeSignatureSegment,
    SonareProjectTrack,
    SonareStringList,
    _bool_flag,
    _check,
    _get_lib,
    _mapping_keys,
    _narrow_int,
    _ordinal_token,
    _to_c_float_array,
    _to_c_int,
    _to_c_int64,
    _to_c_size_t,
    _to_c_uint32,
    _token_ordinal,
    _utf8_arg,
    _validate_samples,
)
from ._types_engine import MarkerKind
from ._types_enums import _AUTOMATION_CURVES, AutomationCurve
from .types import (
    AutomationPoint,
    MixSceneDocument,
    ProjectAutomationLane,
    ProjectAutoTempoResult,
    ProjectClip,
    ProjectMarker,
    ProjectSource,
    ProjectTrack,
    TimeSignature,
)

_KEY_SEGMENT_KEYS = ("start_ppq", "end_ppq", "tonic_pc", "mode")
_SECTION_KEYS = ("start_ppq", "end_ppq", "label")
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
_MAX_PROJECT_CHORD_EXTENSIONS = 32


def _project_pitch_class(value: int, path: str) -> int | None:
    if value == _NO_PITCH_CLASS:
        return None
    if 0 <= value <= 11:
        return value
    raise _malformed_output(f"{path} has unknown pitch-class value: {value}")


class _ProjectInspectionMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    @staticmethod
    def _tempo_option_values(
        adaptive_tempo: bool | None,
        tempo_update_interval_beats: int | None,
        ramp_threshold: float | None,
        include_octave_candidates: bool | None,
    ) -> dict[str, int | float]:
        """Validate supplied overrides before initializing the C options."""
        options: dict[str, int | float] = {}
        if adaptive_tempo is not None:
            options["adaptive_tempo"] = _bool_flag(adaptive_tempo, "adaptive_tempo")
        if include_octave_candidates is not None:
            options["include_octave_candidates"] = _bool_flag(
                include_octave_candidates, "include_octave_candidates"
            )
        if tempo_update_interval_beats is not None:
            options["tempo_update_interval_beats"] = _to_c_int(
                tempo_update_interval_beats, "tempo_update_interval_beats"
            ).value
        if ramp_threshold is not None:
            options["ramp_threshold"] = _narrow_double(ramp_threshold, "ramp_threshold")
        return options

    @_entry("Project.analyze_tempo")
    def analyze_tempo(
        self,
        audio: Sequence[float] | np.ndarray,
        sample_rate: int,
        *,
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
        overrides = self._tempo_option_values(
            adaptive_tempo,
            tempo_update_interval_beats,
            ramp_threshold,
            include_octave_candidates,
        )
        rate = _to_c_int(sample_rate, "sample_rate")
        validated = _validate_samples(audio, arg_name="audio")
        c_array, length = _to_c_float_array(validated)
        count = ctypes.c_size_t()
        candidates = (SonareProjectTempoCandidate * 3)()
        options = SonareProjectTempoOptions._given(**overrides)
        _check(
            _get_lib().sonare_project_analyze_tempo(
                self._require_handle(),
                c_array,
                _to_c_size_t(length, "length"),
                rate,
                ctypes.byref(options),
                candidates,
                ctypes.c_size_t(len(candidates)),
                ctypes.byref(count),
            ),
            flattened="options",
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
    def auto_tempo(
        self,
        audio: Sequence[float] | np.ndarray,
        sample_rate: int,
        *,
        candidate_index: int = 0,
        apply_time_signatures: bool = False,
        adaptive_tempo: bool | None = None,
        tempo_update_interval_beats: int | None = None,
        ramp_threshold: float | None = None,
        include_octave_candidates: bool | None = None,
    ) -> ProjectAutoTempoResult:
        """Detect tempo from a mono buffer and install it (undoable).

        The scoring arguments mean what they do on :meth:`analyze_tempo`, and
        ``candidate_index`` indexes the ranking that method returns, so pair the
        two on the same values rather than mixing two option sets. Read the
        installed map back with :meth:`tempo_segments`.

        Returns:
            The selected BPM, its confidence, and the detected meter. The meter
            is returned whether or not ``apply_time_signatures`` installs it.

        Raises:
            SonareValueError: If ``audio`` is empty or holds a NaN or Inf sample.
        """
        apply_meter = _bool_flag(apply_time_signatures, "apply_time_signatures")
        overrides = self._tempo_option_values(
            adaptive_tempo,
            tempo_update_interval_beats,
            ramp_threshold,
            include_octave_candidates,
        )
        rate = _to_c_int(sample_rate, "sample_rate")
        candidate = _to_c_size_t(candidate_index, "candidate_index")
        validated = _validate_samples(audio, arg_name="audio")
        c_array, length = _to_c_float_array(validated)
        result = SonareProjectAutoTempoResult()
        options = SonareProjectTempoOptions._given(**overrides)
        _check(
            _get_lib().sonare_project_auto_tempo(
                self._require_handle(),
                c_array,
                _to_c_size_t(length, "length"),
                rate,
                ctypes.byref(options),
                candidate,
                ctypes.c_int32(apply_meter),
                ctypes.byref(result),
            ),
            flattened="options",
        )
        return ProjectAutoTempoResult(
            bpm=float(result.bpm),
            bpm_confidence=float(result.bpm_confidence),
            time_signature=TimeSignature(
                numerator=int(result.time_signature.numerator),
                denominator=int(result.time_signature.denominator),
                confidence=float(result.time_signature.confidence),
            ),
        )

    @_entry("Project.snap_to_grid")
    def snap_to_grid(self, ppq: float, strength: float = 1.0, division: int = 1) -> float:
        """Snap a PPQ coordinate to a project-grid line.

        ``division`` is ``0`` for bars, ``1`` for beats, or the number of
        subdivisions per beat (for example, ``4`` for sixteenths). ``strength``
        is in ``[0, 1]`` (0 = no snap, 1 = exact grid line).
        """
        ppq_value = _domain_double(ppq, "ppq")
        strength_value = _domain_double(strength, "strength")
        try:
            division_value = _to_c_int(division, "division")
        except SonareValueError:
            if isinstance(division, (bool, np.bool_)) or not isinstance(division, numbers.Real):
                raise
            # Preserve C's PPQ/strength-before-division order for unmarshalable numbers.
            division_low = _INTEGER_DOMAINS[("Project.snap_to_grid", "division")][0]
            division_value = division_low - 1
        out_ppq = ctypes.c_double()
        options = SonareProjectSnapToGridOptions._given(division=division_value)
        _check(
            _get_lib().sonare_project_snap_to_grid(
                self._require_handle(),
                ppq_value,
                strength_value,
                ctypes.byref(options),
                ctypes.byref(out_ppq),
            ),
            flattened="options",
        )
        return float(out_ppq.value)

    @_entry("Project.key_annotations")
    def key_annotations(self) -> list[ProjectKeySegment]:
        """Read key annotation segments in their stored order."""
        lib = _get_lib()
        raw = SonareProjectKeySegmentList()
        try:
            _check(lib.sonare_project_key_annotations(self._require_handle(), ctypes.byref(raw)))
            count = int(raw.count)
            if count and not raw.items:
                raise _malformed_output("key_annotations returned a null items pointer")
            segments: list[ProjectKeySegment] = []
            for index in range(count):
                item = raw.items[index]
                segments.append(
                    ProjectKeySegment(
                        start_ppq=float(item.start_ppq),
                        end_ppq=float(item.end_ppq),
                        tonic_pc=_project_pitch_class(
                            int(item.tonic_pc), f"key_annotations[{index}].tonic_pc"
                        ),
                        mode=cast(
                            ProjectKeyMode,
                            _ordinal_token(int(item.mode), _PROJECT_KEY_MODES, "key mode"),
                        ),
                    )
                )
            return segments
        finally:
            lib.sonare_free_project_key_segment_list(ctypes.byref(raw))

    @_entry("Project.chord_annotations")
    def chord_annotations(self) -> list[ProjectChordSymbol]:
        """Read chord annotation symbols in their stored order."""
        lib = _get_lib()
        raw = SonareProjectChordSymbolList()
        try:
            _check(lib.sonare_project_chord_annotations(self._require_handle(), ctypes.byref(raw)))
            count = int(raw.count)
            if count and not raw.items:
                raise _malformed_output("chord_annotations returned a null items pointer")
            chords: list[ProjectChordSymbol] = []
            for index in range(count):
                item = raw.items[index]
                extension_count = int(item.extension_count)
                if extension_count > _MAX_PROJECT_CHORD_EXTENSIONS:
                    raise _malformed_output(
                        f"chord_annotations[{index}].extensions has too many values: "
                        f"{extension_count}"
                    )
                if extension_count and not item.extensions:
                    raise _malformed_output(
                        f"chord_annotations[{index}].extensions is null with nonzero count"
                    )
                extensions = [int(item.extensions[j]) for j in range(extension_count)]
                chord = ProjectChordSymbol(
                    start_ppq=float(item.start_ppq),
                    end_ppq=float(item.end_ppq),
                    root_pc=_project_pitch_class(
                        int(item.root_pc), f"chord_annotations[{index}].root_pc"
                    ),
                    quality=cast(
                        ProjectChordQuality,
                        _ordinal_token(
                            int(item.quality), _PROJECT_CHORD_QUALITIES, "chord quality"
                        ),
                    ),
                    extensions=extensions,
                    slash_bass_pc=_project_pitch_class(
                        int(item.slash_bass_pc), f"chord_annotations[{index}].slash_bass_pc"
                    ),
                    modulation_boundary=bool(item.modulation_boundary),
                )
                roman_numeral = item.roman_numeral
                if roman_numeral:
                    try:
                        chord["roman_numeral"] = roman_numeral.decode("utf-8")
                    except UnicodeDecodeError:
                        raise _malformed_output(
                            f"chord_annotations[{index}].roman_numeral is not UTF-8"
                        ) from None
                chords.append(chord)
            return chords
        finally:
            lib.sonare_free_project_chord_symbol_list(ctypes.byref(raw))

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
    def annotate_chords(self, chords: Sequence[Mapping[str, object]]) -> None:
        """Replace the project's chord-symbol annotation stream (undoable command).

        Each chord is a mapping with keys ``start_ppq``, ``end_ppq``, ``root_pc``
        (0..11, absent or ``None`` when the root is unknown), ``quality``
        (a :data:`ProjectChordQuality` token; absent or ``None`` is
        ``"unknown"``), optional ``extensions`` (sequence of scale degrees
        such as 7, 9, 13, up to 32), ``slash_bass_pc`` (0..11, absent or ``None``
        for a chord that is not a slash chord), ``roman_numeral`` (optional str) and
        ``modulation_boundary`` (bool). Pass an empty sequence to clear.
        """
        if not isinstance(chords, Sequence) or isinstance(chords, (str, bytes, bytearray)):
            raise _refuse_type("chords must be a sequence")
        count = len(chords)
        c_chords = (SonareProjectChordSymbol * count)()
        # Keep extension arrays and roman-numeral byte strings alive for the call.
        backing: list[object] = []
        for i, chord in enumerate(chords):
            path = f"chords[{i}]"
            c = _mapping_keys(chord, _CHORD_KEYS, path, required=("start_ppq", "end_ppq"))
            extension_values = c.get("extensions")
            if extension_values is None:
                extension_values = ()
            if not isinstance(extension_values, Sequence) or isinstance(
                extension_values, (str, bytes, bytearray)
            ):
                raise _refuse_type(f"{path}.extensions must be a sequence")
            modulation_boundary = c.get("modulation_boundary")
            modulation_flag = (
                0
                if modulation_boundary is None
                else _bool_flag(modulation_boundary, f"{path}.modulation_boundary")
            )
            ext_count = len(extension_values)
            if ext_count > _MAX_PROJECT_CHORD_EXTENSIONS:
                raise _refuse(
                    f"{path}.extensions must contain at most "
                    f"{_MAX_PROJECT_CHORD_EXTENSIONS} entries"
                )
            c_ext = (
                (ctypes.c_uint8 * ext_count)(
                    *[
                        _narrow_int(e, f"{path}.extensions[{j}]", 0, 255)
                        for j, e in enumerate(extension_values)
                    ]
                )
                if ext_count
                else None
            )
            backing.append(c_ext)
            roman = c.get("roman_numeral")
            try:
                roman_bytes = (
                    _utf8_arg(roman, f"{path}.roman_numeral") if roman is not None else None
                )
            except UnicodeEncodeError:
                raise _refuse(f"{path}.roman_numeral must be valid UTF-8") from None
            backing.append(roman_bytes)
            c_chords[i].start_ppq = _narrow_double(c["start_ppq"], f"chords[{i}].start_ppq")
            c_chords[i].end_ppq = _narrow_double(c["end_ppq"], f"chords[{i}].end_ppq")
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
            c_chords[i].extension_count = _to_c_size_t(ext_count, f"{path}.extensions")
            c_chords[i].slash_bass_pc = _absent_or_in_range(
                c.get("slash_bass_pc"), f"chords[{i}].slash_bass_pc", 11, _NO_PITCH_CLASS
            )
            c_chords[i].roman_numeral = roman_bytes
            c_chords[i].modulation_boundary = modulation_flag
        _check(
            _get_lib().sonare_project_annotate_chords(
                self._require_handle(), c_chords, _to_c_size_t(count, "count")
            )
        )
        del backing

    @_entry("Project.annotate_sections")
    def annotate_sections(self, sections: Sequence[ProjectSection]) -> None:
        """Replace the labeled section stream; overlapping sections are allowed."""
        if isinstance(sections, (str, bytes)) or not isinstance(sections, Sequence):
            raise _refuse_type("sections must be a sequence of mappings")
        rows = list(sections)
        count = len(rows)
        c_sections = (SonareProjectSection * count)()
        labels: list[bytes] = []
        for i, section in enumerate(rows):
            where = f"sections[{i}]"
            fields = _mapping_keys(section, _SECTION_KEYS, where, required=_SECTION_KEYS)
            label = _utf8_arg(fields["label"], f"{where}.label")
            if all(character in b" \t\n\v\f\r" for character in label):
                raise _refuse(f"{where}.label must not be blank")
            labels.append(label)
            c_sections[i].start_ppq = _narrow_double(fields["start_ppq"], f"{where}.start_ppq")
            c_sections[i].end_ppq = _narrow_double(fields["end_ppq"], f"{where}.end_ppq")
            c_sections[i].label = label
        _check(
            _get_lib().sonare_project_annotate_sections(
                self._require_handle(), c_sections if count else None, _to_c_size_t(count, "count")
            )
        )
        del labels

    @_entry("Project.section_annotations")
    def section_annotations(self) -> list[ProjectSection]:
        """Read labeled sections in their stored order."""
        lib = _get_lib()
        raw = SonareProjectSectionList()
        try:
            _check(
                lib.sonare_project_section_annotations(self._require_handle(), ctypes.byref(raw))
            )
            count = int(raw.count)
            if count and not raw.items:
                raise _malformed_output("section_annotations returned a null items pointer")
            sections: list[ProjectSection] = []
            for index in range(count):
                item = raw.items[index]
                if item.label is None:
                    raise _malformed_output(f"section_annotations[{index}].label is null")
                try:
                    label = item.label.decode("utf-8")
                except UnicodeDecodeError:
                    raise _malformed_output(
                        f"section_annotations[{index}].label is not UTF-8"
                    ) from None
                sections.append(
                    ProjectSection(
                        start_ppq=float(item.start_ppq),
                        end_ppq=float(item.end_ppq),
                        label=label,
                    )
                )
            return sections
        finally:
            lib.sonare_free_project_section_list(ctypes.byref(raw))

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
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw)
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
        Nonzero or true values on other kinds are refused.

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
            raw.kind = _marker_kind_value(kind, "kind")
        if key_fifths is not None:
            # Assigned unconverted so the struct's own narrowing sees the value.
            raw.key_fifths = key_fifths
        if key_minor is not None:
            raw.key_minor = 1 if key_minor else 0
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_set_marker(
                self._require_handle(),
                ctypes.byref(raw),
                None if name is None else _utf8_arg(_marker_name_arg(name), "name"),
                ctypes.byref(out_id),
            ),
            flattened="marker",
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
            float(raw.ppq),
            id=int(raw.id),
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
        lib = _get_lib()
        raw = SonareProjectTrack()
        _check(
            lib.sonare_project_track_by_index(
                self._require_handle(), _to_c_size_t(index, "index"), ctypes.byref(raw)
            )
        )
        strip_ref = ctypes.c_char_p()
        output_target = ctypes.c_char_p()
        try:
            _check(
                lib.sonare_project_track_route_by_index(
                    self._require_handle(),
                    _to_c_size_t(index, "index"),
                    ctypes.byref(strip_ref),
                    ctypes.byref(output_target),
                )
            )
            channel_strip_ref = self._decode_optional_project_string(strip_ref.value)
            route_target = self._decode_optional_project_string(output_target.value)
        finally:
            if strip_ref.value is not None:
                lib.sonare_free_string(strip_ref)
            if output_target.value is not None:
                lib.sonare_free_string(output_target)
        return ProjectTrack(
            id=int(raw.id),
            kind=cast(ProjectTrackKind, _TRACK_KINDS[int(raw.kind)]),
            midi_destination_id=int(raw.midi_destination_id),
            gain=float(raw.gain),
            pan=float(raw.pan),
            mute=bool(raw.mute),
            solo=bool(raw.solo),
            name=self._string_by_index("sonare_project_track_name_by_index", index) or None,
            channel_strip_ref=channel_strip_ref,
            output_target=route_target,
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
            loop_length_ppq=float(raw.loop_length_ppq) or None,
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
        if int(raw.kind) == 0:
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

    @staticmethod
    def _decode_optional_project_string(value: bytes | None) -> str | None:
        if value is None:
            return None
        try:
            return value.decode("utf-8")
        except UnicodeDecodeError:
            raise _malformed_output("project reader returned invalid UTF-8") from None

    @_entry("Project.set_mixer_scene")
    def set_mixer_scene(self, scene: MixSceneDocument | None) -> None:
        """Replace the project's mixer scene, or clear it when ``scene`` is ``None``.

        A scene key the reader does not recognise raises ``SonareValueError``
        naming its path under ``scene``; keys starting with ``$`` or ``x-`` are
        exempt. The scene is left unchanged on a refusal.
        """
        if scene is not None and not isinstance(scene, Mapping):
            raise _refuse_type("scene must be a mapping (parse JSON text with json.loads first)")
        lib = _get_lib()
        _check(
            lib.sonare_project_set_mixer_scene_json(
                self._require_handle(),
                _utf8_arg(json.dumps(dict(scene)), "scene") if scene is not None else None,
            ),
            document_root="scene",
        )

    @_entry("Project.mixer_scene")
    def mixer_scene(self) -> MixSceneDocument | None:
        """Read the project's mixer scene, or return ``None`` when it is unset."""
        lib = _get_lib()
        out = ctypes.c_char_p()
        try:
            _check(lib.sonare_project_mixer_scene_json(self._require_handle(), ctypes.byref(out)))
            if out.value is None:
                return None
            try:
                scene = json.loads(out.value)
            except (UnicodeDecodeError, json.JSONDecodeError):
                raise _malformed_output("mixer_scene returned invalid scene JSON") from None
            if not isinstance(scene, dict):
                raise _malformed_output("mixer_scene returned a non-object scene")
            return cast(MixSceneDocument, scene)
        finally:
            if out.value is not None:
                lib.sonare_free_string(out)

    @_entry("Project.unresolved_sound_font_uris")
    def unresolved_sound_font_uris(self) -> list[str]:
        """Return the distinct unresolved SoundFont URIs in the project."""
        lib = _get_lib()
        result = SonareStringList()
        try:
            _check(
                lib.sonare_project_unresolved_sound_font_uris(
                    self._require_handle(), ctypes.byref(result)
                )
            )
            count = int(result.count)
            if bool(result.items) != bool(count):
                raise _malformed_output(
                    "unresolved SoundFont URI list has an inconsistent pointer and count"
                )
            uris: list[str] = []
            for index in range(count):
                raw_uri = result.items[index]
                if raw_uri is None:
                    raise _malformed_output("unresolved SoundFont URI list contains a null item")
                try:
                    uris.append(raw_uri.decode("utf-8"))
                except UnicodeDecodeError:
                    raise _malformed_output(
                        "unresolved SoundFont URI list contains invalid UTF-8"
                    ) from None
            return uris
        finally:
            if result.items:
                lib.sonare_free_string_list(ctypes.byref(result))

    @_entry("Project.set_tempo_segments")
    def set_tempo_segments(
        self,
        segments: Sequence[Mapping[str, float]],
    ) -> None:
        """Replace the project's tempo map.

        Each segment is a mapping with ``start_ppq``, ``bpm`` and an optional
        ``end_bpm``; sample positions are derived during normalization.
        ``bpm`` and a non-zero ``end_bpm`` must be finite and in
        ``[1e-6, 100000]`` BPM. ``end_bpm`` 0 means a constant-tempo segment.
        Pass an empty sequence to clear.
        """
        rows = list(segments)
        count = len(rows)
        c_segments = (SonareProjectTempoSegment * count)()
        for i, seg in enumerate(rows):
            fields = _mapping_keys(seg, _TEMPO_SEGMENT_KEYS, f"segments[{i}]")
            start_ppq = _narrow_double(fields["start_ppq"], f"segments[{i}].start_ppq")
            bpm = _narrow_double(fields["bpm"], f"segments[{i}].bpm")
            end_bpm = _narrow_double(fields.get("end_bpm", 0.0), f"segments[{i}].end_bpm")
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
            c_segments[i].start_ppq = _narrow_double(
                fields["start_ppq"], f"segments[{i}].start_ppq"
            )
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

    @_entry("Project.automation_lanes")
    def automation_lanes(self) -> list[ProjectAutomationLane]:
        """Read every project automation lane, including its target and breakpoints."""
        lib = _get_lib()
        result = SonareProjectAutomationLaneList()
        try:
            _check(
                lib.sonare_project_automation_lanes(self._require_handle(), ctypes.byref(result))
            )
            count = int(result.count)
            if bool(result.items) != bool(count):
                raise _malformed_output(
                    "automation lane list has an inconsistent pointer and count"
                )
            lanes: list[ProjectAutomationLane] = []
            target_kind_names = _automation_target_kind_names()
            for index in range(count):
                item = result.items[index]
                desc = item.desc
                kind = _automation_target_kind_token(int(desc.target_kind), target_kind_names)
                point_count = int(desc.point_count)
                if bool(desc.points) != bool(point_count):
                    raise _malformed_output(
                        "automation lane points have an inconsistent pointer and count"
                    )
                points = tuple(
                    AutomationPoint(
                        ppq=float(desc.points[point_index].ppq),
                        value=float(desc.points[point_index].value),
                        curve_to_next=cast(
                            AutomationCurve,
                            _ordinal_token(
                                int(desc.points[point_index].curve_to_next),
                                _AUTOMATION_CURVES,
                                "automation curve",
                            ),
                        ),
                    )
                    for point_index in range(point_count)
                )
                send_id_bytes = desc.send_id
                processor_bytes = desc.processor
                param_name_bytes = desc.param_name
                try:
                    send_id = send_id_bytes.decode("utf-8") if send_id_bytes is not None else None
                    processor = (
                        processor_bytes.decode("utf-8") if processor_bytes is not None else None
                    )
                    param_name = (
                        param_name_bytes.decode("utf-8") if param_name_bytes is not None else None
                    )
                except UnicodeDecodeError:
                    raise _malformed_output(
                        "automation lane identity contains invalid UTF-8"
                    ) from None
                insert_index_value = int(desc.insert_index)
                insert_index = (
                    None
                    if insert_index_value == SONARE_AUTOMATION_INSERT_INDEX_NONE
                    else insert_index_value
                )
                if kind == "opaque":
                    if int(desc.target_param_id) == 0 or any(
                        value is not None
                        for value in (send_id, insert_index, processor, param_name)
                    ):
                        raise _malformed_output(
                            "opaque automation lane has an invalid target identity"
                        )
                    target_param_id = int(desc.target_param_id)
                elif kind == "trackSendDb":
                    if (
                        int(desc.target_param_id) != 0
                        or not send_id
                        or any(value is not None for value in (insert_index, processor, param_name))
                    ):
                        raise _malformed_output(
                            "send automation lane has an invalid target identity"
                        )
                    target_param_id = None
                elif kind == "trackInsertParam":
                    if (
                        int(desc.target_param_id) != 0
                        or insert_index is None
                        or insert_index < 0
                        or not processor
                        or not param_name
                        or send_id is not None
                    ):
                        raise _malformed_output(
                            "insert automation lane has an invalid target identity"
                        )
                    target_param_id = None
                else:
                    if int(desc.target_param_id) != 0 or any(
                        value is not None
                        for value in (send_id, insert_index, processor, param_name)
                    ):
                        raise _malformed_output(
                            "typed automation lane has an invalid target identity"
                        )
                    target_param_id = None
                lanes.append(
                    ProjectAutomationLane(
                        track_id=int(item.track_id),
                        lane_id=int(item.lane_id),
                        target_param_id=target_param_id,
                        target_kind=kind,
                        points=points,
                        send_id=send_id,
                        insert_index=insert_index,
                        processor=processor,
                        param_name=param_name,
                    )
                )
            return lanes
        finally:
            lib.sonare_free_project_automation_lane_list(ctypes.byref(result))

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
        lib = _get_lib()
        result = SonareProjectTempoSegmentList()
        try:
            _check(lib.sonare_project_tempo_segments(self._require_handle(), ctypes.byref(result)))
            count = int(result.count)
            if bool(result.items) != bool(count):
                raise _malformed_output("tempo segment list has an inconsistent pointer and count")
            return [
                {
                    "start_ppq": float(result.items[index].start_ppq),
                    "bpm": float(result.items[index].bpm),
                    "end_bpm": float(result.items[index].end_bpm),
                }
                for index in range(count)
            ]
        finally:
            lib.sonare_free_project_tempo_segment_list(ctypes.byref(result))

    @_entry("Project.time_signatures")
    def time_signatures(self) -> list[dict[str, float | int]]:
        """Every time-signature segment in stored order, as :meth:`set_time_signatures` takes it."""
        lib = _get_lib()
        result = SonareProjectTimeSignatureList()
        try:
            _check(lib.sonare_project_time_signatures(self._require_handle(), ctypes.byref(result)))
            count = int(result.count)
            if bool(result.items) != bool(count):
                raise _malformed_output("time-signature list has an inconsistent pointer and count")
            return [
                {
                    "start_ppq": float(result.items[index].start_ppq),
                    "numerator": int(result.items[index].numerator),
                    "denominator": int(result.items[index].denominator),
                }
                for index in range(count)
            ]
        finally:
            lib.sonare_free_project_time_signature_list(ctypes.byref(result))

    @_entry("Project.ppq_to_sec")
    def ppq_to_sec(self, ppq: float) -> float:
        """Convert a project PPQ position to seconds from PPQ zero."""
        out_sec = ctypes.c_double()
        _check(
            _get_lib().sonare_project_ppq_to_sec(
                self._require_handle(), _narrow_double(ppq, "ppq"), ctypes.byref(out_sec)
            )
        )
        return float(out_sec.value)

    @_entry("Project.sec_to_ppq")
    def sec_to_ppq(self, sec: float) -> float:
        """Convert seconds from PPQ zero to a project PPQ position."""
        out_ppq = ctypes.c_double()
        _check(
            _get_lib().sonare_project_sec_to_ppq(
                self._require_handle(), _narrow_double(sec, "sec"), ctypes.byref(out_ppq)
            )
        )
        return float(out_ppq.value)

    @_entry("Project.position_at_ppq")
    def position_at_ppq(self, ppq: float) -> ProjectPosition:
        """Read the effective tempo, time signature, bar, and beat at ``ppq``."""
        raw = SonareProjectPosition()
        _check(
            _get_lib().sonare_project_position_at_ppq(
                self._require_handle(), _narrow_double(ppq, "ppq"), ctypes.byref(raw)
            )
        )
        return ProjectPosition(
            bpm=float(raw.bpm),
            time_signature=ProjectTimeSignatureSegment(
                start_ppq=float(raw.time_signature.start_ppq),
                numerator=int(raw.time_signature.numerator),
                denominator=int(raw.time_signature.denominator),
            ),
            bar_start_ppq=float(raw.bar_start_ppq),
            bar_index=int(raw.bar_index),
            beat=int(raw.beat),
            beat_fraction=float(raw.beat_fraction),
        )

    @_entry("Project.bar_start_ppq")
    def bar_start_ppq(self, bar_index: int) -> float:
        """Read the PPQ start of a zero-based project bar."""
        out_ppq = ctypes.c_double()
        _check(
            _get_lib().sonare_project_bar_start_ppq(
                self._require_handle(), _to_c_int64(bar_index, "bar_index"), ctypes.byref(out_ppq)
            )
        )
        return float(out_ppq.value)

    @_entry("Project.end_ppq")
    def end_ppq(self) -> float:
        """Read the end in PPQ of the last clip with a valid source and range on an audible track.

        A track is audible when it is unmuted and, if any track is soloed, soloed itself.
        Return zero when no clip qualifies. Render tails are excluded.
        """
        out_end_ppq = ctypes.c_double()
        _check(_get_lib().sonare_project_end_ppq(self._require_handle(), ctypes.byref(out_end_ppq)))
        return float(out_end_ppq.value)

    # -- compile / render ---------------------------------------------------

    @_entry("Project.last_bounce_compile_result")
    def last_bounce_compile_result(self) -> ProjectCompileResult | None:
        """Return the compile result captured by the last successful bounce.

        Mirrors :meth:`compile`'s structured :class:`ProjectCompileResult` but
        reads the diagnostics recorded during that :meth:`bounce` call instead
        of recompiling. ``None`` until a bounce succeeds. A later refusal or
        compile failure leaves the last successful result unchanged; empty
        ``diagnostics`` means that bounce compiled cleanly.
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
