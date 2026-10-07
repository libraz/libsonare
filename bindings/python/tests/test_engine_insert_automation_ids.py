"""Insert automation id lifetimes; mirrors tests/api/sonare_c_engine_strip_test.cpp."""

import json

import pytest

from libsonare import RealtimeEngine, SonareError


def _scene(kind: str, strip_id: str, processors: list[str]) -> str:
    inserts = [
        {"slot": "pre" if index < 32 else "post", "processor": processor, "params": "{}"}
        for index, processor in enumerate(processors)
    ]
    strip = {"id": strip_id, "inserts": inserts}
    return json.dumps(
        {
            "version": 1,
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
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_track_lanes([10, 20])
        engine.set_track_strip_json(10, _scene("strips", "track-10", ["utility.gain"]))
        engine.set_track_strip_json(20, _scene("strips", "track-20", ["utility.gain"]))
        id10 = engine.resolve_track_insert_automation_id(10, 0, "levelDb")
        id20 = engine.resolve_track_insert_automation_id(20, 0, "levelDb")
        assert id10 != id20

        engine.set_track_lanes([20, 10])
        assert engine.resolve_track_insert_automation_id(10, 0, "levelDb") == id10

        engine.set_track_lanes([10])
        assert _alive(engine, id10)
        assert not _alive(engine, id20)

        engine.set_track_lanes([20, 10])
        engine.set_track_strip_json(20, _scene("strips", "track-20", ["utility.gain"]))
        assert not _alive(engine, id20)
        assert engine.resolve_track_insert_automation_id(20, 0, "levelDb") != id20


def test_bus_id_survives_reorder_and_retires_when_the_bus_is_removed() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}, {"bus_id": 2, "gain_db": 0.0}])
        engine.set_bus_strip_json(1, _scene("buses", "1", ["utility.gain"]))
        engine.set_bus_strip_json(2, _scene("buses", "2", ["utility.gain"]))
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
        engine.set_bus_strip_json(1, _scene("buses", "1", ["utility.gain"]))
        assert not _alive(engine, id1)
        assert engine.resolve_bus_insert_automation_id(1, 0, "levelDb") != id1


def test_ids_retire_when_the_slot_changes_processor_type() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_track_lanes([10])
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])

        def set_all(processor: str) -> None:
            engine.set_track_strip_json(10, _scene("strips", "track-10", [processor]))
            engine.set_bus_strip_json(1, _scene("buses", "1", [processor]))
            engine.set_master_strip_json(_scene("strips", "master", [processor]))

        def resolve_all(key: str) -> list[int]:
            return [
                engine.resolve_track_insert_automation_id(10, 0, key),
                engine.resolve_bus_insert_automation_id(1, 0, key),
                engine.resolve_master_insert_automation_id(0, key),
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
    with RealtimeEngine(sample_rate=48000.0, max_block_size=64) as engine:
        # 64 slots per rebuild against 8192 entries: rebuild 129 cannot fit.
        gain = _scene("strips", "master", ["utility.gain"] * 64)
        tilt = _scene("strips", "master", ["eq.tilt"] * 64)
        for round_index in range(128):
            engine.set_master_strip_json(gain if round_index % 2 == 0 else tilt)
        before = engine.resolve_master_insert_automation_id(0, "tiltDb")
        with pytest.raises(SonareError):
            engine.set_master_strip_json(gain)
        assert _alive(engine, before)
        engine.set_master_strip_json(tilt)
