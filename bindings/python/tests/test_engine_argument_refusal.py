"""Engine argument refusals are SonareValueError; state refusals stay a coded SonareError."""

from __future__ import annotations

from collections.abc import Callable

import pytest

from libsonare import RealtimeEngine, SonareError, SonareValueError, SynthPatch
from libsonare._errors import ErrorCode

_BAND = '{"type":"Peak","frequencyHz":1000,"gainDb":3,"q":1,"enabled":true}'
_UNKNOWN_ID = 99

_VALUE_REFUSALS: list[tuple[str, Callable[[RealtimeEngine], object]]] = [
    (
        "unknown_engine_param_key",
        lambda e: e.set_synth_instrument(
            SynthPatch(engine_mode="bowed-string", engine_params={"noSuchKey": 1.0}),
            destination_id=3,
        ),
    ),
    ("unknown_synth_preset", lambda e: e.set_synth_instrument("no-such-preset", destination_id=3)),
    ("track_sends_unknown_track", lambda e: e.set_track_sends(_UNKNOWN_ID, [])),
    ("track_output_bus_unknown_track", lambda e: e.set_track_output_bus(_UNKNOWN_ID, 0)),
    ("bus_strip_pan_bus_zero", lambda e: e.set_bus_strip_pan(0, 0.0)),
]


@pytest.mark.parametrize(("name", "call"), _VALUE_REFUSALS, ids=[n for n, _ in _VALUE_REFUSALS])
def test_value_refusal_is_sonare_value_error(
    name: str, call: Callable[[RealtimeEngine], object]
) -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=64) as engine:
        with pytest.raises(SonareValueError) as exc:
            call(engine)
        assert exc.value.code == int(ErrorCode.INVALID_PARAMETER)
        assert isinstance(exc.value, SonareError)


_MASTER_STRIP_SETTERS: list[tuple[str, Callable[[RealtimeEngine], object]]] = [
    ("eq_band_json", lambda e: e.set_master_strip_eq_band_json(0, _BAND)),
    ("insert_bypassed", lambda e: e.set_master_strip_insert_bypassed(0, True)),
    (
        "insert_param_by_name",
        lambda e: e.set_master_strip_insert_param_by_name(0, "levelDb", -3.0),
    ),
]


@pytest.mark.parametrize(
    ("name", "call"), _MASTER_STRIP_SETTERS, ids=[n for n, _ in _MASTER_STRIP_SETTERS]
)
def test_master_strip_setter_without_a_master_strip_is_coded_invalid_state(
    name: str, call: Callable[[RealtimeEngine], object]
) -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=64) as engine:
        with pytest.raises(SonareError) as exc:
            call(engine)
        assert not isinstance(exc.value, SonareValueError)
        assert exc.value.code == int(ErrorCode.INVALID_STATE)


def test_call_on_closed_engine_is_coded_invalid_state() -> None:
    engine = RealtimeEngine(sample_rate=48000.0, max_block_size=64)
    engine.close()
    with pytest.raises(SonareError) as exc:
        engine.set_track_output_bus(1, 0)
    assert not isinstance(exc.value, SonareValueError)
    assert exc.value.code == int(ErrorCode.INVALID_STATE)
