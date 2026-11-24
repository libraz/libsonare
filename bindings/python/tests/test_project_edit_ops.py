"""Tests for the headless-DAW edit ops + annotation / assist /
realtime helpers on the Python :class:`Project` wrapper.

These mirror the C-ABI keystone coverage (tests/arrangement/c_abi_edit_ops_test.cpp):
each edit op is undoable and round-trips the deterministic serialized bytes.
"""

from __future__ import annotations

import ctypes
import json

import pytest

from libsonare import (
    AutomationPoint,
    ErrorCode,
    Project,
    SonareError,
    SonareValueError,
    automation_target_kind_names,
    mastering_insert_names,
)

from ._helpers import raises_coded


def _audio_project() -> tuple[Project, int, int]:
    """A project with one audio track and one short audio clip."""
    p = Project()
    track = p.add_track("audio", "gtr")
    clip = p.add_clip(
        track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_channels=1, audio_sample_rate=48000
    )
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
    with raises_coded():
        p.set_clip_gain(clip, -1.0)


def test_set_clip_fade_and_loop_round_trip() -> None:
    p, _track, clip = _audio_project()
    before = p.to_json()
    p.set_clip_fade(
        clip,
        fade_in_ppq=24.0,
        fade_out_ppq=48.0,
        fade_in_curve="exponential",
        fade_out_curve="equalPower",
    )
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before
    p.set_clip_loop(clip, "loop", 240.0)
    assert p.to_json() != before
    p.undo()
    assert p.to_json() == before
    # An optional loop crossfade round-trips through serialization and undo.
    p.set_clip_loop(clip, "loop", 240.0, loop_crossfade_ppq=12.0)
    assert "loopCrossfadePpq" in p.to_json()
    p.undo()
    assert p.to_json() == before
    # An omitted loop length loops the entire clip and reads back as None.
    p.set_clip_loop(clip, "loop")
    assert p.to_json() != before
    assert p.clips()[0].loop_length_ppq is None
    p.undo()
    assert p.to_json() == before
    with pytest.raises(SonareValueError) as zero:
        p.set_clip_loop(clip, "loop", 0.0)
    assert str(zero.value) == (
        "Project.set_clip_loop: loop_length_ppq must not be 0; omit it to loop the whole clip"
    )
    with pytest.raises(SonareValueError) as negative:
        p.set_clip_loop(clip, "loop", -1.0)
    assert str(negative.value) == (
        "Project.set_clip_loop: loop_length_ppq must be finite and in (0, 1e12]"
    )
    with pytest.raises(SonareValueError) as crossfade:
        p.set_clip_loop(clip, "loop", 240.0, loop_crossfade_ppq=-1.0)
    assert str(crossfade.value) == (
        "Project.set_clip_loop: loop_crossfade_ppq must be finite and in [0, 1e12]"
    )
    assert p.to_json() == before


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

    for bad_pan in (1.5, -1.01, float("nan")):
        with pytest.raises(SonareError):
            p.set_track_pan(track, bad_pan)
    assert p.to_json() == before


def test_set_max_undo_depth_limits_history() -> None:
    p, track, _clip = _audio_project()
    for gain in (0.1, 0.2, 0.3, 0.4, 0.5):
        p.set_track_gain(track, gain)
    # Retain only the two most recent edits; older history is evicted.
    p.set_max_undo_depth(2)
    for invalid in (0, 500_000_001):
        with pytest.raises(SonareValueError) as refused:
            p.set_max_undo_depth(invalid)
        assert refused.value.code == ErrorCode.INVALID_PARAMETER
        assert str(refused.value).endswith("depth must be an integer in [1, 500000000]")

    # New edits show that neither refusal changed the previous two-entry cap.
    for gain in (0.6, 0.7, 0.8):
        p.set_track_gain(track, gain)
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
        refused = isinstance(invalid, bool)
        expected = TypeError if refused else ValueError
        domain = "$" if refused else r" in \[0, "
        with pytest.raises(
            expected, match=rf"^Project\.set_max_history_bytes: bytes must be an integer{domain}"
        ):
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


def test_automation_lane_add_edit_remove_uses_lane_id() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    lane_id = p.add_automation_lane(
        track,
        [
            AutomationPoint(0.0, 0.0, "linear"),
            AutomationPoint(480.0, 1.0, "linear"),
        ],
        target_param_id=1,
    )
    assert lane_id == 1
    after_add = p.to_json()
    assert after_add != before
    p.edit_automation_lane(track, lane_id, [AutomationPoint(0.0, 0.5, "hold")])
    assert p.to_json() != after_add
    p.undo()  # undo edit
    assert p.to_json() == after_add
    p.remove_automation_lane(track, lane_id)
    assert p.to_json() != after_add
    p.undo()  # undo remove restores the lane
    assert p.to_json() == after_add


