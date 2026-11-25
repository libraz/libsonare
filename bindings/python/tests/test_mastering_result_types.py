"""The explainable-mastering result records and documents match what the core emits."""

from __future__ import annotations

import math
from typing import Any

import numpy as np
import pytest

import libsonare

SR = 22050


def _tones(low: float, high: float, amplitude: float) -> np.ndarray:
    t = np.arange(SR, dtype=np.float32) / SR
    return (
        amplitude * (0.6 * np.sin(2 * np.pi * low * t) + 0.4 * np.sin(2 * np.pi * high * t))
    ).astype(np.float32)


SOURCE = _tones(220, 1760, 0.2)
REFERENCE = _tones(330, 2200, 0.4)


def _capture_wire(monkeypatch: pytest.MonkeyPatch, parser: str) -> list[Any]:
    """Record the parsed native document each call hands ``parser``."""
    from libsonare import _mastering_pair

    seen: list[Any] = []
    original = getattr(_mastering_pair, parser)

    def spy(*args: Any) -> Any:
        seen.append(args[-1])
        return original(*args)

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
    expected["chain_config"] = result.chain_config
    assert _result_to_json_dict(result) == _without_nulls(expected)


def test_streaming_preview_result(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare._result_records import _result_to_json_dict

    wires = _capture_wire(monkeypatch, "_streaming_preview_from_wire")
    result = libsonare.mastering_streaming_preview(SOURCE, SR)
    assert all(isinstance(p, libsonare.MasteringStreamingPreviewPlatform) for p in result.platforms)
    (wire,) = wires
    assert _result_to_json_dict(result) == _without_nulls(_cli_view(wire))


def test_a_silent_take_reads_minus_infinity_and_prints_it_as_null(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from libsonare._cli_common import _sanitize_json_value
    from libsonare._result_records import _result_to_json_dict

    silence = np.zeros(SR, dtype=np.float32)
    wires = _capture_wire(monkeypatch, "_audio_profile_from_wire")
    profile = libsonare.mastering_audio_profile(silence, SR)
    preview = libsonare.mastering_streaming_preview(silence, SR)
    assert profile.loudness.integrated_lufs == -math.inf
    assert all(p.integrated_lufs == -math.inf for p in preview.platforms)
    # The reader restores the document's null to the infinite end in place; the command
    # line prints that end as null again.
    (wire,) = wires
    assert _cli_view(wire)["loudness"]["integrated_lufs"] == -math.inf
    printed = _sanitize_json_value(_result_to_json_dict(profile))
    assert printed["loudness"]["integrated_lufs"] is None


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
    assert result.defects.clip.flat_run_count == 0
    assert result.defects.clip.flat_level is None
    assert "flat_level" not in expected["defects"]["clip"]
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
        ("match.estimateReferenceDelaySamples", "MatchEstimateReferenceDelaySamplesResult"),
    ],
)
def test_pair_analysis_results(monkeypatch: pytest.MonkeyPatch, name: str, typed: str) -> None:
    from libsonare._result_records import _result_to_json_dict

    wires = _capture_wire(monkeypatch, "_pair_analysis_from_wire")
    result = libsonare.mastering_analyze_pair(name, SOURCE, REFERENCE, SR)
    assert type(result) is getattr(libsonare, typed)
    (wire,) = wires
    assert _result_to_json_dict(result) == _without_nulls(_cli_view(wire))


def test_pair_analysis_eq_curve_is_float32_arrays(monkeypatch: pytest.MonkeyPatch) -> None:
    wires = _capture_wire(monkeypatch, "_pair_analysis_from_wire")
    result = libsonare.mastering_analyze_pair("match.matchEqCurve", SOURCE, REFERENCE, SR)
    assert isinstance(result, libsonare.MatchEqCurveResult)
    (wire,) = wires
    assert set(wire) == {"frequencies", "gainDb"}
    assert result.frequencies.dtype == np.float32
    assert result.gain_db.dtype == np.float32
    assert result.frequencies.shape == result.gain_db.shape == (len(wire["frequencies"]),)
    np.testing.assert_allclose(result.frequencies, wire["frequencies"], rtol=1e-6)
    np.testing.assert_allclose(result.gain_db, wire["gainDb"], rtol=1e-6, atol=1e-6)


