"""Tests for the realtime/offline DAW engine Python wrapper."""

from __future__ import annotations

import math

import pytest

from libsonare import (
    AutomationPoint,
    EngineMarker,
    EngineMetronomeConfig,
    ErrorCode,
    ParameterInfo,
    Project,
    RealtimeEngine,
    SonareError,
    SonareValueError,
    engine_abi_version,
    voice_changer_abi_version,
)
from libsonare._tempo_domains import PROJECT_TEMPO_MIN

from ._helpers import raises_coded, scene_bus, scene_strip


def test_engine_abi_version() -> None:
    assert engine_abi_version() > 0


def test_voice_changer_abi_version() -> None:
    """The RVC POD ABI version is exposed for parity with Node/WASM."""
    v = voice_changer_abi_version()
    if v is None:
        pytest.skip("voice changer is compiled out")
    assert isinstance(v, int)
    assert v > 0


def test_engine_rejects_hostile_tempo_and_metronome_lengths() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_tempo(100000.0)
        with pytest.raises(SonareError):
            engine.set_tempo(100000.1)
        with pytest.raises(SonareError):
            engine.set_tempo_segments([{"start_ppq": 0.0, "bpm": 120.0, "end_bpm": 100000.1}])
        with pytest.raises(SonareError):
            engine.set_metronome(EngineMetronomeConfig(enabled=True, click_samples=2_000_000_000))
        with pytest.raises(SonareError):
            engine.set_metronome(EngineMetronomeConfig(enabled=True, click_sec=2.0))
        with pytest.raises(SonareError):
            engine.set_metronome(EngineMetronomeConfig(enabled=True, click_sec=math.nan))


def test_engine_tempo_domain_and_refused_segment_preserve_state() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_tempo(PROJECT_TEMPO_MIN)
        assert engine.transport_state().bpm == PROJECT_TEMPO_MIN

        with pytest.raises(SonareValueError) as caught:
            engine.set_tempo(1e-8)
        assert "RealtimeEngine.set_tempo: bpm must be finite and in [1e-6, 100000]" in str(
            caught.value
        )
        assert engine.transport_state().bpm == PROJECT_TEMPO_MIN

        engine.set_tempo_segments([{"start_ppq": 0.0, "bpm": 90.0, "end_bpm": 0.0}])
        before = engine.transport_state().bpm
        for segments, subject in (
            ([{"start_ppq": 0.0, "bpm": 1e-8}], "segments[0].bpm"),
            ([{"start_ppq": 0.0, "bpm": 90.0, "end_bpm": 1e-8}], "segments[0].end_bpm"),
        ):
            with pytest.raises(SonareValueError) as caught:
                engine.set_tempo_segments(segments)
            assert (
                f"RealtimeEngine.set_tempo_segments: {subject} must be finite and in [1e-6, 100000]"
            ) in str(caught.value)
            assert engine.transport_state().bpm == before


def test_metronome_default_click_length_is_absent() -> None:
    assert EngineMetronomeConfig(enabled=True).click_samples is None


@pytest.mark.parametrize("render_frame", [-2, -1])
def test_scheduled_render_frame_keeps_absence_and_refuses_below_sentinel(
    render_frame: int,
) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(SonareValueError) as refused:
            engine.play(render_frame=render_frame)
        assert refused.value.code == ErrorCode.INVALID_PARAMETER
        assert str(refused.value) == (
            "RealtimeEngine.play: render_frame must be an integer in [0, 9223372036854775807]"
        )

        engine.play()  # Omission is the Python spelling for the C sentinel.
        engine.play(render_frame=0)


def test_engine_transport_state_and_live_parameters() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
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
                default_curve="linear",
            )
        )
        engine.set_parameter(3, 1.5)
        engine.set_parameter(3, 0.5, render_frame=0, smoothed=True)
        engine.play()
        engine.process([[0.1] * 128, [0.1] * 128])

        state = engine.transport_state()
        assert isinstance(state.playing, bool)
        assert state.playing
        assert state.bpm == pytest.approx(90.0)
        assert state.looping
        assert state.loop_end_ppq == pytest.approx(4.0)
        assert state.sample_rate == 48000 and isinstance(state.sample_rate, int)
        assert isinstance(state.sample_position, int)
        # Musical beat readout: `beat` is one-based, `beat_fraction` in [0, 1).
        assert isinstance(state.beat, int)
        assert state.beat >= 1
        assert isinstance(state.beat_fraction, float)
        assert 0.0 <= state.beat_fraction < 1.0

        # Meter telemetry always drains to a list (possibly empty without a
        # configured meter tap).
        records = engine.drain_meter_telemetry()
        assert isinstance(records, list)


