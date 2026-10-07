"""Tests for the headless arrangement / DAW :class:`Project` Python wrapper.

Mirrors the C keystone parity test
(``tests/bindings/binding_project_parity_test.cpp``): it drives the
``sonare_project_*`` C ABI end to end through the Python wrapper and pins the
ABI version, deterministic serialize byte-stability, bit-exact bounce,
undo/redo, MIR snap-to-grid, and malformed-input handling.
"""

from __future__ import annotations

import ctypes
import json
from typing import Any

import numpy as np
import pytest

from libsonare import (
    MarkerKind,
    Project,
    ProjectMarker,
    SonareError,
    project_abi_version,
)
from libsonare._project import EXPECTED_PROJECT_ABI_VERSION

from ._project_helpers import _build_project


def _dangling_source_json() -> str:
    return (
        '{"version":1,"sample_rate":48000,'
        '"tracks":[{"id":1,"name":"audio","kind":0,"channel_strip_ref":"",'
        '"output_target":"","midi_destination_id":0,"automation_lanes":[]}],'
        '"clips":[{"id":1,"track_id":1,"source_id":99,"start_ppq":0,'
        '"length_ppq":1,"source_offset_ppq":0,"gain":1,'
        '"fade_in":{"length_ppq":0,"curve":0},'
        '"fade_out":{"length_ppq":0,"curve":0},'
        '"loop_mode":0,"loop_length_ppq":0,"warp_ref_id":0,"warp_mode":0}]}'
    )


# --- ABI version -----------------------------------------------------------


def test_project_abi_version_matches_expected() -> None:
    """The runtime project ABI version is exposed and matches the binding."""
    assert project_abi_version() == EXPECTED_PROJECT_ABI_VERSION
    assert project_abi_version() > 0


def test_create_factory_matches_constructor() -> None:
    """The cross-binding factory creates an empty project usable like ``Project()``."""
    created = Project.create()
    constructed = Project()
    try:
        assert isinstance(created, Project)
        assert created.to_json_bytes() == constructed.to_json_bytes()
        created.set_sample_rate(48000.0)
        track_id = created.add_track("audio", "factory")
        assert track_id > 0
        assert created.track_by_index(0).id == track_id
    finally:
        created.close()
        constructed.close()


def test_reads_stored_tracks_clips_and_sources_by_index() -> None:
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

        track = project.track_by_index(0)
        assert track.id == track_id
        assert track.kind == 0
        assert track.name == "readback"
        assert track.gain == 1.0
        assert track.pan == 0.0
        assert not track.mute
        assert not track.solo

        clip = project.clip_by_index(0)
        assert clip.id == clip_id
        assert clip.track_id == track_id
        assert clip.source_kind == 0
        assert clip.start_ppq == 2.0
        assert clip.length_ppq == 4.0
        assert clip.source_offset_ppq == 1.0
        assert clip.gain == 0.75
        assert clip.loop_mode == 0
        assert clip.loop_length_ppq == 0.0

        source = project.source_by_index(0)
        assert source.id == clip.source_id
        assert source.kind == 0
        assert source.name_or_uri == "asset://readback.wav"
        assert source.content_hash == ""
        assert source.external_stem_role == ""

        with pytest.raises(SonareError):
            project.track_by_index(1)
        with pytest.raises(SonareError):
            project.clip_by_index(1)
        with pytest.raises(SonareError):
            project.source_by_index(1)
    finally:
        project.close()


def test_audio_source_metadata_roundtrips_clears_and_undoes() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "metadata")
        clip_id = project.add_clip(track_id, 0.0, 1.0, source_uri="asset://metadata.wav")
        source_id = project.clip_by_index(0).source_id
        assert clip_id != 0

        project.set_audio_source_metadata(source_id, "sha256:deadbeef", "lead")
        source = project.source_by_index(0)
        assert source.content_hash == "sha256:deadbeef"
        assert source.external_stem_role == "lead"

        # Empty values clear each field while preserving the source itself.
        project.set_audio_source_metadata(source_id, "", "")
        source = project.source_by_index(0)
        assert source.id == source_id
        assert source.name_or_uri == "asset://metadata.wav"
        assert source.content_hash == ""
        assert source.external_stem_role == ""

        project.undo()
        source = project.source_by_index(0)
        assert source.content_hash == "sha256:deadbeef"
        assert source.external_stem_role == "lead"
        project.redo()
        source = project.source_by_index(0)
        assert source.content_hash == ""
        assert source.external_stem_role == ""

        project.set_audio_source_metadata(source_id, "sha256:roundtrip", "vocals")
        restored = Project.from_json(project.to_json())
        try:
            source = restored.source_by_index(0)
            assert source.content_hash == "sha256:roundtrip"
            assert source.external_stem_role == "vocals"
        finally:
            restored.close()
    finally:
        project.close()


