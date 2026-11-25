"""Tests for the headless arrangement / DAW :class:`Project` Python wrapper.

Mirrors the C keystone parity test
(``tests/bindings/binding_project_parity_test.cpp``): it drives the
``sonare_project_*`` C ABI end to end through the Python wrapper and pins the
ABI version, deterministic serialize byte-stability, bit-exact bounce,
undo/redo, MIR snap-to-grid, and malformed-input handling.
"""

from __future__ import annotations

import ctypes
import dataclasses
import json
import re
from collections.abc import Callable
from types import MappingProxyType
from typing import Any, get_args

import numpy as np
import pytest

import libsonare
from libsonare import (
    ErrorCode,
    ExternalSeparatedStemImportResult,
    Project,
    ProjectAutoTempoResult,
    ProjectDiagnosticCode,
    ProjectDiagnosticSeverity,
    ProjectLoopRecordingResult,
    ProjectMidiClipResult,
    ProjectSection,
    SonareError,
    SonareValueError,
    project_abi_version,
)
from libsonare._project_model import _EXPECTED_PROJECT_ABI_VERSION
from libsonare._runtime import _get_lib
from libsonare._tempo_domains import PROJECT_TEMPO_MIN

from ._helpers import raises_coded
from ._project_helpers import _build_project, dangling_source_json

# --- ABI version -----------------------------------------------------------


def test_project_abi_version_matches_expected() -> None:
    """The runtime project ABI version is exposed and matches the binding."""
    assert project_abi_version() == _EXPECTED_PROJECT_ABI_VERSION
    assert project_abi_version() > 0


def test_create_factory_matches_constructor() -> None:
    """The cross-binding factory creates an empty project usable like ``Project()``."""
    created = Project.create()
    constructed = Project()
    try:
        assert isinstance(created, Project)
        assert created.to_json_bytes() == constructed.to_json_bytes()
        created.set_sample_rate(48000)
        track_id = created.add_track("audio", "factory")
        assert track_id > 0
        assert created.tracks()[0].id == track_id
    finally:
        created.close()
        constructed.close()


def test_lists_stored_tracks_clips_and_sources() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "readback")
        clip_id = project.add_clip(
            track_id,
            start_ppq=2.0,
            length_ppq=4.0,
            source_offset_ppq=1.0,
            gain=0.75,
            source_uri="asset://readback.wav",
        )

        track = project.tracks()[0]
        assert track.id == track_id
        assert track.kind == "audio"
        assert track.name == "readback"
        assert track.gain == 1.0
        assert track.pan == 0.0
        assert not track.mute
        assert not track.solo

        clip = project.clips()[0]
        assert clip.id == clip_id
        assert clip.track_id == track_id
        assert clip.source_kind == "audio"
        assert clip.start_ppq == 2.0
        assert clip.length_ppq == 4.0
        assert clip.source_offset_ppq == 1.0
        assert clip.gain == 0.75
        assert clip.loop_mode == "off"
        assert clip.loop_length_ppq is None

        source = project.sources()[0]
        assert source.id == clip.source_id
        assert source.kind == "audio"
        assert source.name_or_uri == "asset://readback.wav"
        assert source.content_hash is None
        assert source.external_stem_role is None

        assert len(project.tracks()) == len(project.clips()) == len(project.sources()) == 1
    finally:
        project.close()


def test_audio_source_metadata_roundtrips_clears_and_undoes() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "metadata")
        clip_id = project.add_clip(track_id, 0.0, 1.0, source_uri="asset://metadata.wav")
        source_id = project.clips()[0].source_id
        assert clip_id != 0

        project.set_audio_source_metadata(source_id, "sha256:deadbeef", "lead")
        source = project.sources()[0]
        assert source.content_hash == "sha256:deadbeef"
        assert source.external_stem_role == "lead"

        # None clears each field while preserving the source itself.
        project.set_audio_source_metadata(source_id, None, None)
        source = project.sources()[0]
        assert source.id == source_id
        assert source.name_or_uri == "asset://metadata.wav"
        assert source.content_hash is None
        assert source.external_stem_role is None

        project.undo()
        source = project.sources()[0]
        assert source.content_hash == "sha256:deadbeef"
        assert source.external_stem_role == "lead"
        project.redo()
        source = project.sources()[0]
        assert source.content_hash is None
        assert source.external_stem_role is None

        # An empty string is not a second spelling of "clear".
        for args in (("", "lead"), ("sha256:x", "")):
            with pytest.raises(SonareValueError, match="must not be empty; pass None to clear it"):
                project.set_audio_source_metadata(source_id, *args)

        project.set_audio_source_metadata(source_id, "sha256:roundtrip", "vocals")
        restored = Project.from_json(project.to_json())
        try:
            source = restored.sources()[0]
            assert source.content_hash == "sha256:roundtrip"
            assert source.external_stem_role == "vocals"
        finally:
            restored.close()
    finally:
        project.close()


def test_project_track_clip_and_source_descriptors_read_as_tokens() -> None:
    project = Project()
    try:
        aux = project.add_track("aux", "fx")
        _midi = project.add_midi_clip(0.0, 4.0)
        midi_track = _midi.track_id
        midi_clip = _midi.clip_id
        project.set_clip_loop(midi_clip, "loop", 2.0)
        kinds = {track.id: track.kind for track in project.tracks()}
        assert kinds == {aux: "aux", midi_track: "midi"}
        clip = project.clips()[0]
        assert (clip.source_kind, clip.loop_mode) == ("midi", "loop")
        assert project.sources()[0].kind == "midi"
    finally:
        project.close()


def test_audio_source_metadata_rejects_midi_and_unknown_sources() -> None:
    project = Project()
    try:
        audio_track = project.add_track("audio", "audio")
        project.add_clip(audio_track, 0.0, 1.0, source_uri="asset://audio.wav")
        midi_clip = project.add_midi_clip(0.0, 1.0).clip_id
        midi_source_id = project.clips()[1].source_id

        with pytest.raises(SonareError):
            project.set_audio_source_metadata(midi_source_id, "sha256:bad", "lead")
        with pytest.raises(SonareError):
            project.set_audio_source_metadata(999999, "sha256:bad", "lead")

        # The invalid edits do not alter the valid audio source or MIDI model.
        assert midi_clip != 0
        assert project.sources()[0].content_hash is None
        assert project.sources()[1].content_hash is None
        with pytest.raises(TypeError, match="content_hash must be a string or None"):
            project.set_audio_source_metadata(
                project.sources()[0].id,
                0,  # type: ignore[arg-type]
                "lead",
            )
    finally:
        project.close()