def test_engine_tempo_and_time_signature_segments() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        # Valid ramp + time-signature map (mappings and tuples both accepted).
        engine.set_tempo_segments(
            [
                {"start_ppq": 0.0, "bpm": 120.0},
                {"start_ppq": 1920.0, "bpm": 120.0, "end_bpm": 140.0},
            ]
        )
        engine.set_time_signature_segments(
            [
                {"start_ppq": 0.0, "numerator": 4, "denominator": 4},
                {"start_ppq": 1920.0, "numerator": 3, "denominator": 4},
            ]
        )
        # An empty sequence clears the map without error.
        engine.set_tempo_segments([])
        engine.set_time_signature_segments([])
        # Invalid input is rejected, matching the C ABI and the other surfaces.
        with pytest.raises(SonareError):
            engine.set_tempo_segments([{"start_ppq": 0.0, "bpm": 0.0}])
        with pytest.raises(SonareError):
            engine.set_tempo_segments([{"start_ppq": float("nan"), "bpm": 120.0}])
        with pytest.raises(SonareError):
            engine.set_time_signature_segments(
                [{"start_ppq": 0.0, "numerator": 0, "denominator": 4}]
            )


def test_engine_rejects_out_of_range_automation_curve() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.add_parameter(
            ParameterInfo(
                id=5,
                name="gain",
                unit="dB",
                min_value=0.0,
                max_value=1.0,
                default_value=0.0,
                rt_safe=True,
                default_curve="linear",
            )
        )
        # A curve ordinal is a type error, and an unknown curve token is refused.
        with pytest.raises(TypeError):
            engine.set_automation_lane(5, [AutomationPoint(ppq=0.0, value=0.5, curve_to_next=3)])  # type: ignore[arg-type]
        with pytest.raises(SonareValueError):
            engine.set_automation_lane(
                5, [AutomationPoint(ppq=0.0, value=0.5, curve_to_next="s-curve")]
            )  # type: ignore[arg-type]
        with pytest.raises(TypeError):
            engine.add_parameter(
                ParameterInfo(
                    id=6,
                    name="pan",
                    unit="",
                    min_value=-1.0,
                    max_value=1.0,
                    default_value=0.0,
                    rt_safe=True,
                    default_curve=99,  # type: ignore[arg-type]
                )
            )


def test_engine_close_is_the_one_release_name() -> None:
    engine = RealtimeEngine(sample_rate=48000, max_block_size=128)
    engine.close()
    engine.close()  # a second close is a no-op
    assert not hasattr(engine, "destroy")
    assert not hasattr(engine, "delete")
    with raises_coded(ErrorCode.INVALID_STATE):
        engine.play()


def test_engine_marker_kind_and_key_signature_round_trip() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_markers(
            [
                EngineMarker(0.0, id=1, name="verse", kind="cuePoint"),
                EngineMarker(
                    4.0,
                    id=2,
                    name="G major",
                    kind="keySignature",
                    key_fifths=1,
                    key_minor=False,
                ),
                EngineMarker(
                    8.0,
                    id=3,
                    name="C minor",
                    kind="keySignature",
                    key_fifths=-3,
                    key_minor=True,
                ),
            ]
        )
        assert len(engine.markers()) == 3

        cue = engine.markers()[0]
        assert cue.kind == "cuePoint"
        assert cue.name == "verse"
        assert cue.key_fifths == 0
        assert cue.key_minor is False

        major = engine.marker(2)
        assert major.kind == "keySignature"
        assert major.key_fifths == 1
        assert major.key_minor is False

        minor = engine.marker(3)
        assert minor.kind == "keySignature"
        assert minor.key_fifths == -3
        assert minor.key_minor is True


@pytest.mark.parametrize("kind", ["marker", "text", "lyric", "cuePoint"])
@pytest.mark.parametrize(
    ("key_fifths", "key_minor", "field"),
    [(1, False, "key_fifths"), (0, True, "key_minor")],
)
def test_engine_markers_refuse_inactive_key_signature_fields(
    kind: str, key_fifths: int, key_minor: bool, field: str
) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        marker = EngineMarker(1.0, id=1, kind=kind, key_fifths=key_fifths, key_minor=key_minor)
        with pytest.raises(SonareError) as caught:
            engine.set_markers([marker])
        assert caught.value.code == ErrorCode.INVALID_PARAMETER
        assert str(caught.value) == (
            f"RealtimeEngine.set_markers: markers[0].{field} must be 0 unless "
            "markers[0].kind is key signature"
        )
        assert engine.markers() == []


