"""Realtime engine mixer tests: track, bus and master strips, lanes, sends and monitoring."""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import pytest

from libsonare import (
    MAX_TRACK_LANES,
    AutomationPoint,
    EngineClip,
    EngineProcessWithMonitorResult,
    ErrorCode,
    Project,
    RealtimeEngine,
    SonareError,
    SonareValueError,
)
from libsonare._engine_conversions import _track_monitor_mode_value

from ._helpers import raises_coded, scene_bus, scene_strip

_PAN_LAW_CORPUS = json.loads(
    (Path(__file__).resolve().parents[3] / "tests/conformance/pan_law_names.json").read_text(
        encoding="utf-8"
    )
)

_SR = 48000
_BLOCK = 256
_SMOOTHING_MS = 20.0
# A smoothed set ramps over the engine smoother's time and then through each reserved
# target's fixed 5 ms click guard (sonare_engine_set_param_smoothing_ms).
_CLICK_GUARD_MS = 5.0


def _blocks_to_settle(start: float, target: float, tolerance: float) -> int:
    """Blocks after which a smoothed set from ``start`` is within ``tolerance`` of ``target``."""
    settle_ms = (_SMOOTHING_MS + _CLICK_GUARD_MS) * math.log(abs(start - target) / tolerance)
    return math.ceil(settle_ms * 1e-3 * _SR / _BLOCK) + 1


def test_track_monitor_mode_tokens_map_to_their_ordinals() -> None:
    assert [_track_monitor_mode_value(mode, "mode") for mode in ("off", "pfl", "afl")] == [0, 1, 2]


@pytest.mark.parametrize("value", ["OFF", "Pfl", "cue", "pfl\N{SNOWMAN}"])
def test_another_track_monitor_mode_spelling_is_refused(value: str) -> None:
    with pytest.raises(SonareValueError, match="afl"):
        _track_monitor_mode_value(value, "mode")  # type: ignore[arg-type]


@pytest.mark.parametrize("value", [True, 0, 1, 1.0, math.nan])
def test_a_track_monitor_mode_number_is_a_type_error(value: object) -> None:
    with pytest.raises(TypeError):
        _track_monitor_mode_value(value, "mode")  # type: ignore[arg-type]


