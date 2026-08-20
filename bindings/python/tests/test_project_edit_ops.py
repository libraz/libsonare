"""Tests for the headless-DAW edit ops + annotation / assist /
realtime helpers on the Python :class:`Project` wrapper.

These mirror the C-ABI keystone coverage (tests/arrangement/c_abi_edit_ops_test.cpp):
each edit op is undoable and round-trips the deterministic serialized bytes.
"""

from __future__ import annotations

import ctypes
import json

import pytest

from libsonare import Project, SonareError, mastering_insert_names


def _audio_project() -> tuple[Project, int, int]:
    """A project with one audio track and one short audio clip."""
    p = Project()
    track = p.add_track("audio", "gtr")
    clip = p.add_clip(track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_sample_rate=48000)
    return p, track, clip


def test_remove_clip_is_undoable() -> None:
    p, _track, clip = _audio_project()
    before = p.to_json()
    p.remove_clip(clip)
    # The clip's source metadata is reclaimed along with its decoded PCM.
    assert '"sources":[]' in p.to_json()
    p.undo()
    assert p.to_json() == before


def test_set_clip_gain_accepts_zero_mute_and_undoes() -> None:
    p, _track, clip = _audio_project()
    before = p.to_json()
    p.set_clip_gain(
        clip, 0.0
    )  # explicit mute -- the add_clip default-coercion path cannot express this
    muted = p.to_json()
    assert muted != before
    p.undo()
    assert p.to_json() == before
    with pytest.raises((ValueError, Exception)):
        p.set_clip_gain(clip, -1.0)


def test_set_clip_fade_and_loop_round_trip() -> None:
    p, _track, clip = _audio_project()
    before = p.to_json()
    p.set_clip_fade(clip, 24.0, 48.0, fade_in_curve="EXP", fade_out_curve="equal_power")
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before
    p.set_clip_loop(clip, "loop", 240.0)
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before
    # An optional loop crossfade round-trips through serialization and undo.
    p.set_clip_loop(clip, "loop", 240.0, loop_crossfade_ppq=12.0)
    assert "loop_crossfade_ppq" in p.to_json()
    p.undo()
    assert p.to_json() == before
    # loop_length_ppq == 0 while looping means "loop the entire clip" (the
    # C-ABI semantics); Python must accept it rather than over-validate.
    p.set_clip_loop(clip, "loop", 0.0)
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before
    with pytest.raises((ValueError, Exception)):
        p.set_clip_loop(clip, "loop", 240.0, loop_crossfade_ppq=-1.0)  # crossfade must be >= 0


def test_duplicate_clip_allocates_new_id() -> None:
    p, _track, clip = _audio_project()
    new_id = p.duplicate_clip(clip, 480.0)
    assert new_id != 0
    assert new_id != clip


def test_rename_and_remove_track_undo() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    p.rename_track(track, "lead")
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before
    p.remove_track(track)  # removes the track and its clip
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before


def test_set_track_route_undo() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    p.set_track_route(track, "strip-a", "master")
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before


def test_set_track_gain_mute_solo_pan_undo() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    for apply in (
        lambda: p.set_track_gain(track, 0.5),
        lambda: p.set_track_mute(track, True),
        lambda: p.set_track_solo(track, True),
        lambda: p.set_track_pan(track, -0.5),
    ):
        apply()
        assert p.to_json() != before
        p.undo()
        assert p.to_json() == before

    with pytest.raises(SonareError):
        p.set_track_gain(999999, 1.0)


def test_set_max_undo_depth_limits_history() -> None:
    p, track, _clip = _audio_project()
    for gain in (0.1, 0.2, 0.3, 0.4, 0.5):
        p.set_track_gain(track, gain)
    # Retain only the two most recent edits; older history is evicted.
    p.set_max_undo_depth(2)
    p.undo()
    p.undo()
    with pytest.raises(SonareError):
        p.undo()