def test_import_external_stems_creates_normal_audio_tracks() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000)
        imported = project.import_external_stems(
            sample_rate=48000,
            stems=[
                {
                    "name": "vocals",
                    "role": "lead",
                    "layout": "stereo",
                    "planar_samples": [
                        np.array([1.0, 0.0, 0.0], dtype=np.float32),
                        np.array([-0.5, 0.0, 0.0], dtype=np.float32),
                    ],
                    "start_frame": 113,
                },
                {
                    "name": "drums",
                    "layout": "mono",
                    "planar_samples": [np.array([0.25, 0.0, 0.0], dtype=np.float32)],
                },
            ],
        )
        assert isinstance(imported, ExternalSeparatedStemImportResult)
        with pytest.raises(TypeError):
            iter(imported)
        assert imported.track_ids.dtype == np.uint32
        assert imported.clip_ids.dtype == np.uint32
        assert len(imported.track_ids) == 2
        assert len(imported.clip_ids) == 2
        assert len(project.tracks()) == 2
        assert len(project.clips()) == 2
        with raises_coded():
            project.import_external_stems(
                48000,
                [
                    {
                        "name": "duplicate",
                        "layout": "mono",
                        "planar_samples": [np.array([0.0], dtype=np.float32)],
                    },
                    {
                        "name": "duplicate",
                        "layout": "mono",
                        "planar_samples": [np.array([0.0], dtype=np.float32)],
                    },
                ],
            )
        assert len(project.tracks()) == 2
        assert len(project.clips()) == 2
    finally:
        project.close()


# --- serialization ---------------------------------------------------------


def test_serialize_round_trips_byte_identically() -> None:
    project, *_ = _build_project()
    try:
        first = project.to_json_bytes()
        assert first

        second = Project.from_json(first)
        try:
            assert second.to_json_bytes() == first
        finally:
            second.close()
    finally:
        project.close()


def test_to_json_is_utf8_decoded() -> None:
    project, *_ = _build_project()
    try:
        text = project.to_json()
        assert isinstance(text, str)
        assert text.encode("utf-8") == project.to_json_bytes()
    finally:
        project.close()


def test_metadata_only_audio_clip_accepts_source_uri() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "lead")
        clip_id = project.add_clip(track_id, 0.0, 4.0, source_uri="asset://lead.wav")
        assert clip_id > 0
        assert "asset://lead.wav" in project.to_json()
    finally:
        project.close()


def test_unresolved_audio_sources_carry_full_uri_and_metadata() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "lead")
        long_uri = "asset://" + "a" * 300 + ".wav"
        project.add_clip(track_id, 0.0, 1.0, source_uri=long_uri)
        project.add_clip(track_id, 2.0, 1.0, source_uri="asset://stem.wav")
        restored = Project.from_json(project.to_json())
        try:
            ids = restored.unresolved_audio_source_ids()
            assert isinstance(ids, np.ndarray)
            assert ids.dtype == np.uint32
            assert len(ids) == 2
            restored.set_audio_source_metadata(ids[1], "sha256:abc", "vocals")
            sources = restored.unresolved_audio_sources()
            assert [source.id for source in sources] == ids.tolist()
            assert sources[0].name_or_uri == long_uri
            assert sources[0].kind == "audio"
            assert sources[1].name_or_uri == "asset://stem.wav"
            assert sources[1].content_hash == "sha256:abc"
            assert sources[1].external_stem_role == "vocals"
            assert restored.sources()[0].name_or_uri == long_uri
            restored.set_source_audio(ids[0], np.full((1, 480), 0.25, dtype=np.float32), 48000)
            assert [s.id for s in restored.unresolved_audio_sources()] == [ids[1]]
        finally:
            restored.close()
    finally:
        project.close()


def test_unresolved_audio_sources_empty_for_midi_only_project() -> None:
    project = Project()
    try:
        track_id = project.add_track("midi", "keys")
        project.add_clip(track_id, 0.0, 1.0, is_midi=True)
        assert project.unresolved_audio_sources() == []
    finally:
        project.close()


def test_add_audio_source_and_relink_preserve_source_identity() -> None:
    project = Project()
    try:
        uri = "asset://shared.wav"
        first_id = project.add_audio_source(uri)
        second_id = project.add_audio_source(uri)
        assert first_id != second_id
        assert [source.name_or_uri for source in project.sources()] == [uri, uri]

        assert project.relink_source(first_id, uri) == first_id
        changed_uri = "asset://replacement.wav"
        assert project.relink_source(first_id, changed_uri) == first_id
        assert [source.name_or_uri for source in project.sources()] == [changed_uri, uri]
        relinked = project.to_json_bytes()

        project.undo()
        assert [source.name_or_uri for source in project.sources()] == [uri, uri]
        project.redo()
        assert project.to_json_bytes() == relinked

        with pytest.raises(SonareValueError, match="uri"):
            project.add_audio_source("")
        with pytest.raises(SonareValueError, match="NUL"):
            project.relink_source(first_id, "asset://bad\0name")

        midi_clip = project.add_midi_clip(0.0, 1.0)
        midi_source = next(source for source in project.sources() if source.kind == "midi")
        with pytest.raises(SonareError):
            project.relink_source(midi_source.id, "asset://not-midi.wav")
        assert midi_clip.clip_id > 0
    finally:
        project.close()


