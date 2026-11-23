"""Tests for bus-to-bus routing/sends, bus/master sidechain, and MIDI clip gain/fade.

Python counterpart of ``tests/api/sonare_c_engine_routing_test.cpp``; the DC-track
and ducker/limiter/master rigs below are the same fixtures translated to the
Python surface so the two stay comparable.
"""

from __future__ import annotations

import ctypes
import json
from pathlib import Path

import pytest

from libsonare import (
    BuiltinSynthConfig,
    EngineClip,
    EngineMidiClipSchedule,
    EngineMidiEvent,
    RealtimeEngine,
    SidechainCheck,
    SidechainSource,
    SonareError,
    SonareValueError,
)
from libsonare._engine_mixing import _sidechain_check
from libsonare._ffi_types_core import SonareEngineBus as _RawEngineBus
from libsonare._ffi_types_mastering_project import (
    SonareEngineMidiClipSchedule as _RawMidiClipSchedule,
)
from libsonare._runtime import _sidechain_source_kind_value

from ._helpers import scene_bus, scene_strip

_FRAMES = 256 * 40
_BLOCK = 256

_DUCKER_BUS_JSON = (
    '{"version":2,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre",'
    '"processor":"dynamics.duckingProcessor","params":{"thresholdDb":-20,'
    '"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30}}]}],'
    '"connections":[]}'
)
_LIMITER_BUS_JSON = (
    '{"version":2,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre",'
    '"processor":"dynamics.limiter","params":{"thresholdDb":24,'
    '"lookaheadMs":0,"releaseMs":50}}]}],"connections":[]}'
)
_DUCKER_MASTER_JSON = (
    '{"version":2,"strips":[{"id":"master","inserts":[{"slot":"pre",'
    '"processor":"dynamics.duckingProcessor","params":{"thresholdDb":-20,'
    '"ratio":20,"attackMs":0.05,"releaseMs":80,"rangeDb":30}}]}],'
    '"buses":[],"connections":[]}'
)


def _render_last(engine: RealtimeEngine, blocks: int = 30) -> float:
    """Seek to the top, play, and return the last rendered mono sample."""
    engine.seek_sample(0)
    engine.play()
    out: list[float] = []
    for _ in range(blocks):
        out = engine.process([[0.0] * _BLOCK])[0]
    return out[-1]


def _direct_level() -> float:
    """Direct-to-master level of one DC lane, the unit the routing cases scale."""
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=1,
                    track_id=10,
                    channels=[[1.0] * _FRAMES],
                    start_ppq=0.0,
                    length_samples=_FRAMES,
                )
            ]
        )
        engine.set_track_lanes([{"track_id": 10}])
        level = _render_last(engine)
        assert level > 0.5
        return level


def test_engine_bus_output_bus_id_routes_into_another_bus() -> None:
    x = _direct_level()
    half = 10.0 ** (-6.0 / 20.0)

    def run(bus1_output: int | None) -> float:
        with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
            engine.set_clips(
                [
                    EngineClip(
                        id=1,
                        track_id=10,
                        channels=[[1.0] * _FRAMES],
                        start_ppq=0.0,
                        length_samples=_FRAMES,
                    )
                ]
            )
            engine.set_track_buses(
                [
                    {"bus_id": 1, "gain_db": 0.0, "output_bus_id": bus1_output},
                    {"bus_id": 2, "gain_db": -6.0},
                ]
            )
            engine.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
            return _render_last(engine)

    assert run(None) == pytest.approx(x, rel=1e-3)
    # Through bus 2, its -6 dB gain_db applies on top.
    assert run(2) == pytest.approx(x * half, rel=1e-3)


def test_engine_bus_sends_tap_before_or_after_gain_db() -> None:
    x = _direct_level()
    g1 = 10.0 ** (-12.0 / 20.0)

    def run(timing: str, level_db: float, enabled: bool) -> float:
        with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
            engine.set_clips(
                [
                    EngineClip(
                        id=1,
                        track_id=10,
                        channels=[[1.0] * _FRAMES],
                        start_ppq=0.0,
                        length_samples=_FRAMES,
                    )
                ]
            )
            engine.set_track_buses(
                [
                    {
                        "bus_id": 1,
                        "gain_db": -12.0,
                        "sends": [
                            {
                                "bus_id": 2,
                                "level_db": level_db,
                                "enabled": enabled,
                                "send_timing": timing,
                            }
                        ],
                    },
                    {"bus_id": 2, "gain_db": 0.0},
                ]
            )
            engine.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
            return _render_last(engine)

    assert run("preFader", 0.0, True) == pytest.approx(x * (g1 + 1.0), rel=1e-3)
    assert run("postFader", 0.0, True) == pytest.approx(x * 2.0 * g1, rel=1e-3)
    half = 10.0 ** (-6.0 / 20.0)
    assert run("preFader", -6.0, True) == pytest.approx(x * (g1 + half), rel=1e-3)
    assert run("postFader", 0.0, False) == pytest.approx(x * g1, rel=1e-2)


