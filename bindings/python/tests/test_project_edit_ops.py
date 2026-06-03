"""Tests for the newly-exposed headless-DAW edit ops + annotation / assist /
realtime helpers on the Python :class:`Project` wrapper.

These mirror the C-ABI keystone coverage (tests/arrangement/c_abi_edit_ops_test.cpp):
each edit op is undoable and round-trips the deterministic serialized bytes.
"""

from __future__ import annotations

import pytest

from libsonare import Project, mastering_insert_names


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
    assert p.to_json() != before
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
    p.set_clip_fade(clip, 24.0, 48.0, fade_in_curve="equal-power")
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before
    p.set_clip_loop(clip, "loop", 240.0)
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before
    with pytest.raises((ValueError, Exception)):
        p.set_clip_loop(clip, "loop", 0.0)  # looping needs length > 0


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


def test_automation_lane_add_edit_remove() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    idx = p.add_automation_lane(track, 1, [(0.0, 0.0, "linear"), (480.0, 1.0, "linear")])
    assert idx == 0
    after_add = p.to_json()
    assert after_add != before
    p.edit_automation_lane(track, idx, 1, [(0.0, 0.5, "hold")])
    assert p.to_json() != after_add
    p.undo()  # undo edit
    assert p.to_json() == after_add
    p.remove_automation_lane(track, idx)
    assert p.to_json() != after_add
    p.undo()  # undo remove restores the lane
    assert p.to_json() == after_add


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


def test_mastering_insert_names_lists_fx() -> None:
    names = mastering_insert_names()
    assert isinstance(names, list)
    assert len(names) > 0
    # The newly-registered creative FX are advertised here.
    assert any(n.startswith("effects.reverb.") for n in names)