def test_annotate_sections_replaces_overlapping_sections_and_undoes() -> None:
    project = Project()
    try:
        before = project.to_json_bytes()
        sections = [
            ProjectSection(start_ppq=0.0, end_ppq=8.0, label="序章"),
            ProjectSection(start_ppq=4.0, end_ppq=12.0, label="Verse"),
        ]
        assert project.section_annotations() == []
        project.annotate_sections(sections)
        assert project.section_annotations() == sections
        annotated = project.to_json_bytes()
        assert json.loads(annotated)["annotation"]["sections"] == [
            {"startPpq": 0.0, "endPpq": 8.0, "label": "序章"},
            {"startPpq": 4.0, "endPpq": 12.0, "label": "Verse"},
        ]

        project.undo()
        assert project.to_json_bytes() == before
        assert project.section_annotations() == []
        project.redo()
        assert project.to_json_bytes() == annotated
        assert project.section_annotations() == sections
        project.annotate_sections([])
        assert json.loads(project.to_json_bytes())["annotation"]["sections"] == []
        assert project.section_annotations() == []
        project.undo()
        assert project.to_json_bytes() == annotated
        assert project.section_annotations() == sections

        with pytest.raises(TypeError, match=r"sections\[0\] must be a mapping"):
            project.annotate_sections(["not a mapping"])  # type: ignore[list-item]
        with pytest.raises(SonareValueError, match=r"sections\[0\]\.label must be given"):
            project.annotate_sections([{"start_ppq": 0.0, "end_ppq": 1.0}])  # type: ignore[list-item]
        with pytest.raises(SonareValueError, match=r"unknown key sections\[0\]\.unknown"):
            project.annotate_sections(
                [{"start_ppq": 0.0, "end_ppq": 1.0, "label": "A", "unknown": 1}]
            )  # type: ignore[list-item]
        with pytest.raises(SonareValueError, match=r"sections\[0\]\.label"):
            project.annotate_sections([ProjectSection(start_ppq=0.0, end_ppq=1.0, label=" \t\n")])
    finally:
        project.close()


def test_section_annotations_refuses_closed_project() -> None:
    project = Project()
    project.close()
    with pytest.raises(SonareError):
        project.section_annotations()


def test_deserialized_audio_source_can_be_rebound_before_bounce() -> None:
    project = Project()
    audio = np.full(480, 0.25, dtype=np.float32)
    try:
        track_id = project.add_track("audio", "lead")
        project.add_clip(
            track_id,
            0.0,
            1.0,
            audio=audio,
            audio_channels=1,
            audio_sample_rate=48000,
            source_uri="asset://lead.wav",
        )
        restored = Project.from_json(project.to_json())
        try:
            ids = restored.unresolved_audio_source_ids()
            assert len(ids) == 1
            restored.set_source_audio(ids[0], audio.reshape(1, -1), 48000)
            assert restored.unresolved_audio_source_ids().size == 0
            assert restored.bounce(total_frames=480, num_channels=1).channels.shape == (1, 480)
        finally:
            restored.close()
    finally:
        project.close()


def test_malformed_deserialize_raises_without_crashing() -> None:
    with raises_coded(ErrorCode.INVALID_FORMAT):
        Project.from_json("{ this is not valid project json ]]")
    with raises_coded(ErrorCode.INVALID_FORMAT):
        Project.from_json(b"")
    with raises_coded(ErrorCode.INVALID_FORMAT):
        Project.from_json_with_diagnostics("{ this is not valid project json ]]")


@pytest.mark.parametrize(
    ("segment", "subject"),
    [
        ({"startPpq": 0.0, "bpm": 1e-8}, "tempoSegments[0].bpm"),
        (
            {"startPpq": 0.0, "bpm": 120.0, "endBpm": 1e-8},
            "tempoSegments[0].endBpm",
        ),
    ],
)
def test_from_json_refuses_subfloor_tempo_at_document_path(
    segment: dict[str, float], subject: str
) -> None:
    project = Project()
    try:
        document = json.loads(project.to_json())
    finally:
        project.close()
    document["tempoSegments"] = [segment]
    text = json.dumps(document)
    for load in (Project.from_json, Project.from_json_with_diagnostics):
        with raises_coded(
            ErrorCode.INVALID_FORMAT,
            match=rf"{re.escape(subject)} must be finite and in \[1e-6, 100000\]",
        ):
            load(text)


@pytest.mark.parametrize("version", [1, 2, 6, 8])
def test_from_json_refuses_any_version_but_the_current_one(version: int) -> None:
    project = Project()
    try:
        document = json.loads(project.to_json())
    finally:
        project.close()
    assert document["version"] == 7
    document["version"] = version
    with raises_coded(ErrorCode.INVALID_FORMAT, match=f"project schema version {version} is "):
        Project.from_json(json.dumps(document))


def test_a_refused_load_names_no_diagnostic_code() -> None:
    project = Project()
    try:
        document = json.loads(project.to_json())
    finally:
        project.close()
    document["bogus"] = 1
    loads: list[tuple[str, Callable[[str], object]]] = [
        ("Project.from_json", Project.from_json),
        ("Project.from_json_with_diagnostics", Project.from_json_with_diagnostics),
    ]
    for entry, load in loads:
        with pytest.raises(SonareError) as caught:
            load(json.dumps(document))
        assert re.fullmatch(
            rf"\[{int(ErrorCode.INVALID_FORMAT)}\] {re.escape(entry)}: unknown key bogus "
            r"\(expected [^)]*\)",
            str(caught.value),
        )


def test_from_json_with_diagnostics_returns_success_warnings() -> None:
    result = Project.from_json_with_diagnostics(dangling_source_json())
    try:
        # Warnings keep their code, one "<code>: <message>" line each.
        assert result.diagnostics.startswith("dangling_clip_source: ")
        assert len(result.project.tracks()) == 1
    finally:
        result.project.close()


# --- compile ---------------------------------------------------------------


def test_compile_surfaces_renderable_timeline() -> None:
    project, *_ = _build_project()
    try:
        result = project.compile()
        assert isinstance(result, libsonare.ProjectCompileResult)
        with pytest.raises(TypeError):
            iter(result)  # type: ignore[call-overload]
        assert [f.name for f in dataclasses.fields(result)] == ["diagnostics", "timeline"]
        assert result.timeline is None
        # The project carries a MIDI clip, so the compiler emits a single
        # best-effort warning (code 10 = kMidiClipNoInstrument) that the bounce is
        # silent unless an instrument is bound. It is non-fatal (timeline valid).
        assert len(result.diagnostics) == 1
        assert result.diagnostics[0].code == "midiClipNoInstrument"
        assert result.diagnostics[0].severity == "warning"
        assert "project contains MIDI clips" in result.diagnostics[0].message
    finally:
        project.close()


# --- undo / redo -----------------------------------------------------------


def test_undo_restores_serialized_bytes_and_redo_reapplies() -> None:
    project, audio_clip, *_ = _build_project()
    try:
        before = project.to_json_bytes()

        new_clip = project.split_clip(audio_clip, 1.0)
        assert new_clip != 0
        after = project.to_json_bytes()
        assert after != before

        project.undo()
        assert project.to_json_bytes() == before

        project.redo()
        assert project.to_json_bytes() == after
    finally:
        project.close()


