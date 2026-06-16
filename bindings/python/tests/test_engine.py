"""Tests for the realtime/offline DAW engine Python wrapper."""

from __future__ import annotations

import gc
import math

import numpy as np
import pytest

from libsonare import (
    AutomationCurve,
    AutomationPoint,
    BuiltinSynthConfig,
    ClipPageProvider,
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
    EngineMidiClipSchedule,
    EngineMidiEvent,
    EngineTelemetryError,
    EngineTelemetryType,
    FileClipPageProvider,
    MarkerKind,
    ParameterInfo,
    RealtimeEngine,
    SonareError,
    engine_abi_version,
    voice_changer_abi_version,
)


def test_engine_abi_version() -> None:
    assert engine_abi_version() > 0


def test_voice_changer_abi_version() -> None:
    """The RVC POD ABI version is exposed for parity with Node/WASM."""
    v = voice_changer_abi_version()
    assert isinstance(v, int)
    assert v > 0


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
                default_curve=AutomationCurve.LINEAR,
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


def test_engine_track_lanes_route_clips_and_lane_commands() -> None:
    frames = 256 * 10
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[[1.0] * frames, [1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                ),
                EngineClip(
                    id=2,
                    track_id=20,
                    channels=[[1.0] * frames, [1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                ),
            ]
        )
        engine.set_track_lanes([10, 20])
        with pytest.raises(SonareError) as duplicate_lane_error:
            engine.set_track_lanes([10, 10])
        assert duplicate_lane_error.value.code == 4
        engine.set_track_lanes([10, 20])

        engine.play()
        processed = engine.process([[0.0] * 256, [0.0] * 256])
        assert processed[0][-1] == pytest.approx(2.0)
        assert processed[1][-1] == pytest.approx(2.0)

        engine.set_solo_mute(0, solo=True, mute=False)
        for _ in range(4):
            processed = engine.process([[0.0] * 256, [0.0] * 256])
        assert 0.75 < processed[0][-1] < 1.25

        engine.set_parameter_smoothed(0x4D580001, -12.0, render_frame=-1)
        for _ in range(6):
            processed = engine.process([[0.0] * 256, [0.0] * 256])
        assert processed[0][-1] < 0.45
        assert processed[1][-1] < 0.45


def test_engine_track_buses_route_lane_sends() -> None:
    frames = 256 * 40
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[[1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                )
            ]
        )
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        with pytest.raises(SonareError) as duplicate_bus_error:
            engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}, {"bus_id": 1, "gain_db": 0.0}])
        assert duplicate_bus_error.value.code == 4

        engine.set_track_lanes([{"track_id": 10, "sends": [{"bus_id": 1, "level_db": 0.0}]}])
        bad_lanes = [
            {"track_id": 10, "sends": [{"bus_id": 99, "level_db": 0.0}]},
            {
                "track_id": 10,
                "sends": [
                    {"bus_id": 1, "level_db": 0.0},
                    {"bus_id": 1, "level_db": -6.0},
                ],
            },
            {"track_id": 10, "sends": [{"bus_id": 1, "level_db": 99.0}]},
        ]
        for lane in bad_lanes:
            with pytest.raises(SonareError) as lane_error:
                engine.set_track_lanes([lane])
            assert lane_error.value.code == 4

        engine.play()
        out = engine.process([[0.0] * 256])[0]
        assert 2.82 < out[-1] < 2.84
        meter_targets = {record.target_id for record in engine.drain_meter_telemetry()}
        assert {0, 1, 33}.issubset(meter_targets)

        engine.set_track_lanes([{"track_id": 10, "sends": [{"bus_id": 1, "level_db": -6.0206}]}])
        engine.seek_sample(0)
        out = engine.process([[0.0] * 256])[0]
        assert 2.11 < out[-1] < 2.13

        engine.set_track_lanes(
            [{"track_id": 10, "sends": [{"bus_id": 1, "level_db": 0.0, "enabled": False}]}]
        )
        engine.seek_sample(0)
        out = engine.process([[0.0] * 256])[0]
        assert 1.41 < out[-1] < 1.42

        with pytest.raises(SonareError) as bad_json_error:
            engine.set_bus_strip_json(1, "{bad json")
        assert bad_json_error.value.code == 2
        engine.set_bus_strip_json(
            1,
            '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}',
        )


