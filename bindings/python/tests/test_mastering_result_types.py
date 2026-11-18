"""The TypedDicts describing the explainable-mastering helpers' JSON match what they emit."""

from __future__ import annotations

import json
from typing import Any, NotRequired, get_origin, get_type_hints

import numpy as np
import pytest

import libsonare
from libsonare import types as sonare_types

SR = 22050


def _tones(low: float, high: float, amplitude: float) -> np.ndarray:
    t = np.arange(SR, dtype=np.float32) / SR
    return (
        amplitude * (0.6 * np.sin(2 * np.pi * low * t) + 0.4 * np.sin(2 * np.pi * high * t))
    ).astype(np.float32)


def _typed_dict_for(annotation: Any) -> type | None:
    """The TypedDict an annotation names directly or as a list element, if any."""
    candidates = [annotation, *getattr(annotation, "__args__", ())]
    for candidate in candidates:
        if hasattr(candidate, "__required_keys__"):
            return candidate  # type: ignore[no-any-return]
    return None


def _assert_matches(value: Any, typed: type, where: str) -> None:
    """Every key the document carries is declared, every required key is carried."""
    assert isinstance(value, dict), where
    hints = get_type_hints(typed, globalns=vars(sonare_types))
    # Under postponed annotations 3.11 cannot see NotRequired in __required_keys__.
    extras = get_type_hints(typed, globalns=vars(sonare_types), include_extras=True)
    required = {k for k, v in extras.items() if get_origin(v) is not NotRequired}
    assert set(value) <= set(hints), f"{where}: undeclared {sorted(set(value) - set(hints))}"
    assert required <= set(value), f"{where}: missing {sorted(required - set(value))}"
    for key, annotation in hints.items():
        nested = _typed_dict_for(annotation)
        if nested is None or key not in value:
            continue
        child = value[key]
        if isinstance(child, list):
            for index, element in enumerate(child):
                _assert_matches(element, nested, f"{where}.{key}[{index}]")
        else:
            _assert_matches(child, nested, f"{where}.{key}")


SOURCE = _tones(220, 1760, 0.2)
REFERENCE = _tones(330, 2200, 0.4)


def test_assistant_result() -> None:
    result = json.loads(libsonare.mastering_assistant_suggest(SOURCE, SR))
    # The chain document's params are keyed by the suggested stages and stay opaque.
    _assert_matches(result, libsonare.MasteringAssistantResult, "assistant")


def test_streaming_preview_result() -> None:
    result = json.loads(libsonare.mastering_streaming_preview(SOURCE, SR))
    _assert_matches(result, libsonare.MasteringStreamingPreviewResult, "preview")


def test_audio_profile_result() -> None:
    result = json.loads(libsonare.mastering_audio_profile(SOURCE, SR))
    _assert_matches(result, libsonare.MasteringAudioProfile, "profile")
    assert "defects" not in result


def test_audio_profile_result_with_defects() -> None:
    params = {"detectDefects": True}
    result = json.loads(libsonare.mastering_audio_profile(SOURCE, SR, params))
    assert "defects" in result
    _assert_matches(result, libsonare.MasteringAudioProfile, "profile")


def test_repair_analysis_result() -> None:
    result = libsonare.mastering_repair_analyze([SOURCE, REFERENCE], SR)
    assert isinstance(result, libsonare.MasteringRepairAnalysis)
    assert isinstance(result.defects, libsonare.AudioDefects)
    assert result.channels is not None and len(result.channels) == 2
    assert all(isinstance(c, libsonare.AudioDefects) for c in result.channels)
    assert isinstance(result.declip_threshold_safe, bool)
    assert isinstance(result.integrated_lufs, float)
    assert result.defects.reverb.late_predictability is None


@pytest.mark.parametrize(
    ("name", "typed"),
    [
        ("match.referenceLoudness", "MatchReferenceLoudnessResult"),
        ("match.tonalBalance", "MatchTonalBalanceResult"),
        ("match.tonalBalanceLogBands", "MatchTonalBalanceResult"),
        ("match.matchEqCurve", "MatchEqCurveResult"),
        ("match.estimateReferenceDelaySamples", "MatchEstimateReferenceDelaySamplesResult"),
    ],
)
def test_pair_analysis_results(name: str, typed: str) -> None:
    result = json.loads(libsonare.mastering_pair_analyze(name, SOURCE, REFERENCE, SR))
    _assert_matches(result, getattr(libsonare, typed), name)


@pytest.mark.parametrize(
    ("name", "typed"),
    [
        ("stereo.monoCompatCheck", "StereoMonoCompatCheckResult"),
        ("stereo.monoCompatCheckLogBands", "StereoMonoCompatCheckLogBandsResult"),
    ],
)
def test_stereo_analysis_results(name: str, typed: str) -> None:
    # Only the log-band analysis reads the band ceiling; the plain check refuses it.
    params = {"highHz": 10000.0} if name.endswith("LogBands") else None
    result = json.loads(libsonare.mastering_stereo_analyze(name, SOURCE, REFERENCE, SR, params))
    _assert_matches(result, getattr(libsonare, typed), name)