def test_undo_on_empty_stack_raises() -> None:
    project = Project()
    try:
        with pytest.raises(SonareError) as exc:
            project.undo()
        assert exc.value.code != 0
        assert str(exc.value).startswith(f"[{exc.value.code}] ")
    finally:
        project.close()


def test_add_midi_clip_is_one_undo_transaction() -> None:
    project = Project()
    try:
        before = project.to_json_bytes()
        added = project.add_midi_clip(2.0, 4.0)
        assert isinstance(added, ProjectMidiClipResult)
        with pytest.raises(TypeError):
            iter(added)
        added = project.to_json_bytes()
        assert len(project.tracks()) == 1
        assert len(project.sources()) == 1
        assert len(project.clips()) == 1

        project.undo()
        assert project.to_json_bytes() == before
        assert len(project.tracks()) == 0
        assert len(project.sources()) == 0
        assert len(project.clips()) == 0

        project.redo()
        assert project.to_json_bytes() == added
        assert len(project.tracks()) == 1
        assert len(project.sources()) == 1
        assert len(project.clips()) == 1
    finally:
        project.close()


def test_set_track_midi_destination_round_trips_and_undoes() -> None:
    project = Project()
    try:
        track_id = project.add_midi_clip(0.0, 4.0).track_id
        before = project.to_json_bytes()

        project.set_track_midi_destination(track_id, 7)
        after = project.to_json_bytes()
        assert after != before
        assert b'"midiDestinationId":7' in after

        # Routes through the edit history, so undo restores the prior routing.
        project.undo()
        assert project.to_json_bytes() == before
    finally:
        project.close()


def test_set_clip_warp_ref_round_trips_and_undoes() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "audio")
        clip_id = project.add_clip(track_id=track_id, start_ppq=0.0, length_ppq=4.0)
        before = project.to_json_bytes()

        # A warp ref must name a registered warp map.
        with pytest.raises(SonareError):
            project.set_clip_warp_ref(clip_id, 123)

        project.set_warp_map(123, [(0.0, 0.0), (4.0, 4.0)])
        project.set_clip_warp_ref(clip_id, 123)
        project.set_clip_warp_mode(clip_id, "repitch")
        after = project.to_json_bytes()
        assert after != before
        assert b'"warpRefId":123' in after
        assert b'"warpMode":"repitch"' in after
        restored = Project.from_json(after)
        try:
            assert restored.to_json_bytes() == after
        finally:
            restored.close()

        project.undo()
        assert b'"warpMode":"off"' in project.to_json_bytes()
        project.undo()
        assert b'"warpRefId":0' in project.to_json_bytes()
        project.undo()
        assert project.to_json_bytes() == before

        project.set_clip_warp_mode(clip_id, "tempoSync")
        assert b'"warpMode":"tempoSync"' in project.to_json_bytes()
    finally:
        project.close()


def test_removing_warp_map_clears_clip_references_and_undo_restores_them() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "audio")
        clip_id = project.add_clip(track_id=track_id, start_ppq=0.0, length_ppq=4.0)
        project.set_warp_map(123, [(0.0, 0.0), (4.0, 4.0)])
        project.set_clip_warp_ref(clip_id, 123)
        mapped = project.to_json_bytes()

        project.remove_warp_map(123)
        assert b'"warpRefId":0' in project.to_json_bytes()
        project.undo()
        assert project.to_json_bytes() == mapped
        with pytest.raises(SonareError):
            project.set_warp_map(124, [(0.0, 0.0)])
    finally:
        project.close()


def test_set_clip_takes_and_comp_segments_round_trip_and_undo() -> None:
    project, audio_clip, *_ = _build_project()
    try:
        before = project.to_json_bytes()

        project.set_clip_takes(
            audio_clip,
            [
                {"id": 1, "source_offset_ppq": 0.0, "name": "take A"},
                {"id": 2, "source_offset_ppq": 0.5, "name": "take B"},
            ],
            active_take_id=1,
        )
        with_takes = project.to_json_bytes()
        assert with_takes != before
        assert b'"takes"' in with_takes
        assert b'"activeTakeId":1' in with_takes

        project.set_clip_comp_segments(
            audio_clip,
            [
                {"start_ppq": 0.0, "end_ppq": 1.0, "take_id": 1},
                {"start_ppq": 1.0, "end_ppq": 2.0, "take_id": 2},
            ],
        )
        with_comp = project.to_json_bytes()
        assert b'"compSegments"' in with_comp

        project.undo()
        assert project.to_json_bytes() == with_takes
        project.undo()
        assert project.to_json_bytes() == before
        project.redo()
        assert project.to_json_bytes() == with_takes

        with pytest.raises(SonareError):
            project.set_clip_takes(
                audio_clip,
                [{"id": 1, "name": "duplicate A"}, {"id": 1, "name": "duplicate B"}],
                active_take_id=1,
            )
        with pytest.raises(SonareError):
            project.set_clip_comp_segments(
                audio_clip, [{"start_ppq": 0.0, "end_ppq": 1.0, "take_id": 99}]
            )
    finally:
        project.close()


def test_comp_segment_crossfade_ppq_ctypes_offset() -> None:
    """The trailing crossfade_ppq field sits at the C offset (32-byte struct)."""
    from libsonare._ffi_types_mastering_project import SonareProjectClipCompSegment

    assert ctypes.sizeof(SonareProjectClipCompSegment) == 4 * ctypes.sizeof(ctypes.c_double)
    assert SonareProjectClipCompSegment.crossfade_ppq.offset == 3 * ctypes.sizeof(ctypes.c_double)


def test_comp_segment_crossfade_ppq_reaches_the_library() -> None:
    project, audio_clip, *_ = _build_project()
    try:
        project.set_clip_takes(
            audio_clip,
            [{"id": 1, "source_offset_ppq": 0.0}, {"id": 2, "source_offset_ppq": 0.5}],
            active_take_id=1,
        )

        # Mapping form; an omitted crossfade_ppq defaults to 0.0.
        project.set_clip_comp_segments(
            audio_clip,
            [
                {"start_ppq": 0.0, "end_ppq": 1.0, "take_id": 1},
                {"start_ppq": 1.0, "end_ppq": 2.0, "take_id": 2, "crossfade_ppq": 0.25},
            ],
        )
        as_dict = json.loads(project.to_json_bytes())
        segments = as_dict["clips"][0]["compSegments"]
        assert segments[0]["crossfadePpq"] == 0.0
        assert segments[1]["crossfadePpq"] == 0.25
    finally:
        project.close()