def test_engine_track_strip_json_routes_lane_strip() -> None:
    frames = 256 * 4
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[[1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                ),
                EngineClip(
                    id=2,
                    track_id=20,
                    channels=[[1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                ),
            ]
        )
        engine.set_track_lanes([10, 20])
        scene_json = (
            '{"version":1,"strips":[{"id":"track-10","faderDb":-12,"panLaw":3}],'
            '"buses":[],"connections":[]}'
        )
        engine.set_track_strip_json(10, scene_json)
        with pytest.raises(SonareError) as bad_json_error:
            engine.set_track_strip_json(10, "{bad json")
        assert bad_json_error.value.code == 2
        with pytest.raises(SonareError) as bad_processor_error:
            engine.set_track_strip_json(
                10,
                '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
                '"processor":"missing.processor","params":"{}"}]}],"buses":[],"connections":[]}',
            )
        assert bad_processor_error.value.code == 4
        with pytest.raises(SonareError) as bad_param_error:
            engine.set_track_strip_json(
                10,
                '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
                '"processor":"eq.parametric","params":"{\\"band0.gainDb\\":\\"loud\\"}"}]}],'
                '"buses":[],"connections":[]}',
            )
        assert bad_param_error.value.code == 4

        engine.play()
        processed = engine.process([[0.0] * 256])
        assert 1.20 < processed[0][-1] < 1.40


def test_engine_track_strip_insert_bypass_toggles_insert() -> None:
    frames = 256 * 16
    source = [math.sin(2.0 * math.pi * 1000.0 * i / 48000.0) for i in range(frames)]
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[source],
                    start_ppq=0.0,
                    length_samples=frames,
                )
            ]
        )
        engine.set_track_lanes([10])
        engine.set_track_strip_json(
            10,
            '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
            '"processor":"eq.parametric","params":"{\\"band0.type\\":1,'
            '\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":12,\\"band0.enabled\\":1}"}]}],'
            '"buses":[],"connections":[]}',
        )
        with pytest.raises(SonareError) as bad_index_error:
            engine.set_track_strip_insert_bypassed(10, 7, True)
        assert bad_index_error.value.code == 4

        engine.play()
        eq_out = [0.0] * 256
        for _ in range(6):
            eq_out = engine.process([[0.0] * 256])[0]
        engine.set_track_strip_insert_bypassed(10, 0, True, True)
        engine.seek_sample(0)
        bypassed_out = engine.process([[0.0] * 256])[0]
        assert math.sqrt(sum(sample * sample for sample in eq_out) / len(eq_out)) > (
            math.sqrt(sum(sample * sample for sample in bypassed_out) / len(bypassed_out)) * 1.5
        )


def test_engine_track_strip_eq_band_updates_embedded_eq() -> None:
    frames = 256 * 16
    source = [math.sin(2.0 * math.pi * 1000.0 * i / 48000.0) for i in range(frames)]
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[source],
                    start_ppq=0.0,
                    length_samples=frames,
                )
            ]
        )
        engine.set_track_lanes([10])
        engine.set_track_strip_json(
            10,
            '{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}',
        )
        with pytest.raises(SonareError) as bad_index_error:
            engine.set_track_strip_eq_band(10, 99, {"type": "Peak", "enabled": True})
        assert bad_index_error.value.code == 4

        engine.play()
        flat_out = engine.process([[0.0] * 256])[0]
        engine.set_track_strip_eq_band(
            10,
            0,
            {
                "type": "Peak",
                "frequencyHz": 1000,
                "gainDb": 12,
                "q": 1,
                "enabled": True,
            },
        )
        engine.seek_sample(0)
        eq_out = [0.0] * 256
        for _ in range(6):
            eq_out = engine.process([[0.0] * 256])[0]
        assert math.sqrt(sum(sample * sample for sample in eq_out) / len(eq_out)) > (
            math.sqrt(sum(sample * sample for sample in flat_out) / len(flat_out)) * 1.5
        )