def test_audio_source_metadata_rejects_midi_and_unknown_sources() -> None:
    project = Project()
    try:
        audio_track = project.add_track("audio", "audio")
        project.add_clip(audio_track, 0.0, 1.0, source_uri="asset://audio.wav")
        _, midi_clip = project.add_midi_clip(0.0, 1.0)
        midi_source_id = project.clip_by_index(1).source_id

        with pytest.raises(SonareError):
            project.set_audio_source_metadata(midi_source_id, "sha256:bad", "lead")
        with pytest.raises(SonareError):
            project.set_audio_source_metadata(999999, "sha256:bad", "lead")

        # The invalid edits do not alter the valid audio source or MIDI model.
        assert midi_clip != 0
        assert project.source_by_index(0).content_hash == ""
        assert project.source_by_index(1).content_hash == ""
        with pytest.raises(TypeError):
            project.set_audio_source_metadata(
                project.source_by_index(0).id,
                None,
                "lead",  # type: ignore[arg-type]
            )
    finally:
        project.close()


def test_import_external_stems_creates_normal_audio_tracks() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000)
        track_ids, clip_ids = project.import_external_stems(
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
        assert len(track_ids) == 2
        assert len(clip_ids) == 2
        assert project.track_count() == 2
        assert project.clip_count() == 2
        with pytest.raises(RuntimeError):
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
        assert project.track_count() == 2
        assert project.clip_count() == 2
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
            assert len(ids) == 2
            restored.set_audio_source_metadata(ids[1], "sha256:abc", "vocals")
            sources = restored.unresolved_audio_sources()
            assert [source.id for source in sources] == ids
            assert sources[0].name_or_uri == long_uri
            assert sources[0].kind == 0
            assert sources[1].name_or_uri == "asset://stem.wav"
            assert sources[1].content_hash == "sha256:abc"
            assert sources[1].external_stem_role == "vocals"
            assert restored.source_by_index(0).name_or_uri == long_uri
            restored.set_source_audio(ids[0], np.full(480, 0.25, dtype=np.float32), 1, 48000)
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
            restored.set_source_audio(ids[0], audio, 1, 48000)
            assert restored.unresolved_audio_source_ids() == []
            assert restored.bounce(total_frames=480, num_channels=1).shape == (480, 1)
        finally:
            restored.close()
    finally:
        project.close()


def test_malformed_deserialize_raises_without_crashing() -> None:
    with pytest.raises(ValueError):
        Project.from_json("{ this is not valid project json ]]")
    with pytest.raises(ValueError):
        Project.from_json(b"")


def test_from_json_with_diagnostics_returns_success_warnings() -> None:
    result = Project.from_json_with_diagnostics(_dangling_source_json())
    try:
        assert "dangling_clip_source" in result.diagnostics
        assert result.project.track_count() == 1
    finally:
        result.project.close()


# --- compile ---------------------------------------------------------------


def test_compile_surfaces_renderable_timeline() -> None:
    project, *_ = _build_project()
    try:
        has_timeline, messages = project.compile()
        assert has_timeline is True
        assert isinstance(messages, str)
        result = project.compile()
        assert result.has_timeline is True
        # The project carries a MIDI clip, so the compiler emits a single
        # best-effort warning (code 10 = kMidiClipNoInstrument) that the bounce is
        # silent unless an instrument is bound. It is non-fatal (timeline valid).
        assert result.diagnostic_count == 1
        assert result.diagnostics[0].code == 10
        assert result.diagnostics[0].severity == 1  # warning
        assert "project contains MIDI clips" in result.diagnostics[0].message
        assert result.messages.splitlines()[0] == result.diagnostics[0].message
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
        project.add_midi_clip(2.0, 4.0)
        added = project.to_json_bytes()
        assert project.track_count() == 1
        assert project.source_count() == 1
        assert project.clip_count() == 1

        project.undo()
        assert project.to_json_bytes() == before
        assert project.track_count() == 0
        assert project.source_count() == 0
        assert project.clip_count() == 0

        project.redo()
        assert project.to_json_bytes() == added
        assert project.track_count() == 1
        assert project.source_count() == 1
        assert project.clip_count() == 1
    finally:
        project.close()


def test_set_track_midi_destination_round_trips_and_undoes() -> None:
    project = Project()
    try:
        track_id, _clip_id = project.add_midi_clip(0.0, 4.0)
        before = project.to_json_bytes()

        project.set_track_midi_destination(track_id, 7)
        after = project.to_json_bytes()
        assert after != before
        assert b'"midi_destination_id":7' in after

        # Routes through the edit history, so undo restores the prior routing.
        project.undo()
        assert project.to_json_bytes() == before
    finally:
        project.close()


def test_set_clip_warp_ref_round_trips_and_undoes() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "audio")
        clip_id = project.add_clip(
            track_id=track_id, start_ppq=0.0, length_ppq=4.0, audio_channels=0
        )
        before = project.to_json_bytes()

        # A warp ref must name a registered warp map.
        with pytest.raises(SonareError):
            project.set_clip_warp_ref(clip_id, 123)

        project.set_warp_map(123, [(0.0, 0.0), (4.0, 4.0)])
        project.set_clip_warp_ref(clip_id, 123)
        project.set_clip_warp_mode(clip_id, "repitch")
        after = project.to_json_bytes()
        assert after != before
        assert b'"warp_ref_id":123' in after
        assert b'"warp_mode":1' in after
        restored = Project.from_json(after)
        try:
            assert restored.to_json_bytes() == after
        finally:
            restored.close()

        project.undo()
        assert b'"warp_mode":0' in project.to_json_bytes()
        project.undo()
        assert b'"warp_ref_id":0' in project.to_json_bytes()
        project.undo()
        assert project.to_json_bytes() == before

        project.set_clip_warp_mode(clip_id, "tempo-sync")
        assert b'"warp_mode":2' in project.to_json_bytes()
    finally:
        project.close()


