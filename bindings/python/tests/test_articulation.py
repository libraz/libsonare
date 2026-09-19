"""Articulation binding tests: the enum name table, the per-channel round trip,
the three refusals it has to keep apart, and that a mode set through Python
reaches the sound.

The last one is the point. A slurred pair of notes and a retriggered pair are
the same two pitches, so every call here can return success with the mode
dropped on the floor and a round trip alone would not say so. One case renders
both and requires them to differ; another takes the engine that declines to be
carried and requires the fallback counter to move, which is the only thing
separating a refusal from a mode that was never set.
"""

from __future__ import annotations

import pytest

from libsonare import (
    ErrorCode,
    RealtimeEngine,
    SonareError,
    SonareValueError,
    SynthPatch,
    synth_enum_tables,
)
from libsonare._project import SYNTH_ENUM_TABLES

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

SAMPLE_RATE = 48000.0
BLOCK = 128
DESTINATION = 3


def _synth_engine(engine_mode: str) -> RealtimeEngine:
    """An engine with a NativeSynth on DESTINATION running ``engine_mode``."""
    engine = RealtimeEngine()
    engine.prepare(SAMPLE_RATE, BLOCK, 16, 16)
    engine.set_synth_instrument(SynthPatch(engine_mode=engine_mode), destination_id=DESTINATION)
    return engine


def _render_blocks(engine: RealtimeEngine, blocks: int) -> list[float]:
    silence = [[0.0] * BLOCK, [0.0] * BLOCK]
    out: list[float] = []
    for _ in range(blocks):
        out.extend(engine.process(silence)[0])
    return out


def _render_slur(engine_mode: str, articulation: str) -> tuple[list[float], int]:
    """C4 held, then G4 on top of it, then the LATE note-off of C4 -- the order
    a player's slur actually sends, and the one a mode that ignores the overlap
    cannot be told apart from by the first half alone. Returns the left channel
    and the legato fallbacks the destination counted."""
    with _synth_engine(engine_mode) as engine:
        engine.set_articulation(DESTINATION, 0, articulation)
        engine.push_midi_note_on(DESTINATION, 0, 0, 60, 100)
        out = _render_blocks(engine, 96)
        engine.push_midi_note_on(DESTINATION, 0, 0, 67, 100)
        engine.push_midi_note_off(DESTINATION, 0, 0, 60)
        out.extend(_render_blocks(engine, 128))
        return out, engine.legato_fallback_count(DESTINATION)


def test_the_articulation_name_table_comes_from_the_c_abi() -> None:
    tables = synth_enum_tables()
    # Named rather than counted alone: a table that lost an entry and gained an
    # empty one keeps its count, and these names are what this surface spells
    # the ordinals as.
    assert tables["articulations"] == ("poly", "mono-retrigger", "mono-legato")
    assert tables["articulations"] == SYNTH_ENUM_TABLES["articulations"]


def test_articulation_round_trips_per_channel() -> None:
    with _synth_engine("reed") as engine:
        assert engine.articulation(DESTINATION, 0) == "poly"

        engine.set_articulation(DESTINATION, 0, "mono-legato")
        assert engine.articulation(DESTINATION, 0) == "mono-legato"

        # A second channel is untouched by the first, so the mode is per channel
        # rather than per instrument -- a host slurring one part must not slur
        # the rest of the rack.
        assert engine.articulation(DESTINATION, 1) == "poly"
        engine.set_articulation(DESTINATION, 1, "mono-retrigger")
        assert engine.articulation(DESTINATION, 1) == "mono-retrigger"
        assert engine.articulation(DESTINATION, 0) == "mono-legato"

        # The ordinal is accepted wherever the name is, and reads back as the
        # name, so a caller holding a C enum value and one holding a spelling
        # reach the same mode.
        for ordinal, name in enumerate(SYNTH_ENUM_TABLES["articulations"]):
            engine.set_articulation(DESTINATION, 2, ordinal)
            assert engine.articulation(DESTINATION, 2) == name

        # Nothing has been asked for and declined yet, so the counter starts
        # where a later case can see it move.
        assert engine.legato_fallback_count(DESTINATION) == 0