def test_engine_master_strip_json_routes_master_strip() -> None:
    frames = 256 * 16
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    channels=[[1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                ),
                EngineClip(
                    id=2,
                    channels=[[1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                ),
            ]
        )
        scene_json = (
            '{"version":1,"strips":[{"id":"master","faderDb":-12,"panLaw":3}],'
            '"buses":[],"connections":[]}'
        )
        engine.set_master_strip_json(scene_json)
        with pytest.raises(SonareError) as bad_json_error:
            engine.set_master_strip_json("{bad json")
        assert bad_json_error.value.code == 2
        with pytest.raises(SonareError) as bad_param_error:
            engine.set_master_strip_json(
                '{"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre",'
                '"processor":"eq.parametric","params":"{\\"band0.gainDb\\":\\"loud\\"}"}]}],'
                '"buses":[],"connections":[]}'
            )
        assert bad_param_error.value.code == 4
        with pytest.raises(SonareError) as bad_bypass_error:
            engine.set_master_strip_insert_bypassed(0, True)
        assert bad_bypass_error.value.code == 4

        engine.play()
        processed = engine.process([[0.0] * 256])
        assert 0.65 < processed[0][-1] < 0.80
        engine.set_parameter_smoothed(0x4D58FF01, -24.0)
        engine.set_parameter(0x4D58FF02, 0.25)
        attenuated = processed
        for _ in range(8):
            attenuated = engine.process([[0.0] * 256])
        assert 0.05 < attenuated[0][-1] < 0.25


def test_engine_master_strip_eq_band_updates_embedded_eq() -> None:
    frames = 256 * 16
    source = [math.sin(2.0 * math.pi * 1000.0 * i / 48000.0) for i in range(frames)]
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    channels=[source],
                    start_ppq=0.0,
                    length_samples=frames,
                )
            ]
        )
        engine.set_master_strip_json(
            '{"version":1,"strips":[{"id":"master"}],"buses":[],"connections":[]}'
        )
        with pytest.raises(SonareError) as bad_index_error:
            engine.set_master_strip_eq_band(99, {"type": "Peak", "enabled": True})
        assert bad_index_error.value.code == 4

        engine.play()
        flat_out = engine.process([[0.0] * 256])[0]
        engine.set_master_strip_eq_band(
            0,
            {
                "type": "Peak",
                "frequencyHz": 1000,
                "gainDb": 12,
                "q": 1,
                "enabled": True,
            },
        )
        engine.seek_sample(0)
        eq_out = [0.0] * 256
        for _ in range(6):
            eq_out = engine.process([[0.0] * 256])[0]
        assert math.sqrt(sum(sample * sample for sample in eq_out) / len(eq_out)) > (
            math.sqrt(sum(sample * sample for sample in flat_out) / len(flat_out)) * 1.5
        )


def test_engine_destroy_and_delete_aliases() -> None:
    engine = RealtimeEngine(sample_rate=48000.0, max_block_size=128)
    engine.destroy()
    # delete() is the second cross-binding alias and is safe to call again.
    engine.delete()
    with pytest.raises(RuntimeError):
        engine.play()


def test_realtime_engine_process_with_monitor_returns_separate_bus() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=16) as engine:
        output, monitor = engine.process_with_monitor([[0.25] * 16, [-0.25] * 16])
        assert output[0][0] == pytest.approx(0.25)
        assert output[1][0] == pytest.approx(-0.25)
        assert monitor[0][0] == pytest.approx(0.0)
        assert monitor[1][0] == pytest.approx(0.0)


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


