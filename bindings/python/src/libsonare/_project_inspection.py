# ruff: noqa: F405
"""Project MIR annotation, metadata, and compilation methods."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING

import numpy as np

from ._project_model import *  # noqa: F403
from ._project_model import _marker_name_bytes
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    SonareProjectAssistSidecar,
    SonareProjectChordSymbol,
    SonareProjectCompileResult,
    SonareProjectKeySegment,
    SonareProjectMarker,
    SonareProjectTempoCandidate,
    SonareProjectTempoSegment,
    SonareProjectTimeSignatureSegment,
    _check,
    _get_lib,
    _to_c_float_array,
)
from .types import ProjectMarker


class _ProjectInspectionMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    def analyze_tempo(
        self, audio: Sequence[float] | np.ndarray, sample_rate: int
    ) -> list[dict[str, object]]:
        """Return ranked primary/half/double tempo and meter candidates without editing."""
        c_array, length = _to_c_float_array(audio)
        count = ctypes.c_size_t()
        candidates = (SonareProjectTempoCandidate * 3)()
        _check(
            _get_lib().sonare_project_analyze_tempo(
                self._require_handle(),
                c_array,
                ctypes.c_size_t(length),
                int(sample_rate),
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
    ) -> float:
        """Detect tempo from a mono buffer and install it (undoable).

        Returns the primary BPM estimate.
        """
        c_array, length = _to_c_float_array(audio)
        out_bpm = ctypes.c_float()
        _check(
            _get_lib().sonare_project_auto_tempo_ex(
                self._require_handle(),
                c_array,
                ctypes.c_size_t(length),
                int(sample_rate),
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
        raw.name = _marker_name_bytes(marker.name)
        out_id = ctypes.c_uint32()
        _check(
            _get_lib().sonare_project_set_marker_ex(
                self._require_handle(), ctypes.byref(raw), ctypes.byref(out_id)
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
        return ProjectMarker(
            id=int(raw.id),
            ppq=float(raw.ppq),
            name=bytes(raw.name).split(b"\x00", 1)[0].decode("utf-8", "replace"),
            kind=int(raw.kind),
            key_fifths=int(raw.key_fifths),
            key_minor=bool(raw.key_minor),
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
        c_segments = (SonareProjectTempoSegment * count)() if count else None
        for i, seg in enumerate(rows):
            if isinstance(seg, Mapping):
                start_ppq = float(seg["start_ppq"])
                bpm = float(seg["bpm"])
                end_bpm = float(seg.get("end_bpm", 0.0))
            else:
                tup = tuple(seg)
                if len(tup) < 2:
                    raise ValueError(f"segments[{i}] must contain (start_ppq, bpm)")
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
        c_segments = (SonareProjectTimeSignatureSegment * count)() if count else None
        for i, seg in enumerate(rows):
            if isinstance(seg, Mapping):
                start_ppq = float(seg["start_ppq"])
                numerator = int(seg["numerator"])
                denominator = int(seg["denominator"])
            else:
                tup = tuple(seg)
                if len(tup) < 3:
                    raise ValueError(
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

    def track_count(self) -> int:
        """Number of tracks in the project."""
        return self._count("sonare_project_track_count")

    # -- compile / render ---------------------------------------------------

    def last_bounce_compile_result(self) -> ProjectCompileResult:
        """Return the compile result captured by the most recent bounce.

        Mirrors :meth:`compile`'s structured :class:`ProjectCompileResult` but
        reads the timeline + diagnostics recorded during the last
        ``bounce*`` call instead of recompiling.
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
