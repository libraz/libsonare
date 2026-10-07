"""Realtime engine mixer tests: track, bus and master strips, lanes, sends and monitoring."""

from __future__ import annotations

import json
import math
from pathlib import Path

import pytest

from libsonare import (
    ChannelLayout,
    EngineClip,
    EngineTrackMonitorMode,
    RealtimeEngine,
    SonareError,
)
from libsonare._engine_conversions import _track_monitor_mode_value

_PAN_LAW_CORPUS = json.loads(
    (Path(__file__).resolve().parents[3] / "tests/conformance/pan_law_names.json").read_text(
        encoding="utf-8"
    )
)


@pytest.mark.parametrize(
    ("value", "expected"),
    [
        (EngineTrackMonitorMode.OFF, 0),
        (EngineTrackMonitorMode.PFL, 1),
        (EngineTrackMonitorMode.AFL, 2),
        (0, 0),
        (1, 1),
        (2, 2),
        ("OFF", 0),
        ("pfl", 1),
        ("AfL", 2),
    ],
)
def test_track_monitor_mode_conversion(value: object, expected: int) -> None:
    assert _track_monitor_mode_value(value) == expected  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "value",
    [True, False, 1.0, 1.5, math.nan, math.inf, -1, 3, "cue", "pfl\N{SNOWMAN}"],
)
def test_track_monitor_mode_conversion_rejects_invalid_values(value: object) -> None:
    with pytest.raises(ValueError):
        _track_monitor_mode_value(value)  # type: ignore[arg-type]


def test_engine_track_monitor_mode_pfl_and_afl() -> None:
    from libsonare._runtime import _get_lib

    lib = _get_lib()
    # Presence is asserted for the whole suite by test_imports.py; a missing
    # symbol here is a broken build rather than an older library to tolerate.
    assert hasattr(lib, "sonare_engine_set_track_monitor_mode"), (
        "loaded libsonare is missing sonare_engine_set_track_monitor_mode"
    )

    block = 64
    frames = block * 16
    with RealtimeEngine(sample_rate=48000.0, max_block_size=block) as engine:
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
        engine.set_track_lanes([{"track_id": 10, "source_channel_layout": ChannelLayout.STEREO}])
        engine.set_track_strip_json(
            10,
            '{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}',
        )
        engine.play()
        engine.set_track_monitor_mode(0, "PFL", render_frame=block // 2)
        main, monitor = engine.process_with_monitor([[0.0] * block])
        assert main[0][-1] == pytest.approx(1.0, abs=0.01)
        assert monitor[0][0] == pytest.approx(0.0, abs=0.01)
        assert monitor[0][block // 2] == pytest.approx(1.0, abs=0.01)
        assert monitor[0][-1] == pytest.approx(1.0, abs=0.01)

        engine.set_parameter(0x4D580001, -6.0)
        for _ in range(9):
            main, monitor = engine.process_with_monitor([[0.0] * block])
        assert 0.4 < main[0][-1] < 0.7
        assert monitor[0][-1] == pytest.approx(1.0, abs=0.01)

        engine.set_track_monitor_mode(0, EngineTrackMonitorMode.AFL)
        main, monitor = engine.process_with_monitor([[0.0] * block])
        assert monitor[0][-1] == pytest.approx(main[0][-1], abs=0.01)
        assert 0.4 < monitor[0][-1] < 0.7


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

        # A surround bus/source layout flows through to the native struct fields;
        # an out-of-range value is rejected.
        engine.set_track_buses([{"bus_id": 1, "channel_layout": ChannelLayout.FIVE_POINT_ONE}])
        with pytest.raises(SonareError):
            engine.set_track_buses([{"bus_id": 1, "channel_layout": 99}])
        with pytest.raises(SonareError):
            engine.set_track_lanes([{"track_id": 10, "source_channel_layout": 99}])
        # Only stereo (or an omitted layout) is accepted for a lane.
        engine.set_track_lanes([{"track_id": 10, "source_channel_layout": ChannelLayout.STEREO}])
        engine.set_track_lanes([{"track_id": 10}])
        for layout in (
            ChannelLayout.MONO,
            ChannelLayout.FIVE_POINT_ONE,
            ChannelLayout.SEVEN_POINT_ONE,
        ):
            with pytest.raises(SonareError, match="must be stereo"):
                engine.set_track_lanes([{"track_id": 10, "source_channel_layout": layout}])

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
        # A 5.1 bus feeding a narrower (mono) output downmixes rather than
        # truncating to the front channels, so the unity-level send adds half
        # the direct level instead of doubling it.
        assert 2.12 < out[-1] < 2.13
        meter_targets = {record.target_id for record in engine.drain_meter_telemetry()}
        assert {0, 1, 33}.issubset(meter_targets)

        engine.set_track_lanes([{"track_id": 10, "sends": [{"bus_id": 1, "level_db": -6.0206}]}])
        engine.seek_sample(0)
        out = engine.process([[0.0] * 256])[0]
        assert 1.76 < out[-1] < 1.78

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


def test_realtime_engine_process_with_monitor_returns_separate_bus() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=16) as engine:
        output, monitor = engine.process_with_monitor([[0.25] * 16, [-0.25] * 16])
        assert output[0][0] == pytest.approx(0.25)
        assert output[1][0] == pytest.approx(-0.25)
        assert monitor[0][0] == pytest.approx(0.0)
        assert monitor[1][0] == pytest.approx(0.0)


def test_engine_track_strip_pan_and_send_setters() -> None:
    frames = 256 * 4
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[[1.0] * frames, [1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                )
            ]
        )
        engine.set_track_lanes([10])
        # A lane strip must exist before its pan can be set.
        engine.set_track_strip_json(
            10,
            '{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}',
        )

        # An unknown / zero track id is rejected by every setter.
        with pytest.raises(SonareError) as bad_pan_error:
            engine.set_track_strip_pan(0, -1.0)
        assert bad_pan_error.value.code == 4
        with pytest.raises(SonareError):
            engine.set_track_strip_pan_law(0, "linear")
        with pytest.raises(SonareError):
            engine.set_track_strip_pan_mode(0, "balance")
        with pytest.raises(SonareError):
            engine.set_track_strip_dual_pan(0, -1.0, 1.0)
        with pytest.raises(SonareError):
            engine.set_track_strip_channel_delay_samples(0, 8)

        # Pan law / mode accept enum names and ints; the helpers coerce them.
        engine.set_track_strip_pan_law(10, "linear")
        for case in _PAN_LAW_CORPUS["accepted"] + _PAN_LAW_CORPUS["normalization"]:
            engine.set_track_strip_pan_law(10, case["value"])
        for ordinal in _PAN_LAW_CORPUS["numeric"]:
            engine.set_track_strip_pan_law(10, ordinal)
        for value in _PAN_LAW_CORPUS["rejected"]:
            with pytest.raises(ValueError):
                engine.set_track_strip_pan_law(10, value)
        engine.set_track_strip_pan_mode(10, "stereo-pan")
        engine.set_track_strip_channel_delay_samples(10, 0)
        engine.set_track_strip_dual_pan(10, 0.0, 0.0)

        # Hard-left pan should leave the left channel louder than the right.
        engine.set_track_strip_pan(10, -1.0)
        engine.play()
        left, right = engine.process([[0.0] * 256, [0.0] * 256])
        left_rms = math.sqrt(sum(s * s for s in left) / len(left))
        right_rms = math.sqrt(sum(s * s for s in right) / len(right))
        assert left_rms > right_rms