def test_engine_marker_kind_and_key_signature_round_trip() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_markers(
            [
                EngineMarker(1, 0.0, "verse", kind=MarkerKind.CUE_POINT),
                EngineMarker(
                    2,
                    4.0,
                    "G major",
                    kind=MarkerKind.KEY_SIGNATURE,
                    key_fifths=1,
                    key_minor=False,
                ),
                EngineMarker(
                    3,
                    8.0,
                    "C minor",
                    kind=MarkerKind.KEY_SIGNATURE,
                    key_fifths=-3,
                    key_minor=True,
                ),
            ]
        )
        assert engine.marker_count() == 3

        cue = engine.marker_by_index(0)
        assert cue.kind == MarkerKind.CUE_POINT
        assert cue.name == "verse"
        assert cue.key_fifths == 0
        assert cue.key_minor is False

        major = engine.marker(2)
        assert major.kind == MarkerKind.KEY_SIGNATURE
        assert major.key_fifths == 1
        assert major.key_minor is False

        minor = engine.marker(3)
        assert minor.kind == MarkerKind.KEY_SIGNATURE
        assert minor.key_fifths == -3
        assert minor.key_minor is True


def test_engine_streams_paged_clip_provider_and_drains_requests() -> None:
    with (
        RealtimeEngine(sample_rate=48000.0, max_block_size=8) as engine,
        ClipPageProvider(1, 8, 4) as provider,
    ):
        provider.supply(0, [[1.0, 2.0, 3.0, 4.0]])
        engine.set_clips(
            [
                EngineClip(
                    id=123,
                    channels=None,
                    start_ppq=0.0,
                    page_provider=provider,
                )
            ]
        )
        engine.play()
        first = engine.process([[0.0] * 8])
        assert first[0] == [1.0, 2.0, 3.0, 4.0, 0.0, 0.0, 0.0, 0.0]

        request = engine.pop_clip_page_request()
        assert request is not None
        assert request.clip_id == 123
        assert request.channel == 0
        assert request.sample == 4
        assert any(
            record.type == EngineTelemetryType.ERROR
            and record.error == EngineTelemetryError.CLIP_PAGE_UNDERRUN
            and record.value == 123
            for record in engine.drain_telemetry()
        )

        provider.supply(1, [[5.0, 6.0, 7.0, 8.0]])
        engine.seek_sample(0)
        second = engine.process([[0.0] * 8])
        assert second[0] == [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]


def test_engine_feeds_paged_clips_from_raw_float32_files(tmp_path) -> None:
    raw_path = tmp_path / "clip.f32"
    np.asarray([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0], dtype="<f4").tofile(raw_path)

    with (
        RealtimeEngine(sample_rate=48000.0, max_block_size=8) as engine,
        FileClipPageProvider(raw_path, num_channels=1, num_samples=8, page_frames=4) as provider,
    ):
        assert provider.supply_page(0) is True
        engine.set_clips(
            [
                EngineClip(
                    id=124,
                    channels=None,
                    start_ppq=0.0,
                    page_provider=provider,
                )
            ]
        )
        engine.play()
        first = engine.process([[0.0] * 8])
        assert first[0] == [1.0, 2.0, 3.0, 4.0, 0.0, 0.0, 0.0, 0.0]

        request = engine.pop_clip_page_request()
        assert request is not None
        assert request.clip_id == 124
        assert request.sample == 4
        assert provider.supply_request(request) is True
        engine.seek_sample(0)
        second = engine.process([[0.0] * 8])
        assert second[0] == [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]


def test_clip_page_provider_failed_construction_does_not_emit_del_noise(capsys) -> None:
    with pytest.raises(SonareError):
        ClipPageProvider(0, 0, 0)
    gc.collect()
    assert "Exception ignored in" not in capsys.readouterr().err


def test_file_clip_page_provider_failed_open_cleans_up_without_del_noise(tmp_path, capsys) -> None:
    missing = tmp_path / "missing.f32"
    with pytest.raises(FileNotFoundError):
        FileClipPageProvider(missing, num_channels=1, num_samples=8, page_frames=4)
    gc.collect()
    assert "Exception ignored in" not in capsys.readouterr().err