def test_add_loop_recording_takes_splits_capture_into_active_take() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000)
        track_id = project.add_track("audio", "record")
        before = project.to_json_bytes()
        audio = np.empty(48000, dtype=np.float32)
        audio[:24000] = 0.25
        audio[24000:] = 0.75

        recorded = project.add_loop_recording_takes(
            track_id,
            start_ppq=0.0,
            loop_length_ppq=1.0,
            audio=audio,
            audio_channels=1,
            audio_sample_rate=48000,
        )
        assert isinstance(recorded, ProjectLoopRecordingResult)
        with pytest.raises(TypeError):
            iter(recorded)
        assert recorded.clip_id != 0
        assert recorded.take_count == 2
        json = project.to_json_bytes()
        assert b'"takes"' in json
        assert b'"partial"' not in json
        assert b'"activeTakeId":2' in json
        assert len(project.sources()) == 2

        project.undo()
        assert project.to_json_bytes() == before
        assert len(project.sources()) == 0
        project.redo()
        assert project.to_json_bytes() == json
        assert len(project.sources()) == 2
    finally:
        project.close()


def _loop_recording_active_take(project: Project) -> int:
    return int(json.loads(project.to_json_bytes())["clips"][0]["activeTakeId"])


def _partial_take_ids(project: Project) -> list[int]:
    takes = json.loads(project.to_json_bytes())["clips"][0]["takes"]
    return [int(take["id"]) for take in takes if take.get("partial")]


def _capture(frames: int, **kwargs: Any) -> tuple[int, list[int], int]:
    """Add a mono capture to a fresh project; return take count, partial take ids, active take."""
    project = Project()
    try:
        project.set_sample_rate(48000)
        track_id = project.add_track("audio", "record")
        result = project.add_loop_recording_takes(
            track_id,
            0.0,
            1.0,
            np.full(frames, 0.5, dtype=np.float32),
            audio_channels=1,
            audio_sample_rate=48000,
            **kwargs,
        )
        return result.take_count, _partial_take_ids(project), _loop_recording_active_take(project)
    finally:
        project.close()


def test_add_loop_recording_takes_partial_last_take_is_not_active() -> None:
    take_count, partial, active = _capture(36000)
    assert (take_count, partial) == (2, [2])
    assert active == 1


def test_add_loop_recording_takes_drop_policy_and_short_capture() -> None:
    dropped_count, dropped_partial, _ = _capture(36000, partial_tail="drop")
    assert (dropped_count, dropped_partial) == (1, [])
    short_count, short_partial, active = _capture(12000)
    assert (short_count, short_partial) == (1, [1])
    assert active == 1
    with pytest.raises(ValueError, match="partial_tail"):
        _capture(100, partial_tail="activate")


def test_add_loop_recording_takes_one_frame_remainder_is_not_partial() -> None:
    for frames in (48001, 47999):
        take_count, partial, active = _capture(frames)
        assert (take_count, partial, active) == (2, [], 2)


def test_partial_take_mark_survives_json_round_trip() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000)
        track_id = project.add_track("audio", "record")
        project.add_loop_recording_takes(
            track_id,
            0.0,
            1.0,
            np.full(36000, 0.5, dtype=np.float32),
            audio_channels=1,
            audio_sample_rate=48000,
        )
        text = project.to_json_bytes()
        reloaded = Project.from_json(text.decode())
        try:
            assert _partial_take_ids(reloaded) == [2]
        finally:
            reloaded.close()
    finally:
        project.close()


def test_add_loop_recording_takes_planar_matches_interleaved() -> None:
    left = np.linspace(-0.5, 0.5, 48000, dtype=np.float32)
    right = np.linspace(0.5, -0.5, 48000, dtype=np.float32)
    interleaved = np.stack([left, right], axis=1).reshape(-1)
    outputs = []
    for form in ("interleaved", "list", "matrix"):
        project = Project()
        try:
            project.set_sample_rate(48000)
            track_id = project.add_track("audio", "record")
            if form == "interleaved":
                result = project.add_loop_recording_takes(
                    track_id, 0.0, 1.0, interleaved, audio_channels=2, audio_sample_rate=48000
                )
            elif form == "list":
                result = project.add_loop_recording_takes(
                    track_id, 0.0, 1.0, [left, right], audio_sample_rate=48000
                )
            else:
                result = project.add_loop_recording_takes(
                    track_id, 0.0, 1.0, np.stack([left, right]), audio_sample_rate=48000
                )
            assert result.take_count == 2
            outputs.append(project.to_json_bytes())
        finally:
            project.close()
    assert outputs[0] == outputs[1] == outputs[2]


def test_add_loop_recording_takes_refuses_mismatched_planar_lengths() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "record")
        with pytest.raises(ValueError, match="equal lengths"):
            project.add_loop_recording_takes(
                track_id,
                0.0,
                1.0,
                [np.zeros(100, dtype=np.float32), np.zeros(99, dtype=np.float32)],
                audio_sample_rate=48000,
            )
    finally:
        project.close()


def test_add_loop_recording_takes_requires_the_capture_format() -> None:
    """The library has no default for a capture's format, so each part is asked for by name."""
    project = Project()
    try:
        track_id = project.add_track("audio", "record")
        mono = np.zeros(4800, dtype=np.float32)
        with pytest.raises(SonareValueError, match="audio_channels"):
            project.add_loop_recording_takes(track_id, 0.0, 1.0, mono, audio_sample_rate=48000)
        with pytest.raises(TypeError, match="audio_sample_rate"):
            project.add_loop_recording_takes(track_id, 0.0, 1.0, mono, audio_channels=1)  # type: ignore[call-arg]
        with pytest.raises(SonareValueError, match="audio_sample_rate"):
            project.add_loop_recording_takes(
                track_id, 0.0, 1.0, mono, audio_channels=1, audio_sample_rate=0
            )
        for given in (1, 2):
            # The shape carries the count, so a second spelling is refused even when it agrees.
            with pytest.raises(SonareValueError, match="audio_channels"):
                project.add_loop_recording_takes(
                    track_id,
                    0.0,
                    1.0,
                    np.zeros((2, 2400), dtype=np.float32),
                    audio_channels=given,
                    audio_sample_rate=48000,
                )
    finally:
        project.close()


