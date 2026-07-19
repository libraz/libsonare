"""Cross-binding public-input transaction corpus."""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

import pytest

from libsonare import EngineMarker, RealtimeEngine, mix_stereo

_CORPUS = json.loads(
    (Path(__file__).resolve().parents[3] / "tests/conformance/public_input_corpus.json").read_text()
)


def _marker_value(raw: dict[str, Any]) -> EngineMarker:
    marker_id = 0xFFFFFFFF if raw["id"] == "uint32_max" else raw["id"]
    ppq = {"nan": math.nan, "inf": math.inf}.get(raw["ppq"], raw["ppq"])
    return EngineMarker(marker_id, ppq, raw["name"])


def _snapshot(engine: RealtimeEngine) -> bytes:
    values = []
    for index in range(engine.marker_count()):
        marker = engine.marker_by_index(index)
        values.append(
            [
                marker.id,
                marker.ppq,
                marker.name,
                int(marker.kind),
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
            assert engine.marker_count() == len(candidate)
            for index, expected in enumerate(candidate):
                actual = engine.marker_by_index(index)
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