def test_clear_history_discards_undo_stack() -> None:
    p, track, _clip = _audio_project()
    p.set_track_gain(track, 0.25)
    after = p.to_json()
    p.clear_history()
    # Project state is untouched, but there is nothing left to undo.
    assert p.to_json() == after
    with pytest.raises(SonareError):
        p.undo()


def test_set_max_history_bytes_rejects_invalid_values_without_mutating_history() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    size_t_max = (1 << (ctypes.sizeof(ctypes.c_size_t) * 8)) - 1

    for invalid in (True, 1.0, -1, size_t_max + 1):
        with pytest.raises(ValueError, match="bytes must be a non-negative integer"):
            p.set_max_history_bytes(invalid)
        assert p.to_json() == before

    # Invalid calls must not poison the existing history; a normal edit still
    # mutates state and remains undoable.
    p.set_track_gain(track, 0.5)
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before


def test_set_max_history_bytes_accepts_size_t_max_and_zero() -> None:
    p, track, _clip = _audio_project()
    size_t_max = (1 << (ctypes.sizeof(ctypes.c_size_t) * 8)) - 1
    before = p.to_json()

    p.set_max_history_bytes(size_t_max)
    p.set_track_gain(track, 0.5)
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before


def test_set_max_history_bytes_zero_keeps_mutation_but_discards_undo() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    p.set_max_history_bytes(0)

    p.set_track_gain(track, 0.5)
    after = p.to_json()
    assert after != before
    with pytest.raises(SonareError):
        p.undo()
    assert p.to_json() == after


