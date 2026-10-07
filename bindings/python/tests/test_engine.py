"""Tests for the realtime/offline DAW engine Python wrapper."""

from __future__ import annotations

import math

import pytest

from libsonare import (
    AutomationCurve,
    AutomationPoint,
    EngineMarker,
    EngineMetronomeConfig,
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


def test_engine_rejects_hostile_tempo_and_metronome_lengths() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_tempo(100000.0)
        with pytest.raises(SonareError):
            engine.set_tempo(100000.1)
        with pytest.raises(SonareError):
            engine.set_tempo_segments([{"start_ppq": 0.0, "bpm": 120.0, "end_bpm": 100000.1}])
        with pytest.raises(SonareError):
            engine.set_metronome(EngineMetronomeConfig(enabled=True, click_samples=2_000_000_000))
        with pytest.raises(SonareError):
            engine.set_metronome(
                EngineMetronomeConfig(enabled=True, click_samples=0, click_seconds=2.0)
            )
        with pytest.raises(SonareError):
            engine.set_metronome(
                EngineMetronomeConfig(enabled=True, click_samples=0, click_seconds=math.nan)
            )


def test_metronome_default_click_uses_sample_rate_sentinel() -> None:
    assert EngineMetronomeConfig(enabled=True).click_samples == 0


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
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        # Valid ramp + time-signature map (mappings and tuples both accepted).
        engine.set_tempo_segments(
            [
                {"start_ppq": 0.0, "bpm": 120.0},
                {"start_ppq": 1920.0, "bpm": 120.0, "end_bpm": 140.0},
            ]
        )
        engine.set_time_signature_segments(
            [{"start_ppq": 0.0, "numerator": 4, "denominator": 4}, (1920.0, 3, 4)]
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
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.add_parameter(
            ParameterInfo(
                id=5,
                name="gain",
                unit="dB",
                min_value=0.0,
                max_value=1.0,
                default_value=0.0,
                rt_safe=True,
                default_curve=AutomationCurve.LINEAR,
            )
        )
        # An out-of-range breakpoint curve ordinal is rejected (not clamped).
        with pytest.raises((ValueError, SonareError)):
            engine.set_automation_lane(5, [AutomationPoint(ppq=0.0, value=0.5, curve_to_next=99)])
        # An out-of-range default curve is likewise rejected.
        with pytest.raises((ValueError, SonareError)):
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


def test_engine_destroy_and_delete_aliases() -> None:
    engine = RealtimeEngine(sample_rate=48000.0, max_block_size=128)
    engine.destroy()
    # delete() is the second cross-binding alias and is safe to call again.
    engine.delete()
    with pytest.raises(RuntimeError):
        engine.play()


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


def test_engine_prepare_rejects_nonfinite_and_out_of_range_sample_rates() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        for sample_rate in (float("nan"), 7999.0, 384001.0):
            with pytest.raises(SonareError):
                engine.prepare(sample_rate, 128)


def test_engine_prepare_accepts_declared_channel_capacity() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128, max_channels=2) as engine:
        engine.prepare(48000.0, 128, max_channels=2)
        for max_channels in (0, 65):
            with pytest.raises(SonareError):
                engine.prepare(48000.0, 128, max_channels=max_channels)


def test_engine_resolve_and_set_bus_master_insert_automation_ids() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        engine.set_bus_strip_json(
            1,
            '{"version":1,"strips":[],"buses":[{"id":"1","inserts":['
            '{"slot":"pre","processor":"eq.parametric",'
            '"params":"{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,'
            '\\"band0.gainDb\\":0,\\"band0.enabled\\":1}"}]}],"connections":[]}',
        )

        # Resolving an insert parameter yields a reserved automation id
        # (top three bits set) usable like a fader/pan id.
        bus_param = engine.resolve_bus_insert_automation_id(1, 0, "band0.gainDb")
        assert bus_param & 0xE0000000 == 0xE0000000
        engine.set_automation_lane(bus_param, [AutomationPoint(ppq=0.0, value=6.0)])
        engine.set_parameter(bus_param, 3.0)

        # The by-name manual set reaches the same target.
        engine.set_bus_strip_insert_param_by_name(1, 0, "band0.gainDb", -3.0)

        # Unknown bus / insert / name are rejected.
        with pytest.raises(SonareError) as bad_bus_error:
            engine.resolve_bus_insert_automation_id(9, 0, "band0.gainDb")
        assert bad_bus_error.value.code == 4
        with pytest.raises(SonareError):
            engine.resolve_bus_insert_automation_id(1, 0, "nope")
        with pytest.raises(SonareError):
            engine.set_bus_strip_insert_param_by_name(9, 0, "band0.gainDb", 1.0)

        # Master strip insert resolution mirrors the bus path.
        engine.set_master_strip_json(
            '{"version":1,"strips":[{"id":"master","inserts":['
            '{"slot":"pre","processor":"dynamics.compressor",'
            '"params":"{\\"thresholdDb\\":-3,\\"ratio\\":4}"}]}],'
            '"buses":[],"connections":[]}'
        )
        master_param = engine.resolve_master_insert_automation_id(0, "thresholdDb")
        assert master_param & 0xE0000000 == 0xE0000000
        assert master_param != bus_param


def test_engine_apply_commands_due_now_preserves_future_commands() -> None:
    block = 256
    with RealtimeEngine(sample_rate=48000.0, max_block_size=block) as engine:
        engine.settle_insert_parameters()
        # Unscheduled (-1) and frame-0 seeks are due now and apply in FIFO order;
        # the later two fall inside the second and third blocks.
        engine.seek_sample(111, render_frame=-1)
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
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
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
        engine.set_automation_lane(7, [AutomationPoint(ppq=0.0, value=0.0)])
        assert engine.automation_lane_count() == 1
        engine.set_automation_lane(7, [])
        assert engine.automation_lane_count() == 0


def test_engine_apply_restore_clear_insert_param_by_name_now() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
        engine.set_track_lanes([10])
        engine.set_track_strip_json(
            10,
            '{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre",'
            '"processor":"eq.parametric","params":"{\\"band0.type\\":1,'
            '\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":0,\\"band0.enabled\\":1}"}]}],'
            '"buses":[],"connections":[]}',
        )
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}])
        engine.set_bus_strip_json(
            1,
            '{"version":1,"strips":[],"buses":[{"id":"1","inserts":['
            '{"slot":"pre","processor":"eq.parametric",'
            '"params":"{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,'
            '\\"band0.gainDb\\":0,\\"band0.enabled\\":1}"}]}],"connections":[]}',
        )
        engine.set_master_strip_json(
            '{"version":1,"strips":[{"id":"master","inserts":['
            '{"slot":"pre","processor":"dynamics.compressor",'
            '"params":"{\\"thresholdDb\\":-3,\\"ratio\\":4}"}]}],'
            '"buses":[],"connections":[]}'
        )

        # A known target applies immediately and reports True.
        assert engine.apply_track_strip_insert_param_by_name_now(10, 0, "band0.gainDb", 3.0)
        assert engine.apply_master_strip_insert_param_by_name_now(0, "thresholdDb", -6.0)
        assert engine.apply_bus_strip_insert_param_by_name_now(1, 0, "band0.gainDb", 3.0)

        # An unknown name -- or an otherwise-unknown target -- is not an
        # error; it just does not apply.
        assert not engine.apply_track_strip_insert_param_by_name_now(10, 0, "nope", 1.0)
        assert not engine.apply_master_strip_insert_param_by_name_now(0, "nope", 1.0)
        assert not engine.apply_bus_strip_insert_param_by_name_now(1, 0, "nope", 1.0)
        assert not engine.apply_track_strip_insert_param_by_name_now(99, 0, "band0.gainDb", 1.0)

        # A retained value replays exactly, without a ramp.
        engine.restore_track_strip_insert_param_by_name(10, 0, "band0.gainDb", -3.0)
        engine.restore_master_strip_insert_param_by_name(0, "thresholdDb", -9.0)
        engine.restore_bus_strip_insert_param_by_name(1, 0, "band0.gainDb", -3.0)

        # Unlike apply_*_now, restore raises for an unknown track.
        with pytest.raises(SonareError):
            engine.restore_track_strip_insert_param_by_name(99, 0, "band0.gainDb", 1.0)

        # Forgetting the remembered manual values does not error for a known
        # track/bus and the master strip.
        engine.clear_track_insert_parameter_bases(10)
        engine.clear_master_insert_parameter_bases()
        engine.clear_bus_insert_parameter_bases(1)

        # Unlike the bus/master forms, an unknown track is rejected.
        with pytest.raises(SonareError):
            engine.clear_track_insert_parameter_bases(99)


def test_engine_set_param_smoothing_ms_validates_argument() -> None:
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_param_smoothing_ms(0.0)
        engine.set_param_smoothing_ms(75.0)
        with pytest.raises((ValueError, RuntimeError)):
            engine.set_param_smoothing_ms(-1.0)
        with pytest.raises((ValueError, RuntimeError)):
            engine.set_param_smoothing_ms(float("nan"))
