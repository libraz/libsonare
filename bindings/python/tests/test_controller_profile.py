"""Controller-profile binding tests: the preset and enum name tables, the
bind/clear/count family, and that a binding made through Python reaches the
sound.

The last one is the point. Every call here can return success while the profile
is dropped on the floor, and the only evidence would be a note that sounds wrong
later, so one case renders: a CC bound to the excitation axis must change the
audio, and a cleared profile must leave it bit-identical. Exact equality on the
cleared side rather than a tolerance, because the CC never reaches a setter
there and the two renders are the same arithmetic on the same state.
"""

from __future__ import annotations

import math

import pytest

from libsonare import (
    RealtimeEngine,
    SonareError,
    SonareValueError,
    SynthPatch,
    controller_profile_names,
    synth_enum_tables,
)
from libsonare._project import SYNTH_ENUM_TABLES

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

SAMPLE_RATE = 48000.0
BLOCK = 128
DESTINATION = 3


def _reed_engine() -> RealtimeEngine:
    """An engine with a reed synth on DESTINATION: an engine whose excitation
    axis a controller can actually reach, so a binding has somewhere to land."""
    engine = RealtimeEngine()
    engine.prepare(SAMPLE_RATE, BLOCK, 16, 16)
    engine.set_synth_instrument(SynthPatch(engine_mode="reed"), destination_id=DESTINATION)
    return engine