@pytest.mark.parametrize("fifths", [-8, 8])
def test_engine_key_signature_marker_refuses_out_of_domain_fifths(fifths: int) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_markers([EngineMarker(1, id=1, kind="keySignature", key_fifths=7)])
        with pytest.raises(
            SonareValueError,
            match=r"^RealtimeEngine\.set_markers: markers\[0\]\.key_fifths "
            r"must be an integer in \[-7, 7\]$",
        ):
            engine.set_markers([EngineMarker(2, id=1, kind="keySignature", key_fifths=fifths)])
        assert engine.marker(1).key_fifths == 7


def test_engine_key_signature_marker_accepts_boundary_fifths() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_markers(
            [
                EngineMarker(1.0, id=1, kind="keySignature", key_fifths=-7),
                EngineMarker(2.0, id=2, kind="keySignature", key_fifths=7, key_minor=True),
            ]
        )
        assert [(marker.key_fifths, marker.key_minor) for marker in engine.markers()] == [
            (-7, False),
            (7, True),
        ]


def test_engine_prepare_rejects_nonfinite_and_out_of_range_sample_rates() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        for sample_rate in (float("nan"), 7999, 384001):
            with pytest.raises(SonareError):
                engine.prepare(sample_rate, 128)


def test_float_sample_rate_is_refused_by_engine_and_project() -> None:
    with pytest.raises(SonareValueError):
        RealtimeEngine(sample_rate=48000.0, max_block_size=128)  # type: ignore[arg-type]
    with (
        RealtimeEngine(sample_rate=48000, max_block_size=128) as engine,
        pytest.raises(SonareValueError),
    ):
        engine.prepare(48000.0, 128)  # type: ignore[arg-type]
    with Project() as project, pytest.raises(SonareValueError):
        project.set_sample_rate(48000.0)  # type: ignore[arg-type]


def test_engine_prepare_accepts_declared_channel_capacity() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128, max_channels=2) as engine:
        engine.prepare(48000, 128, max_channels=2)
        for max_channels in (0, 65):
            with pytest.raises(SonareError):
                engine.prepare(48000, 128, max_channels=max_channels)


def test_engine_resolve_and_set_bus_master_insert_automation_ids() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        engine.set_bus_strip(
            1,
            scene_bus(
                '{"version":1,"strips":[],"buses":[{"id":"1","inserts":['
                '{"slot":"pre","processor":"eq.parametric",'
                '"params":{"band0.type":1,"band0.frequencyHz":1000,'
                '"band0.gainDb":0,"band0.enabled":1}}]}],"connections":[]}'
            ),
        )

        # Resolving an insert parameter yields a reserved automation id
        # (top three bits set) usable like a fader/pan id.
        bus_param = engine.resolve_bus_insert_automation_id(1, 0, "band0.gainDb")
        assert bus_param & 0xE0000000 == 0xE0000000
        engine.set_automation_lane(bus_param, [AutomationPoint(ppq=0.0, value=6.0)])
        engine.set_parameter(bus_param, 3.0)

        # The by-name manual set reaches the same target.
        engine.set_bus_insert_param(1, 0, "band0.gainDb", -3.0)

        # Unknown bus / insert / name are rejected.
        with pytest.raises(SonareValueError) as bad_bus_error:
            engine.resolve_bus_insert_automation_id(9, 0, "band0.gainDb")
        assert bad_bus_error.value.code == 4
        with pytest.raises(SonareError):
            engine.resolve_bus_insert_automation_id(1, 0, "nope")
        with pytest.raises(SonareError):
            engine.set_bus_insert_param(9, 0, "band0.gainDb", 1.0)

        # Master strip insert resolution mirrors the bus path.
        engine.set_strip(
            "master",
            scene_strip(
                '{"version":1,"strips":[{"id":"master","inserts":['
                '{"slot":"pre","processor":"dynamics.compressor",'
                '"params":{"thresholdDb":-3,"ratio":4}}]}],'
                '"buses":[],"connections":[]}'
            ),
        )
        master_param = engine.resolve_strip_insert_automation_id("master", 0, "thresholdDb")
        assert master_param & 0xE0000000 == 0xE0000000
        assert master_param != bus_param


