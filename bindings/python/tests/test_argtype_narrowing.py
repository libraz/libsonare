"""An argument reaching a narrowing ``argtypes`` is refused, not folded into a legal one.

The conversion here is written nowhere near the call: ctypes applies the declared
parameter type inside the foreign call, so a bare ``int(track_id)`` handed to a
``c_uint32`` parameter wraps exactly as ``ctypes.c_uint32(track_id)`` would. The
value that discriminates is the one that wraps back into the domain --
``2**32 + n`` for a 32-bit parameter, ``256 + n`` for a ``c_uint8`` -- because a
wrap is in range by construction and ``INT_MAX`` or ``-1`` would be caught by
the core's own guards either way.

Each case opens with a positive control: two legitimate values whose results
differ, so an entry point that ignores the argument cannot pass for one that
read it.
"""

from __future__ import annotations

import pytest

import libsonare as ls
from libsonare import MidiCcBinding, RealtimeEngine, SonareValueError
from libsonare.types import EngineMarker

from ._helpers import scene_bus

# One bus strip with two inserts, so bus id and insert index each select a
# different automation id.
_BUS_STRIP = (
    '{{"version":1,"strips":[],"buses":[{{"id":"{bus}","inserts":['
    '{{"slot":"pre","processor":"eq.parametric",'
    '"params":{{"band0.type":1,"band0.frequencyHz":1000,'
    '"band0.gainDb":0,"band0.enabled":1}}}},'
    '{{"slot":"pre","processor":"dynamics.compressor",'
    '"params":{{"thresholdDb":-3,"ratio":4}}}}]}}],"connections":[]}}'
)


def test_a_wrapped_sample_rate_is_refused_rather_than_read_as_a_smaller_one() -> None:
    """A one-shot generator's ``c_int`` parameter, driven where the wrap lands in domain."""
    assert len(ls.tone(sr=8000, duration=0.1)) != len(
        ls.tone(sr=16000, duration=0.1)
    )  # positive control
    for value in (2**32 + 8000, 2**32 + 16000, 2**32, -(2**31) - 1, 8000.5):
        with pytest.raises(SonareValueError, match="sr"):
            ls.tone(sr=value, duration=0.1)


def test_a_wrapped_marker_id_is_refused_rather_than_read_as_a_smaller_one() -> None:
    """The engine's ``c_uint32`` lookup key: a wrap reads a different marker."""
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_markers(
            [EngineMarker(1.0, id=11, name="intro"), EngineMarker(2.0, id=12, name="out")]
        )
        assert engine.marker(11).ppq != engine.marker(12).ppq  # positive control
        for value in (2**32 + 11, 2**32 + 12, 2**32, -1, 11.5):
            with pytest.raises(SonareValueError, match="id"):
                engine.marker(value)


def test_a_wrapped_cc_number_is_refused_rather_than_replacing_another_binding() -> None:
    """The ``c_uint8`` half: 256 + n aliases onto n and silently replaces its binding."""
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.bind_midi_cc(MidiCcBinding(cc_number=1, channel=0, param_id=42))
        engine.bind_midi_cc(MidiCcBinding(cc_number=2, channel=0, param_id=42))
        assert engine.midi_cc_binding_count == 2  # positive control
        engine.bind_midi_cc(MidiCcBinding(cc_number=1, channel=0, param_id=42))
        assert engine.midi_cc_binding_count == 2  # rebinding replaces rather than adds

        for value in (256 + 1, 256, -1, 1.5):
            with pytest.raises(SonareValueError, match="cc_number"):
                engine.bind_midi_cc(MidiCcBinding(cc_number=value, channel=0, param_id=42))  # type: ignore[arg-type]
            with pytest.raises(SonareValueError, match="channel"):
                engine.bind_midi_cc(MidiCcBinding(cc_number=1, channel=value, param_id=42))  # type: ignore[arg-type]
        assert engine.midi_cc_binding_count == 2


def test_a_wrapped_bus_id_or_insert_index_is_refused_rather_than_resolving_another_insert() -> None:
    """Two mixer parameters at once: a ``c_uint32`` id and a ``c_uint`` index."""
    with RealtimeEngine(sample_rate=48000, max_block_size=256) as engine:
        engine.set_track_buses([{"bus_id": 1, "gain_db": 0.0}, {"bus_id": 2, "gain_db": 0.0}])
        engine.set_bus_strip(1, scene_bus(_BUS_STRIP.format(bus=1)))
        engine.set_bus_strip(2, scene_bus(_BUS_STRIP.format(bus=2)))

        first = engine.resolve_bus_insert_automation_id(1, 0, "band0.gainDb")
        other_bus = engine.resolve_bus_insert_automation_id(2, 0, "band0.gainDb")
        other_insert = engine.resolve_bus_insert_automation_id(1, 1, "thresholdDb")
        assert len({first, other_bus, other_insert}) == 3  # positive control

        for value in (2**32 + 1, 2**32 + 2, 2**32, -1, 1.5):
            with pytest.raises(SonareValueError, match="bus_id"):
                engine.resolve_bus_insert_automation_id(value, 0, "band0.gainDb")
        for value in (2**32, 2**32 + 1, -1, 0.5):
            with pytest.raises(SonareValueError, match="insert_index"):
                engine.resolve_bus_insert_automation_id(1, value, "band0.gainDb")
