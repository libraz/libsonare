"""Realtime engine telemetry tests: meter and scope records, counts and error ordinals."""

from __future__ import annotations

import math

import pytest

from libsonare import (
    AutomationCurve,
    AutomationPoint,
    ChannelLayout,
    EngineClip,
    EngineGraphConnection,
    EngineGraphNode,
    EngineGraphNodeType,
    EngineGraphParameterBinding,
    EngineGraphSpec,
    EngineMarker,
    EngineMetronomeConfig,
    EngineTelemetryError,
    EngineTelemetryType,
    ParameterInfo,
    RealtimeEngine,
    ScopeTelemetryRecord,
    SonareError,
)


def test_engine_drain_meter_telemetry_wide_surround_bus() -> None:
    frames = 256
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[[0.5] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                )
            ]
        )
        # 5.1 group bus; the lane routes into it and is panned hard to Ls.
        engine.set_track_buses([{"bus_id": 1, "channel_layout": ChannelLayout.FIVE_POINT_ONE}])
        engine.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
        engine.set_track_strip_json(
            10,
            '{"version":1,"buses":[{"id":"master","role":"master"}],'
            '"strips":[{"id":"s","surroundPan":{"azimuth":-110}}]}',
        )
        engine.play()
        engine.process([[0.0] * frames for _ in range(6)])
        wide = engine.drain_meter_telemetry_wide()
        bus_meter = next((r for r in wide if r.target_id == 33), None)
        assert bus_meter is not None
        assert bus_meter.channel_count == 6
        assert len(bus_meter.peak_db) == 6
        # Ls (plane 4) carries the panned lane, above the silent front-left.
        assert bus_meter.peak_db[4] > bus_meter.peak_db[0] + 10.0


