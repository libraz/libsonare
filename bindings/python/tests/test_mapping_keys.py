"""A mapping the caller builds takes its snake_case keys only.

Any other key -- another spelling of an accepted one included -- is refused by
name rather than read as an alias or dropped. Each case first passes the
mapping it then perturbs, so a guard refusing everything cannot read as one
refusing the right thing.
"""

from __future__ import annotations

import re
from collections.abc import Callable, Mapping

import pytest

from libsonare import SonareValueError
from libsonare._analysis_music import _key_pair
from libsonare._runtime import _mapping_keys

from ._helpers import LIB_AVAILABLE

needs_lib = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def _refuses(
    call: Callable[[dict[str, object]], object], good: Mapping[str, object], key: str
) -> None:
    call(dict(good))
    with pytest.raises(SonareValueError, match=rf"unknown key [^ ]*\b{re.escape(key)}\b"):
        call({**good, key: 1})


@pytest.mark.parametrize(
    ("call", "good", "key"),
    [
        (_key_pair, {"root": 0, "mode": "major"}, "tonic"),
    ],
    ids=[
        "key",
    ],
)
def test_a_helper_mapping_refuses_an_unknown_key(
    call: Callable[[dict[str, object]], object], good: Mapping[str, object], key: str
) -> None:
    _refuses(call, good, key)


def test_a_non_mapping_descriptor_is_a_type_error() -> None:
    with pytest.raises(TypeError, match="segment must be a mapping"):
        _mapping_keys([("start_ppq", 0.0)], ("start_ppq",), "segment")


@needs_lib
def test_engine_mixing_mappings_take_one_spelling() -> None:
    from libsonare import RealtimeEngine

    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        bus = {"bus_id": 1, "gain_db": 0.0, "channel_layout": "stereo", "output_bus_id": None}
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

    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
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
            track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_channels=1, audio_sample_rate=48000
        )
        take = {"id": 1, "source_id": None, "source_offset_ppq": 0.0, "name": "a"}
        _refuses(lambda m: project.set_clip_takes(clip, [m], active_take_id=1), take, "sourceId")
        segment = {"start_ppq": 0.0, "end_ppq": 1.0, "take_id": 1, "crossfade_ppq": 0.0}
        _refuses(lambda m: project.set_clip_comp_segments(clip, [m]), segment, "takeId")

        chord = {"start_ppq": 0.0, "end_ppq": 480.0, "root_pc": 0, "quality": "major"}
        _refuses(lambda m: project.annotate_chords([m]), chord, "rootPc")
        key = {"start_ppq": 0.0, "end_ppq": 480.0, "tonic_pc": 0, "mode": "major"}
        _refuses(lambda m: project.annotate_keys([m]), key, "tonicPc")
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
        with pytest.raises(SonareValueError, match=r"unknown key stems\[0\]\.channels \(expected "):
            project.import_external_stems(48000, [{**stem, "channels": stem["planar_samples"]}])
        # A channel count is not the layout token, and a near spelling lists the tokens.
        with pytest.raises(TypeError, match=r"stems\[0\]\.layout must be one of mono, stereo"):
            project.import_external_stems(48000, [{**stem, "layout": 1}])
        with pytest.raises(
            SonareValueError, match=r"stems\[0\]\.layout must be one of mono, stereo"
        ):
            project.import_external_stems(48000, [{**stem, "layout": "Mono"}])
        assert project.to_json() == before
        imported = project.import_external_stems(48000, [stem])
        assert len(imported.track_ids) == 1
    finally:
        project.close()


@needs_lib
def test_a_positional_tuple_or_descriptor_in_place_of_a_mapping_is_a_type_error() -> None:
    """Each list below takes one item form: its record class or, with none, a mapping."""
    from libsonare import AutomationPoint, Project, RealtimeEngine

    project = Project()
    try:
        track = project.add_track("audio", "gtr")
        clip = project.add_clip(
            track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_channels=1, audio_sample_rate=48000
        )
        point = AutomationPoint(ppq=0.0, value=0.5)
        project.add_automation_lane(track, 7, [point])
        project.set_tempo_segments([{"start_ppq": 0.0, "bpm": 120.0}])
        project.set_time_signatures([{"start_ppq": 0.0, "numerator": 3, "denominator": 4}])
        project.set_clip_takes(clip, [{"id": 1}], active_take_id=1)
        project.set_clip_comp_segments(clip, [{"start_ppq": 0.0, "end_ppq": 1.0, "take_id": 1}])
        project.set_assist_sidecar("mod", b"x")
        before = project.to_json()
        with pytest.raises(TypeError):
            project.add_automation_lane(track, 8, [(0.0, 0.5, "linear")])  # type: ignore[list-item]
        with pytest.raises(TypeError):
            project.edit_automation_lane(track, 7, [(0.0, 0.5)])  # type: ignore[list-item]
        # AutomationPoint is the record for a point, so a mapping is refused too.
        with pytest.raises(TypeError, match="AutomationPoint"):
            project.add_automation_lane(track, 8, [{"ppq": 0.0, "value": 0.5}])  # type: ignore[list-item]
        with pytest.raises(TypeError, match="AutomationPoint"):
            project.edit_automation_lane(track, 7, [{"ppq": 0.0, "value": 0.5}])  # type: ignore[list-item]
        with pytest.raises(TypeError, match="target_param_id must be an integer$"):
            project.add_automation_lane(track, {"target_param_id": 9, "points": [point]})  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            project.set_tempo_segments([(0.0, 120.0)])  # type: ignore[list-item]
        with pytest.raises(TypeError):
            project.set_time_signatures([(0.0, 3, 4)])  # type: ignore[list-item]
        with pytest.raises(TypeError):
            project.set_clip_takes(clip, [(1, 0, 0.0, "a")], active_take_id=1)  # type: ignore[list-item]
        with pytest.raises(TypeError):
            project.set_clip_comp_segments(clip, [(0.0, 1.0, 1)])  # type: ignore[list-item]
        with pytest.raises(TypeError, match="module_id"):
            project.set_assist_sidecar({"module_id": "mod"})  # type: ignore[arg-type]
        assert project.to_json() == before
    finally:
        project.close()

    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_tempo_segments([{"start_ppq": 0.0, "bpm": 120.0}])
        engine.set_time_signature_segments([{"start_ppq": 0.0, "numerator": 3, "denominator": 4}])
        engine.set_automation_lane(5, [AutomationPoint(ppq=0.0, value=0.5)])
        with pytest.raises(TypeError, match="AutomationPoint"):
            engine.set_automation_lane(5, [{"ppq": 0.0, "value": 0.5}])  # type: ignore[list-item]
        with pytest.raises(TypeError):
            engine.set_track_lanes([10])  # type: ignore[list-item]
        with pytest.raises(TypeError):
            engine.set_tempo_segments([(0.0, 120.0)])  # type: ignore[list-item]
        with pytest.raises(TypeError):
            engine.set_time_signature_segments([(0.0, 3, 4)])  # type: ignore[list-item]
        # Refused before the library is reached, so no master strip is needed.
        with pytest.raises(TypeError, match="band must be a mapping"):
            engine.set_strip_eq_band("master", 0, '{"type":"peak"}')  # type: ignore[arg-type]