def test_project_rejects_audio_length_not_matching_channels() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "record")
        audio = np.zeros(5, dtype=np.float32)
        with pytest.raises(ValueError, match="audio length must be a multiple of audio_channels"):
            project.add_clip(
                track_id,
                start_ppq=0.0,
                length_ppq=1.0,
                audio=audio,
                audio_channels=2,
                audio_sample_rate=48000,
            )
        with pytest.raises(ValueError, match="audio length must be a multiple of audio_channels"):
            project.add_loop_recording_takes(
                track_id,
                start_ppq=0.0,
                loop_length_ppq=1.0,
                audio=audio,
                audio_channels=2,
                audio_sample_rate=48000,
            )
    finally:
        project.close()


def test_set_track_midi_destination_rejects_unknown_track() -> None:
    project = Project()
    try:
        with raises_coded():
            project.set_track_midi_destination(9999, 1)
    finally:
        project.close()


# --- MIR -------------------------------------------------------------------


def test_snap_to_grid_snaps_near_beat_to_line() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000)
        assert project.snap_to_grid(1.02, 1.0) == 1.0
        assert project.snap_to_grid(0.27, 1.0, division=4) == 0.25
        assert project.snap_to_grid(4.02, 1.0, division=0) == 4.0
    finally:
        project.close()