def test_realtime_engine_process_and_telemetry() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_tempo(60.0)
        engine.set_time_signature(3, 4)
        engine.set_markers([EngineMarker(11, 1.0, "intro"), EngineMarker(12, 2.0, "out")])
        assert engine.marker_count() == 2
        assert engine.marker_by_index(0).name == "intro"
        assert engine.marker(12).ppq == 2.0
        engine.set_loop_from_markers(11, 12)
        engine.set_metronome(
            EngineMetronomeConfig(enabled=True, beat_gain=0.25, accent_gain=0.75, click_samples=16)
        )
        assert engine.metronome().enabled
        assert engine.metronome().click_samples == 16
        assert engine.count_in_end_sample(0, 2) == 288000
        assert engine.sample_at_ppq(1.5) == 72000
        with pytest.raises(SonareError) as bad_ppq_error:
            engine.sample_at_ppq(math.nan)
        assert bad_ppq_error.value.code == 4
        with pytest.raises(SonareError):
            engine.sample_at_ppq(1.0e300)
        with pytest.raises(SonareError):
            engine.seek_ppq(1.0e300)
        with pytest.raises(SonareError):
            engine.set_loop(0.0, 1.0e300)
        engine.set_metronome(EngineMetronomeConfig(enabled=False))
        engine.add_parameter(
            ParameterInfo(
                id=7,
                name="gain",
                unit="dB",
                min_value=-60.0,
                max_value=12.0,
                default_value=0.0,
                rt_safe=True,
                default_curve=AutomationCurve.LINEAR,
            )
        )
        assert engine.parameter_count() == 1
        parameter = engine.parameter_info(7)
        assert parameter.name == "gain"
        assert parameter.unit == "dB"
        assert engine.parameter_info_by_index(0).id == 7
        engine.set_automation_lane(
            7,
            [
                AutomationPoint(0.0, 0.0),
                AutomationPoint(1.0, 6.0205999, AutomationCurve.LINEAR),
            ],
        )
        assert engine.automation_lane_count() == 1
        engine.set_graph(
            EngineGraphSpec(
                nodes=[
                    EngineGraphNode("in", num_ports=2),
                    EngineGraphNode("gain", EngineGraphNodeType.GAIN, gain_db=0.0, num_ports=2),
                    EngineGraphNode("out", num_ports=2),
                ],
                connections=[
                    EngineGraphConnection("in", 0, "gain", 0),
                    EngineGraphConnection("in", 1, "gain", 1),
                    EngineGraphConnection("gain", 0, "out", 0),
                    EngineGraphConnection("gain", 1, "out", 1),
                ],
                input_node="in",
                output_node="out",
                num_channels=2,
                parameter_bindings=[EngineGraphParameterBinding(7, "gain")],
            )
        )
        assert engine.graph_node_count() == 3
        assert engine.graph_connection_count() == 4
        engine.set_clips(
            [
                EngineClip(
                    id=101,
                    channels=[[0.125] * 128, [-0.125] * 128],
                    start_ppq=1.0,
                    length_samples=128,
                )
            ]
        )
        assert engine.clip_count() == 1
        engine.set_capture_buffer(2, 128)
        engine.set_capture_punch(48000, 48128)
        engine.arm_capture()
        engine.seek_marker(11)
        engine.play()
        left = [0.25] * 128
        right = [-0.25] * 128

        processed = engine.process([left, right])
        assert all(math.isclose(sample, 0.75, abs_tol=0.0001) for sample in processed[0])
        assert all(math.isclose(sample, -0.75, abs_tol=0.0001) for sample in processed[1])
        capture_status = engine.capture_status()
        assert capture_status.captured_frames == 128
        assert capture_status.overflow_count == 0
        assert capture_status.armed
        assert capture_status.source == "output"
        assert capture_status.record_offset_samples == 0
        captured = engine.captured_audio()
        assert all(math.isclose(sample, 0.75, abs_tol=0.0001) for sample in captured[0])
        assert all(math.isclose(sample, -0.75, abs_tol=0.0001) for sample in captured[1])
        engine.reset_capture()
        assert engine.capture_status().captured_frames == 0

        telemetry = engine.drain_telemetry()
        assert telemetry
        last = telemetry[-1]
        assert last.type == EngineTelemetryType.PROCESS_BLOCK
        assert last.error == EngineTelemetryError.NONE
        assert last.render_frame == 0
        assert last.timeline_sample == 48000 + 128
        assert last.audible_timeline_sample == 48000 + 128

        engine.set_capture_source("input")
        engine.set_record_offset_samples(-37)
        engine.arm_capture()
        engine.seek_marker(11)
        engine.process([left, right])
        input_capture_status = engine.capture_status()
        assert input_capture_status.source == "input"
        assert input_capture_status.record_offset_samples == -37
        captured = engine.captured_audio()
        assert all(math.isclose(sample, 0.25, abs_tol=0.0001) for sample in captured[0])
        assert all(math.isclose(sample, -0.25, abs_tol=0.0001) for sample in captured[1])
        assert any(record.target_id == 0xFFFF for record in engine.drain_meter_telemetry())

        engine.set_input_monitor(False)
        engine.reset_capture()
        engine.arm_capture()
        engine.seek_marker(11)
        monitored = engine.process([[0.25] * 128, [-0.25] * 128])
        assert all(math.isclose(sample, 0.25, abs_tol=0.0001) for sample in monitored[0])
        assert all(math.isclose(sample, -0.25, abs_tol=0.0001) for sample in monitored[1])
        captured = engine.captured_audio()
        assert all(math.isclose(sample, 0.25, abs_tol=0.0001) for sample in captured[0])

        engine.set_input_monitor(True, 0.5)
        engine.seek_marker(11)
        monitored = engine.process([[0.25] * 128, [-0.25] * 128])
        assert all(math.isclose(sample, 0.5, abs_tol=0.0001) for sample in monitored[0])
        assert all(math.isclose(sample, -0.5, abs_tol=0.0001) for sample in monitored[1])
        with pytest.raises(SonareError) as bad_monitor_gain_error:
            engine.set_input_monitor(True, math.nan)
        assert bad_monitor_gain_error.value.code == 4


def test_realtime_engine_reports_oversized_channel_telemetry() -> None:
    assert EngineTelemetryError.MAX_CHANNELS_EXCEEDED.value == 20
    assert 19 not in {error.value for error in EngineTelemetryError}

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128, max_channels=2) as engine:
        output = engine.process([[0.0] * 8, [0.0] * 8, [0.0] * 8])
        assert output == [[0.0] * 8, [0.0] * 8, [0.0] * 8]

        oversized = [
            record
            for record in engine.drain_telemetry()
            if record.type == EngineTelemetryType.ERROR
            and record.error == EngineTelemetryError.MAX_CHANNELS_EXCEEDED
        ]
        assert oversized
        assert oversized[-1].value == 3