@pytest.mark.parametrize("silent_role", ["source", "reference"])
def test_match_eq_refuses_a_silent_take_naming_it(silent_role: str) -> None:
    takes = {"source": SOURCE, "reference": REFERENCE}
    takes[silent_role] = np.zeros(SR, dtype=np.float32)
    expected = f"{silent_role} has no measurable level$"
    with pytest.raises(libsonare.SonareValueError, match=expected):
        libsonare.mastering_analyze_pair(
            "match.matchEqCurve", takes["source"], takes["reference"], SR
        )
    with pytest.raises(libsonare.SonareValueError, match=expected):
        libsonare.mastering_apply_pair_processor(
            "match.applyMatchEq", takes["source"], takes["reference"], SR
        )


def test_pair_analysis_keyword_reports_delay_in_source_samples() -> None:
    reference_rate = 32000
    source_rate = 48000
    reference_delay = 100
    signal = np.random.default_rng(23).uniform(-0.5, 0.5, 16000).astype(np.float32)
    reference = np.pad(signal, (reference_delay, 0))
    source = libsonare.resample(signal, reference_rate, source_rate)

    result = libsonare.mastering_analyze_pair(
        analysis="match.estimateReferenceDelaySamples",
        source=source,
        reference=reference,
        sample_rate=source_rate,
        reference_sample_rate=reference_rate,
    )
    assert isinstance(result, libsonare.MatchEstimateReferenceDelaySamplesResult)
    expected = reference_delay * source_rate / reference_rate
    assert result.delay_samples == pytest.approx(expected, abs=1)


@pytest.mark.parametrize(
    ("name", "typed"),
    [
        ("stereo.monoCompatCheck", "StereoMonoCompatCheckResult"),
        ("stereo.monoCompatCheckLogBands", "StereoMonoCompatCheckLogBandsResult"),
    ],
)
def test_stereo_analysis_results(monkeypatch: pytest.MonkeyPatch, name: str, typed: str) -> None:
    from libsonare._result_records import _result_to_json_dict

    wires = _capture_wire(monkeypatch, "_stereo_analysis_from_wire")
    # Only the log-band analysis reads the band ceiling; the plain check refuses it.
    params = {"highHz": 10000.0} if name.endswith("LogBands") else None
    result = libsonare.mastering_analyze_stereo(name, SOURCE, REFERENCE, SR, params)
    assert type(result) is getattr(libsonare, typed)
    (wire,) = wires
    assert _result_to_json_dict(result) == _without_nulls(_cli_view(wire))


def test_stereo_analysis_takes_the_analysis_keyword() -> None:
    result = libsonare.mastering_analyze_stereo(
        analysis="stereo.monoCompatCheck", left=SOURCE, right=-SOURCE, sample_rate=SR
    )
    assert isinstance(result, libsonare.StereoMonoCompatCheckResult)
    assert result.width == math.inf


def test_a_silent_band_has_no_correlation() -> None:
    silent = np.zeros_like(SOURCE)
    result = libsonare.mastering_analyze_stereo(
        "stereo.monoCompatCheckLogBands", SOURCE, silent, SR, {"highHz": 10000.0}
    )
    assert isinstance(result, libsonare.StereoMonoCompatCheckLogBandsResult)
    assert result.bands
    assert all(band.correlation is None for band in result.bands)


def test_a_null_curve_element_is_refused_rather_than_read_as_nan() -> None:
    from libsonare._mastering_pair import _pair_analysis_from_wire

    wire = {"frequencies": [100.0, 200.0], "gainDb": [0.5, None]}
    with pytest.raises(libsonare.SonareError, match=r"gainDb\[\] is null, which names no"):
        _pair_analysis_from_wire("match.matchEqCurve", wire)


def test_an_out_of_phase_pair_is_infinitely_wide() -> None:
    result = libsonare.mastering_analyze_stereo("stereo.monoCompatCheck", SOURCE, -SOURCE, SR)
    assert isinstance(result, libsonare.StereoMonoCompatCheckResult)
    assert result.width == math.inf
    assert result.correlation == pytest.approx(-1.0, abs=1e-3)