def test_removing_warp_map_clears_clip_references_and_undo_restores_them() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "audio")
        clip_id = project.add_clip(
            track_id=track_id, start_ppq=0.0, length_ppq=4.0, audio_channels=0
        )
        project.set_warp_map(123, [(0.0, 0.0), (4.0, 4.0)])
        project.set_clip_warp_ref(clip_id, 123)
        mapped = project.to_json_bytes()

        project.remove_warp_map(123)
        assert b'"warp_ref_id":0' in project.to_json_bytes()
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
                {"id": 1, "sourceOffsetPpq": 0.0, "name": "take A"},
                {"id": 2, "source_offset_ppq": 0.5, "name": "take B"},
            ],
            active_take_id=1,
        )
        with_takes = project.to_json_bytes()
        assert with_takes != before
        assert b'"takes"' in with_takes
        assert b'"active_take_id":1' in with_takes

        project.set_clip_comp_segments(
            audio_clip,
            [
                {"startPpq": 0.0, "endPpq": 1.0, "takeId": 1},
                {"start_ppq": 1.0, "end_ppq": 2.0, "take_id": 2},
            ],
        )
        with_comp = project.to_json_bytes()
        assert b'"comp_segments"' in with_comp

        project.undo()
        assert project.to_json_bytes() == with_takes
        project.undo()
        assert project.to_json_bytes() == before
        project.redo()
        assert project.to_json_bytes() == with_takes

        with pytest.raises(SonareError):
            project.set_clip_takes(
                audio_clip,
                [(1, 0, 0.0, "duplicate A"), (1, 0, 0.0, "duplicate B")],
                active_take_id=1,
            )
        with pytest.raises(SonareError):
            project.set_clip_comp_segments(
                audio_clip, [{"startPpq": 0.0, "endPpq": 1.0, "takeId": 99}]
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

        # Mapping form, both naming conventions; omitted defaults to 0.0.
        project.set_clip_comp_segments(
            audio_clip,
            [
                {"start_ppq": 0.0, "end_ppq": 1.0, "take_id": 1},
                {"startPpq": 1.0, "endPpq": 2.0, "takeId": 2, "crossfadePpq": 0.25},
            ],
        )
        as_dict = json.loads(project.to_json_bytes())
        segments = as_dict["clips"][0]["comp_segments"]
        assert segments[0]["crossfade_ppq"] == 0.0
        assert segments[1]["crossfade_ppq"] == 0.25

        # 4-tuple positional form round-trips the same way.
        project.set_clip_comp_segments(
            audio_clip,
            [(0.0, 1.0, 1, 0.0), (1.0, 2.0, 2, 0.25)],
        )
        as_tuple_dict = json.loads(project.to_json_bytes())
        assert as_tuple_dict["clips"][0]["comp_segments"] == segments

        # The 3-tuple compatibility form still works and defaults to 0.0.
        project.set_clip_comp_segments(audio_clip, [(0.0, 1.0, 1), (1.0, 2.0, 2)])
        as_three_tuple_dict = json.loads(project.to_json_bytes())
        assert as_three_tuple_dict["clips"][0]["comp_segments"][1]["crossfade_ppq"] == 0.0
    finally:
        project.close()


def test_add_loop_recording_takes_splits_capture_into_active_take() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000.0)
        track_id = project.add_track("audio", "record")
        before = project.to_json_bytes()
        audio = np.empty(48000, dtype=np.float32)
        audio[:24000] = 0.25
        audio[24000:] = 0.75

        clip_id, take_count = project.add_loop_recording_takes(
            track_id,
            start_ppq=0.0,
            loop_length_ppq=1.0,
            audio=audio,
            audio_channels=1,
            audio_sample_rate=48000,
        )
        assert clip_id != 0
        assert take_count == 2
        json = project.to_json_bytes()
        assert b'"takes"' in json
        assert b'"partial"' not in json
        assert b'"active_take_id":2' in json
        assert project.source_count() == 2

        project.undo()
        assert project.to_json_bytes() == before
        assert project.source_count() == 0
        project.redo()
        assert project.to_json_bytes() == json
        assert project.source_count() == 2
    finally:
        project.close()