def test_engine_track_monitor_mode_pfl_and_afl() -> None:
    block = 64
    frames = block * 16
    with RealtimeEngine(sample_rate=48000, max_block_size=block) as engine:
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
        engine.set_track_lanes([{"track_id": 10, "source_channel_layout": "stereo"}])
        engine.set_strip(
            10,
            scene_strip('{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}'),
        )
        engine.play()
        engine.set_track_monitor_mode(10, "pfl", render_frame=block // 2)
        processed = engine.process([[0.0] * block], with_monitor=True)
        main, monitor = processed.output, processed.monitor
        assert main[0][-1] == pytest.approx(1.0, abs=0.01)
        assert monitor[0][0] == pytest.approx(0.0, abs=0.01)
        assert monitor[0][block // 2] == pytest.approx(1.0, abs=0.01)
        assert monitor[0][-1] == pytest.approx(1.0, abs=0.01)

        engine.set_parameter(engine.resolve_strip_automation_id(10, "faderDb"), -6.0)
        for _ in range(9):
            processed = engine.process([[0.0] * block], with_monitor=True)
            main, monitor = processed.output, processed.monitor
        assert 0.4 < main[0][-1] < 0.7
        assert monitor[0][-1] == pytest.approx(1.0, abs=0.01)

        engine.set_track_monitor_mode(10, "afl")
        processed = engine.process([[0.0] * block], with_monitor=True)
        main, monitor = processed.output, processed.monitor
        assert monitor[0][-1] == pytest.approx(main[0][-1], abs=0.01)
        assert 0.4 < monitor[0][-1] < 0.7


def test_engine_track_lanes_route_clips_and_lane_commands() -> None:
    frames = 256 * 10
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        engine.set_track_lanes([{"track_id": 10}, {"track_id": 20}])
        with pytest.raises(SonareValueError) as duplicate_lane_error:
            engine.set_track_lanes([{"track_id": 10}, {"track_id": 10}])
        assert duplicate_lane_error.value.code == 4
        engine.set_track_lanes([{"track_id": 10}, {"track_id": 20}])

        engine.play()
        processed = engine.process([[0.0] * 256, [0.0] * 256])
        assert processed[0][-1] == pytest.approx(2.0)
        assert processed[1][-1] == pytest.approx(2.0)

        engine.set_track_solo_mute(10, solo=True, mute=False)
        for _ in range(4):
            processed = engine.process([[0.0] * 256, [0.0] * 256])
        assert 0.75 < processed[0][-1] < 1.25

        fader = engine.resolve_strip_automation_id(10, "faderDb")
        engine.set_parameter(fader, -12.0, smoothed=True)
        for _ in range(6):
            processed = engine.process([[0.0] * 256, [0.0] * 256])
        assert processed[0][-1] < 0.45
        assert processed[1][-1] < 0.45


def test_engine_track_buses_route_lane_sends() -> None:
    frames = 256 * 40
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        with pytest.raises(SonareValueError) as duplicate_bus_error:
            engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}, {"bus_id": 1, "gain_db": 0.0}])
        assert duplicate_bus_error.value.code == 4

        # A surround bus/source layout flows through to the native struct fields;
        # an ordinal or an unknown token is rejected.
        engine.set_track_buses([{"bus_id": 1, "channel_layout": "5.1"}])
        with pytest.raises(TypeError):
            engine.set_track_buses([{"bus_id": 1, "channel_layout": 2}])
        with pytest.raises(SonareValueError, match="5.1"):
            engine.set_track_buses([{"bus_id": 1, "channel_layout": "quad"}])
        with pytest.raises(TypeError):
            engine.set_track_lanes([{"track_id": 10, "source_channel_layout": 1}])
        # Only stereo (or an omitted layout) is accepted for a lane.
        engine.set_track_lanes([{"track_id": 10, "source_channel_layout": "stereo"}])
        engine.set_track_lanes([{"track_id": 10}])
        for layout in (
            "mono",
            "5.1",
            "7.1",
        ):
            with pytest.raises(
                SonareError,
                match=r"^\[6\] RealtimeEngine\.set_track_lanes: lanes\[0\]\.source_channel_layout "
                r"other than stereo is not supported$",
            ):
                engine.set_track_lanes([{"track_id": 10, "source_channel_layout": layout}])

        assert MAX_TRACK_LANES == 32
        engine.set_track_lanes([{"track_id": i} for i in range(1, MAX_TRACK_LANES + 1)])
        with pytest.raises(
            SonareError,
            match=(
                r"^RealtimeEngine\.set_track_lanes: lanes length must be an integer in \[0, 32\]$"
            ),
        ):
            engine.set_track_lanes([{"track_id": i} for i in range(1, MAX_TRACK_LANES + 2)])

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
            with pytest.raises(SonareValueError) as lane_error:
                engine.set_track_lanes([lane])
            assert lane_error.value.code == 4

        engine.play()
        out = engine.process([[0.0] * 256])[0]
        # A 5.1 bus feeding a narrower (mono) output downmixes rather than
        # truncating to the front channels, so the unity-level send adds half
        # the direct level instead of doubling it.
        # Direct 1 + half of the unity send.
        assert 1.49 < out[-1] < 1.51
        meter_targets = {record.target_id for record in engine.drain_meter_telemetry()}
        assert {0, 1, 33}.issubset(meter_targets)

        engine.set_track_lanes([{"track_id": 10, "sends": [{"bus_id": 1, "level_db": -6.0206}]}])
        engine.seek_sample(0)
        out = engine.process([[0.0] * 256])[0]
        # 1 + 0.5 * 10^(-6.0206/20).
        assert 1.24 < out[-1] < 1.26

        engine.set_track_lanes(
            [{"track_id": 10, "sends": [{"bus_id": 1, "level_db": 0.0, "enabled": False}]}]
        )
        engine.seek_sample(0)
        out = engine.process([[0.0] * 256])[0]
        assert 0.99 < out[-1] < 1.01

        with pytest.raises(TypeError, match="json.loads"):
            engine.set_bus_strip(1, "{bad json")  # type: ignore[arg-type]
        engine.set_bus_strip(
            1,
            scene_bus(
                '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}'
            ),
        )