def test_automation_lane_read_edit_identity_for_every_target_kind() -> None:
    _require_typed_automation_api()
    p, track, _clip = _audio_project()
    descriptors = (
        ("opaque", {"target_param_id": 101}, 0.25),
        ("trackFaderDb", {}, -3.0),
        ("trackPan", {}, 0.25),
        ("trackSendDb", {"send_id": "send-main"}, -6.0),
        ("trackWidth", {}, 0.5),
        (
            "trackInsertParam",
            {"insert_index": 0, "processor": "dynamics.compressor", "param_name": "ratio"},
            2.0,
        ),
    )
    lane_ids: dict[str, int] = {}
    for kind, identity, value in descriptors:
        lane_ids[kind] = p.add_automation_lane(
            track,
            points=[AutomationPoint(0.0, value, "hold")],
            target_kind=kind,
            **identity,
        )

    before = p.to_json()
    lanes = p.automation_lanes()
    by_kind = {lane.target_kind: lane for lane in lanes}
    assert set(by_kind) == {kind for kind, _identity, _value in descriptors}
    for kind, identity, value in descriptors:
        lane = by_kind[kind]
        assert lane.track_id == track
        assert lane.lane_id == lane_ids[kind]
        assert lane.target_param_id == (identity.get("target_param_id"))
        assert lane.points == (AutomationPoint(0.0, value, "hold"),)
        assert lane.send_id == identity.get("send_id")
        assert lane.insert_index == identity.get("insert_index")
        assert lane.processor == identity.get("processor")
        assert lane.param_name == identity.get("param_name")
        p.edit_automation_lane(
            lane.track_id,
            lane.lane_id,
            lane.points,
            target_kind=lane.target_kind,
            target_param_id=lane.target_param_id,
            send_id=lane.send_id,
            insert_index=lane.insert_index,
            processor=lane.processor,
            param_name=lane.param_name,
        )
    assert p.to_json() == before

    with pytest.raises(SonareValueError, match="target identity fields require target_kind"):
        p.edit_automation_lane(
            track,
            by_kind["opaque"].lane_id,
            [AutomationPoint(0.0, 0.5, "linear")],
            target_param_id=101,
        )
    with pytest.raises(SonareValueError, match="target_param_id must be omitted"):
        p.edit_automation_lane(
            track,
            by_kind["trackFaderDb"].lane_id,
            [AutomationPoint(0.0, 0.5, "linear")],
            target_kind="trackFaderDb",
            target_param_id=0,
        )
    assert p.to_json() == before

    send_lane = by_kind["trackSendDb"]
    p.edit_automation_lane(track, send_lane.lane_id, [AutomationPoint(0.0, -4.0, "hold")])
    edited_send_lane = next(
        lane for lane in p.automation_lanes() if lane.lane_id == send_lane.lane_id
    )
    assert edited_send_lane.lane_id == send_lane.lane_id
    assert edited_send_lane.target_kind == "trackSendDb"
    assert edited_send_lane.target_param_id is None
    assert edited_send_lane.send_id == "send-main"
    assert edited_send_lane.points == (AutomationPoint(0.0, -4.0, "hold"),)

    with pytest.raises(SonareValueError, match="lane_id"):
        p.edit_automation_lane(track, 999_999, [AutomationPoint(0.0, 0.0, "linear")])
    with pytest.raises(SonareValueError, match="lane_id"):
        p.remove_automation_lane(track, 999_999)


def test_automation_target_kind_names_follow_c_token_order() -> None:
    assert automation_target_kind_names() == [
        "opaque",
        "trackFaderDb",
        "trackPan",
        "trackSendDb",
        "trackWidth",
        "trackInsertParam",
    ]


def test_automation_lane_rejects_zero_as_reserved_target_id() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    with pytest.raises(
        ValueError, match=r"target_param_id must be an integer in \[1, 4294967295\]$"
    ):
        p.add_automation_lane(track, [AutomationPoint(0.0, 0.0, "linear")], target_param_id=0)
    assert p.to_json() == before


def _require_typed_automation_api() -> None:
    """Fail, rather than skip, when the typed-lane C ABI is missing.

    These symbols are asserted present for the whole suite by
    test_imports.py::test_loaded_library_exports_every_declared_symbol, so an
    absence here is a broken build, not a configuration to opt out of. Skipping
    made a dylib missing the ABI look green while the shipped facade raised.
    """
    from libsonare._runtime import _get_lib

    lib = _get_lib()
    missing = [
        name
        for name in (
            "sonare_project_add_automation_lane",
            "sonare_project_edit_automation_lane",
            "sonare_project_automation_lanes",
            "sonare_free_project_automation_lane_list",
            "sonare_automation_target_kind_names",
        )
        if not hasattr(lib, name)
    ]
    assert not missing, f"loaded libsonare is missing the typed automation-lane C ABI: {missing}"