def test_realtime_engine_scope_telemetry() -> None:
    sample_rate = 48000.0
    block = 256
    num_blocks = 12
    total = block * num_blocks
    freq = 1000.0
    tone = [0.5 * math.sin(2.0 * math.pi * freq * n / sample_rate) for n in range(total)]
    with RealtimeEngine(sample_rate=sample_rate, max_block_size=block) as engine:
        applied = engine.configure_scope_telemetry(256, 32)
        assert applied == 32
        engine.set_clips(
            [
                EngineClip(
                    id=201,
                    channels=[list(tone), list(tone)],
                    start_ppq=0.0,
                    length_samples=total,
                )
            ]
        )
        engine.play()
        for _ in range(num_blocks):
            engine.process([[0.0] * block, [0.0] * block])
        records = engine.drain_scope_telemetry()
        assert records
        master = [r for r in records if r.target_id == 0]
        assert master
        record = master[-1]
        assert isinstance(record, ScopeTelemetryRecord)
        assert len(record.bands) == 32
        assert len(record.points) > 0
        argmax = max(range(len(record.bands)), key=lambda i: record.bands[i])
        assert argmax <= 2


@pytest.mark.parametrize("reader", ["scope", "meter_wide"])
def test_telemetry_counts_are_clamped_to_the_mirror_arrays(reader: str) -> None:
    """An over-large count truncates, as it does on Node and WASM.

    The count is trusted from the C record, so a count past the fixed-array
    capacity used to raise IndexError out of the Python reader while the WASM
    reader (`b < rec.band_count && b < rec.bands.size()`) and the Node addon
    (engine/common.h clamps to SONARE_SCOPE_MAX_BANDS) returned a short record
    for the same bytes.
    """
    from libsonare._engine_conversions import (
        _meter_telemetry_wide_from_c,
        _scope_telemetry_from_c,
    )
    from libsonare._ffi_types_core import (
        SonareMeterTelemetryRecordWide,
        SonareScopeTelemetryRecord,
    )

    if reader == "scope":
        raw = SonareScopeTelemetryRecord()
        max_bands = len(raw.bands)
        max_points = len(raw.points) // 2
        raw.band_count = max_bands + 7
        raw.point_count = max_points + 7
        record = _scope_telemetry_from_c(raw)
        assert len(record.bands) == max_bands
        assert len(record.points) == max_points
    else:
        raw_wide = SonareMeterTelemetryRecordWide()
        max_planes = len(raw_wide.peak_db)
        raw_wide.channel_count = max_planes + 7
        wide = _meter_telemetry_wide_from_c(raw_wide)
        assert wide.channel_count == max_planes
        assert len(wide.peak_db) == max_planes
        assert len(wide.rms_db) == max_planes
        assert len(wide.true_peak_db) == max_planes


def test_scope_telemetry_still_returns_a_count_it_can_serve() -> None:
    """The clamp must not truncate a count the mirror can actually hold."""
    from libsonare._engine_conversions import _scope_telemetry_from_c
    from libsonare._ffi_types_core import SonareScopeTelemetryRecord

    raw = SonareScopeTelemetryRecord()
    raw.band_count = 3
    raw.point_count = 2
    raw.bands[0], raw.bands[1], raw.bands[2] = 1.0, 2.0, 3.0
    raw.points[0], raw.points[1] = 0.25, -0.25
    raw.points[2], raw.points[3] = 0.5, -0.5

    record = _scope_telemetry_from_c(raw)
    assert record.bands == [1.0, 2.0, 3.0]
    assert record.points == [(0.25, -0.25), (0.5, -0.5)]


def test_engine_overflow_counters_start_at_zero_and_stay_there_when_nothing_drops() -> None:
    """The two advisory overflow counters mirror external_midi_dropped_count.

    Both are monotonic within a prepared session and reset by prepare, so a
    freshly prepared engine reads zero and an ordinary block leaves it there.
    A non-zero reading is the only signal the host gets that a clip page was
    never requested, or that a time-stretched clip fell back to resampling.
    """
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        assert engine.clip_page_request_overflow_count() == 0
        assert engine.warp_stretch_overflow_count() == 0
        engine.play()
        engine.process([[0.0] * 128, [0.0] * 128])
        assert engine.clip_page_request_overflow_count() == 0
        assert engine.warp_stretch_overflow_count() == 0


def test_engine_telemetry_error_parameter_base_overflow_ordinal() -> None:
    assert EngineTelemetryError.PARAMETER_BASE_OVERFLOW.value == 21
    assert EngineTelemetryError.PARAMETER_BASE_OVERFLOW.name == "PARAMETER_BASE_OVERFLOW"
