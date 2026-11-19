"""Cross-binding public-input transaction corpus."""

from __future__ import annotations

import json
import math
import re
from pathlib import Path
from typing import Any

import pytest

from libsonare import (
    EngineMarker,
    MidiCcBinding,
    Project,
    RealtimeEngine,
    SonareValueError,
    mix_stereo,
)

_CORPUS = json.loads(
    (Path(__file__).resolve().parents[3] / "tests/conformance/public_input_corpus.json").read_text()
)


def _marker_value(raw: dict[str, Any]) -> EngineMarker:
    marker_id = 0xFFFFFFFF if raw["id"] == "uint32_max" else raw["id"]
    ppq = {"nan": math.nan, "inf": math.inf}.get(raw["ppq"], raw["ppq"])
    return EngineMarker(marker_id, ppq, raw["name"])


def _snapshot(engine: RealtimeEngine) -> bytes:
    values = []
    for marker in engine.markers():
        values.append(
            [
                marker.id,
                marker.ppq,
                marker.name,
                marker.kind,
                marker.key_fifths,
                marker.key_minor,
            ]
        )
    return json.dumps(values, ensure_ascii=False, separators=(",", ":")).encode()


def _mix_option_value(raw: object) -> object:
    return {
        "nan": math.nan,
        "inf": math.inf,
        "neg_inf": -math.inf,
    }.get(raw, raw)


@pytest.mark.parametrize(
    "case", _CORPUS["marker_transaction"]["cases"], ids=lambda case: case["id"]
)
def test_shared_marker_transaction_corpus(case: dict[str, Any]) -> None:
    initial = [_marker_value(raw) for raw in _CORPUS["marker_transaction"]["initial"]]
    candidate = [_marker_value(raw) for raw in case["markers"]]
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_markers(initial)
        before = _snapshot(engine)
        if case["accepted"]:
            engine.set_markers(candidate)
            stored = engine.markers()
            assert len(stored) == len(candidate)
            for actual, expected in zip(stored, candidate, strict=True):
                assert (actual.id, actual.ppq, actual.name) == (
                    expected.id,
                    expected.ppq,
                    expected.name,
                )
        else:
            with pytest.raises((ValueError, OverflowError, TypeError, RuntimeError)):
                engine.set_markers(candidate)
            assert _snapshot(engine) == before


@pytest.mark.parametrize("case", _CORPUS["mix_options"]["cases"], ids=lambda case: case["id"])
def test_shared_mix_option_corpus_rejects_and_recovers(case: dict[str, Any]) -> None:
    value = _mix_option_value(case["value"])
    options = (
        {"pan": [0.0], "pan_mode": value}
        if case["field"] == "pan_mode"
        else {case["field"]: [value]}
    )
    strips = [([0.25, -0.25], [0.25, -0.25])]

    with pytest.raises((ValueError, OverflowError, TypeError, RuntimeError)):
        mix_stereo(strips, sample_rate=48000, **options)

    recovered = mix_stereo(strips, sample_rate=48000)
    assert len(recovered.left) == 2
    assert len(recovered.right) == 2


def _python_field(case: dict[str, Any]) -> str:
    """The corpus field is the Node spelling; the Python keyword is its snake case."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", case["field"]).lower()


def _midi_cc_binding(case: dict[str, Any]) -> MidiCcBinding:
    return MidiCcBinding(**{"cc_number": 7, "param_id": 1, _python_field(case): case["value"]})


_MIDI_CC_CASES = _CORPUS["midi_cc_binding"]["cases"]


def test_midi_cc_binding_corpus_has_both_outcomes() -> None:
    assert len(_MIDI_CC_CASES) >= 11
    assert {case["accepted"] for case in _MIDI_CC_CASES} == {True, False}


@pytest.mark.parametrize("case", _MIDI_CC_CASES, ids=lambda case: case["id"])
def test_bind_midi_cc_byte_fields_follow_the_corpus(case: dict[str, Any]) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        if case["accepted"]:
            engine.bind_midi_cc(_midi_cc_binding(case))
            assert engine.midi_cc_binding_count == 1
        else:
            with pytest.raises(
                SonareValueError, match=rf"^\S+: {_python_field(case)} must be an integer in \[0, "
            ):
                engine.bind_midi_cc(_midi_cc_binding(case))
            assert engine.midi_cc_binding_count == 0


@pytest.mark.parametrize("case", _MIDI_CC_CASES, ids=lambda case: case["id"])
def test_midi_cc_to_breakpoint_byte_fields_follow_the_corpus(case: dict[str, Any]) -> None:
    event = Project.midi_cc(0.0, 0, 0, 7, 64)
    if case["accepted"]:
        Project.midi_cc_to_breakpoint([_midi_cc_binding(case)], event)
    else:
        with pytest.raises(
            SonareValueError, match=rf"^\S+: {_python_field(case)} must be an integer in \[0, "
        ):
            Project.midi_cc_to_breakpoint([_midi_cc_binding(case)], event)