def test_an_unknown_articulation_spelling_or_ordinal_is_refused_by_the_binding() -> None:
    """The refusals the binding makes before the C ABI is reached.

    Each one asserts :class:`SonareValueError` rather than :class:`SonareError`,
    which is what says the binding refused it: the C ABI range-checks the same
    ordinals and would raise the plain base class, so the two sides are
    indistinguishable here unless the exception type is the one asserted.
    """
    with _synth_engine("reed") as engine:
        with pytest.raises(SonareValueError, match="articulation"):
            engine.set_articulation(DESTINATION, 0, "no-such-articulation")
        # An ordinal past the enum, taken from the table's own length so it
        # follows a value being added rather than pinning today's count.
        with pytest.raises(SonareValueError, match="articulation"):
            engine.set_articulation(DESTINATION, 0, len(SYNTH_ENUM_TABLES["articulations"]))
        with pytest.raises(SonareValueError, match="articulation"):
            engine.set_articulation(DESTINATION, 0, -1)
        # None of the above took: the mode is still what the instrument started
        # at, so a refusal changes nothing rather than clamping to poly.
        assert engine.articulation(DESTINATION, 0) == "poly"


def test_articulation_keeps_its_three_refusals_apart() -> None:
    with _synth_engine("reed") as engine:
        # A channel past 15 is the C ABI's refusal, not the binding's: 16 fits
        # the uint8 the binding narrows to, so it reaches the library.
        with pytest.raises(SonareError) as channel_refusal:
            engine.set_articulation(DESTINATION, 16, "poly")
        assert not isinstance(channel_refusal.value, SonareValueError)
        assert channel_refusal.value.code == int(ErrorCode.INVALID_PARAMETER)
        with pytest.raises(SonareError):
            engine.articulation(DESTINATION, 16)
        assert engine.articulation(DESTINATION, 0) == "poly"

        # A destination nothing is bound to, and a destination holding an
        # instrument with no articulation of its own, are different answers.
        # Both would otherwise read as "the call worked" to a caller that only
        # checks for the absence of an exception.
        for unbound in (
            lambda: engine.set_articulation(5, 0, "mono-legato"),
            lambda: engine.articulation(5, 0),
            lambda: engine.legato_fallback_count(5),
        ):
            with pytest.raises(SonareError) as unbound_refusal:
                unbound()
            assert unbound_refusal.value.code == int(ErrorCode.INVALID_PARAMETER)

        # The built-in oscillator synth has nowhere to put an articulation, so
        # the same calls now answer NOT_SUPPORTED instead. The counter is in
        # that list rather than answering zero: an instrument that never had an
        # articulation has refused nothing, and a host reading that zero would
        # read it as "every slur took".
        engine.set_builtin_instrument(destination_id=5)
        for unsupported in (
            lambda: engine.set_articulation(5, 0, "mono-legato"),
            lambda: engine.articulation(5, 0),
            lambda: engine.legato_fallback_count(5),
        ):
            with pytest.raises(SonareError) as unsupported_refusal:
                unsupported()
            assert unsupported_refusal.value.code == int(ErrorCode.NOT_SUPPORTED)


def test_an_articulation_set_through_python_reaches_the_sound() -> None:
    slurred, slurred_fallbacks = _render_slur("reed", "mono-legato")
    retriggered, retriggered_fallbacks = _render_slur("reed", "mono-retrigger")

    # The renders carry energy, so "they differ" is not two kinds of silence.
    assert max(abs(sample) for sample in retriggered) > 0.0
    assert len(slurred) == len(retriggered)
    assert slurred != retriggered
    # A reed accepts the carry, so nothing was refused on either side.
    assert slurred_fallbacks == 0
    assert retriggered_fallbacks == 0

    # And the same call twice is bit-identical, so the difference above is the
    # articulation rather than anything free-running in the render.
    assert _render_slur("reed", "mono-retrigger")[0] == retriggered


def test_an_engine_that_declines_to_be_carried_is_counted_through_python() -> None:
    # A struck string cannot be slurred: its exciter is spent before the second
    # sample. The note still sounds, so the counter is the only evidence.
    audio, declined = _render_slur("piano", "mono-legato")
    assert declined >= 1
    assert max(abs(sample) for sample in audio) > 0.0

    # The same phrase under the default mode refuses nothing, so the count above
    # is the request being declined rather than a counter that only ever rises.
    assert _render_slur("piano", "poly")[1] == 0
