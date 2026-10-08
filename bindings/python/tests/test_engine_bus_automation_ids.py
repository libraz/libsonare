"""Bus fader automation id lifetimes; mirrors tests/api/sonare_c_engine_strip_test.cpp."""

import pytest

from libsonare import EngineClip, RealtimeEngine, SonareError

_BLOCK = 256
_FRAMES = _BLOCK * 80


def _alive(engine: RealtimeEngine, param_id: int) -> bool:
    try:
        engine.parameter_info(param_id)
    except SonareError:
        return False
    return True


def _settled(engine: RealtimeEngine, blocks: int = 30) -> float:
    out = [[0.0] * _BLOCK]
    for _ in range(blocks):
        out = engine.process([[0.0] * _BLOCK])
    return out[0][-1]


def test_bus_fader_id_keeps_its_bus_across_a_reorder_and_retires_with_it() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=_BLOCK) as engine:
        # Only track 20 sounds and it feeds bus 200, so the output reads bus 200's fader alone.
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=20,
                    channels=[[1.0] * _FRAMES],
                    start_ppq=0.0,
                    length_samples=_FRAMES,
                )
            ]
        )
        engine.set_track_buses([{"bus_id": 100}, {"bus_id": 200}])
        engine.set_track_lanes([{"track_id": 20, "output_bus_id": 200}])
        fader100 = engine.resolve_bus_automation_id(100, "faderDb")
        fader200 = engine.resolve_bus_automation_id(200, "faderDb")
        assert fader100 != fader200
        with pytest.raises(SonareError):
            engine.resolve_bus_automation_id(300, "faderDb")
        with pytest.raises(SonareError):
            engine.resolve_bus_automation_id(100, "pan")

        engine.set_track_buses([{"bus_id": 200}, {"bus_id": 100}])
        assert engine.resolve_bus_automation_id(200, "faderDb") == fader200
        engine.set_parameter(fader200, -60.0)
        engine.play()
        assert _settled(engine) == pytest.approx(0.001, abs=1e-4)

        engine.set_parameter(fader200, 0.0)
        engine.set_track_buses([{"bus_id": 200}])
        assert not _alive(engine, fader100)
        engine.set_parameter(fader100, -60.0)
        assert _settled(engine) == pytest.approx(1.0, abs=1e-3)
        engine.set_track_buses([{"bus_id": 200}, {"bus_id": 100}])
        assert engine.resolve_bus_automation_id(100, "faderDb") != fader100
