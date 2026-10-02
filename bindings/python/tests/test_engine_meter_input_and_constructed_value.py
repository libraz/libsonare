"""Meter input peaks, the master loudness reset and constructed insert values."""

import math

import pytest

from libsonare import RealtimeEngine, SonareError

_STRIP_JSON = (
    '{"version":1,"strips":[{"id":"track-10","inserts":['
    '{"slot":"pre","processor":"eq.parametric","params":"{'
    '\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,'
    '\\"band0.gainDb\\":-3,\\"band0.enabled\\":1}"}]}],'
    '"buses":[],"connections":[]}'
)


def test_meter_input_peaks_and_reset_command() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_input_monitor(True)
        engine.play()
        engine.process([[0.25] * 128, [-0.25] * 128])
        records = engine.drain_meter_telemetry()
        master = [record for record in records if record.target_id == 0]
        assert master
        expected_db = 20.0 * math.log10(0.25)
        assert master[-1].input_peak_db_l == pytest.approx(expected_db, abs=0.05)
        assert master[-1].input_peak_db_r == pytest.approx(expected_db, abs=0.05)
        engine.reset_master_loudness_meter()


def test_constructed_insert_value_and_invalid_id() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_track_lanes([10])
        engine.set_track_strip_json(10, _STRIP_JSON)
        param_id = engine.resolve_track_insert_automation_id(10, 0, "band0.gainDb")
        assert param_id > 0
        assert engine.insert_parameter_constructed_value(param_id) == pytest.approx(-3.0, abs=1e-5)
        with pytest.raises(SonareError):
            engine.insert_parameter_constructed_value(0)


def test_wide_input_peak_conversion() -> None:
    from libsonare._engine_conversions import _meter_telemetry_wide_from_c
    from libsonare._ffi_types_core import SonareMeterTelemetryRecordWideV2

    raw = SonareMeterTelemetryRecordWideV2()
    raw.channel_count = 2
    raw.input_peak_db[0] = -6.0
    raw.input_peak_db[1] = -12.0
    record = _meter_telemetry_wide_from_c(raw)
    assert record.input_peak_db == pytest.approx([-6.0, -12.0])