def _render_breath_ramp(engine: RealtimeEngine) -> list[float]:
    """Hold a note while stepping CC2 across its range; return the left channel."""
    engine.push_midi_note_on(DESTINATION, 0, 0, 60, 100)
    silence = [[0.0] * BLOCK, [0.0] * BLOCK]
    out: list[float] = []
    for block in range(32):
        engine.push_midi_cc(DESTINATION, 0, 0, 2, (block * 127) // 31)
        out.extend(engine.process(silence)[0])
    return out


def test_controller_profile_names_lists_the_presets() -> None:
    names = controller_profile_names()
    # Each preset is named rather than counted alone: a table that lost an entry
    # and gained an empty one keeps its count.
    for expected in ("gm", "breath", "breath-aftertouch", "mpe"):
        assert expected in names
    assert len(names) >= 4


def test_controller_enum_tables_come_from_the_c_abi() -> None:
    tables = synth_enum_tables()
    assert len(tables["controller_inputs"]) == 5
    assert len(tables["controller_axes"]) == 8
    assert tables["controller_inputs"] == SYNTH_ENUM_TABLES["controller_inputs"]
    assert tables["controller_axes"] == SYNTH_ENUM_TABLES["controller_axes"]
    assert "channel-pressure" in tables["controller_inputs"]
    assert "excitation" in tables["controller_axes"]
    assert "vibrato-depth" in tables["controller_axes"]


def test_a_controller_profile_installs_binds_counts_and_clears() -> None:
    with _reed_engine() as engine:
        gm_bindings = engine.controller_binding_count(DESTINATION)

        # A named preset replaces the table wholesale, so the count is the
        # preset's own rather than the sum of the two.
        engine.set_controller_profile(DESTINATION, "breath")
        breath_bindings = engine.controller_binding_count(DESTINATION)
        assert breath_bindings > 0

        # An unknown name is refused and changes nothing.
        with pytest.raises(SonareError):
            engine.set_controller_profile(DESTINATION, "no-such-device")
        assert engine.controller_binding_count(DESTINATION) == breath_bindings

        engine.bind_controller(DESTINATION, input="control-change", index=2, axis="excitation")
        assert engine.controller_binding_count(DESTINATION) == breath_bindings + 1

        engine.clear_controller_bindings(DESTINATION)
        assert engine.controller_binding_count(DESTINATION) == 0

        # The gm preset is still reachable after a clear, so clearing empties
        # the table rather than removing the profile. The count it restores has
        # to be non-zero, or this would pass for a set that did nothing.
        assert gm_bindings > 0
        engine.set_controller_profile(DESTINATION, "gm")
        assert engine.controller_binding_count(DESTINATION) == gm_bindings


def test_an_unknown_spelling_or_ordinal_is_refused_by_the_binding() -> None:
    """The refusals the binding makes before the C ABI is reached.

    Each one asserts :class:`SonareValueError` rather than :class:`SonareError`,
    which is what says the binding refused it: the C ABI range-checks the same
    ordinals and would raise the plain base class, so the two sides are
    indistinguishable here unless the exception type is the one asserted.
    """
    with _reed_engine() as engine:
        engine.clear_controller_bindings(DESTINATION)
        with pytest.raises(SonareValueError, match="controller axis"):
            engine.bind_controller(DESTINATION, input="control-change", axis="no-such-axis")
        with pytest.raises(SonareValueError, match="controller input"):
            engine.bind_controller(DESTINATION, input="no-such-input", axis="excitation")
        # An ordinal past the enum, taken from the table's own length so it
        # follows a value being added rather than pinning today's count.
        with pytest.raises(SonareValueError, match="controller axis"):
            engine.bind_controller(
                DESTINATION,
                input="control-change",
                axis=len(SYNTH_ENUM_TABLES["controller_axes"]),
            )
        with pytest.raises(SonareValueError, match="controller input"):
            engine.bind_controller(
                DESTINATION,
                input=len(SYNTH_ENUM_TABLES["controller_inputs"]),
                axis="excitation",
            )
        with pytest.raises(SonareValueError, match="controller input"):
            engine.bind_controller(DESTINATION, input=-1, axis="excitation")
        # A non-finite range or curve would reach the audio thread and stay
        # there, so the struct's own narrowing refuses it before the C ABI.
        for field in ("lo", "hi", "curve"):
            with pytest.raises(SonareValueError, match=field):
                engine.bind_controller(
                    DESTINATION,
                    input="control-change",
                    axis="excitation",
                    **{field: math.nan},
                )
        assert engine.controller_binding_count(DESTINATION) == 0


def test_a_binding_that_cannot_mean_anything_is_refused_by_the_c_abi() -> None:
    with _reed_engine() as engine:
        engine.clear_controller_bindings(DESTINATION)

        # A per-note value on a channel-level axis: refused rather than widened,
        # because a caller cannot tell a widened binding from one that took.
        with pytest.raises(SonareError) as refusal:
            engine.bind_controller(DESTINATION, input="poly-pressure", axis="loudness")
        # Named as the C ABI's refusal rather than the binding's own, so a
        # Python-side guard growing over it would fail here instead of passing.
        assert not isinstance(refusal.value, SonareValueError)

        with pytest.raises(SonareError) as none_refusal:
            engine.bind_controller(DESTINATION, input="control-change", axis="none")
        # "none" is a name the C table supplies, so the spelling resolves here
        # and the refusal is the C ABI's -- asserted the same way as above.
        assert not isinstance(none_refusal.value, SonareValueError)

        # The same input on an excitation axis is accepted, so the refusal above
        # is about the axis and not about poly pressure.
        engine.bind_controller(DESTINATION, input="poly-pressure", axis="brightness")
        assert engine.controller_binding_count(DESTINATION) == 1


def test_controller_velocity_meaningfulness_round_trips() -> None:
    with _reed_engine() as engine:
        assert engine.controller_velocity_meaningful(DESTINATION) is True
        engine.set_controller_velocity_meaningful(DESTINATION, False)
        assert engine.controller_velocity_meaningful(DESTINATION) is False
        engine.set_controller_velocity_meaningful(DESTINATION, True)
        assert engine.controller_velocity_meaningful(DESTINATION) is True


def test_controller_calls_name_the_destination_that_has_no_instrument() -> None:
    engine = RealtimeEngine()
    try:
        engine.prepare(SAMPLE_RATE, BLOCK, 16, 16)
        for call in (
            lambda: engine.set_controller_profile(5, "gm"),
            lambda: engine.bind_controller(5, input="control-change", axis="excitation"),
            lambda: engine.clear_controller_bindings(5),
            lambda: engine.controller_binding_count(5),
            lambda: engine.controller_velocity_meaningful(5),
        ):
            with pytest.raises(SonareError):
                call()
    finally:
        engine.close()


def test_a_binding_made_through_python_reaches_the_sound() -> None:
    # Bound: CC2 drives the reed's excitation axis.
    with _reed_engine() as engine:
        engine.clear_controller_bindings(DESTINATION)
        engine.bind_controller(DESTINATION, input="control-change", index=2, axis="excitation")
        with_binding = _render_breath_ramp(engine)

    # Unbound: the same ramp, the same patch, nothing bound.
    with _reed_engine() as engine:
        engine.clear_controller_bindings(DESTINATION)
        without_binding = _render_breath_ramp(engine)

    # The renders carry energy, so "they differ" is not two kinds of silence.
    assert max(abs(sample) for sample in without_binding) > 0.0
    assert len(with_binding) == len(without_binding)
    assert with_binding != without_binding

    # And the unbound side is reproducible to the bit, so the difference above
    # is the binding rather than anything free-running in the render.
    with _reed_engine() as engine:
        engine.clear_controller_bindings(DESTINATION)
        assert _render_breath_ramp(engine) == without_binding
