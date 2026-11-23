"""MIDI 1.0 <-> MIDI 2.0 value scalers: the module functions return what the C ABI computes."""

from __future__ import annotations

import pytest

from libsonare import (
    SonareValueError,
    midi2_control_to_midi1,
    midi2_control_to_midi1_control14,
    midi2_note_on_velocity_to_midi1,
    midi2_pitch_bend_to_midi1,
    midi2_velocity_to_midi1,
    midi_control14_to_midi2,
    midi_control_to_midi2,
    midi_note_on_velocity_to_midi2,
    midi_pitch_bend_to_midi2,
    midi_velocity_to_midi2,
)

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")


def test_widening_lands_min_center_and_max() -> None:
    assert [midi_velocity_to_midi2(v) for v in (0, 64, 127)] == [0, 0x8000, 0xFFFF]
    assert midi_note_on_velocity_to_midi2(1) == 0x0200
    assert [midi_control_to_midi2(v) for v in (0, 64, 127)] == [0, 0x80000000, 0xFFFFFFFF]
    assert [midi_control14_to_midi2(v) for v in (0, 8192, 16383)] == [0, 0x80000000, 0xFFFFFFFF]
    assert [midi_pitch_bend_to_midi2(v) for v in (0, 8192, 16383)] == [0, 0x80000000, 0xFFFFFFFF]


def test_narrowing_truncates_and_a_note_on_never_reads_zero() -> None:
    assert midi2_velocity_to_midi1(0x01FF) == 0
    assert midi2_note_on_velocity_to_midi1(0x01FF) == 1
    assert midi2_note_on_velocity_to_midi1(0) == 1
    assert midi2_control_to_midi1((64 << 25) - 1) == 63
    assert midi2_control_to_midi1_control14((8192 << 18) - 1) == 8191
    assert midi2_pitch_bend_to_midi1(0xFFFFFFFF) == 16383


def test_widen_then_narrow_returns_every_input() -> None:
    for v in range(128):
        assert midi2_velocity_to_midi1(midi_velocity_to_midi2(v)) == v
        assert midi2_control_to_midi1(midi_control_to_midi2(v)) == v
    for v in range(1, 128):
        assert midi2_note_on_velocity_to_midi1(midi_note_on_velocity_to_midi2(v)) == v
    for v in range(0, 16384, 7):
        assert midi2_control_to_midi1_control14(midi_control14_to_midi2(v)) == v
        assert midi2_pitch_bend_to_midi1(midi_pitch_bend_to_midi2(v)) == v


@pytest.mark.parametrize(
    ("call", "message"),
    [
        (lambda: midi_velocity_to_midi2(128), "velocity must be an integer in [0, 127]"),
        (lambda: midi_note_on_velocity_to_midi2(0), "velocity must be an integer in [1, 127]"),
        (lambda: midi_control_to_midi2(200), "value must be an integer in [0, 127]"),
        (lambda: midi_control14_to_midi2(16384), "value14 must be an integer in [0, 16383]"),
        (lambda: midi_pitch_bend_to_midi2(16384), "bend14 must be an integer in [0, 16383]"),
        (lambda: midi_velocity_to_midi2(300), "velocity must be an integer in [0, 127]"),
        (lambda: midi2_velocity_to_midi1(-1), "velocity16 must be an integer in [0, 65535]"),
        (lambda: midi2_control_to_midi1(1 << 32), "value32 must be an integer in [0, 4294967295]"),
        (lambda: midi_pitch_bend_to_midi2(1.5), "bend14 must be an integer"),
    ],
)
def test_an_out_of_domain_value_is_refused_by_name(call: object, message: str) -> None:
    with pytest.raises(SonareValueError) as exc:
        call()  # type: ignore[operator]
    assert message in str(exc.value)


def test_a_non_number_is_a_type_error() -> None:
    with pytest.raises(TypeError):
        midi_control_to_midi2("64")  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        midi2_pitch_bend_to_midi1(True)
