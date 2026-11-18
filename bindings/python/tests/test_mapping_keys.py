"""A mapping the caller builds takes its snake_case keys only.

Any other key -- another spelling of an accepted one included -- is refused by
name rather than read as an alias or dropped. Each case first passes the
mapping it then perturbs, so a guard refusing everything cannot read as one
refusing the right thing.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping

import pytest

from libsonare import SonareValueError
from libsonare._analysis_music import _key_pair
from libsonare._project_model import (
    _automation_lane_args,
    _automation_lane_desc,
    _cc_binding_to_c,
)
from libsonare._project_synth import _part_rig_inserts_arg
from libsonare._runtime import _mapping_keys

from ._helpers import LIB_AVAILABLE

needs_lib = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def _refuses(
    call: Callable[[dict[str, object]], object], good: Mapping[str, object], key: str
) -> None:
    call(dict(good))
    with pytest.raises(SonareValueError, match=f"unknown key '{key}'"):
        call({**good, key: 1})


@pytest.mark.parametrize(
    ("call", "good", "key"),
    [
        (_key_pair, {"root": 0, "mode": "major"}, "tonic"),
        (_cc_binding_to_c, {"cc_number": 7, "param_id": 3}, "ccNumber"),
        (lambda m: _part_rig_inserts_arg(2, [m]), {"processor": "eq", "params": {}}, "parameters"),
        (
            lambda m: _automation_lane_args(m, None, None),
            {"target_param_id": 7, "points": [], "target_kind": "opaque"},
            "targetParamId",
        ),
        (lambda m: _automation_lane_args(m, None, None), {"target_param_id": 7}, "targetKind"),
        (lambda m: _automation_lane_args(7, m, None), {"points": []}, "curveToNext"),
        (
            lambda m: _automation_lane_desc(7, [m]),
            {"ppq": 0.0, "value": 0.5, "curve": "linear"},
            "curveToNext",
        ),
        (lambda m: _automation_lane_desc(7, [m]), {"ppq": 0.0, "value": 0.5}, "curve_to_next"),
    ],
    ids=[
        "key",
        "midi_cc_binding",
        "part_rig_insert",
        "automation_lane",
        "automation_lane_kind",
        "automation_lane_points_form",
        "automation_point",
        "automation_point_snake_alias",
    ],
)
def test_a_helper_mapping_refuses_an_unknown_key(
    call: Callable[[dict[str, object]], object], good: Mapping[str, object], key: str
) -> None:
    _refuses(call, good, key)


def test_a_non_mapping_descriptor_is_a_type_error() -> None:
    with pytest.raises(TypeError, match="MIDI CC binding must be a mapping"):
        _mapping_keys([("cc_number", 7)], ("cc_number",), "MIDI CC binding")


@needs_lib
def test_engine_mixing_mappings_take_one_spelling() -> None:
    from libsonare import RealtimeEngine

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        bus = {"bus_id": 1, "gain_db": 0.0, "channel_layout": "stereo", "output_bus_id": 0}
        target = {"bus_id": 2, "gain_db": 0.0}
        _refuses(lambda m: engine.set_track_buses([m, target]), bus, "gainDb")
        engine.set_track_buses([bus, target])
        send = {"bus_id": 2, "level_db": -3.0, "enabled": True, "send_timing": "preFader"}
        for key in ("timing", "sendTiming", "busId"):
            _refuses(lambda m: engine.set_track_lanes([{"track_id": 10, "sends": [m]}]), send, key)
            _refuses(lambda m: engine.set_track_buses([{**bus, "sends": [m]}, target]), send, key)
        lane = {"track_id": 10, "sends": [send], "source_channel_layout": "stereo"}
        _refuses(lambda m: engine.set_track_lanes([m]), lane, "trackId")
        engine.set_track_lanes([lane])
        _refuses(lambda m: engine.set_track_sends(10, [m]), send, "sendTiming")


@needs_lib
def test_engine_tempo_and_meter_segments_take_one_spelling() -> None:
    from libsonare import RealtimeEngine

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        tempo = {"start_ppq": 0.0, "bpm": 120.0, "end_bpm": 0.0}
        _refuses(lambda m: engine.set_tempo_segments([m]), tempo, "startPpq")
        _refuses(lambda m: engine.set_tempo_segments([m]), tempo, "start_sample")
        meter = {"start_ppq": 0.0, "numerator": 4, "denominator": 4}
        _refuses(lambda m: engine.set_time_signature_segments([m]), meter, "beatUnit")


@needs_lib
def test_project_mappings_take_one_spelling() -> None:
    from libsonare import Project

    project = Project()
    try:
        track = project.add_track("audio", "gtr")
        clip = project.add_clip(
            track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_sample_rate=48000
        )
        take = {"id": 1, "source_id": 0, "source_offset_ppq": 0.0, "name": "a"}
        _refuses(lambda m: project.set_clip_takes(clip, [m], active_take_id=1), take, "sourceId")
        segment = {"start_ppq": 0.0, "end_ppq": 1.0, "take_id": 1, "crossfade_ppq": 0.0}
        _refuses(lambda m: project.set_clip_comp_segments(clip, [m]), segment, "takeId")

        point = {"ppq": 0.0, "value": 0.5, "curve": "linear"}
        _refuses(
            lambda m: project.add_automation_lane(track, {"target_param_id": 7, "points": [m]}),
            point,
            "curveToNext",
        )
        project.add_automation_lane(track, 8, [(0.0, 0.0, "linear")])
        lane = {"target_param_id": 8, "points": [point]}
        _refuses(lambda m: project.edit_automation_lane(track, m), lane, "targetParamId")

        chord = {"start_ppq": 0.0, "end_ppq": 480.0, "root_pc": 0, "quality": 0}
        _refuses(lambda m: project.annotate_chords([m]), chord, "rootPc")
        tempo = {"start_ppq": 0.0, "bpm": 120.0}
        _refuses(lambda m: project.set_tempo_segments([m]), tempo, "start_sample")
        meter = {"start_ppq": 0.0, "numerator": 3, "denominator": 4}
        _refuses(lambda m: project.set_time_signatures([m]), meter, "startPpq")
    finally:
        project.close()


@needs_lib
def test_an_external_stem_takes_one_spelling_and_a_layout_token() -> None:
    from libsonare import Project

    project = Project()
    try:
        stem = {"name": "vox", "layout": "mono", "planar_samples": [[0.0] * 64], "role": "vocals"}
        before = project.to_json()
        with pytest.raises(SonareValueError, match="unknown key 'channels'"):
            project.import_external_stems(48000, [{**stem, "channels": stem["planar_samples"]}])
        # A channel count is not the layout token, and a near spelling lists the tokens.
        with pytest.raises(TypeError, match="stem layout"):
            project.import_external_stems(48000, [{**stem, "layout": 1}])
        with pytest.raises(SonareValueError, match="'mono'"):
            project.import_external_stems(48000, [{**stem, "layout": "Mono"}])
        assert project.to_json() == before
        tracks, _clips = project.import_external_stems(48000, [stem])
        assert len(tracks) == 1
    finally:
        project.close()
