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
    SidechainSourceKind,
    SonareError,
    SonareValueError,
)
from libsonare._ffi_types_core import SonareEngineBus as _RawEngineBus
from libsonare._ffi_types_mastering_project import (
    SonareEngineMidiClipSchedule as _RawMidiClipSchedule,
)
from libsonare._runtime import _sidechain_source_kind_value

_FRAMES = 256 * 40
_BLOCK = 256

_DUCKER_BUS_JSON = (
    '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre",'
    '"processor":"dynamics.duckingProcessor","params":"{\\"thresholdDb\\":-20,'
    '\\"ratio\\":20,\\"attackMs\\":0.05,\\"releaseMs\\":80,\\"rangeDb\\":30}"}]}],'
    '"connections":[]}'
)
_LIMITER_BUS_JSON = (
    '{"version":1,"strips":[],"buses":[{"id":"1","inserts":[{"slot":"pre",'
    '"processor":"dynamics.limiter","params":"{\\"thresholdDb\\":24,'
    '\\"lookaheadMs\\":0,\\"releaseMs\\":50}"}]}],"connections":[]}'
)
_DUCKER_MASTER_JSON = (
    '{"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre",'
    '"processor":"dynamics.duckingProcessor","params":"{\\"thresholdDb\\":-20,'
    '\\"ratio\\":20,\\"attackMs\\":0.05,\\"releaseMs\\":80,\\"rangeDb\\":30}"}]}],'
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
    with RealtimeEngine(sample_rate=48000.0, max_block_size=_BLOCK) as engine:
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
        engine.set_track_lanes([10])
        level = _render_last(engine)
        assert level > 0.5
        return level


def test_engine_bus_output_bus_id_routes_into_another_bus() -> None:
    x = _direct_level()
    half = 10.0 ** (-6.0 / 20.0)

    def run(bus1_output: int) -> float:
        with RealtimeEngine(sample_rate=48000.0, max_block_size=_BLOCK) as engine:
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

    assert run(0) == pytest.approx(x, rel=1e-3)
    # Through bus 2, its -6 dB gain_db applies on top.
    assert run(2) == pytest.approx(x * half, rel=1e-3)


def test_engine_bus_sends_tap_before_or_after_gain_db() -> None:
    x = _direct_level()
    g1 = 10.0 ** (-12.0 / 20.0)

    def run(timing: str, level_db: float, enabled: bool) -> float:
        with RealtimeEngine(sample_rate=48000.0, max_block_size=_BLOCK) as engine:
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
                                "timing": timing,
                            }
                        ],
                    },
                    {"bus_id": 2, "gain_db": 0.0},
                ]
            )
            engine.set_track_lanes([{"track_id": 10, "output_bus_id": 1}])
            return _render_last(engine)

    assert run("pre", 0.0, True) == pytest.approx(x * (g1 + 1.0), rel=1e-3)
    assert run("post", 0.0, True) == pytest.approx(x * 2.0 * g1, rel=1e-3)
    half = 10.0 ** (-6.0 / 20.0)
    assert run("pre", -6.0, True) == pytest.approx(x * (g1 + half), rel=1e-3)
    assert run("post", 0.0, False) == pytest.approx(x * g1, rel=1e-2)


# Track 10 is quiet program (-26 dB, under the duckers' -20 dB threshold) into
# bus 2, which carries a ducker keyed from bus 1. Track 30 is a loud key-only
# source into bus 1, whose -60 dB gain_db keeps it out of the mix while its key
# (tapped before gain_db) stays loud. Bus 1 carries a pass-through limiter so it
# has an insert 0 to key; the master carries a ducker. Mirrors make_keyed_rig()
# in the C ABI reference test.
def _make_keyed_rig() -> RealtimeEngine:
    engine = RealtimeEngine(sample_rate=48000.0, max_block_size=_BLOCK)
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
    engine.set_bus_strip_json(1, _LIMITER_BUS_JSON)
    engine.set_bus_strip_json(2, _DUCKER_BUS_JSON)
    engine.set_master_strip_json(_DUCKER_MASTER_JSON)
    engine.set_bus_sidechain(2, 0, "bus", 1)
    return engine


