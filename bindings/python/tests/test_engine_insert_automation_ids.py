"""Insert automation id lifetimes; mirrors tests/api/sonare_c_engine_strip_test.cpp."""

import json

import pytest

from libsonare import RealtimeEngine, SonareError, SonareValueError, SynthPatch
from libsonare._runtime import _get_lib

from ._helpers import scene_bus, scene_strip


def _scene(kind: str, strip_id: str, processors: list[str]) -> str:
    inserts = [
        {"slot": "pre" if index < 32 else "post", "processor": processor, "params": {}}
        for index, processor in enumerate(processors)
    ]
    strip = {"id": strip_id, "inserts": inserts}
    return json.dumps(
        {
            "version": 2,
            "strips": [strip] if kind == "strips" else [],
            "buses": [strip] if kind == "buses" else [],
            "connections": [],
        }
    )


def _alive(engine: RealtimeEngine, param_id: int) -> bool:
    try:
        engine.parameter_info(param_id)
    except SonareError:
        return False
    return True


def test_track_id_survives_reorder_and_removal_of_another_track() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}, {"track_id": 20}])
        engine.set_strip(10, scene_strip(_scene("strips", "track-10", ["utility.gain"])))
        engine.set_strip(20, scene_strip(_scene("strips", "track-20", ["utility.gain"])))
        id10 = engine.resolve_strip_insert_automation_id(10, 0, "levelDb")
        id20 = engine.resolve_strip_insert_automation_id(20, 0, "levelDb")
        assert id10 != id20

        engine.set_track_lanes([{"track_id": 20}, {"track_id": 10}])
        assert engine.resolve_strip_insert_automation_id(10, 0, "levelDb") == id10

        engine.set_track_lanes([{"track_id": 10}])
        assert _alive(engine, id10)
        assert not _alive(engine, id20)

        engine.set_track_lanes([{"track_id": 20}, {"track_id": 10}])
        engine.set_strip(20, scene_strip(_scene("strips", "track-20", ["utility.gain"])))
        assert not _alive(engine, id20)
        assert engine.resolve_strip_insert_automation_id(20, 0, "levelDb") != id20


def test_bus_id_survives_reorder_and_retires_when_the_bus_is_removed() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}, {"bus_id": 2, "gain_db": 0.0}])
        engine.set_bus_strip(1, scene_bus(_scene("buses", "1", ["utility.gain"])))
        engine.set_bus_strip(2, scene_bus(_scene("buses", "2", ["utility.gain"])))
        id1 = engine.resolve_bus_insert_automation_id(1, 0, "levelDb")
        id2 = engine.resolve_bus_insert_automation_id(2, 0, "levelDb")
        assert id1 != id2

        engine.set_track_buses([{"bus_id": 2, "gain_db": 0.0}, {"bus_id": 1, "gain_db": 0.0}])
        assert engine.resolve_bus_insert_automation_id(1, 0, "levelDb") == id1
        assert _alive(engine, id2)

        engine.set_track_buses([{"bus_id": 2, "gain_db": 0.0}])
        assert _alive(engine, id2)
        assert not _alive(engine, id1)
        engine.set_track_buses([{"bus_id": 2, "gain_db": 0.0}, {"bus_id": 1, "gain_db": 0.0}])
        engine.set_bus_strip(1, scene_bus(_scene("buses", "1", ["utility.gain"])))
        assert not _alive(engine, id1)
        assert engine.resolve_bus_insert_automation_id(1, 0, "levelDb") != id1


def test_ids_retire_when_the_slot_changes_processor_type() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])

        def set_all(processor: str) -> None:
            engine.set_strip(10, scene_strip(_scene("strips", "track-10", [processor])))
            engine.set_bus_strip(1, scene_bus(_scene("buses", "1", [processor])))
            engine.set_strip("master", scene_strip(_scene("strips", "master", [processor])))

        def resolve_all(key: str) -> list[int]:
            return [
                engine.resolve_strip_insert_automation_id(10, 0, key),
                engine.resolve_bus_insert_automation_id(1, 0, key),
                engine.resolve_strip_insert_automation_id("master", 0, key),
            ]

        set_all("utility.gain")
        gain_ids = resolve_all("levelDb")
        set_all("utility.gain")
        assert resolve_all("levelDb") == gain_ids

        set_all("dynamics.compressor")
        assert not any(_alive(engine, param_id) for param_id in gain_ids)
        compressor_ids = resolve_all("thresholdDb")
        for compressor_id, gain_id in zip(compressor_ids, gain_ids, strict=True):
            assert _alive(engine, compressor_id)
            assert compressor_id != gain_id
        set_all("utility.gain")
        assert not any(_alive(engine, param_id) for param_id in gain_ids)


def test_strip_change_overflowing_the_id_table_is_refused() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=64) as engine:
        # 64 slots per rebuild against 8192 entries: rebuild 129 cannot fit.
        gain = _scene("strips", "master", ["utility.gain"] * 64)
        tilt = _scene("strips", "master", ["eq.tilt"] * 64)
        for round_index in range(128):
            engine.set_strip("master", scene_strip(gain if round_index % 2 == 0 else tilt))
        before = engine.resolve_strip_insert_automation_id("master", 0, "tiltDb")
        with pytest.raises(SonareError):
            engine.set_strip("master", scene_strip(gain))
        assert _alive(engine, before)
        engine.set_strip("master", scene_strip(tilt))