def _loop_recording_active_take(project: Project) -> int:
    return int(json.loads(project.to_json_bytes())["clips"][0]["active_take_id"])


def _partial_take_ids(project: Project) -> list[int]:
    takes = json.loads(project.to_json_bytes())["clips"][0]["takes"]
    return [int(take["id"]) for take in takes if take.get("partial")]


def _capture(frames: int, **kwargs: Any) -> tuple[int, list[int], int]:
    """Add a mono capture to a fresh project; return take count, partial take ids, active take."""
    project = Project()
    try:
        project.set_sample_rate(48000.0)
        track_id = project.add_track("audio", "record")
        result = project.add_loop_recording_takes(
            track_id,
            0.0,
            1.0,
            np.full(frames, 0.5, dtype=np.float32),
            audio_sample_rate=48000,
            **kwargs,
        )
        return result[1], _partial_take_ids(project), _loop_recording_active_take(project)
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
        project.set_sample_rate(48000.0)
        track_id = project.add_track("audio", "record")
        project.add_loop_recording_takes(
            track_id, 0.0, 1.0, np.full(36000, 0.5, dtype=np.float32), audio_sample_rate=48000
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
            project.set_sample_rate(48000.0)
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
            assert result[1] == 2
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
        with pytest.raises(RuntimeError):
            project.set_track_midi_destination(9999, 1)
    finally:
        project.close()


# --- MIR -------------------------------------------------------------------


def test_snap_to_grid_snaps_near_beat_to_line() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000.0)
        assert project.snap_to_grid(1.02, 1.0) == 1.0
        assert project.snap_to_grid(0.27, 1.0, division=4) == 0.25
        assert project.snap_to_grid(4.02, 1.0, division=0) == 4.0
    finally:
        project.close()