def _routing_engine(lanes: list[object]) -> RealtimeEngine:
    """Engine with one DC lane (track 10), buses 1 and 2 declared, and ``lanes`` applied."""
    engine = RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK)
    engine.set_clips(
        [
            EngineClip(
                id=1,
                track_id=10,
                channels=[[1.0] * _FRAMES],
                start_ppq=0.0,
                length_samples=_FRAMES,
            )
        ]
    )
    engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}, {"bus_id": 2, "gain_db": -6.0}])
    engine.set_track_lanes(lanes)  # type: ignore[arg-type]
    return engine


_SEND_TO_2 = [{"bus_id": 2, "level_db": -3.0}]


def test_set_track_sends_matches_set_track_lanes_with_the_same_final_config() -> None:
    with (
        _routing_engine([{"track_id": 10, "output_bus_id": 1}]) as partial,
        _routing_engine([{"track_id": 10, "output_bus_id": 1, "sends": _SEND_TO_2}]) as full,
    ):
        partial.set_track_sends(10, _SEND_TO_2)
        assert _render_last(partial) == pytest.approx(_render_last(full), abs=1e-6)

    # Clearing goes through the same replace path as an omitted-sends lane list.
    with (
        _routing_engine([{"track_id": 10, "output_bus_id": 1, "sends": _SEND_TO_2}]) as partial,
        _routing_engine([{"track_id": 10, "output_bus_id": 1, "sends": _SEND_TO_2}]) as full,
    ):
        partial.set_track_sends(10, [])
        full.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
        assert _render_last(partial) == pytest.approx(_render_last(full), abs=1e-6)


def test_set_track_output_bus_matches_set_track_lanes_with_the_same_final_config() -> None:
    for target in (2, None):
        with (
            _routing_engine([{"track_id": 10, "output_bus_id": 1, "sends": _SEND_TO_2}]) as partial,
            _routing_engine(
                [{"track_id": 10, "output_bus_id": target, "sends": _SEND_TO_2}]
            ) as full,
        ):
            partial.set_track_output_bus(10, target)
            assert _render_last(partial) == pytest.approx(_render_last(full), abs=1e-6)


def test_partial_lane_edits_refuse_bad_input_and_leave_the_lane_rendering() -> None:
    with _routing_engine([{"track_id": 10, "output_bus_id": 1, "sends": _SEND_TO_2}]) as engine:
        before = _render_last(engine)
        with pytest.raises(
            SonareValueError,
            match=(
                r"^RealtimeEngine\.set_track_sends: "
                r"track_id must be the id of a track in this engine$"
            ),
        ):
            engine.set_track_sends(99, [])
        with pytest.raises(
            SonareValueError,
            match=(
                r"^RealtimeEngine\.set_track_output_bus: "
                r"track_id must be the id of a track in this engine$"
            ),
        ):
            engine.set_track_output_bus(99, None)
        with pytest.raises(SonareError):
            engine.set_track_sends(10, [{"bus_id": 7}])
        with pytest.raises(SonareError):
            engine.set_track_sends(10, [{"bus_id": 2}, {"bus_id": 2}])
        with pytest.raises(SonareError):
            engine.set_track_sends(10, [{"bus_id": 2, "level_db": 30.0}])
        with pytest.raises(SonareError):
            engine.set_track_output_bus(10, 7)
        assert _render_last(engine) == pytest.approx(before, abs=1e-6)


def test_partial_lane_edits_refuse_every_track_before_the_first_lane_list() -> None:
    with (
        RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine,
        pytest.raises(
            SonareValueError,
            match=(
                r"^RealtimeEngine\.set_track_sends: "
                r"track_id must be the id of a track in this engine$"
            ),
        ),
    ):
        engine.set_track_sends(10, [])


