"""Track fader/pan automation id lifetimes; mirrors tests/api/sonare_c_engine_strip_test.cpp."""

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


def test_fader_id_keeps_its_track_across_a_reorder_and_retires_with_it() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=_BLOCK) as engine:
        # Only track 20 sounds, so the output reads track 20's fader alone.
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
        engine.set_track_lanes([10, 20])
        fader10 = engine.resolve_track_lane_automation_id(10, "faderDb")
        assert engine.resolve_track_lane_automation_id(10, "pan") != fader10
        with pytest.raises(SonareError):
            engine.resolve_track_lane_automation_id(30, "faderDb")
        with pytest.raises(SonareError):
            engine.resolve_track_lane_automation_id(10, "width")

        engine.set_track_lanes([20, 10])
        assert engine.resolve_track_lane_automation_id(10, "faderDb") == fader10
        engine.set_parameter(fader10, -60.0)
        engine.play()
        assert _settled(engine) == pytest.approx(1.0, abs=1e-3)

        engine.set_track_lanes([20])
        assert not _alive(engine, fader10)
        engine.set_parameter(fader10, -60.0)
        assert _settled(engine) == pytest.approx(1.0, abs=1e-3)
        engine.set_track_lanes([20, 10])
        assert engine.resolve_track_lane_automation_id(10, "faderDb") != fader10