def test_engine_set_strip_routes_a_track_strip() -> None:
    frames = 256 * 4
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        engine.set_track_lanes([{"track_id": 10}, {"track_id": 20}])
        scene_json = (
            '{"version":1,"strips":[{"id":"track-10","faderDb":-12,"panLaw":"linear0Db"}],'
            '"buses":[],"connections":[]}'
        )
        engine.set_strip(10, scene_strip(scene_json))
        with pytest.raises(TypeError, match="json.loads"):
            engine.set_strip(10, "{bad json")  # type: ignore[arg-type]
        with pytest.raises(SonareValueError) as bad_processor_error:
            engine.set_strip(
                10,
                scene_strip(
                    '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
                    '"processor":"missing.processor","params":{}}]}],"buses":[],"connections":[]}'
                ),
            )
        assert bad_processor_error.value.code == 4
        with pytest.raises(SonareValueError) as bad_param_error:
            engine.set_strip(
                10,
                scene_strip(
                    '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
                    '"processor":"eq.parametric","params":{"band0.gainDb":"loud"}}]}],'
                    '"buses":[],"connections":[]}'
                ),
            )
        assert bad_param_error.value.code == 4

        engine.play()
        processed = engine.process([[0.0] * 256])
        assert 1.20 < processed[0][-1] < 1.40


_STRIP = '{"version":1,"strips":[{"id":"track-10",%s}],"buses":[],"connections":[]}'


@pytest.mark.parametrize(
    ("strip_fields", "current"),
    [
        ('"inserts":[{"slot":"pre","processor":"eq.parametric","params":"{}"}]', "params"),
        ('"fader_db":-3', "faderDb"),
        ('"panLaw":1', "const3Db"),
        ('"sends":[{"destinationBusId":"1","timing":"pre"}]', "preFader"),
    ],
)
def test_strip_scene_refuses_earlier_spellings(strip_fields: str, current: str) -> None:
    """Earlier spellings inside the strip settings are refused as a refused argument."""
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        with raises_coded(int(ErrorCode.INVALID_PARAMETER), match=current):
            engine.set_strip(10, scene_strip(_STRIP % strip_fields))