def test_file_clip_page_provider_truncated_page_returns_false(tmp_path) -> None:
    raw_path = tmp_path / "truncated.f32"
    np.asarray([1.0, 2.0, 3.0, 4.0, 5.0], dtype="<f4").tofile(raw_path)

    with FileClipPageProvider(raw_path, num_channels=1, num_samples=8, page_frames=4) as provider:
        assert provider.supply_page(0) is True
        assert provider.supply_page(1) is False


def test_realtime_engine_renders_repitch_warped_clip() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=4) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=303,
                    channels=[[0.0, 10.0, 20.0, 30.0]],
                    start_ppq=0.0,
                    length_samples=4,
                    warp_mode="repitch",
                    warp_anchors=[(0.0, 0.0), (3.0, 1.5)],
                )
            ]
        )
        engine.play()
        processed = engine.process([[0.0] * 4])
        assert math.isclose(processed[0][0], 0.0, abs_tol=0.0001)
        assert math.isclose(processed[0][1], 5.0, abs_tol=0.0001)
        assert math.isclose(processed[0][2], 10.0, abs_tol=0.0001)
        assert math.isclose(processed[0][3], 15.0, abs_tol=0.0001)
    tempo_source = [math.sin(i * 0.02) for i in range(4096)]
    with RealtimeEngine(sample_rate=48000.0, max_block_size=8192) as tempo_engine:
        tempo_engine.set_clips(
            [
                EngineClip(
                    id=304,
                    channels=[tempo_source],
                    start_ppq=0.0,
                    length_samples=8192,
                    warp_mode="tempo-sync",
                    warp_anchors=[
                        (0.0, 0.0),
                        (2048.0, 1024.0),
                        (8192.0, 4096.0),
                    ],
                )
            ]
        )
        tempo_engine.play()
        tempo_synced = tempo_engine.process([[0.0] * 8192])
        assert max(abs(sample) for sample in tempo_synced[0]) > 0.1


def test_realtime_engine_warp_mode_validation_matches_project_helper() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=4) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=305,
                    channels=[[0.0, 10.0, 20.0, 30.0]],
                    start_ppq=0.0,
                    length_samples=4,
                    warp_mode="Repitch",
                    warp_anchors=[(0.0, 0.0), (3.0, 1.5)],
                )
            ]
        )
        engine.play()
        processed = engine.process([[0.0] * 4])
        assert math.isclose(processed[0][1], 5.0, abs_tol=0.0001)

    with RealtimeEngine(sample_rate=48000.0, max_block_size=4) as engine:
        for mode in ("typo", 1.9, True):
            with pytest.raises(ValueError, match="unknown warp mode"):
                engine.set_clips(
                    [
                        EngineClip(
                            id=306,
                            channels=[[0.0, 10.0, 20.0, 30.0]],
                            start_ppq=0.0,
                            length_samples=4,
                            warp_mode=mode,  # type: ignore[arg-type]
                        )
                    ]
                )


def test_realtime_engine_process_zero_copy_matches_list_input() -> None:
    """The zero-copy ``_channel_arrays`` marshalling preserves process() output.

    A numpy float32 input and the equivalent Python-list input must yield the
    same result, and the caller's input array must not be mutated in-place.
    """
    left = [math.sin(i * 0.013) for i in range(128)]
    right = [-sample for sample in left]
    np_left = np.asarray(left, dtype=np.float32)
    np_right = np.asarray(right, dtype=np.float32)
    np_left_before = np_left.copy()

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as list_engine:
        list_engine.play()
        list_out = list_engine.process([left, right])

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as np_engine:
        np_engine.play()
        np_out = np_engine.process([np_left, np_right])

    assert np_out[0] == pytest.approx(list_out[0])
    assert np_out[1] == pytest.approx(list_out[1])
    # The input buffer is copied, never aliased: the engine writes its output
    # into an internal buffer, leaving the caller's array untouched.
    assert np.array_equal(np_left, np_left_before)