def test_realtime_engine_dual_pan_routes_left_input_to_expected_output() -> None:
    block = 128
    frames = block * 4
    clip = EngineClip(
        id=1,
        track_id=10,
        channels=[[1.0] * frames, [0.0] * frames],
        start_ppq=0.0,
        length_samples=frames,
    )

    def energy(channel: list[float]) -> float:
        return sum(sample * sample for sample in channel)

    with RealtimeEngine(sample_rate=48000.0, max_block_size=block) as engine:
        engine.set_clips([clip])
        engine.set_track_lanes([10])
        engine.set_track_strip_json(
            10,
            '{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}',
        )
        engine.set_track_strip_pan_mode(10, "dual-pan")

        # A +1 left-input pan routes that input to the right output. Settling
        # removes the panner's normal smoothing ramp from this routing check.
        engine.set_track_strip_dual_pan(10, 1.0, -1.0)
        engine.settle_parameters()
        engine.play()
        right_routed = engine.process([[0.0] * block, [0.0] * block])
        assert energy(right_routed[0]) < 1e-6
        assert energy(right_routed[1]) > 1.0

        # Swapping the dual-pan positions reverses the same left-only source.
        engine.set_track_strip_dual_pan(10, -1.0, 1.0)
        engine.settle_parameters()
        left_routed = engine.process([[0.0] * block, [0.0] * block])
        assert energy(left_routed[0]) > 1.0
        assert energy(left_routed[1]) < 1e-6


