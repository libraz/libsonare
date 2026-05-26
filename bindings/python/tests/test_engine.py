"""Tests for the realtime/offline DAW engine Python wrapper."""

from __future__ import annotations

import math

import pytest

from libsonare import (
    AutomationPoint,
    AutomationPointCurve,
    EngineBounceOptions,
    EngineClip,
    EngineFreezeOptions,
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
    engine_abi_version,
)


def test_engine_abi_version() -> None:
    assert engine_abi_version() > 0


def test_engine_transport_state_and_live_parameters() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_tempo(90.0)
        engine.set_loop(0.0, 4.0, enabled=True)
        engine.add_parameter(
            ParameterInfo(
                id=3,
                name="gain",
                unit="dB",
                min_value=-60.0,
                max_value=12.0,
                default_value=0.0,
                rt_safe=True,
                default_curve=AutomationPointCurve.LINEAR,
            )
        )
        engine.set_parameter(3, 1.5)
        engine.set_parameter_smoothed(3, 0.5, render_frame=0)
        engine.play()
        engine.process([[0.1] * 128, [0.1] * 128])

        state = engine.transport_state()
        assert isinstance(state.playing, bool)
        assert state.playing
        assert state.bpm == pytest.approx(90.0)
        assert state.looping
        assert state.loop_end_ppq == pytest.approx(4.0)
        assert state.sample_rate == pytest.approx(48000.0)
        assert isinstance(state.sample_position, int)

        # Meter telemetry always drains to a list (possibly empty without a
        # configured meter tap).
        records = engine.drain_meter_telemetry()
        assert isinstance(records, list)


def test_engine_destroy_and_delete_aliases() -> None:
    engine = RealtimeEngine(sample_rate=48000.0, max_block_size=128)
    engine.destroy()
    # delete() is the second cross-binding alias and is safe to call again.
    engine.delete()
    with pytest.raises(RuntimeError):
        engine.play()


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
                default_curve=AutomationPointCurve.LINEAR,
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
                AutomationPoint(1.0, 6.0205999, AutomationPointCurve.LINEAR),
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


def test_realtime_engine_offline_render_matches_process() -> None:
    frames = 256
    left = [math.sin(i * 0.01) for i in range(frames)]
    right = [-sample for sample in left]

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as realtime:
        realtime.play()
        rt_left: list[float] = []
        rt_right: list[float] = []
        for offset in range(0, frames, 128):
            block = realtime.process([left[offset : offset + 128], right[offset : offset + 128]])
            rt_left.extend(block[0])
            rt_right.extend(block[1])

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as offline:
        offline.play()
        rendered = offline.render_offline([left, right], block_size=128)

    assert rendered[0] == rt_left
    assert rendered[1] == rt_right

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as bounce_engine:
        bounce_engine.play()
        bounced = bounce_engine.bounce_offline(
            EngineBounceOptions(
                total_frames=256,
                block_size=128,
                num_channels=2,
                source_sample_rate=48000,
                target_sample_rate=24000,
            )
        )

    assert bounced.frames == 128
    assert bounced.num_channels == 2
    assert bounced.sample_rate == 24000
    assert len(bounced.interleaved) == 256
    # Integrated LUFS is finite for audible material, or -inf (the LUFS floor)
    # for signals below the gating threshold (e.g. this very short bounce).
    assert math.isfinite(bounced.integrated_lufs) or bounced.integrated_lufs == float("-inf")

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as freeze_engine:
        freeze_engine.set_clips(
            [
                EngineClip(
                    id=7,
                    channels=[[0.125] * 128, [-0.25] * 128],
                    start_ppq=0.0,
                    length_samples=128,
                )
            ]
        )
        freeze_engine.play()
        frozen = freeze_engine.freeze_offline(
            EngineFreezeOptions(total_frames=128, block_size=128, num_channels=2, clip_id=77)
        )
        assert frozen.clip_id == 77
        assert frozen.frames == 128
        assert frozen.num_channels == 2
        assert freeze_engine.clip_count() == 1
        freeze_engine.seek_sample(0)
        frozen_render = freeze_engine.render_offline([[0.0] * 128, [0.0] * 128], block_size=128)

    assert frozen_render[0][0] == pytest.approx(0.125, abs=0.0001)
    assert frozen_render[1][0] == pytest.approx(-0.25, abs=0.0001)