def test_set_track_lanes_omitting_sends_clears_them() -> None:
    """Pins the replace semantics: a lane with a send is louder than the same lane without."""
    with _routing_engine([{"track_id": 10, "output_bus_id": 1, "sends": _SEND_TO_2}]) as engine:
        with_send = _render_last(engine)
        engine.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
        without_send = _render_last(engine)
    assert with_send > without_send * 1.1
    assert without_send > 0.1


# Track 10 is quiet program (-26 dB, under the duckers' -20 dB threshold) into
# bus 2, which carries a ducker keyed from bus 1. Track 30 is a loud key-only
# source into bus 1, whose -60 dB gain_db keeps it out of the mix while its key
# (tapped before gain_db) stays loud. Bus 1 carries a pass-through limiter so it
# has an insert 0 to key; the master carries a ducker. Mirrors make_keyed_rig()
# in the C ABI reference test.
def _make_keyed_rig() -> RealtimeEngine:
    engine = RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK)
    engine.set_clips(
        [
            EngineClip(
                id=1,
                track_id=10,
                channels=[[0.05] * _FRAMES],
                start_ppq=0.0,
                length_samples=_FRAMES,
            ),
            EngineClip(
                id=2, track_id=30, channels=[[1.0] * _FRAMES], start_ppq=0.0, length_samples=_FRAMES
            ),
        ]
    )
    engine.set_track_buses([{"bus_id": 1, "gain_db": -60.0}, {"bus_id": 2, "gain_db": 0.0}])
    engine.set_track_lanes(
        [
            {"track_id": 10, "output_bus_id": 2},
            {"track_id": 30, "output_bus_id": 1},
        ]
    )
    engine.set_bus_strip(1, scene_bus(_LIMITER_BUS_JSON))
    engine.set_bus_strip(2, scene_bus(_DUCKER_BUS_JSON))
    engine.set_strip("master", scene_strip(_DUCKER_MASTER_JSON))
    engine.set_bus_sidechain(2, 0, SidechainSource("bus", 1))
    return engine


def test_engine_bus_and_master_sidechain_duck_and_clear() -> None:
    # Unkeyed reference: the same rig with its bus key cleared before any render.
    reference = _make_keyed_rig()
    reference.set_bus_sidechain(2, 0, None)
    unkeyed = _render_last(reference)
    reference.close()
    assert unkeyed > 0.02

    engine = _make_keyed_rig()
    try:
        # Bus 1's loud key ducks bus 2, which carries nearly all of the mix.
        assert _render_last(engine) < unkeyed * 0.3
        # A track-sourced key on bus 2 ducks it the same way.
        engine.set_bus_sidechain(2, 0, SidechainSource("track", 30))
        assert _render_last(engine) < unkeyed * 0.3
        # The master ducker keyed from a track or a bus pulls the whole mix down.
        engine.set_strip_sidechain("master", 0, SidechainSource("track", 30))
        assert _render_last(engine) < unkeyed * 0.3
        engine.set_strip_sidechain("master", 0, SidechainSource("bus", 1))
        assert _render_last(engine) < unkeyed * 0.3

        # A None source clears a binding: the master one leaves bus 2 keyed alone,
        # and clearing that too returns the render to the unkeyed level once the
        # ducker's release has run out over several replays.
        engine.set_strip_sidechain("master", 0, None)
        assert _render_last(engine) < unkeyed * 0.3
        engine.set_bus_sidechain(2, 0, None)
        for _ in range(8):
            _render_last(engine)
        assert _render_last(engine) == pytest.approx(unkeyed, rel=1e-3)
    finally:
        engine.close()


def test_engine_bus_and_master_sidechain_reject_invalid_source_kind() -> None:
    engine = _make_keyed_rig()
    try:
        # An unrecognized string name is rejected before it ever reaches the C ABI.
        with pytest.raises(SonareValueError):
            engine.set_bus_sidechain(2, 0, SidechainSource("lane", 30))
        with pytest.raises(SonareValueError):
            engine.set_strip_sidechain("master", 0, SidechainSource("lane", 30))
        # An ordinal is not a token.
        for bad_kind in (0, 2, -1, True):
            with pytest.raises(TypeError):
                engine.set_bus_sidechain(2, 0, SidechainSource(bad_kind, 30))  # type: ignore[arg-type]
            with pytest.raises(TypeError):
                engine.set_strip_sidechain("master", 0, SidechainSource(bad_kind, 30))  # type: ignore[arg-type]
    finally:
        engine.close()


