"""Meter input peaks, the master loudness reset and constructed insert values."""

import math

import numpy as np
import pytest

from libsonare import EngineClip, RealtimeEngine, SonareError

from ._helpers import scene_strip

_STRIP_JSON = (
    '{"version":1,"strips":[{"id":"track-10","inserts":['
    '{"slot":"pre","processor":"eq.parametric","params":{'
    '"band0.type":1,"band0.frequencyHz":1000,'
    '"band0.gainDb":-3,"band0.enabled":1}}]}],'
    '"buses":[],"connections":[]}'
)


@pytest.mark.parametrize("num_channels", [1, 2])
def test_meter_input_peaks_and_reset_command(num_channels: int) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.configure_capture(capacity_frames=128)
        engine.play()
        engine.process([[0.25] * 128, [-0.25] * 128][:num_channels])
        records = engine.drain_meter_telemetry()
        master = [record for record in records if record.target_id == 0]
        assert master
        expected_db = 20.0 * math.log10(0.25)
        record = master[-1]
        assert record.num_channels == num_channels
        assert record.input_peak_db == pytest.approx([expected_db] * num_channels, abs=0.05)
        engine.reset_master_loudness_meter()


def test_constructed_insert_value_and_invalid_id() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_strip(10, scene_strip(_STRIP_JSON))
        param_id = engine.resolve_strip_insert_automation_id(10, 0, "band0.gainDb")
        assert param_id > 0
        assert engine.insert_parameter_constructed_value(param_id) == pytest.approx(-3.0, abs=1e-5)
        with pytest.raises(SonareError):
            engine.insert_parameter_constructed_value(0)


@pytest.mark.parametrize("num_channels", [1, 2])
def test_meter_record_keeps_only_measured_planes(num_channels: int) -> None:
    from libsonare._engine_conversions import _meter_telemetry_from_c
    from libsonare._ffi_types_core import SonareMeterTelemetryRecord

    raw = SonareMeterTelemetryRecord()
    raw.target_id = 1
    raw.num_channels = num_channels
    levels = {
        "peak_db": (-3.0, -9.0),
        "rms_db": (-6.0, -12.0),
        "true_peak_dbtp": (-2.0, -8.0),
        "input_peak_db": (-4.0, -10.0),
    }
    for field, (left, right) in levels.items():
        plane = getattr(raw, field)
        plane[:] = [-120.0] * len(plane)
        plane[0] = left
        if num_channels == 2:
            plane[1] = right
    record = _meter_telemetry_from_c(raw)
    assert record.target_id == 1
    assert record.num_channels == num_channels
    for field, (left, right) in levels.items():
        assert getattr(record, field) == pytest.approx([left, right][:num_channels])
        assert not hasattr(record, f"{field}_r")


_DYNAMICS_STRIP_JSON = (
    '{"version":1,"strips":[{"id":"track-10","inserts":['
    '{"slot":"pre","processor":"dynamics.compressor","params":'
    '{"thresholdDb":-6,"ratio":2,"attackMs":0.1,"releaseMs":100,"kneeDb":0}},'
    '{"slot":"post","processor":"dynamics.limiter","params":'
    '{"thresholdDb":-20,"lookaheadMs":0,"releaseMs":50}}]}],'
    '"buses":[],"connections":[]}'
)


def test_meter_target_insert_gain_reduction() -> None:
    block = 256
    frames = block * 16
    clip = EngineClip(
        id=1,
        channels=[[1.0] * frames, [1.0] * frames],
        start_ppq=0.0,
        track_id=10,
        length_samples=frames,
    )
    with RealtimeEngine(sample_rate=48000, max_block_size=block) as engine:
        engine.set_clips([clip])
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_strip(10, scene_strip(_DYNAMICS_STRIP_JSON))
        engine.play()
        lane: list[object] = []
        for _ in range(12):
            engine.process([[0.0] * block, [0.0] * block])
            lane = [r for r in engine.drain_meter_telemetry() if r.target_id == 1] or lane
        reductions = engine.meter_target_insert_gain_reduction(1)
        assert isinstance(reductions, np.ndarray)
        assert reductions.dtype == np.float32
        assert len(reductions) == 2
        assert all(value <= 0.0 for value in reductions)
        assert min(reductions) < -1.0
        assert lane
        assert min(reductions) == pytest.approx(lane[-1].gain_reduction_db, abs=0.05)
        assert engine.meter_target_insert_gain_reduction(0xFFFF).size == 0
        with pytest.raises(SonareError):
            engine.meter_target_insert_gain_reduction(41)
