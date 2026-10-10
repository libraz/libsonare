"""Live typed program change on the realtime engine."""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import ErrorCode, Project, RealtimeEngine, SonareError, SonareValueError

DESTINATION = 7
BLOCK = 128
BLOCKS = 32


def _render(prepare) -> np.ndarray:
    """Render one held note on the fallback Sf2Player after ``prepare(engine)``."""
    with RealtimeEngine(48000.0, BLOCK) as engine:
        engine.set_sf2_instrument(destination_id=DESTINATION)
        prepare(engine)
        engine.push_midi_note_on(DESTINATION, 0, 0, 60, 100)
        out = [
            np.asarray(engine.process([[0.0] * BLOCK, [0.0] * BLOCK])[0], dtype=np.float64)
            for _ in range(BLOCKS)
        ]
    return np.concatenate(out)


def _render_input(prepare) -> np.ndarray:
    with RealtimeEngine(48000.0, BLOCK) as engine:
        engine.set_sf2_instrument(destination_id=DESTINATION)
        engine.set_midi_input_source(DESTINATION)
        prepare(engine)
        engine.push_midi_input_note_on(0, 0, 60, 100, 0)
        out = [
            np.asarray(engine.process([[0.0] * BLOCK, [0.0] * BLOCK])[0], dtype=np.float64)
            for _ in range(BLOCKS)
        ]
    return np.concatenate(out)


def _bank8_words() -> list[int]:
    _, w0, w1 = Project.midi2_program(0.0, 0, 0, 0, True, 8, 0)
    return [w0, w1]


def test_push_midi_program_matches_raw_ump_and_reaches_the_voice() -> None:
    typed = _render(lambda e: e.push_midi_program(DESTINATION, 0, 0, 0, True, 8, 0))
    raw = _render(lambda e: e.push_midi_ump(DESTINATION, _bank8_words()))
    bank0 = _render(lambda e: e.push_midi_program(DESTINATION, 0, 0, 0, True, 0, 0))
    none = _render(lambda e: None)

    assert np.max(np.abs(typed)) > 0.0
    assert np.array_equal(typed, raw)
    assert not np.array_equal(typed, bank0)
    assert not np.array_equal(typed, none)


def test_push_midi_program_default_bank_keeps_bank() -> None:
    plain = _render(lambda e: e.push_midi_program(DESTINATION, 0, 0, 0))
    bank0 = _render(lambda e: e.push_midi_program(DESTINATION, 0, 0, 0, True, 0, 0))
    assert np.array_equal(plain, bank0)


def test_push_midi_input_program_matches_raw_ump_and_reaches_the_voice() -> None:
    typed = _render_input(lambda e: e.push_midi_input_program(0, 0, 0, True, 8, 0))
    raw = _render_input(lambda e: e.push_midi_input_ump(_bank8_words()))
    none = _render_input(lambda e: None)

    assert np.max(np.abs(typed)) > 0.0
    assert np.array_equal(typed, raw)
    assert not np.array_equal(typed, none)


@pytest.mark.parametrize("bank_msb,bank_lsb", [(1, 0), (0, 1), (8, 3)])
def test_bank_without_bank_valid_is_refused(bank_msb: int, bank_lsb: int) -> None:
    with RealtimeEngine(48000.0, BLOCK) as engine:
        engine.set_sf2_instrument(destination_id=DESTINATION)
        engine.set_midi_input_source(DESTINATION)
        with pytest.raises(SonareError) as push:
            engine.push_midi_program(DESTINATION, 0, 0, 0, False, bank_msb, bank_lsb)
        assert push.value.code == int(ErrorCode.INVALID_PARAMETER)
        with pytest.raises(SonareError) as live:
            engine.push_midi_input_program(0, 0, 0, False, bank_msb, bank_lsb)
        assert live.value.code == int(ErrorCode.INVALID_PARAMETER)
        assert engine.midi_input_pending_count() == 0
    with pytest.raises(SonareError) as builder:
        Project.midi2_program(0.0, 0, 0, 0, False, bank_msb, bank_lsb)
    assert builder.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_midi2_program_with_bank_valid_still_packs_a_bank() -> None:
    _, w0, w1 = Project.midi2_program(0.0, 0, 0, 5, True, 8, 3)
    assert [w0, w1] == [0x40C00001, 0x05000803]


@pytest.mark.parametrize(
    "kwargs",
    [
        {"group": 16},
        {"channel": 16},
        {"program": 128},
        {"bank_valid": True, "bank_msb": 128},
        {"bank_valid": True, "bank_lsb": 128},
    ],
)
def test_out_of_range_is_refused_like_push_midi_cc(kwargs) -> None:
    args = {"group": 0, "channel": 0, "program": 0} | kwargs
    with RealtimeEngine(48000.0, BLOCK) as engine:
        engine.set_sf2_instrument(destination_id=DESTINATION)
        engine.set_midi_input_source(DESTINATION)
        with pytest.raises(SonareError) as push:
            engine.push_midi_program(DESTINATION, **args)
        assert push.value.code == int(ErrorCode.INVALID_PARAMETER)
        with pytest.raises(SonareError) as live:
            engine.push_midi_input_program(**args)
        assert live.value.code == int(ErrorCode.INVALID_PARAMETER)
        # The sibling shape: the same out-of-range group is a C refusal for a CC too.
        with pytest.raises(SonareError):
            engine.push_midi_cc(DESTINATION, 16, 0, 1, 0)


def test_values_beyond_uint8_are_narrowing_errors() -> None:
    with RealtimeEngine(48000.0, BLOCK) as engine:
        engine.set_sf2_instrument(destination_id=DESTINATION)
        with pytest.raises(SonareValueError):
            engine.push_midi_program(DESTINATION, 0, 0, 256)
        with pytest.raises(SonareValueError):
            engine.push_midi_input_program(0, 0, -1)


def test_input_program_requires_the_input_source() -> None:
    with RealtimeEngine(48000.0, BLOCK) as engine:
        engine.set_sf2_instrument(destination_id=DESTINATION)
        with pytest.raises(SonareError):
            engine.push_midi_input_program(0, 0, 0)
