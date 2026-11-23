"""The live input track and the input monitor on RealtimeEngine."""

from __future__ import annotations

import pytest

from libsonare import (
    EngineInputMonitor,
    EngineInputMonitorInput,
    RealtimeEngine,
    SonareValueError,
)


def _engine() -> RealtimeEngine:
    return RealtimeEngine(sample_rate=48000, max_block_size=128)


def test_input_track_reads_what_was_set_and_none_after_a_clear() -> None:
    with _engine() as engine:
        engine.set_track_lanes([{"track_id": 10}, {"track_id": 20}])
        assert engine.input_track is None
        engine.set_input_track(10)
        assert engine.input_track == 10
        engine.set_input_track(20)
        assert engine.input_track == 20
        engine.clear_input_track()
        assert engine.input_track is None
        engine.clear_input_track()
        assert engine.input_track is None


def test_unknown_input_track_is_refused_and_changes_nothing() -> None:
    with _engine() as engine:
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_input_track(10)
        with pytest.raises(SonareValueError) as refused:
            engine.set_input_track(11)
        assert "track_id" in str(refused.value)
        assert "track lane" in str(refused.value)
        assert engine.input_track == 10


def test_input_track_out_of_uint32_is_refused_under_track_id() -> None:
    with _engine() as engine:
        with pytest.raises(SonareValueError, match="track_id"):
            engine.set_input_track(2**32)
        with pytest.raises(SonareValueError, match="track_id"):
            engine.set_input_track(-1)


def test_dropping_the_input_tracks_lane_leaves_no_input_track() -> None:
    with _engine() as engine:
        engine.set_track_lanes([{"track_id": 10}, {"track_id": 20}])
        engine.set_input_track(10)
        engine.set_track_lanes([{"track_id": 20}])
        assert engine.input_track is None


def test_input_monitor_round_trips_through_its_reader() -> None:
    with _engine() as engine:
        assert engine.input_monitor() == EngineInputMonitor(enabled=True, gain=1.0)
        engine.set_input_monitor(EngineInputMonitorInput(enabled=False, gain=0.25))
        current = engine.input_monitor()
        assert current == EngineInputMonitor(enabled=False, gain=0.25)
        engine.set_input_monitor(current)
        assert engine.input_monitor() == current
        engine.set_input_monitor(EngineInputMonitorInput(enabled=True))
        assert engine.input_monitor() == EngineInputMonitor(enabled=True, gain=1.0)


def test_configuring_capture_leaves_the_input_monitor_as_set() -> None:
    with _engine() as engine:
        engine.set_input_monitor(EngineInputMonitorInput(enabled=False, gain=0.5))
        engine.configure_capture(capacity_frames=128, source="input")
        assert engine.input_monitor() == EngineInputMonitor(enabled=False, gain=0.5)


def test_configure_capture_no_longer_takes_an_input_monitor() -> None:
    with _engine() as engine, pytest.raises(TypeError):
        engine.configure_capture(  # type: ignore[call-arg]
            capacity_frames=128, input_monitor=EngineInputMonitorInput(enabled=False)
        )


def test_set_input_monitor_refuses_a_value_of_the_wrong_type() -> None:
    with _engine() as engine, pytest.raises(TypeError, match="monitor"):
        engine.set_input_monitor({"enabled": False, "gain": 1.0})  # type: ignore[arg-type]