def test_automation_lane_typed_kind_roundtrip_and_legacy_edit_preservation() -> None:
    _require_typed_automation_api()
    p, track, _clip = _audio_project()

    legacy_lane_id = p.add_automation_lane(
        track, [AutomationPoint(0.0, 0.0, "linear")], target_param_id=1
    )
    legacy_doc = json.loads(p.to_json())
    assert legacy_doc["version"] == 7
    assert "targetKind" not in legacy_doc["tracks"][0]["automationLanes"][0]
    p.remove_automation_lane(track, legacy_lane_id)

    typed_target = p.add_automation_lane(
        track,
        points=[
            AutomationPoint(0.0, 0.0, "linear"),
            AutomationPoint(480.0, 1.0, "hold"),
        ],
        target_kind="trackFaderDb",
    )
    typed_before_edit = p.to_json()
    typed_doc = json.loads(typed_before_edit)
    assert typed_doc["version"] == 7
    assert typed_doc["tracks"][0]["automationLanes"][0]["targetKind"] == "trackFaderDb"

    # An absent kind keeps the lane's existing typed classification while
    # replacing points.
    p.edit_automation_lane(track, typed_target, [AutomationPoint(0.0, 0.5, "hold")])
    edited_doc = json.loads(p.to_json())
    assert edited_doc["version"] == 7
    assert edited_doc["tracks"][0]["automationLanes"][0]["targetKind"] == "trackFaderDb"

    restored = Project.from_json(p.to_json())
    assert restored.to_json() == p.to_json()


def test_automation_lane_typed_kind_conflict_is_atomic() -> None:
    _require_typed_automation_api()
    p, track, _clip = _audio_project()
    p.add_automation_lane(
        track,
        points=[AutomationPoint(0.0, 0.0, "linear")],
        target_kind="trackFaderDb",
    )
    pan_target = p.add_automation_lane(
        track, points=[AutomationPoint(0.0, 0.0, "linear")], target_kind="trackPan"
    )
    before = p.to_json()

    with pytest.raises(SonareError):
        p.add_automation_lane(
            track,
            points=[AutomationPoint(0.0, 1.0, "linear")],
            target_kind="trackFaderDb",
        )
    assert p.to_json() == before

    with pytest.raises(SonareError):
        p.edit_automation_lane(
            track,
            pan_target,
            [AutomationPoint(0.0, 1.0, "linear")],
            target_kind="trackFaderDb",
        )
    assert p.to_json() == before


def test_automation_lane_rejects_invalid_target_kind_without_mutation() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    for invalid in (
        "unknown",
        "track-fader-db",
        "TrackPan",
        0,
        1,
        3,
        1.5,
        float("nan"),
        True,
    ):
        expected = ValueError if isinstance(invalid, str) else TypeError
        with pytest.raises(expected):
            p.add_automation_lane(
                track,
                points=[AutomationPoint(0.0, 0.0, "linear")],
                target_kind=invalid,
            )
        assert p.to_json() == before


def test_automation_lane_rejects_nonintegral_target_without_mutation() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    for invalid in (1.5, float("nan"), float("inf"), True, "1"):
        refused = isinstance(invalid, (bool, str))
        with pytest.raises(TypeError if refused else ValueError, match="target_param_id"):
            p.add_automation_lane(
                track, [AutomationPoint(0.0, 0.0, "linear")], target_param_id=invalid
            )
        assert p.to_json() == before


def test_automation_lane_typed_targets_refuse_target_param_id() -> None:
    p, track, _clip = _audio_project()
    before = p.to_json()
    for target_param_id in (0, 17):
        with pytest.raises(SonareValueError, match="target_param_id must be omitted"):
            p.add_automation_lane(
                track,
                target_param_id=target_param_id,
                target_kind="trackFaderDb",
                points=[AutomationPoint(0.0, 0.0, "linear")],
            )
    assert p.to_json() == before