@pytest.mark.parametrize(
    ("name", "method", "args", "c_entry", "subjects", "public_entry", "message"),
    [
        (
            "track insert",
            "resolve_strip_insert_automation_id",
            (10, 0, "noSuchParam"),
            "engine_resolve_track_insert_automation_id",
            (b"param_name",),
            "RealtimeEngine.resolve_strip_insert_automation_id",
            "param_name must be the name of a parameter of the insert "
            "that can be set while running",
        ),
        (
            "master insert",
            "resolve_strip_insert_automation_id",
            ("master", 0, "noSuchParam"),
            "engine_resolve_master_insert_automation_id",
            (b"param_name",),
            "RealtimeEngine.resolve_strip_insert_automation_id",
            "param_name must be the name of a parameter of the insert "
            "that can be set while running",
        ),
        (
            "bus insert",
            "resolve_bus_insert_automation_id",
            (1, 0, "noSuchParam"),
            "engine_resolve_bus_insert_automation_id",
            (b"param_name",),
            "RealtimeEngine.resolve_bus_insert_automation_id",
            "param_name must be the name of a parameter of the insert "
            "that can be set while running",
        ),
        (
            "track lane kind",
            "resolve_strip_automation_id",
            (10, "noSuchParam"),
            "engine_resolve_track_lane_automation_id",
            (b"param_name",),
            "RealtimeEngine.resolve_strip_automation_id",
            "kind must be the name of a track strip parameter: faderDb or pan",
        ),
        (
            "master strip kind",
            "resolve_strip_automation_id",
            ("master", "noSuchParam"),
            "engine_resolve_master_automation_id",
            (b"param_name",),
            "RealtimeEngine.resolve_strip_automation_id",
            "kind must be the name of a master strip parameter: faderDb, pan or width",
        ),
        (
            "bus strip parameter",
            "resolve_bus_automation_id",
            (1, "noSuchParam"),
            "engine_resolve_bus_automation_id",
            (b"param_name",),
            "RealtimeEngine.resolve_bus_automation_id",
            "param_name must be the name of a bus strip parameter: faderDb",
        ),
        (
            "instrument parameter",
            "resolve_instrument_automation_id",
            (5, "noSuchParam"),
            "engine_resolve_instrument_automation_id",
            (b"param_name", b"destination_id"),
            "RealtimeEngine.resolve_instrument_automation_id",
            "param_name must be the name of a parameter of the instrument bound to destination_id",
        ),
    ],
)
def test_unknown_automation_names_keep_the_c_refusal(
    name: str,
    method: str,
    args: tuple[object, ...],
    c_entry: str,
    subjects: tuple[bytes, ...],
    public_entry: str,
    message: str,
) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        engine.set_strip(10, scene_strip(_scene("strips", "track-10", ["utility.gain"])))
        engine.set_strip("master", scene_strip(_scene("strips", "master", ["utility.gain"])))
        engine.set_bus_strip(1, scene_bus(_scene("buses", "1", ["utility.gain"])))
        engine.set_synth_instrument(SynthPatch(), destination_id=5)

        with pytest.raises(SonareValueError) as caught:
            getattr(engine, method)(*args)

        assert str(caught.value) == f"{public_entry}: {message}", name
        assert caught.value.entry == public_entry
        lib = _get_lib()
        raw_entry = lib.sonare_last_refusal_entry()
        assert raw_entry == c_entry.encode("utf-8")
        assert lib.sonare_last_refusal_subject_count() == len(subjects)
        assert (
            tuple(lib.sonare_last_refusal_subject(index) for index in range(len(subjects)))
            == subjects
        )


def test_instrument_unknown_destination_refusal_names_the_destination_subject() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        with pytest.raises(SonareValueError) as caught:
            engine.resolve_instrument_automation_id(99, "gain")

        public_entry = "RealtimeEngine.resolve_instrument_automation_id"
        assert str(caught.value) == (
            f"{public_entry}: destination_id must be the id of a destination "
            "with a MIDI instrument in this engine"
        )
        assert caught.value.entry == public_entry
        lib = _get_lib()
        assert lib.sonare_last_refusal_entry() == b"engine_resolve_instrument_automation_id"
        assert lib.sonare_last_refusal_subject_count() == 1
        assert lib.sonare_last_refusal_subject(0) == b"destination_id"


def test_track_width_refusal_uses_the_track_lane_domain() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        with pytest.raises(SonareValueError) as caught:
            engine.resolve_strip_automation_id(10, "width")

        public_entry = "RealtimeEngine.resolve_strip_automation_id"
        assert str(caught.value) == (
            f"{public_entry}: kind must be the name of a track strip parameter: faderDb or pan"
        )
        assert caught.value.entry == public_entry
        lib = _get_lib()
        assert lib.sonare_last_refusal_entry() == b"engine_resolve_track_lane_automation_id"
        assert lib.sonare_last_refusal_subject_count() == 1
        assert lib.sonare_last_refusal_subject(0) == b"param_name"