def test_engine_track_strip_eq_insert_is_bounded_by_the_engine_rate_nyquist() -> None:
    def strip_json(frequency_hz: int) -> str:
        return (
            '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
            '"processor":"eq.parametric","params":{"band0.frequencyHz":'
            + str(frequency_hz)
            + '}}]}],"buses":[],"connections":[]}'
        )

    with RealtimeEngine(sample_rate=96000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_strip(10, scene_strip(strip_json(30000)))
        with pytest.raises(SonareError):
            engine.set_strip(10, scene_strip(strip_json(48000)))
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        with pytest.raises(SonareError):
            engine.set_strip(10, scene_strip(strip_json(30000)))
        engine.set_strip(10, scene_strip(strip_json(20000)))


def test_engine_track_strip_insert_bypass_toggles_insert() -> None:
    frames = 256 * 16
    source = [math.sin(2.0 * math.pi * 1000.0 * i / 48000.0) for i in range(frames)]
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_strip(
            10,
            scene_strip(
                '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
                '"processor":"eq.parametric","params":{"band0.type":1,'
                '"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1}}]}],'
                '"buses":[],"connections":[]}'
            ),
        )
        with pytest.raises(SonareValueError) as bad_index_error:
            engine.set_strip_insert_bypassed(10, 7, True)
        assert bad_index_error.value.code == 4

        engine.play()
        eq_out = [0.0] * 256
        for _ in range(6):
            eq_out = engine.process([[0.0] * 256])[0]
        engine.set_strip_insert_bypassed(10, 0, True, True)
        engine.seek_sample(0)
        bypassed_out = engine.process([[0.0] * 256])[0]
        assert math.sqrt(sum(sample * sample for sample in eq_out) / len(eq_out)) > (
            math.sqrt(sum(sample * sample for sample in bypassed_out) / len(bypassed_out)) * 1.5
        )


def test_engine_track_strip_eq_band_updates_embedded_eq() -> None:
    frames = 256 * 16
    source = [math.sin(2.0 * math.pi * 1000.0 * i / 48000.0) for i in range(frames)]
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_strip(
            10,
            scene_strip('{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}'),
        )
        with pytest.raises(SonareValueError) as bad_index_error:
            engine.set_strip_eq_band(10, 99, {"type": "peak", "enabled": True})
        assert bad_index_error.value.code == 4

        engine.play()
        flat_out = engine.process([[0.0] * 256])[0]
        engine.set_strip_eq_band(
            10,
            0,
            {
                "type": "peak",
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


def test_engine_set_strip_routes_the_master_strip() -> None:
    before = 2 * 10 ** (-12 / 20)
    # 2 * 10^(-24/20) * sqrt((0.75^2 + 1) / 2): the linear law at pan 0.25 against its centre.
    after = 2 * 10 ** (-24 / 20) * (1.5625 / 2) ** 0.5
    settle_blocks = _blocks_to_settle(before, after, 5e-4)
    frames = _BLOCK * (settle_blocks + 2)
    with RealtimeEngine(sample_rate=_SR, max_block_size=_BLOCK) as engine:
        engine.set_param_smoothing_ms(_SMOOTHING_MS)
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
            '{"version":1,"strips":[{"id":"master","faderDb":-12,"panLaw":"linear0Db"}],'
            '"buses":[],"connections":[]}'
        )
        engine.set_strip("master", scene_strip(scene_json))
        with pytest.raises(TypeError, match="json.loads"):
            engine.set_strip("master", "{bad json")  # type: ignore[arg-type]
        with pytest.raises(SonareValueError) as bad_param_error:
            engine.set_strip(
                "master",
                scene_strip(
                    '{"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre",'
                    '"processor":"eq.parametric","params":{"band0.gainDb":"loud"}}]}],'
                    '"buses":[],"connections":[]}'
                ),
            )
        assert bad_param_error.value.code == 4
        with pytest.raises(SonareValueError) as bad_bypass_error:
            engine.set_strip_insert_bypassed("master", 0, True)
        assert bad_bypass_error.value.code == 4

        engine.play()
        processed = engine.process([[0.0] * _BLOCK])
        # Two unit clips (2) through the -12 dB master fader; a centred mono master is unity.
        assert processed[0][-1] == pytest.approx(before, abs=1e-3)
        engine.set_parameter(0x4D58FF01, -24.0, smoothed=True)
        engine.set_parameter(0x4D58FF02, 0.25)
        attenuated = processed
        for _ in range(settle_blocks):
            attenuated = engine.process([[0.0] * _BLOCK])
        assert attenuated[0][-1] == pytest.approx(after, abs=1e-3)


def test_engine_refuses_a_reserved_mixer_id_that_names_no_strip() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 5}, {"track_id": 7}])
        points = [AutomationPoint(ppq=0.0, value=-6.0)]
        # The retired positional encoding, for the lane that now holds track 7.
        for positional in (0x4D580101, 0x4D580001, 0x4D58FE00):
            with pytest.raises(SonareValueError) as smoothed:
                engine.set_parameter(positional, -6.0, smoothed=True)
            assert smoothed.value.code == 4
            with pytest.raises(SonareValueError) as immediate:
                engine.set_parameter(positional, -6.0)
            assert immediate.value.code == 4
            with pytest.raises(SonareValueError) as lane:
                engine.set_automation_lane(positional, points)
            assert lane.value.code == 4
        lane_fader = engine.resolve_strip_automation_id(7, "faderDb")
        engine.set_parameter(lane_fader, -6.0, smoothed=True)
        engine.set_automation_lane(lane_fader, points)
        engine.set_parameter(0x4D58FF01, -6.0, smoothed=True)
        engine.set_automation_lane(0x4D58FF01, points)