def test_engine_bus_and_master_sidechain_duck_and_clear() -> None:
    # Unkeyed reference: the same rig with its bus key cleared before any render.
    reference = _make_keyed_rig()
    reference.set_bus_sidechain(2, 0, "bus", 0)
    unkeyed = _render_last(reference)
    reference.destroy()
    assert unkeyed > 0.02

    engine = _make_keyed_rig()
    try:
        # Bus 1's loud key ducks bus 2, which carries nearly all of the mix.
        assert _render_last(engine) < unkeyed * 0.3
        # A track-sourced key on bus 2 ducks it the same way.
        engine.set_bus_sidechain(2, 0, "track", 30)
        assert _render_last(engine) < unkeyed * 0.3
        # The master ducker keyed from a track or a bus pulls the whole mix down.
        engine.set_master_sidechain(0, "track", 30)
        assert _render_last(engine) < unkeyed * 0.3
        engine.set_master_sidechain(0, "bus", 1)
        assert _render_last(engine) < unkeyed * 0.3

        # source_id 0 clears a binding: the master one leaves bus 2 keyed alone,
        # and clearing that too returns the render to the unkeyed level once the
        # ducker's release has run out over several replays.
        engine.set_master_sidechain(0, "bus", 0)
        assert _render_last(engine) < unkeyed * 0.3
        engine.set_bus_sidechain(2, 0, "track", 0)
        for _ in range(8):
            _render_last(engine)
        assert _render_last(engine) == pytest.approx(unkeyed, rel=1e-3)
    finally:
        engine.destroy()


def test_engine_bus_and_master_sidechain_reject_invalid_source_kind() -> None:
    engine = _make_keyed_rig()
    try:
        # An unrecognized string name is rejected before it ever reaches the C ABI.
        with pytest.raises(SonareValueError):
            engine.set_bus_sidechain(2, 0, "lane", 30)
        with pytest.raises(SonareValueError):
            engine.set_master_sidechain(0, "lane", 30)
        # An out-of-range ordinal reaches the C ABI's own source_kind check.
        for bad_kind in (2, -1):
            with pytest.raises(SonareError) as bus_exc:
                engine.set_bus_sidechain(2, 0, bad_kind, 30)
            assert bus_exc.value.code == 4
            with pytest.raises(SonareError) as master_exc:
                engine.set_master_sidechain(0, bad_kind, 30)
            assert master_exc.value.code == 4
    finally:
        engine.destroy()


@pytest.mark.parametrize(
    ("kind", "expected"),
    [
        ("track", 0),
        ("Track", 0),
        ("TRACK", 0),
        ("bus", 1),
        ("Bus", 1),
        (0, 0),
        (1, 1),
        (SidechainSourceKind.TRACK, 0),
        (SidechainSourceKind.BUS, 1),
    ],
)
def test_sidechain_source_kind_value_accepts_case_insensitive_aliases(
    kind: SidechainSourceKind | str | int, expected: int
) -> None:
    assert _sidechain_source_kind_value(kind) == expected


def _midi1_word(status: int, channel: int, data0: int, data1: int) -> int:
    return (0x2 << 28) | ((status & 0xF) << 20) | ((channel & 0xF) << 16) | (data0 << 8) | data1


_MIDI_DESTINATION = 9


def _held_note_events() -> list[EngineMidiEvent]:
    return [EngineMidiEvent(0, word0=_midi1_word(0x9, 0, 60, 100), word_count=1)]


def _render_midi_clip(clip: EngineMidiClipSchedule) -> list[float]:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=_BLOCK) as engine:
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
    with RealtimeEngine(sample_rate=48000.0, max_block_size=_BLOCK) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(gain=0.5), _MIDI_DESTINATION)

        clip = EngineMidiClipSchedule(
            id=1,
            track_id=_MIDI_DESTINATION,
            destination_id=_MIDI_DESTINATION,
            length_samples=0,
            events=events,
            fade_out_samples=256,
        )
        with pytest.raises(SonareError) as exc:
            engine.set_midi_clips([clip])
        assert exc.value.code == 4

        # A fade-in has an end to ramp towards even on an open-ended clip.
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=1,
                    track_id=_MIDI_DESTINATION,
                    destination_id=_MIDI_DESTINATION,
                    length_samples=0,
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