def test_sidechain_refusal_names_the_source_field_the_caller_wrote() -> None:
    engine = _make_keyed_rig()
    try:
        with pytest.raises(SonareValueError, match=r"source\.id must be the id of "):
            engine.set_bus_sidechain(2, 0, SidechainSource("bus", 99))
        with pytest.raises(SonareValueError, match=r"source\.id must be the id of "):
            engine.set_strip_sidechain("master", 0, SidechainSource("track", 99))
    finally:
        engine.close()


def test_sidechain_source_is_a_record_and_none_is_the_only_removal() -> None:
    engine = _make_keyed_rig()
    try:
        with pytest.raises(SonareValueError, match="must not be 0"):
            engine.set_bus_sidechain(2, 0, SidechainSource("bus", 0))
        with pytest.raises(SonareValueError, match="must not be 0"):
            engine.set_strip_sidechain("master", 0, SidechainSource("track", 0))
        with pytest.raises(SonareValueError, match="kind 'track'"):
            engine.set_strip_sidechain(10, 0, SidechainSource("bus", 1))
        with pytest.raises(SonareValueError, match="kind 'track'"):
            engine.can_set_strip_sidechain(10, 0, SidechainSource("bus", 1))
        with pytest.raises(TypeError, match="SidechainSource"):
            engine.set_bus_sidechain(2, 0, ("bus", 1))  # type: ignore[arg-type]
        with pytest.raises(TypeError, match="SidechainSource"):
            engine.can_set_bus_sidechain(2, 0, None)  # type: ignore[arg-type]
        with pytest.raises(SonareValueError, match="'master'"):
            engine.set_strip_sidechain("bus", 0, None)
    finally:
        engine.close()


def test_an_optional_bus_is_none_and_bus_id_zero_is_refused() -> None:
    with _routing_engine([{"track_id": 10, "output_bus_id": 1}]) as engine:
        with pytest.raises(SonareValueError, match="bus_id"):
            engine.set_track_output_bus(10, 0)
        with pytest.raises(SonareValueError, match="output_bus_id"):
            engine.set_track_lanes([{"track_id": 10, "output_bus_id": 0}])
        with pytest.raises(SonareValueError, match="output_bus_id"):
            engine.set_track_buses([{"bus_id": 1, "output_bus_id": 0}, {"bus_id": 2}])
        engine.set_track_output_bus(10, None)
        engine.set_track_lanes([{"track_id": 10, "output_bus_id": None}])


def test_sidechain_source_kind_value_takes_exact_tokens() -> None:
    assert [_sidechain_source_kind_value(kind, "kind") for kind in ("track", "bus")] == [0, 1]
    for other in ("Track", "TRACK", "Bus", "lane"):
        with pytest.raises(SonareValueError, match="track"):
            _sidechain_source_kind_value(other, "kind")


def _midi1_word(status: int, channel: int, word0: int, word1: int) -> int:
    return (0x2 << 28) | ((status & 0xF) << 20) | ((channel & 0xF) << 16) | (word0 << 8) | word1


_MIDI_DESTINATION = 9


def _held_note_events() -> list[EngineMidiEvent]:
    return [EngineMidiEvent(0, word0=_midi1_word(0x9, 0, 60, 100), word_count=1)]


def _render_midi_clip(clip: EngineMidiClipSchedule) -> list[float]:
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(gain=0.5), _MIDI_DESTINATION)
        engine.set_midi_clips([clip])
        engine.play()
        out: list[float] = []
        for _ in range(8):
            out.extend(engine.process([[0.0] * _BLOCK, [0.0] * _BLOCK])[0])
        return out


def test_engine_midi_clip_gain_scales_destination_render() -> None:
    events = _held_note_events()
    unity = EngineMidiClipSchedule(
        id=42,
        track_id=_MIDI_DESTINATION,
        destination_id=_MIDI_DESTINATION,
        length_samples=_FRAMES,
        events=events,
    )
    assert unity.gain == 1.0  # default gain is unity, matching a zero-init caller opt-in
    half = EngineMidiClipSchedule(
        id=42,
        track_id=_MIDI_DESTINATION,
        destination_id=_MIDI_DESTINATION,
        length_samples=_FRAMES,
        events=events,
        gain=0.5,
    )
    a = _render_midi_clip(unity)
    b = _render_midi_clip(half)
    peak = max(abs(v) for v in a)
    assert peak > 0.01
    assert len(a) == len(b)
    for av, bv in zip(a, b, strict=True):
        assert bv == pytest.approx(0.5 * av, abs=1e-6)