def test_set_max_history_bytes_reports_missing_additive_symbol(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import libsonare._project_edit as project_edit

    p = Project()
    monkeypatch.setattr(project_edit, "_get_lib", lambda: object())
    with pytest.raises(RuntimeError, match="sonare_project_set_max_history_bytes"):
        p.set_max_history_bytes(0)


def test_automation_lane_add_edit_remove() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    target_param_id = p.add_automation_lane(
        track, 1, [(0.0, 0.0, "linear"), (480.0, 1.0, "linear")]
    )
    assert target_param_id == 1
    after_add = p.to_json()
    assert after_add != before
    p.edit_automation_lane(track, target_param_id, [(0.0, 0.5, "hold")])
    assert p.to_json() != after_add
    p.undo()  # undo edit
    assert p.to_json() == after_add
    p.remove_automation_lane(track, target_param_id)
    assert p.to_json() != after_add
    p.undo()  # undo remove restores the lane
    assert p.to_json() == after_add


def test_automation_lane_rejects_zero_as_reserved_target_id() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    with pytest.raises(ValueError, match="target_param_id must be non-zero"):
        p.add_automation_lane(track, 0, [(0.0, 0.0, "linear")])
    assert p.to_json() == before


def _require_typed_automation_api() -> None:
    """Fail, rather than skip, when the typed-lane C ABI is missing.

    These symbols are asserted present for the whole suite by
    test_imports.py::test_loaded_library_exports_every_guarded_symbol, so an
    absence here is a broken build, not a configuration to opt out of. Skipping
    made a dylib missing the ABI look green while the shipped facade raised.
    """
    from libsonare._runtime import _get_lib

    lib = _get_lib()
    missing = [
        name
        for name in (
            "sonare_project_add_automation_lane_ex",
            "sonare_project_edit_automation_lane_ex",
        )
        if not hasattr(lib, name)
    ]
    assert not missing, f"loaded libsonare is missing the typed automation-lane C ABI: {missing}"


def test_automation_lane_typed_kind_roundtrip_and_legacy_edit_preservation() -> None:
    _require_typed_automation_api()
    p, track, _clip = _audio_project()

    legacy_target = p.add_automation_lane(track, 1, [(0.0, 0.0, "linear")])
    legacy_doc = json.loads(p.to_json())
    assert legacy_doc["version"] == 1
    assert "target_kind" not in legacy_doc["tracks"][0]["automation_lanes"][0]
    p.remove_automation_lane(track, legacy_target)

    typed_target = p.add_automation_lane(
        track,
        2,
        [(0.0, 0.0, "linear"), (480.0, 1.0, "hold")],
        target_kind="track-fader-db",
    )
    typed_before_edit = p.to_json()
    typed_doc = json.loads(typed_before_edit)
    assert typed_doc["version"] == 2
    assert typed_doc["tracks"][0]["automation_lanes"][0]["target_kind"] == 1

    # The absent kind must stay on the legacy C edit path; native semantics
    # preserve the existing typed classification while replacing points.
    p.edit_automation_lane(track, typed_target, [(0.0, 0.5, "hold")])
    edited_doc = json.loads(p.to_json())
    assert edited_doc["version"] == 2
    assert edited_doc["tracks"][0]["automation_lanes"][0]["target_kind"] == 1

    restored = Project.from_json(p.to_json())
    assert restored.to_json() == p.to_json()


def test_automation_lane_typed_kind_conflict_is_atomic() -> None:
    _require_typed_automation_api()
    p, track, _clip = _audio_project()
    p.add_automation_lane(track, 1, [(0.0, 0.0, "linear")], target_kind=1)
    pan_target = p.add_automation_lane(track, 2, [(0.0, 0.0, "linear")], target_kind=2)
    before = p.to_json()

    with pytest.raises(SonareError):
        p.add_automation_lane(track, 3, [(0.0, 1.0, "linear")], target_kind="track-fader-db")
    assert p.to_json() == before

    with pytest.raises(SonareError):
        p.edit_automation_lane(track, pan_target, [(0.0, 1.0, "linear")], target_kind=1)
    assert p.to_json() == before


def test_automation_lane_rejects_invalid_target_kind_without_mutation() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    for invalid in ("unknown", 3, 1.5, float("nan"), float("inf"), True, None):
        with pytest.raises(ValueError):
            p.add_automation_lane(track, 1, [(0.0, 0.0, "linear")], target_kind=invalid)
        assert p.to_json() == before


def test_automation_lane_rejects_nonintegral_target_without_mutation() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    for invalid in (1.5, float("nan"), float("inf"), True, "1"):
        with pytest.raises(ValueError):
            p.add_automation_lane(track, invalid, [(0.0, 0.0, "linear")])
        assert p.to_json() == before


def test_move_clip_rejects_cross_kind_track() -> None:
    p, _audio_track, audio_clip = _audio_project()
    midi_track = p.add_track("midi", "keys")
    before = p.to_json()
    with pytest.raises(RuntimeError):
        p.move_clip(audio_clip, 0.0, midi_track)  # audio clip onto a MIDI track
    assert p.to_json() == before  # rejected without mutating state


def test_annotate_keys_and_chords_round_trip() -> None:
    p, _track, _clip = _audio_project()
    before = p.to_json()
    p.annotate_keys([(0.0, 480.0, 0, 1)])  # C major over the first bar
    assert p.to_json() != before
    p.annotate_chords([{"start_ppq": 0.0, "end_ppq": 480.0, "root_pc": 0, "quality": 0}])
    p.undo()  # undo chords
    p.undo()  # undo keys
    assert p.to_json() == before


def test_assist_sidecar_set_count_get_round_trip() -> None:
    p, track, _clip = _audio_project()
    p.set_assist_sidecar("ai.module", b"\x01\x02\x03", schema_version=2, target_track_id=track)
    assert p.assist_sidecar_count() == 1
    sidecars = p.assist_sidecars()
    assert sidecars[0].module_id == "ai.module"
    assert sidecars[0].payload == b"\x01\x02\x03"
    assert sidecars[0].schema_version == 2


def test_assist_sidecar_descriptor_matches_legacy_keyword_form() -> None:
    descriptor_project, track, _clip = _audio_project()
    legacy_project, legacy_track, _legacy_clip = _audio_project()
    descriptor = {
        "moduleId": "ai.module",
        "payload": b"\x01\x02\x03",
        "schemaVersion": 2,
        "targetTrackId": track,
        "regionStartPpq": 12.0,
        "regionEndPpq": 48.0,
        "unknown": "ignored",
    }

    descriptor_project.set_assist_sidecar(descriptor)
    legacy_project.set_assist_sidecar(
        module_id="ai.module",
        payload=b"\x01\x02\x03",
        schema_version=2,
        target_track_id=legacy_track,
        region_start_ppq=12.0,
        region_end_ppq=48.0,
    )

    assert descriptor_project.to_json() == legacy_project.to_json()
    assert descriptor_project.get_assist_sidecar(0) == legacy_project.get_assist_sidecar(0)


def test_assist_sidecar_descriptor_defaults_and_stable_order() -> None:
    p, _track, _clip = _audio_project()
    p.set_assist_sidecar({"moduleId": "first"})
    p.set_assist_sidecar({"moduleId": "second", "payload": b"payload"})

    assert p.assist_sidecar_count() == 2
    assert [sidecar.module_id for sidecar in p.assist_sidecars()] == ["first", "second"]
    assert p.get_assist_sidecar(0).payload == b""
    assert p.get_assist_sidecar(0).schema_version == 0
    assert p.get_assist_sidecar(0).target_track_id == 0
    assert p.get_assist_sidecar(0).region_start_ppq == 0.0
    assert p.get_assist_sidecar(0).region_end_ppq == 0.0


def test_assist_sidecar_descriptor_same_identity_replaces_without_reordering() -> None:
    p, _track, _clip = _audio_project()
    p.set_assist_sidecar({"moduleId": "first", "payload": b"old"})
    p.set_assist_sidecar({"moduleId": "second", "payload": b"other"})
    p.set_assist_sidecar(
        {
            "moduleId": "first",
            "payload": b"new",
            "schemaVersion": 3,
        }
    )

    assert p.assist_sidecar_count() == 2
    assert [sidecar.module_id for sidecar in p.assist_sidecars()] == ["first", "second"]
    assert p.get_assist_sidecar(0).payload == b"new"
    assert p.get_assist_sidecar(0).schema_version == 3


def test_assist_sidecar_invalid_or_mixed_input_does_not_mutate() -> None:
    p, _track, _clip = _audio_project()
    before = p.to_json()

    for invalid in ({}, {"moduleId": None}, {"moduleId": 42}, {"module_id": "wrong"}):
        with pytest.raises(TypeError):
            p.set_assist_sidecar(invalid)
        assert p.to_json() == before

    with pytest.raises(ValueError):
        p.set_assist_sidecar({"moduleId": ""})
    assert p.to_json() == before

    with pytest.raises(TypeError):
        p.set_assist_sidecar({"moduleId": "mixed"}, b"legacy")
    assert p.to_json() == before

    with pytest.raises(TypeError):
        p.set_assist_sidecar({"moduleId": "mixed"}, schema_version=1)
    assert p.to_json() == before

    for invalid_region in (
        {"moduleId": "bad-region", "regionStartPpq": -1.0},
        {"moduleId": "bad-region", "regionStartPpq": float("nan")},
        {"moduleId": "bad-region", "regionEndPpq": float("inf")},
    ):
        with pytest.raises(SonareError):
            p.set_assist_sidecar(invalid_region)
        assert p.to_json() == before

    with pytest.raises(TypeError):
        p.set_assist_sidecar(None)  # type: ignore[arg-type]
    assert p.to_json() == before


def test_mastering_insert_names_lists_fx() -> None:
    names = mastering_insert_names()
    assert isinstance(names, list)
    assert len(names) > 0
    # The newly-registered creative FX are advertised here.
    assert any(n.startswith("effects.reverb.") for n in names)