def test_engine_master_strip_eq_band_updates_embedded_eq() -> None:
    frames = 256 * 16
    source = [math.sin(2.0 * math.pi * 1000.0 * i / 48000.0) for i in range(frames)]
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        engine.set_strip(
            "master",
            scene_strip('{"version":1,"strips":[{"id":"master"}],"buses":[],"connections":[]}'),
        )
        with pytest.raises(SonareValueError) as bad_index_error:
            engine.set_strip_eq_band("master", 99, {"type": "peak", "enabled": True})
        assert bad_index_error.value.code == 4

        engine.play()
        flat_out = engine.process([[0.0] * 256])[0]
        engine.set_strip_eq_band(
            "master",
            0,
            {
                "type": "peak",
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


def test_realtime_engine_process_monitor_returns_separate_bus() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=16) as engine:
        plain = engine.process([[0.25] * 16, [-0.25] * 16])
        assert isinstance(plain, list)
        assert not isinstance(plain, EngineProcessWithMonitorResult)
        explicit = engine.process([[0.25] * 16, [-0.25] * 16], with_monitor=False)
        assert all(np.array_equal(a, b) for a, b in zip(explicit, plain, strict=True))
        result = engine.process([[0.25] * 16, [-0.25] * 16], with_monitor=True)
        assert isinstance(result, EngineProcessWithMonitorResult)
        with pytest.raises(TypeError):
            iter(result)
        output, monitor = result.output, result.monitor
        assert output[0][0] == pytest.approx(0.25)
        assert output[1][0] == pytest.approx(-0.25)
        assert all(np.array_equal(a, b) for a, b in zip(output, plain, strict=True))
        assert monitor[0][0] == pytest.approx(0.0)
        assert monitor[1][0] == pytest.approx(0.0)


_FADER_TARGET = 10 ** (-12 / 20)
_FADER_BLOCKS = _blocks_to_settle(1.0, _FADER_TARGET, 5e-4)


def _fader_blocks(**set_kwargs: bool) -> list[float]:
    """Last sample of each block after one fader write, with the given set_parameter options."""
    frames = _BLOCK * (_FADER_BLOCKS + 2)
    with RealtimeEngine(sample_rate=_SR, max_block_size=_BLOCK) as engine:
        engine.set_param_smoothing_ms(_SMOOTHING_MS)
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
        engine.set_track_lanes([{"track_id": 10}])
        engine.play()
        engine.set_parameter(engine.resolve_strip_automation_id(10, "faderDb"), -12.0, **set_kwargs)
        return [engine.process([[0.0] * _BLOCK])[0][-1] for _ in range(_FADER_BLOCKS)]


def test_set_parameter_smoothed_flag_defaults_to_the_immediate_write() -> None:
    omitted = _fader_blocks()
    assert _fader_blocks(smoothed=False) == omitted
    ramped = _fader_blocks(smoothed=True)
    assert omitted[-1] == pytest.approx(_FADER_TARGET, abs=1e-3)
    assert ramped[-1] == pytest.approx(_FADER_TARGET, abs=1e-3)
    # The last block ending within one time constant is still at least 1/e of the step away.
    within_one_constant = math.floor(_SMOOTHING_MS * 1e-3 * _SR / _BLOCK) - 1
    step = 1.0 - _FADER_TARGET
    assert abs(ramped[within_one_constant] - _FADER_TARGET) > step * math.exp(-1)


def test_engine_track_strip_pan_and_send_setters() -> None:
    frames = 256 * 4
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        engine.set_track_lanes([{"track_id": 10}])
        # A lane strip must exist before its pan can be set.
        engine.set_strip(
            10,
            scene_strip('{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}'),
        )

        # An unknown / zero track id is rejected by every setter.
        with pytest.raises(SonareValueError) as bad_pan_error:
            engine.set_strip_pan(0, -1.0)
        assert bad_pan_error.value.code == 4
        with pytest.raises(SonareError):
            engine.set_strip_pan_law(0, "linear0Db")
        with pytest.raises(SonareError):
            engine.set_strip_pan_mode(0, "balance")
        with pytest.raises(SonareError):
            engine.set_strip_dual_pan(0, -1.0, 1.0)
        with pytest.raises(SonareError):
            engine.set_strip_channel_delay_samples(0, 8)

        # The pan law takes the exact shared tokens and nothing else.
        for case in _PAN_LAW_CORPUS["accepted"]:
            engine.set_strip_pan_law(10, case["value"])
        for value in _PAN_LAW_CORPUS["rejected"]:
            with pytest.raises(SonareValueError):
                engine.set_strip_pan_law(10, value)
        for value in _PAN_LAW_CORPUS["rejected_numeric"]:
            with pytest.raises(TypeError):
                engine.set_strip_pan_law(10, value)
        engine.set_strip_pan_mode(10, "stereoPan")
        engine.set_strip_channel_delay_samples(10, 0)
        engine.set_strip_dual_pan(10, 0.0, 0.0)

        # Hard-left pan should leave the left channel louder than the right.
        engine.set_strip_pan(10, -1.0)
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

    with RealtimeEngine(sample_rate=48000, max_block_size=block) as engine:
        engine.set_clips([clip])
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_strip(
            10,
            scene_strip('{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}'),
        )
        engine.set_strip_pan_mode(10, "dualPan")

        # A +1 left-input pan routes that input to the right output. Settling
        # removes the panner's normal smoothing ramp from this routing check.
        engine.set_strip_dual_pan(10, 1.0, -1.0)
        engine.settle_parameters()
        engine.play()
        right_routed = engine.process([[0.0] * block, [0.0] * block])
        assert energy(right_routed[0]) < 1e-6
        assert energy(right_routed[1]) > 1.0

        # Swapping the dual-pan positions reverses the same left-only source.
        engine.set_strip_dual_pan(10, -1.0, 1.0)
        engine.settle_parameters()
        left_routed = engine.process([[0.0] * block, [0.0] * block])
        assert energy(left_routed[0]) > 1.0
        assert energy(left_routed[1]) < 1e-6


def test_engine_bus_strip_pan_and_send_setters() -> None:
    frames = 256 * 4
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        engine.set_bus_strip(
            1,
            scene_bus(
                '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}'
            ),
        )

        # An unknown / zero bus id is rejected by every setter.
        with pytest.raises(SonareValueError) as bad_pan_error:
            engine.set_bus_pan(0, -1.0)
        assert bad_pan_error.value.code == 4
        with pytest.raises(SonareError):
            engine.set_bus_pan_law(0, "linear0Db")
        with pytest.raises(SonareError):
            engine.set_bus_pan_mode(0, "balance")
        with pytest.raises(SonareError):
            engine.set_bus_dual_pan(0, -1.0, 1.0)
        with pytest.raises(SonareValueError) as unknown_bus_error:
            engine.set_bus_pan(99, 0.0)
        assert unknown_bus_error.value.code == 4

        # Non-finite pan is refused outright rather than clamped.
        with pytest.raises(SonareError):
            engine.set_bus_pan(1, math.nan)
        with pytest.raises(SonareError):
            engine.set_bus_pan(1, math.inf)
        with pytest.raises(SonareError):
            engine.set_bus_dual_pan(1, math.nan, 0.0)

        # The pan law takes the exact shared tokens and nothing else.
        for case in _PAN_LAW_CORPUS["accepted"]:
            engine.set_bus_pan_law(1, case["value"])
        for value in _PAN_LAW_CORPUS["rejected"]:
            with pytest.raises(SonareValueError):
                engine.set_bus_pan_law(1, value)
        for value in _PAN_LAW_CORPUS["rejected_numeric"]:
            with pytest.raises(TypeError):
                engine.set_bus_pan_law(1, value)
        engine.set_bus_pan_mode(1, "stereoPan")
        engine.set_bus_dual_pan(1, 0.0, 0.0)

        # Hard-left pan should leave the left channel louder than the right.
        engine.set_bus_pan(1, -1.0)
        engine.play()
        left, right = engine.process([[0.0] * 256, [0.0] * 256])
        left_rms = math.sqrt(sum(s * s for s in left) / len(left))
        right_rms = math.sqrt(sum(s * s for s in right) / len(right))
        assert left_rms > right_rms


def test_engine_bus_strip_pan_rejects_surround_bus() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        # A 5.1 bus rejects every pan setter outright (unknown and surround
        # buses share one generic failure), but keeps its EQ available.
        engine.set_track_buses([{"bus_id": 1, "channel_layout": "5.1"}])
        engine.set_bus_strip(
            1,
            scene_bus(
                '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}'
            ),
        )
        with pytest.raises(SonareValueError) as surround_pan_error:
            engine.set_bus_pan(1, 0.0)
        assert surround_pan_error.value.code == 4
        with pytest.raises(SonareError):
            engine.set_bus_pan_law(1, "linear0Db")
        with pytest.raises(SonareError):
            engine.set_bus_pan_mode(1, "balance")
        with pytest.raises(SonareError):
            engine.set_bus_dual_pan(1, -1.0, 1.0)
        engine.set_bus_eq_band(1, 0, {"type": "peak", "enabled": True})


def test_engine_bus_strip_eq_band_updates_embedded_eq() -> None:
    frames = 256 * 16
    source = [math.sin(2.0 * math.pi * 1000.0 * i / 48000.0) for i in range(frames)]
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0, "channel_layout": "mono"}])
        engine.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
        engine.set_bus_strip(
            1,
            scene_bus(
                '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[]}],"connections":[]}'
            ),
        )
        with pytest.raises(SonareValueError) as bad_index_error:
            engine.set_bus_eq_band(1, 99, {"type": "peak", "enabled": True})
        assert bad_index_error.value.code == 4

        engine.play()
        flat_out = engine.process([[0.0] * 256])[0]
        engine.set_bus_eq_band(
            1,
            0,
            {
                "type": "peak",
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


def test_engine_track_lane_send_timing_is_optional() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        # A ``send_timing`` token and an omitted one are both accepted. The engine
        # exposes no readback for the effective send timing, so this only asserts
        # that both are configured successfully.
        engine.set_track_lanes(
            [
                {
                    "track_id": 10,
                    "sends": [
                        {"bus_id": 1, "level_db": 0.0, "send_timing": "preFader"},
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


def test_track_solo_mute_and_monitor_mode_address_a_declared_track_id() -> None:
    frames = 256 * 8
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
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
        # Track 10 is lane 1, so muting lane 0 instead would leave it sounding.
        engine.set_track_lanes([{"track_id": 20}, {"track_id": 10}])
        engine.play()
        assert engine.process([[0.0] * 256])[0][-1] == pytest.approx(1.0, abs=0.01)
        engine.set_track_solo_mute(10, solo=False, mute=True)
        for _ in range(16):
            out = engine.process([[0.0] * 256])
        assert abs(out[0][-1]) < 0.01
        engine.set_track_monitor_mode(10, "pfl")
        for entry, call in (
            ("set_track_solo_mute", lambda: engine.set_track_solo_mute(1, solo=True, mute=False)),
            ("set_track_monitor_mode", lambda: engine.set_track_monitor_mode(1, "pfl")),
        ):
            with pytest.raises(
                SonareValueError,
                match=f"^RealtimeEngine.{entry}: track_id must be "
                "the id of a track in this engine$",
            ):
                call()
        with pytest.raises(TypeError):
            engine.set_track_solo_mute(None, solo=True, mute=False)  # type: ignore[arg-type]


def test_track_solo_mute_after_a_project_timeline_is_coded_invalid_state() -> None:
    with Project() as project, RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        result = project.compile(with_timeline=True)
        assert result.timeline is not None
        with result.timeline as timeline:
            engine.apply_project_timeline(timeline)
        with pytest.raises(SonareError) as refused:
            engine.set_track_solo_mute(10, solo=True, mute=False)
        assert refused.value.code == int(ErrorCode.INVALID_STATE)