def test_engine_midi_clip_fade_out_on_open_ended_clip_is_refused() -> None:
    events = _held_note_events()
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(gain=0.5), _MIDI_DESTINATION)

        clip = EngineMidiClipSchedule(
            id=1,
            track_id=_MIDI_DESTINATION,
            destination_id=_MIDI_DESTINATION,
            events=events,
            fade_out_samples=256,
        )
        with pytest.raises(SonareValueError) as exc:
            engine.set_midi_clips([clip])
        assert exc.value.code == 4

        # A fade-in has an end to ramp towards even on an open-ended clip.
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=1,
                    track_id=_MIDI_DESTINATION,
                    destination_id=_MIDI_DESTINATION,
                    events=events,
                    fade_in_samples=256,
                )
            ]
        )


def test_grown_structs_match_abi_layout_snapshot() -> None:
    """ctypes sizeof of the two grown mirrors matches tools/abi/abi-layout.json."""
    layout_path = Path(__file__).resolve().parents[3] / "tools" / "abi" / "abi-layout.json"
    layout = json.loads(layout_path.read_text())
    assert ctypes.sizeof(_RawEngineBus) == layout["SonareEngineBus"]["size"]
    assert ctypes.sizeof(_RawMidiClipSchedule) == layout["SonareEngineMidiClipSchedule"]["size"]


def test_lane_sidechain_refuses_self_key_and_cycles_but_keeps_valid_bindings() -> None:
    engine = RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK)
    try:
        engine.set_track_lanes([{"track_id": 10}, {"track_id": 20}])
        for track_id in (10, 20):
            engine.set_strip(track_id, scene_strip(_strip_json(track_id, [_PASS_LIMITER])))
        engine.set_strip_sidechain(10, 0, SidechainSource("track", 20))
        with pytest.raises(SonareError):
            engine.set_strip_sidechain(10, 0, SidechainSource("track", 10))
        # 20 keyed from 10 would close the 10 <-> 20 cycle over the bindings.
        with pytest.raises(SonareError):
            engine.set_strip_sidechain(20, 0, SidechainSource("track", 10))
        # The refused bindings changed nothing: the first one still stands, and
        # clearing it frees the opposite direction.
        engine.set_strip_sidechain(10, 0, None)
        engine.set_strip_sidechain(20, 0, SidechainSource("track", 10))
        with pytest.raises(SonareError):
            engine.set_strip_sidechain(10, 0, SidechainSource("track", 20))
    finally:
        engine.close()


_PASS_LIMITER = ("dynamics.limiter", {"thresholdDb": 24.0, "releaseMs": 50.0})


def _strip_json(strip_id: int, inserts: list[tuple[str, dict[str, float]]]) -> str:
    return json.dumps(
        {
            "version": 2,
            "strips": [
                {
                    "id": str(strip_id),
                    "inserts": [
                        {"slot": "pre", "processor": name, "params": params}
                        for name, params in inserts
                    ],
                }
            ],
            "buses": [],
            "connections": [],
        }
    )