def test_auto_tempo_returns_positive_bpm() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000.0)
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
        bpm = project.auto_tempo(audio, sr, candidate_index=0, apply_time_signatures=True)
        assert bpm > 0.0
    finally:
        project.close()


# --- lifecycle -------------------------------------------------------------


def test_context_manager_and_aliases_close_handle() -> None:
    with Project() as project:
        project.set_sample_rate(44100.0)
    # Cross-binding close aliases are all safe no-ops after close.
    project.destroy()
    project.delete()
    project.close()
    with pytest.raises(RuntimeError):
        project.add_track("audio")


def test_project_getters_setters_and_counts() -> None:
    project = Project()
    try:
        project.set_sample_rate(44100.0)
        assert project.get_sample_rate() == 44100.0

        project.set_overlap_policy(1)
        assert project.get_overlap_policy() == 1

        project.set_tempo_segments([(0.0, 120.0), (480.0, 140.0)])
        assert project.tempo_segment_count() == 2
        # The count is only usable alongside a way to read the segments it counts.
        assert project.tempo_segment_by_index(0) == {
            "start_ppq": 0.0,
            "bpm": 120.0,
            "end_bpm": 0.0,
        }
        assert project.tempo_segment_by_index(1)["bpm"] == 140.0
        with pytest.raises(SonareError):
            project.tempo_segment_by_index(2)

        project.set_time_signatures([(0.0, 4, 4), (1920.0, 3, 4)])
        assert project.time_signature_count() == 2
        assert project.time_signature_by_index(1) == {
            "start_ppq": 1920.0,
            "numerator": 3,
            "denominator": 4,
        }
        with pytest.raises(SonareError):
            project.time_signature_by_index(2)

        marker_id = project.set_marker(0, 1.0, "intro")
        assert marker_id > 0

        assert project.track_count() == 0
        assert project.clip_count() == 0
        assert project.source_count() == 0

        track_id = project.add_track("midi", "notes")
        clip_id = project.add_clip(track_id, 0.0, 4.0, is_midi=True)
        assert project.clip_count() == 1
        restored = Project.from_json(project.to_json())
        try:
            assert restored.clip_count() == 1
        finally:
            restored.close()
        project.remove_clip(clip_id)
        assert project.clip_count() == 0
    finally:
        project.close()


def test_project_marker_ex_round_trips_kind_and_key() -> None:
    project = Project()
    try:
        plain_id = project.set_marker_ex(ProjectMarker(0, 1.0, "intro"))
        assert plain_id > 0
        key_id = project.set_marker_ex(
            ProjectMarker(
                0,
                2.0,
                "E flat major",
                kind=MarkerKind.KEY_SIGNATURE,
                key_fifths=-3,
                key_minor=False,
            )
        )
        assert key_id > 0 and key_id != plain_id
        assert project.marker_count() == 2

        # marker_by_index iterates every stored marker in [0, marker_count).
        ids = [project.marker_by_index(i).id for i in range(project.marker_count())]
        assert ids == [plain_id, key_id]

        plain = project.marker_by_index(0)
        assert plain.id == plain_id
        assert plain.kind == MarkerKind.MARKER
        assert plain.name == "intro"
        assert plain.key_fifths == 0
        assert plain.key_minor is False

        key = project.marker_by_index(1)
        assert key.id == key_id
        assert key.kind == MarkerKind.KEY_SIGNATURE
        assert key.name == "E flat major"
        assert key.key_fifths == -3
        assert key.key_minor is False

        with pytest.raises(SonareError):
            project.marker_by_index(2)
    finally:
        project.close()


def test_project_marker_ex_round_trips_long_utf8_name() -> None:
    project = Project()
    try:
        name = "あいうえお" * 7
        assert len(name) == 35
        marker_id = project.set_marker_ex(ProjectMarker(0, 1.0, name))
        assert marker_id > 0
        assert project.marker_by_index(0).name == name
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
        return [project.tempo_segment_by_index(i) for i in range(project.tempo_segment_count())]
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