def test_bounce_options_default_seeded_from_native_layer() -> None:
    """Leaving ``target_lufs`` at its sentinel still tracks the C default.

    The C ``sonare_engine_bounce_options_default`` maps ``target_lufs == 0.0``
    to the -14 LUFS default; the Python wrapper seeds the raw options from it,
    so a bounce request runs without raising regardless of the dataclass copy.
    """
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.play()
        bounced = engine.bounce_offline(
            EngineBounceOptions(
                total_frames=128,
                block_size=128,
                num_channels=2,
                source_sample_rate=48000,
                target_sample_rate=48000,
                normalize_lufs=True,
            )
        )
    assert bounced.num_channels == 2
    assert bounced.frames == 128
    assert bounced.sample_rate == 48000


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


# ---------------------------------------------------------------------------
# Live-MIDI parity surface (built-in instrument bind, CC bindings, queued
# input source, immediate note/CC injection) added for Node/WASM parity.
# ---------------------------------------------------------------------------


def test_engine_builtin_instrument_bind_and_clear() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(waveform="saw", gain=0.5), 0)
        assert engine.midi_instrument_count() == 1
        engine.set_builtin_instrument()  # default sine patch on destination 0
        assert engine.midi_instrument_count() == 1
        engine.clear_midi_instrument(0)
        assert engine.midi_instrument_count() == 0


def test_engine_midi_cc_bindings() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.bind_midi_cc(0, 1, 42, min_value=0.0, max_value=1.0)
        assert engine.midi_cc_binding_count() == 1
        engine.clear_midi_cc_bindings()
        assert engine.midi_cc_binding_count() == 0


def test_engine_live_midi_input_source_queue() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        engine.set_midi_input_source(0)
        engine.push_midi_input_note_on(0, 0, 60, 100, 0)
        engine.push_midi_input_cc(0, 0, 1, 64, 0)
        engine.push_midi_input_note_off(0, 0, 60, 0, 0)
        assert engine.midi_input_pending_count() == 3
        engine.clear_midi_input_source()


def test_engine_push_immediate_notes_do_not_raise() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        engine.push_midi_note_on(0, 0, 0, 60, 100)
        engine.push_midi_note_off(0, 0, 0, 60, 0)


def test_engine_scheduled_midi_clips_render_builtin_instrument() -> None:
    def midi1_word(status: int, channel: int, data0: int, data1: int) -> int:
        return (0x2 << 28) | ((status & 0xF) << 20) | ((channel & 0xF) << 16) | (data0 << 8) | data1

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(gain=0.5), 6)
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=1,
                    track_id=6,
                    destination_id=6,
                    length_samples=8192,
                    events=[
                        EngineMidiEvent(0, word0=midi1_word(0x9, 0, 60, 100), word_count=1),
                        EngineMidiEvent(4096, word0=midi1_word(0x8, 0, 60, 0), word_count=1),
                    ],
                )
            ]
        )
        engine.play()
        out = np.asarray(engine.process([[0.0] * 128, [0.0] * 128]))
        assert float(np.max(np.abs(out))) > 0.0
        with pytest.raises(SonareError) as bad_group_error:
            engine.set_midi_clips(
                [
                    EngineMidiClipSchedule(
                        id=2,
                        track_id=6,
                        destination_id=6,
                        events=[
                            EngineMidiEvent(
                                0,
                                word0=midi1_word(0x9, 0, 60, 100),
                                word_count=1,
                                group=16,
                            )
                        ],
                    )
                ]
            )
        assert bad_group_error.value.code == 4
        with pytest.raises(SonareError) as bad_channel_error:
            engine.push_midi_note_on(6, 0, 16, 60, 100)
        assert bad_channel_error.value.code == 4
        with pytest.raises(SonareError) as bad_soundfont_error:
            engine.load_soundfont(b"not sf2")
        assert bad_soundfont_error.value.code == 2
        engine.set_midi_clips([])