def test_engine_bus_strip_pan_and_send_setters() -> None:
    frames = 256 * 4
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[[1.0] * frames, [1.0] * frames],
                    start_ppq=0.0,
                    length_samples=frames,
                )
            ]
        )
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        engine.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
        # A bus strip must exist before its pan can be set.
        engine.set_bus_strip_json(
            1,
            '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}',
        )

        # An unknown / zero bus id is rejected by every setter.
        with pytest.raises(SonareError) as bad_pan_error:
            engine.set_bus_strip_pan(0, -1.0)
        assert bad_pan_error.value.code == 4
        with pytest.raises(SonareError):
            engine.set_bus_strip_pan_law(0, "linear")
        with pytest.raises(SonareError):
            engine.set_bus_strip_pan_mode(0, "balance")
        with pytest.raises(SonareError):
            engine.set_bus_strip_dual_pan(0, -1.0, 1.0)
        with pytest.raises(SonareError) as unknown_bus_error:
            engine.set_bus_strip_pan(99, 0.0)
        assert unknown_bus_error.value.code == 4

        # Non-finite pan is refused outright rather than clamped.
        with pytest.raises(SonareError):
            engine.set_bus_strip_pan(1, math.nan)
        with pytest.raises(SonareError):
            engine.set_bus_strip_pan(1, math.inf)
        with pytest.raises(SonareError):
            engine.set_bus_strip_dual_pan(1, math.nan, 0.0)

        # Pan law / mode accept enum names and ints; the helpers coerce them.
        engine.set_bus_strip_pan_law(1, "linear")
        for case in _PAN_LAW_CORPUS["accepted"] + _PAN_LAW_CORPUS["normalization"]:
            engine.set_bus_strip_pan_law(1, case["value"])
        for ordinal in _PAN_LAW_CORPUS["numeric"]:
            engine.set_bus_strip_pan_law(1, ordinal)
        for value in _PAN_LAW_CORPUS["rejected"]:
            with pytest.raises(ValueError):
                engine.set_bus_strip_pan_law(1, value)
        engine.set_bus_strip_pan_mode(1, "stereo-pan")
        engine.set_bus_strip_dual_pan(1, 0.0, 0.0)

        # Hard-left pan should leave the left channel louder than the right.
        engine.set_bus_strip_pan(1, -1.0)
        engine.play()
        left, right = engine.process([[0.0] * 256, [0.0] * 256])
        left_rms = math.sqrt(sum(s * s for s in left) / len(left))
        right_rms = math.sqrt(sum(s * s for s in right) / len(right))
        assert left_rms > right_rms


def test_engine_bus_strip_pan_rejects_surround_bus() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        # A 5.1 bus rejects every pan setter outright (unknown and surround
        # buses share one generic failure), but keeps its EQ available.
        engine.set_track_buses([{"bus_id": 1, "channel_layout": ChannelLayout.FIVE_POINT_ONE}])
        engine.set_bus_strip_json(
            1,
            '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}',
        )
        with pytest.raises(SonareError) as surround_pan_error:
            engine.set_bus_strip_pan(1, 0.0)
        assert surround_pan_error.value.code == 4
        with pytest.raises(SonareError):
            engine.set_bus_strip_pan_law(1, "linear")
        with pytest.raises(SonareError):
            engine.set_bus_strip_pan_mode(1, "balance")
        with pytest.raises(SonareError):
            engine.set_bus_strip_dual_pan(1, -1.0, 1.0)
        engine.set_bus_strip_eq_band(1, 0, {"type": "Peak", "enabled": True})


def test_engine_bus_strip_eq_band_updates_embedded_eq() -> None:
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
        engine.set_track_buses(
            [{"bus_id": 1, "gain_db": 0.0, "channel_layout": ChannelLayout.MONO}]
        )
        engine.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
        engine.set_bus_strip_json(
            1,
            '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}',
        )
        with pytest.raises(SonareError) as bad_index_error:
            engine.set_bus_strip_eq_band(1, 99, {"type": "Peak", "enabled": True})
        assert bad_index_error.value.code == 4

        engine.play()
        flat_out = engine.process([[0.0] * 256])[0]
        engine.set_bus_strip_eq_band(
            1,
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


def test_engine_track_lane_accepts_send_timing_aliases() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        # A string timing, the snake_case ``send_timing`` alias, and an omitted
        # timing key are all accepted without error. The engine exposes no
        # readback for the effective send timing, so this only asserts that the
        # three input variants are configured successfully.
        engine.set_track_lanes(
            [
                {
                    "track_id": 10,
                    "sends": [
                        {"bus_id": 1, "level_db": 0.0, "timing": "pre-fader"},
                    ],
                },
                {
                    "track_id": 20,
                    "sends": [
                        {"bus_id": 1, "level_db": 0.0, "send_timing": 1},
                    ],
                },
                {
                    "track_id": 30,
                    "sends": [
                        {"bus_id": 1, "level_db": 0.0},
                    ],
                },
            ]
        )