@pytest.mark.parametrize("apply_meter", [False, True])
def test_auto_tempo_returns_selected_bpm_confidence_and_meter(apply_meter: bool) -> None:
    project = Project()
    try:
        project.set_sample_rate(48000)
        # A steady click-like pulse train so the beat bridge has onsets.
        sr = 48000
        audio = np.zeros(sr * 2, dtype=np.float32)
        for beat in range(0, len(audio), sr // 2):  # 120 BPM
            audio[beat : beat + 64] = 1.0
        candidates = project.analyze_tempo(audio, sr)
        assert candidates
        assert candidates[0]["bpm"] > 0.0
        assert candidates[0]["label"] in {"primary", "half", "double"}
        assert candidates[0]["time_signature"]["numerator"] > 0
        result = project.auto_tempo(audio, sr, candidate_index=0, apply_time_signatures=apply_meter)
        assert isinstance(result, ProjectAutoTempoResult)
        assert result.bpm > 0.0
        assert 0.0 <= result.bpm_confidence <= 1.0
        assert result.time_signature.numerator > 0
        assert result.time_signature.denominator > 0
        assert 0.0 <= result.time_signature.confidence <= 1.0
        if not apply_meter:
            assert project.time_signatures() == []
    finally:
        project.close()


@pytest.mark.parametrize(
    ("option", "value"),
    [
        ("apply_time_signatures", 1),
        ("adaptive_tempo", "yes"),
        ("include_octave_candidates", []),
        ("tempo_update_interval_beats", "four"),
        ("ramp_threshold", "small"),
        ("candidate_index", "first"),
    ],
)
def test_auto_tempo_checks_option_type_before_sample_content(option: str, value: object) -> None:
    project = Project.__new__(Project)
    with pytest.raises(TypeError, match=option):
        project.auto_tempo([float("nan")], 48000, **{option: value})


def test_set_track_route_requires_the_track_id() -> None:
    project = Project.__new__(Project)
    with pytest.raises(TypeError, match="track_id"):
        project.set_track_route(channel_strip_ref=None)  # type: ignore[call-arg]


@pytest.mark.parametrize("method", ["analyze_tempo", "auto_tempo"])
def test_project_tempo_options_are_keyword_only(method: str) -> None:
    project = Project.__new__(Project)
    with pytest.raises(TypeError, match="positional"):
        getattr(project, method)([], 48000, False)


def test_project_bounce_binding_records_are_frozen_and_keyword_only() -> None:
    from libsonare import (
        ProjectInstrumentBinding,
        ProjectSf2InstrumentBinding,
        Sf2InstrumentConfig,
        SynthPatch,
        types,
    )

    synth = SynthPatch()
    binding = ProjectInstrumentBinding[SynthPatch](instrument=synth)
    assert binding.instrument is synth
    assert binding.destination_id is None
    sf2 = ProjectSf2InstrumentBinding(instrument=Sf2InstrumentConfig(), destination_id=3)
    assert sf2.destination_id == 3
    assert sf2.sound_font_uri is None
    assert isinstance(sf2, ProjectInstrumentBinding)
    assert types.ProjectInstrumentBinding is ProjectInstrumentBinding
    assert types.ProjectSf2InstrumentBinding is ProjectSf2InstrumentBinding
    with pytest.raises(dataclasses.FrozenInstanceError):
        binding.destination_id = 2
    with pytest.raises(TypeError):
        ProjectInstrumentBinding(synth)


def test_project_bounce_checks_scalar_and_destination_types_before_count_bounds() -> None:
    from libsonare import ProjectInstrumentBinding, SynthPatch

    project = Project.__new__(Project)
    with pytest.raises(TypeError, match="total_frames"):
        project.bounce(count_in_bars=17, total_frames="1")  # type: ignore[arg-type]
    binding = ProjectInstrumentBinding(
        instrument=SynthPatch(),
        destination_id="1",  # type: ignore[arg-type]
    )
    with pytest.raises(TypeError, match=r"synth_instruments\[0\]\.destination_id"):
        project.bounce(count_in_bars=17, synth_instruments=[binding])


@pytest.mark.parametrize("missing", ["start_ppq", "end_ppq"])
def test_chord_annotation_requires_both_ppq_endpoints(missing: str) -> None:
    project = Project.__new__(Project)
    chord = {"start_ppq": 0.0, "end_ppq": 0.5}
    del chord[missing]
    with pytest.raises(SonareValueError, match=rf"chords\[0\]\.{missing} must be given"):
        project.annotate_chords((MappingProxyType(chord),))


def test_chord_argument_refusals_name_row_and_extension_indices() -> None:
    project = Project.__new__(Project)
    chord = MappingProxyType({"start_ppq": 0.0, "end_ppq": 0.5, "extensions": (7, 2.5)})
    with pytest.raises(SonareValueError, match=r"chords\[1\]\.extensions\[1\]"):
        project.annotate_chords(({"start_ppq": 0.0, "end_ppq": 0.5}, chord))
    with pytest.raises(SonareValueError, match=r"chords\[0\]\.roman_numeral"):
        project.annotate_chords(({"start_ppq": 0.0, "end_ppq": 0.5, "roman_numeral": "I\0V"},))


# --- lifecycle -------------------------------------------------------------


def test_context_manager_closes_handle_and_has_no_close_aliases() -> None:
    with Project() as project:
        project.set_sample_rate(44100)
    project.close()  # a second close is a no-op
    assert not hasattr(project, "destroy")
    assert not hasattr(project, "delete")
    with raises_coded(ErrorCode.INVALID_STATE):
        project.add_track("audio")


def test_overlap_policy_is_a_token_and_an_ordinal_is_refused() -> None:
    project = Project()
    try:
        with pytest.raises(TypeError, match="policy"):
            project.set_overlap_policy(1)  # type: ignore[arg-type]
        with pytest.raises(SonareValueError, match="policy"):
            project.set_overlap_policy("overlap")  # type: ignore[arg-type]
        assert project.overlap_policy == "disallow"
    finally:
        project.close()


def test_project_getters_setters_and_counts() -> None:
    project = Project()
    try:
        project.set_sample_rate(44100)
        assert project.sample_rate == 44100.0

        # The stored-list readers stay empty while the effective timeline uses
        # its default tempo and meter.
        assert project.tempo_segments() == []
        assert project.time_signatures() == []
        default_position = project.position_at_ppq(0.0)
        assert default_position.bpm == 120.0
        assert (
            default_position.time_signature.numerator,
            default_position.time_signature.denominator,
        ) == (4, 4)

        assert project.overlap_policy == "disallow"
        project.set_overlap_policy("allow")
        assert project.overlap_policy == "allow"

        project.set_tempo_segments(
            [{"start_ppq": 0.25, "bpm": 120.0}, {"start_ppq": 480.125, "bpm": 140.0}]
        )
        # A segment read back is in the shape set_tempo_segments takes.
        assert project.tempo_segments() == [
            {"start_ppq": 0.25, "bpm": 120.0, "end_bpm": 0.0},
            {"start_ppq": 480.125, "bpm": 140.0, "end_bpm": 0.0},
        ]

        project.set_time_signatures(
            [
                {"start_ppq": 0.25, "numerator": 4, "denominator": 4},
                {"start_ppq": 1920.125, "numerator": 3, "denominator": 4},
            ]
        )
        assert project.time_signatures() == [
            {"start_ppq": 0.25, "numerator": 4, "denominator": 4},
            {"start_ppq": 1920.125, "numerator": 3, "denominator": 4},
        ]
        project.set_tempo_segments([])
        project.set_time_signatures([])
        assert project.tempo_segments() == []
        assert project.time_signatures() == []
        assert project.position_at_ppq(0.0) == default_position

        marker_id = project.set_marker(1.0, name="intro")
        assert marker_id > 0

        assert project.tracks() == []
        assert project.clips() == []
        assert project.sources() == []

        track_id = project.add_track("midi", "notes")
        clip_id = project.add_clip(track_id, 0.0, 4.0, is_midi=True)
        assert [clip.id for clip in project.clips()] == [clip_id]
        restored = Project.from_json(project.to_json())
        try:
            assert [clip.id for clip in restored.clips()] == [clip_id]
        finally:
            restored.close()
        project.remove_clip(clip_id)
        assert project.clips() == []
    finally:
        project.close()


def test_project_tempo_domain_refusals_preserve_stored_map() -> None:
    project = Project()
    try:
        project.set_tempo_segments([{"start_ppq": 0.0, "bpm": PROJECT_TEMPO_MIN, "end_bpm": 0.0}])
        assert project.tempo_segments() == [
            {"start_ppq": 0.0, "bpm": PROJECT_TEMPO_MIN, "end_bpm": 0.0}
        ]
        before = project.tempo_segments()
        invalid = (
            ({"start_ppq": 0.0, "bpm": 1e-8}, "segments[0].bpm"),
            ({"start_ppq": 0.0, "bpm": 100000.1}, "segments[0].bpm"),
            ({"start_ppq": 0.0, "bpm": 120.0, "end_bpm": 1e-8}, "segments[0].end_bpm"),
            (
                {"start_ppq": 0.0, "bpm": 120.0, "end_bpm": 100000.1},
                "segments[0].end_bpm",
            ),
        )
        for segment, subject in invalid:
            with pytest.raises(SonareValueError) as caught:
                project.set_tempo_segments([segment])
            assert (
                f"Project.set_tempo_segments: {subject} must be finite and in [1e-6, 100000]"
            ) in str(caught.value)
            assert project.tempo_segments() == before
    finally:
        project.close()


def test_project_marker_round_trips_kind_and_key() -> None:
    project = Project()
    try:
        plain_id = project.set_marker(1.0, name="intro")
        assert plain_id > 0
        key_id = project.set_marker(
            2.0, name="E flat major", kind="keySignature", key_fifths=-3, key_minor=False
        )
        assert key_id > 0 and key_id != plain_id

        plain, key = project.markers()
        assert plain.id == plain_id
        assert plain.kind == "marker"
        assert plain.name == "intro"
        assert plain.key_fifths == 0
        assert plain.key_minor is False

        assert key.id == key_id
        assert key.kind == "keySignature"
        assert key.name == "E flat major"
        assert key.key_fifths == -3
        assert key.key_minor is False

        # Naming an existing id replaces that marker rather than adding one.
        assert project.set_marker(3.0, marker_id=plain_id, name="verse") == plain_id
        replaced = {m.id: (m.ppq, m.name) for m in project.markers()}
        assert replaced == {plain_id: (3.0, "verse"), key_id: (2.0, "E flat major")}
    finally:
        project.close()


@pytest.mark.parametrize("kind", ["marker", "text", "lyric", "cuePoint"])
@pytest.mark.parametrize(
    ("key_fifths", "key_minor", "field"),
    [(1, None, "key_fifths"), (0, True, "key_minor")],
)
def test_project_markers_refuse_inactive_key_signature_fields(
    kind: str, key_fifths: int, key_minor: bool | None, field: str
) -> None:
    with Project() as project:
        with pytest.raises(SonareError) as caught:
            project.set_marker(1.0, kind=kind, key_fifths=key_fifths, key_minor=key_minor)
        assert caught.value.code == ErrorCode.INVALID_PARAMETER
        assert str(caught.value) == (
            f"Project.set_marker: {field} must be 0 unless kind is key signature"
        )
        assert project.markers() == []


@pytest.mark.parametrize("key_fifths", [-8, 8])
def test_project_key_signature_marker_core_checks_fifths_domain(key_fifths: int) -> None:
    with Project() as project:
        with pytest.raises(SonareError) as caught:
            project.set_marker(1.0, kind="keySignature", key_fifths=key_fifths)
        assert caught.value.code == ErrorCode.INVALID_PARAMETER
        assert "key_fifths" in str(caught.value)


def test_project_key_signature_marker_accepts_boundary_fifths() -> None:
    with Project() as project:
        low = project.set_marker(1.0, kind="keySignature", key_fifths=-7)
        high = project.set_marker(2.0, kind="keySignature", key_fifths=7, key_minor=True)
        assert [(marker.key_fifths, marker.key_minor) for marker in project.markers()] == [
            (-7, False),
            (7, True),
        ]
        assert low != high


def test_project_marker_refuses_an_explicit_zero_id_and_a_numeric_kind() -> None:
    with Project() as project:
        with pytest.raises(SonareValueError, match="marker_id"):
            project.set_marker(1.0, marker_id=0)
        with pytest.raises(TypeError, match="kind"):
            project.set_marker(1.0, kind=1)  # type: ignore[arg-type]
        assert project.markers() == []


def test_project_marker_round_trips_long_utf8_name() -> None:
    project = Project()
    try:
        name = "あいうえお" * 7
        assert len(name) == 35
        marker_id = project.set_marker(1.0, name=name)
        assert marker_id > 0
        assert project.markers()[0].name == name
        assert json.loads(project.to_json())["markers"][0]["name"] == name
    finally:
        project.close()


def _accelerando(
    sample_rate: int = 22050,
    start_bpm: float = 100.0,
    end_bpm: float = 160.0,
    seconds: float = 24.0,
) -> np.ndarray:
    """A click track whose tempo sweeps from ``start_bpm`` to ``end_bpm``."""
    audio = np.zeros(int(sample_rate * seconds), dtype=np.float32)
    click = np.exp(-np.arange(64) / 12.0) * np.where(np.arange(64) % 2 == 0, 1.0, -1.0)
    t = 0.0
    while t < seconds:
        at = int(t * sample_rate)
        end = min(at + click.size, audio.size)
        audio[at:end] += click[: end - at]
        t += 60.0 / (start_bpm + (end_bpm - start_bpm) * (t / seconds))
    return audio


def _segments(audio: np.ndarray, sample_rate: int = 22050, **options: object) -> list[dict]:
    project = Project()
    try:
        project.auto_tempo(audio, sample_rate, **options)
        return project.tempo_segments()
    finally:
        project.close()


def test_tempo_options_reach_the_bridge() -> None:
    """adaptive_tempo is what lets a tempo map follow a tempo that moves.

    Without it the tracker fits one tempo to the whole take, so the map it
    produces describes the take's average rather than its shape.
    """
    audio = _accelerando()

    fixed = _segments(audio)
    adaptive = _segments(audio, adaptive_tempo=True)
    assert len(adaptive) > len(fixed)

    # The reported tempo has to span the sweep, not sit on its average.
    bpms = [segment["bpm"] for segment in adaptive]
    assert max(bpms) - min(bpms) > 20.0

    # A coarser ramp threshold merges more of the take into constant stretches,
    # which is the direction the argument is documented to move.
    coarse = _segments(audio, adaptive_tempo=True, ramp_threshold=0.2)
    assert len(coarse) < len(adaptive)


def test_tempo_options_default_to_the_previous_behaviour() -> None:
    """Passing nothing must not quietly enable anything."""
    audio = _accelerando()
    assert _segments(audio) == _segments(audio, adaptive_tempo=None, ramp_threshold=None)

    candidates = Project().analyze_tempo(audio, 22050)
    assert candidates
    assert candidates[0]["bpm"] > 0.0
    assert {c["label"] for c in candidates} <= {"primary", "half", "double"}


def test_octave_candidates_can_be_switched_off() -> None:
    audio = _accelerando()
    project = Project()
    try:
        assert len(project.analyze_tempo(audio, 22050, include_octave_candidates=True)) > 1
        assert len(project.analyze_tempo(audio, 22050, include_octave_candidates=False)) == 1
    finally:
        project.close()


def test_tempo_options_reject_an_unusable_value() -> None:
    audio = _accelerando()
    project = Project()
    try:
        with pytest.raises(SonareError):
            project.auto_tempo(audio, 22050, tempo_update_interval_beats=0)
        with pytest.raises(SonareError):
            project.analyze_tempo(audio, 22050, ramp_threshold=-1.0)
    finally:
        project.close()


def test_replacing_source_audio_carries_its_shape_and_undo_restores_it() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "lead")
        project.add_clip(track_id, 0.0, 1.0, source_uri="asset://take.wav")
        source_id = project.sources()[0].id
        project.set_source_audio(source_id, np.full((1, 480), 0.25, dtype=np.float32), 48000)
        mono = project.sources()[0]
        assert (mono.num_channels, mono.sample_rate_hint) == (1, 48000.0)
        project.set_source_audio(source_id, np.full((2, 441), 0.25, dtype=np.float32), 44100)
        stereo = project.sources()[0]
        assert (stereo.num_channels, stereo.sample_rate_hint) == (2, 44100.0)
        project.undo()
        restored = project.sources()[0]
        assert (restored.num_channels, restored.sample_rate_hint) == (1, 48000.0)
    finally:
        project.close()


def _library_tokens(name_fn: Any) -> list[str]:
    tokens: list[str] = []
    while (token := name_fn(len(tokens))) is not None:
        tokens.append(token.decode("ascii"))
    return tokens


def test_diagnostic_token_types_list_the_library_tokens_in_order() -> None:
    lib = _get_lib()
    assert list(get_args(ProjectDiagnosticCode)) == _library_tokens(
        lib.sonare_project_diagnostic_code_name
    )
    assert list(get_args(ProjectDiagnosticSeverity)) == _library_tokens(
        lib.sonare_project_diagnostic_severity_name
    )


@pytest.mark.parametrize("earlier", ["time_signatures", "midi_content", "tempo_segments"])
def test_project_document_refuses_an_earlier_snake_case_key(earlier: str) -> None:
    project = Project()
    try:
        document = json.loads(project.to_json())
    finally:
        project.close()
    document[earlier] = []
    with raises_coded(ErrorCode.INVALID_FORMAT, match=earlier):
        Project.from_json(json.dumps(document))
