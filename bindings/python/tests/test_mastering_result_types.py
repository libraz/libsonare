"""The explainable-mastering result records and documents match what the core emits."""

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


def _capture_wire(monkeypatch: pytest.MonkeyPatch, parser: str) -> list[Any]:
    """Record the parsed native document each call hands ``parser``."""
    from libsonare import _mastering_pair

    seen: list[Any] = []
    original = getattr(_mastering_pair, parser)

    def spy(wire: Any) -> Any:
        seen.append(wire)
        return original(wire)

    monkeypatch.setattr(_mastering_pair, parser, spy)
    return seen


def _cli_view(wire: Any) -> Any:
    """What the command line printed before the records: the document, snake-cased, nulls kept."""
    from libsonare._cli_common import _json_keys_to_snake_case

    return _json_keys_to_snake_case(wire)


def _without_nulls(value: Any) -> Any:
    if isinstance(value, dict):
        return {k: _without_nulls(v) for k, v in value.items() if v is not None}
    if isinstance(value, list):
        return [_without_nulls(item) for item in value]
    return value


def test_assistant_result(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare._result_records import _result_to_json_dict

    wires = _capture_wire(monkeypatch, "_assistant_result_from_wire")
    result = libsonare.mastering_assistant_suggest(SOURCE, SR)
    assert isinstance(result, libsonare.MasteringAssistantResult)
    assert isinstance(result.profile, libsonare.MasteringAssistantProfile)
    (wire,) = wires
    expected = _cli_view(wire)
    # The one intended difference: the chain is the nested overrides form, keys as the document's.
    expected["chain_config"] = libsonare.mastering_assistant_suggest_chain(SOURCE, SR)
    assert _result_to_json_dict(result) == _without_nulls(expected)


def test_streaming_preview_result(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare._result_records import _result_to_json_dict

    wires = _capture_wire(monkeypatch, "_streaming_preview_from_wire")
    result = libsonare.mastering_streaming_preview(SOURCE, SR)
    assert all(isinstance(p, libsonare.MasteringStreamingPreviewPlatform) for p in result.platforms)
    (wire,) = wires
    assert _result_to_json_dict(result) == _without_nulls(_cli_view(wire))


def test_a_silent_take_has_no_integrated_loudness(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare._result_records import _result_to_json_dict

    silence = np.zeros(SR, dtype=np.float32)
    wires = _capture_wire(monkeypatch, "_audio_profile_from_wire")
    profile = libsonare.mastering_audio_profile(silence, SR)
    preview = libsonare.mastering_streaming_preview(silence, SR)
    assert profile.loudness.integrated_lufs is None
    assert all(p.integrated_lufs is None for p in preview.platforms)
    # The command line used to print the absent measurement as null; it now omits the key.
    (wire,) = wires
    assert _cli_view(wire)["loudness"]["integrated_lufs"] is None
    assert "integrated_lufs" not in _result_to_json_dict(profile)["loudness"]


def test_audio_profile_result(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare._result_records import _result_to_json_dict

    wires = _capture_wire(monkeypatch, "_audio_profile_from_wire")
    result = libsonare.mastering_audio_profile(SOURCE, SR)
    assert isinstance(result, libsonare.MasteringAudioProfile)
    assert result.defects is None
    (wire,) = wires
    assert _result_to_json_dict(result) == _without_nulls(_cli_view(wire))


def test_audio_profile_result_with_defects(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare._result_records import _result_to_json_dict

    wires = _capture_wire(monkeypatch, "_audio_profile_from_wire")
    result = libsonare.mastering_audio_profile(SOURCE, SR, {"detectDefects": True})
    assert isinstance(result.defects, libsonare.AudioDefects)
    # The profile never runs WPE, so the reverb reading has nothing to predict with.
    assert result.defects.reverb.late_predictability is None
    (wire,) = wires
    expected = _cli_view(wire)
    # The one intended difference: with no flat run there is no flat level, which the
    # document still writes as a number.
    assert result.defects.clip.flat_run_count == 0
    assert result.defects.clip.flat_level is None
    del expected["defects"]["clip"]["flat_level"]
    assert _result_to_json_dict(result) == _without_nulls(expected)


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