def test_can_set_sidechain_reports_each_refusal_and_matches_the_setters() -> None:
    engine = _make_keyed_rig()
    seen: set[str | None] = set()

    def check(result: SidechainCheck, reason: str | None) -> None:
        assert result.ok is (reason is None)
        assert result.reason == reason
        seen.add(result.reason)

    try:
        # Lane keys; each lane carries one insert to key.
        for track_id in (10, 30):
            engine.set_strip(track_id, scene_strip(_strip_json(track_id, [_PASS_LIMITER])))
        check(engine.can_set_strip_sidechain(0, 0, SidechainSource("track", 30)), "invalidTarget")
        check(
            engine.can_set_strip_sidechain(10, 1, SidechainSource("track", 30)), "insertOutOfRange"
        )
        check(
            engine.can_set_strip_sidechain(10, 0, SidechainSource("track", 99)), "undeclaredSource"
        )
        with pytest.raises(SonareError):
            engine.set_strip_sidechain(10, 1, SidechainSource("track", 30))
        with pytest.raises(SonareError):
            engine.set_strip_sidechain(10, 0, SidechainSource("track", 99))
        check(engine.can_set_strip_sidechain(10, 0, SidechainSource("track", 10)), "selfKey")
        check(engine.can_set_strip_sidechain(10, 0, SidechainSource("track", 30)), None)
        engine.set_strip_sidechain(10, 0, SidechainSource("track", 30))
        check(engine.can_set_strip_sidechain(30, 0, SidechainSource("track", 10)), "cycle")
        with pytest.raises(SonareError):
            engine.set_strip_sidechain(30, 0, SidechainSource("track", 10))
        # Bus keys; the rig already keys bus 2 insert 0 from bus 1.
        check(engine.can_set_bus_sidechain(9, 0, SidechainSource("track", 10)), "invalidTarget")
        check(engine.can_set_bus_sidechain(1, 1, SidechainSource("track", 30)), "insertOutOfRange")
        check(engine.can_set_bus_sidechain(1, 0, SidechainSource("track", 99)), "undeclaredSource")
        with pytest.raises(TypeError):
            engine.can_set_bus_sidechain(1, 0, SidechainSource(2, 30))  # type: ignore[arg-type]
        check(engine.can_set_bus_sidechain(1, 0, SidechainSource("bus", 1)), "selfKey")
        check(engine.can_set_bus_sidechain(1, 0, SidechainSource("bus", 2)), "cycle")
        check(engine.can_set_bus_sidechain(2, 0, SidechainSource("track", 30)), None)
        # Master keys.
        check(
            engine.can_set_strip_sidechain("master", 1, SidechainSource("track", 30)),
            "insertOutOfRange",
        )
        check(
            engine.can_set_strip_sidechain("master", 0, SidechainSource("track", 99)),
            "undeclaredSource",
        )
        with pytest.raises(TypeError):
            engine.can_set_strip_sidechain("master", 0, SidechainSource(2, 30))  # type: ignore[arg-type]
        check(engine.can_set_strip_sidechain("master", 0, SidechainSource("bus", 1)), None)
        # Queries leave the bindings alone: the refused reverse bus key stays refused.
        check(engine.can_set_bus_sidechain(1, 0, SidechainSource("bus", 2)), "cycle")
    finally:
        engine.close()

    # Table full: every slot of the binding table taken by lane 10, then a new key.
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as full:
        full.set_track_lanes([{"track_id": 10}, {"track_id": 30}])
        full.set_strip(10, scene_strip(_strip_json(10, [_PASS_LIMITER] * 33)))
        for insert in range(32):
            full.set_strip_sidechain(10, insert, SidechainSource("track", 30))
        check(full.can_set_strip_sidechain(10, 32, SidechainSource("track", 30)), "tableFull")
        with pytest.raises(SonareError):
            full.set_strip_sidechain(10, 32, SidechainSource("track", 30))

    # Plan refused: lane 20 reports 4 s (192000 samples) of lookahead, so keying
    # lane 30's insert 1 from it needs an alignment past the delay ceiling.
    limiter = ("dynamics.limiter", {"thresholdDb": 24.0, "releaseMs": 50.0})
    long_limiter = (limiter[0], {**limiter[1], "lookaheadMs": 4000.0})
    ducker = (
        "dynamics.duckingProcessor",
        {"thresholdDb": -20.0, "ratio": 20.0, "attackMs": 0.05, "releaseMs": 80.0, "rangeDb": 30.0},
    )
    with RealtimeEngine(sample_rate=48000, max_block_size=_BLOCK) as plan:
        plan.set_track_lanes([{"track_id": 10}, {"track_id": 20}, {"track_id": 30}])
        plan.set_strip(20, scene_strip(_strip_json(20, [long_limiter])))
        plan.set_strip(
            30,
            scene_strip(
                _strip_json(30, [(limiter[0], {**limiter[1], "lookaheadMs": 1.0}), ducker])
            ),
        )
        check(plan.can_set_strip_sidechain(30, 1, SidechainSource("track", 20)), "planRefused")
        with pytest.raises(SonareError):
            plan.set_strip_sidechain(30, 1, SidechainSource("track", 20))
        check(plan.can_set_strip_sidechain(30, 1, SidechainSource("track", 10)), None)

    # The facade refuses a bad source kind before the C ABI can report it, so the
    # code-to-reason table is exercised directly.
    check(_sidechain_check(4), "invalidSourceKind")
    assert seen == {
        None,
        "invalidTarget",
        "insertOutOfRange",
        "undeclaredSource",
        "invalidSourceKind",
        "selfKey",
        "cycle",
        "tableFull",
        "planRefused",
    }


def test_sidechain_check_is_a_frozen_value() -> None:
    check = SidechainCheck(ok=False, reason="cycle")
    assert check == SidechainCheck(False, "cycle")
    assert SidechainCheck(True).reason is None
    with pytest.raises(AttributeError):
        check.ok = True  # type: ignore[misc]