def test_project_scene_route_and_unresolved_soundfont_readers() -> None:
    p, track, _clip = _audio_project()
    default_track = p.tracks()[0]
    assert default_track.channel_strip_ref is None
    assert default_track.output_target is None
    p.set_track_route(track, "strip-main", "master")
    stored_track = p.tracks()[0]
    assert stored_track.channel_strip_ref == "strip-main"
    assert stored_track.output_target == "master"
    assert p.mixer_scene() is None

    p.set_mixer_scene({"version": 2, "strips": [], "buses": [], "vcaGroups": [], "connections": []})
    scene = p.mixer_scene()
    assert scene is not None
    assert scene["version"] == 2
    assert scene["strips"] == []

    project_document = {
        "version": 7,
        "sampleRate": 48000,
        "tracks": [
            {
                "id": 1,
                "name": "keys",
                "kind": "midi",
                "midiDestinationId": 5,
                "channelStripRef": "",
                "outputTarget": "",
                "automationLanes": [],
            }
        ],
        "clips": [],
        "instruments": [
            {
                "destinationId": 5,
                "kind": "sf2",
                "sf2": {
                    "gain": 1.0,
                    "polyphony": 32,
                    "preferModelForModeledFamilies": 0,
                    "clearBankRig": 0,
                    "gsEfxRealization": 0,
                    "soundFontUri": "fonts/missing.sf2",
                },
            }
        ],
    }
    unresolved = Project.from_json(json.dumps(project_document))
    try:
        assert unresolved.unresolved_sound_font_uris() == ["fonts/missing.sf2"]
    finally:
        unresolved.close()


def test_move_clip_rejects_cross_kind_track() -> None:
    p, _audio_track, audio_clip = _audio_project()
    midi_track = p.add_track("midi", "keys")
    before = p.to_json()
    with raises_coded(ErrorCode.INVALID_STATE):
        p.move_clip(audio_clip, 0.0, midi_track)  # audio clip onto a MIDI track
    assert p.to_json() == before  # rejected without mutating state


def test_annotate_keys_and_chords_round_trip() -> None:
    p, _track, _clip = _audio_project()
    before = p.to_json()
    p.annotate_keys([{"start_ppq": 0.0, "end_ppq": 480.0, "tonic_pc": 0, "mode": "major"}])
    assert p.to_json() != before
    p.annotate_chords([{"start_ppq": 0.0, "end_ppq": 480.0, "root_pc": 0, "quality": "major"}])
    p.undo()  # undo chords
    p.undo()  # undo keys
    assert p.to_json() == before


def test_assist_sidecar_set_count_get_round_trip() -> None:
    p, track, _clip = _audio_project()
    p.set_assist_sidecar("ai.module", b"\x01\x02\x03", schema_version=2, target_track_id=track)
    assert len(p.assist_sidecars()) == 1
    sidecars = p.assist_sidecars()
    assert sidecars[0].module_id == "ai.module"
    assert sidecars[0].payload == b"\x01\x02\x03"
    assert sidecars[0].schema_version == 2


def test_assist_sidecar_defaults_and_stable_order() -> None:
    p, _track, _clip = _audio_project()
    p.set_assist_sidecar("first")
    p.set_assist_sidecar("second", b"payload")

    assert len(p.assist_sidecars()) == 2
    assert [sidecar.module_id for sidecar in p.assist_sidecars()] == ["first", "second"]
    assert p.assist_sidecars()[0].payload == b""
    assert p.assist_sidecars()[0].schema_version == 0
    assert p.assist_sidecars()[0].target_track_id == 0
    assert p.assist_sidecars()[0].region_start_ppq == 0.0
    assert p.assist_sidecars()[0].region_end_ppq == 0.0


def test_assist_sidecar_same_identity_replaces_without_reordering() -> None:
    p, _track, _clip = _audio_project()
    p.set_assist_sidecar("first", b"old")
    p.set_assist_sidecar("second", b"other")
    p.set_assist_sidecar("first", b"new", schema_version=3)

    assert len(p.assist_sidecars()) == 2
    assert [sidecar.module_id for sidecar in p.assist_sidecars()] == ["first", "second"]
    assert p.assist_sidecars()[0].payload == b"new"
    assert p.assist_sidecars()[0].schema_version == 3


def test_assist_sidecar_invalid_input_does_not_mutate() -> None:
    p, _track, _clip = _audio_project()
    before = p.to_json()

    for invalid in (None, 42, {"module_id": "descriptor"}):
        with pytest.raises(TypeError):
            p.set_assist_sidecar(invalid)  # type: ignore[arg-type]
        assert p.to_json() == before

    with pytest.raises(ValueError):
        p.set_assist_sidecar("")
    assert p.to_json() == before

    for invalid_region in (
        {"region_start_ppq": -1.0},
        {"region_start_ppq": float("nan")},
        {"region_end_ppq": float("inf")},
    ):
        with pytest.raises(SonareError):
            p.set_assist_sidecar("bad-region", **invalid_region)
        assert p.to_json() == before


def test_mastering_insert_names_lists_fx() -> None:
    names = mastering_insert_names()
    assert isinstance(names, list)
    assert len(names) > 0
    # The newly-registered creative FX are advertised here.
    assert any(n.startswith("effects.reverb.") for n in names)