def test_engine_apply_commands_due_now_preserves_future_commands() -> None:
    block = 256
    with RealtimeEngine(sample_rate=48000, max_block_size=block) as engine:
        engine.settle_insert_parameters()
        # Unscheduled (-1) and frame-0 seeks are due now and apply in FIFO order;
        # the later two fall inside the second and third blocks.
        engine.seek_sample(111)
        engine.seek_sample(222, render_frame=0)
        engine.seek_sample(5000, render_frame=300)
        engine.seek_sample(6000, render_frame=600)
        assert engine.transport_state().sample_position == 0

        engine.apply_commands_due_now_preserving_future()
        assert engine.transport_state().sample_position == 222
        # A second call applies nothing again and does not drain a future command.
        engine.apply_commands_due_now_preserving_future()
        assert engine.transport_state().sample_position == 222

        silence = [[0.0] * block, [0.0] * block]
        engine.process(silence)
        assert engine.transport_state().sample_position == 222
        engine.process(silence)
        assert engine.transport_state().sample_position == 5000
        engine.process(silence)
        assert engine.transport_state().sample_position == 6000


def test_engine_empty_automation_lane_removes_it() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.add_parameter(
            ParameterInfo(
                id=7,
                name="gain",
                unit="dB",
                min_value=-60.0,
                max_value=12.0,
                default_value=0.0,
                rt_safe=True,
                default_curve="linear",
            )
        )
        engine.set_automation_lane(7, [AutomationPoint(ppq=0.0, value=0.0)])
        assert engine.automation_lane_count == 1
        engine.set_automation_lane(7, [])
        assert engine.automation_lane_count == 0


def test_engine_apply_restore_clear_insert_param_by_name_now() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_lanes([{"track_id": 10}])
        engine.set_strip(
            10,
            scene_strip(
                '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
                '"processor":"eq.parametric","params":{"band0.type":1,'
                '"band0.frequencyHz":1000,"band0.gainDb":0,"band0.enabled":1}}]}],'
                '"buses":[],"connections":[]}'
            ),
        )
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        engine.set_bus_strip(
            1,
            scene_bus(
                '{"version":1,"strips":[],"buses":[{"id":"1","inserts":['
                '{"slot":"pre","processor":"eq.parametric",'
                '"params":{"band0.type":1,"band0.frequencyHz":1000,'
                '"band0.gainDb":0,"band0.enabled":1}}]}],"connections":[]}'
            ),
        )
        engine.set_strip(
            "master",
            scene_strip(
                '{"version":1,"strips":[{"id":"master","inserts":['
                '{"slot":"pre","processor":"dynamics.compressor",'
                '"params":{"thresholdDb":-3,"ratio":4}}]}],'
                '"buses":[],"connections":[]}'
            ),
        )

        # A known target applies immediately and reports True.
        assert engine.apply_strip_insert_param_now(10, 0, "band0.gainDb", 3.0)
        assert engine.apply_strip_insert_param_now("master", 0, "thresholdDb", -6.0)
        assert engine.apply_bus_insert_param_now(1, 0, "band0.gainDb", 3.0)

        # An unknown name -- or an otherwise-unknown target -- is not an
        # error; it just does not apply.
        assert not engine.apply_strip_insert_param_now(10, 0, "nope", 1.0)
        assert not engine.apply_strip_insert_param_now("master", 0, "nope", 1.0)
        assert not engine.apply_bus_insert_param_now(1, 0, "nope", 1.0)
        assert not engine.apply_strip_insert_param_now(99, 0, "band0.gainDb", 1.0)

        # A retained value replays exactly, without a ramp.
        engine.restore_strip_insert_param(10, 0, "band0.gainDb", -3.0)
        engine.restore_strip_insert_param("master", 0, "thresholdDb", -9.0)
        engine.restore_bus_insert_param(1, 0, "band0.gainDb", -3.0)

        # Unlike apply_*_now, restore raises for an unknown track.
        with pytest.raises(SonareError):
            engine.restore_strip_insert_param(99, 0, "band0.gainDb", 1.0)

        # Forgetting the remembered manual values does not error for a known
        # track/bus and the master strip.
        engine.clear_strip_insert_parameter_bases(10)
        engine.clear_strip_insert_parameter_bases("master")
        engine.clear_bus_insert_parameter_bases(1)

        # Unlike the bus/master forms, an unknown track is rejected.
        with pytest.raises(SonareError):
            engine.clear_strip_insert_parameter_bases(99)


def test_engine_set_param_smoothing_ms_validates_argument() -> None:
    with RealtimeEngine(
        sample_rate=48000, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_param_smoothing_ms(0.0)
        engine.set_param_smoothing_ms(75.0)
        with raises_coded():
            engine.set_param_smoothing_ms(-1.0)
        with raises_coded():
            engine.set_param_smoothing_ms(float("nan"))
