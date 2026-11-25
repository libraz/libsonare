"""A reference (or track) at another rate is resampled onto the source rate.

Each case compares a call given the reference at its own rate against the same
call given the reference pre-resampled by this surface's ``resample``. Both go
through one resampler, so the two must agree exactly. The control reads the raw
reference at the source rate, which a call ignoring the rate would also do.
"""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    MatchReferenceLoudnessResult,
    MixAssistantTrack,
    SonareError,
    SonareValueError,
    StreamingEqualizer,
    mastering_ab_match_loudness,
    mastering_ab_match_loudness_stereo,
    mastering_analyze_pair,
    mastering_apply_pair_processor,
    mastering_apply_pair_processor_stereo,
    mixing_assistant_suggest,
    resample,
)
from libsonare._result_records import _result_to_json_dict

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000
REF_SR = 32000


def _noise(length: int, seed: int, gain: float) -> np.ndarray:
    rng = np.random.default_rng(seed)
    return (gain * (rng.random(length) - 0.5)).astype(np.float32)


SOURCE = _noise(24000, 7, 0.4)
REFERENCE = _noise(16000, 11, 0.6)
RESAMPLED = np.asarray(resample(REFERENCE, REF_SR, SR), dtype=np.float32) if LIB_AVAILABLE else None


def test_pair_process_resamples_the_reference() -> None:
    params = {"mix": 0.5}
    expected = mastering_apply_pair_processor("match.abCrossfade", SOURCE, RESAMPLED, SR, params)
    actual = mastering_apply_pair_processor(
        "match.abCrossfade", SOURCE, REFERENCE, SR, params, reference_sample_rate=REF_SR
    )
    assert actual.sample_rate == SR
    assert np.array_equal(actual.samples, expected.samples)
    misread = mastering_apply_pair_processor("match.abCrossfade", SOURCE, REFERENCE, SR, params)
    assert not np.array_equal(misread.samples, expected.samples)


def test_pair_process_stereo_resamples_the_reference() -> None:
    params = {"mix": 0.25}
    expected = mastering_apply_pair_processor_stereo(
        "match.abCrossfade", SOURCE, SOURCE, RESAMPLED, RESAMPLED, SR, params
    )
    actual = mastering_apply_pair_processor_stereo(
        "match.abCrossfade",
        SOURCE,
        SOURCE,
        REFERENCE,
        REFERENCE,
        SR,
        params,
        reference_sample_rate=REF_SR,
    )
    assert np.array_equal(actual.left, expected.left)
    assert np.array_equal(actual.right, expected.right)
    misread = mastering_apply_pair_processor_stereo(
        "match.abCrossfade", SOURCE, SOURCE, REFERENCE, REFERENCE, SR, params
    )
    assert not np.array_equal(misread.left, expected.left)


@pytest.mark.parametrize("name", ["match.referenceLoudness", "match.matchEqCurve"])
def test_pair_analyze_resamples_the_reference(name: str) -> None:
    expected = _result_to_json_dict(mastering_analyze_pair(name, SOURCE, RESAMPLED, SR))
    actual = mastering_analyze_pair(name, SOURCE, REFERENCE, SR, reference_sample_rate=REF_SR)
    assert _result_to_json_dict(actual) == expected
    misread = mastering_analyze_pair(name, SOURCE, REFERENCE, SR)
    assert _result_to_json_dict(misread) != expected


def test_pair_delay_is_in_source_rate_samples() -> None:
    signal = _noise(16000, 23, 0.5)
    reference = np.concatenate([np.zeros(100, dtype=np.float32), signal])
    source = np.asarray(resample(signal, REF_SR, SR), dtype=np.float32)
    delay = mastering_analyze_pair(
        "match.estimateReferenceDelaySamples",
        source,
        reference,
        SR,
        reference_sample_rate=REF_SR,
    ).delay_samples
    assert isinstance(delay, int)
    assert abs(delay - 100 * SR / REF_SR) <= 1.0


@pytest.mark.parametrize(
    ("source", "reference", "message"),
    [
        (
            np.zeros((3, 64), dtype=np.float32),
            SOURCE,
            "source must hold 1 or 2 channels",
        ),
        (
            SOURCE,
            np.zeros((3, 64), dtype=np.float32),
            "reference must hold 1 or 2 channels",
        ),
        (
            np.empty((0, 64), dtype=np.float32),
            SOURCE,
            "source must hold 1 or 2 channels",
        ),
        (np.stack((SOURCE, SOURCE)), REFERENCE, "same number of channels"),
        (SOURCE, np.stack((REFERENCE, REFERENCE)), "same number of channels"),
        ([[], []], SOURCE, r"source\[0\].*must not be empty"),
        (SOURCE, np.empty((2, 0), dtype=np.float32), r"reference\[0\].*must not be empty"),
        ([SOURCE, SOURCE[:-1]], REFERENCE, r"source\[1\] must have the same length as source\[0\]"),
        (
            SOURCE,
            [REFERENCE, REFERENCE[:-1]],
            r"reference\[1\] must have the same length as reference\[0\]",
        ),
    ],
)
def test_pair_analyze_refuses_invalid_planar_channels_before_native_call(
    monkeypatch: pytest.MonkeyPatch, source: object, reference: object, message: str
) -> None:
    from libsonare import _mastering_pair

    def native_call_is_unexpected() -> object:
        pytest.fail("invalid channel input reached the native library")

    monkeypatch.setattr(_mastering_pair, "_get_lib", native_call_is_unexpected)
    with pytest.raises(SonareValueError, match=message):
        mastering_analyze_pair("match.referenceLoudness", source, reference, SR)  # type: ignore[arg-type]


def _assert_measurements_close(mono: object, stereo: object, path: str = "") -> None:
    """Compare every serialized field, allowing the two-channel loudness power sum."""
    if isinstance(mono, dict):
        assert isinstance(stereo, dict)
        assert mono.keys() == stereo.keys()
        for key in mono:
            _assert_measurements_close(mono[key], stereo[key], f"{path}.{key}" if path else key)
        return
    if isinstance(mono, list):
        assert isinstance(stereo, list)
        assert len(mono) == len(stereo)
        for index, (mono_value, stereo_value) in enumerate(zip(mono, stereo, strict=True)):
            _assert_measurements_close(mono_value, stereo_value, f"{path}[{index}]")
        return
    if isinstance(mono, (float, int)) and not isinstance(mono, bool):
        assert isinstance(stereo, (float, int)) and not isinstance(stereo, bool)
        if path in {"source_lufs", "reference_lufs"}:
            expected = float(mono) + 10.0 * np.log10(2.0)
        else:
            expected = float(mono)
        scale = max(1.0, abs(expected), abs(float(stereo)))
        assert np.isclose(stereo, expected, rtol=1e-5, atol=1e-5 * scale), path
        return
    assert stereo == mono, path


@pytest.mark.parametrize(
    "name",
    [
        "match.referenceLoudness",
        "match.tonalBalance",
        "match.tonalBalanceLogBands",
        "match.matchEqCurve",
        "match.estimateReferenceDelaySamples",
    ],
)
def test_pair_analyze_duplicate_stereo_matches_mono_measurements(name: str) -> None:
    mono = _result_to_json_dict(mastering_analyze_pair(name, SOURCE, REFERENCE, SR))
    stereo_source = np.stack((SOURCE, SOURCE))
    stereo_reference = np.stack((REFERENCE, REFERENCE))
    stereo = _result_to_json_dict(mastering_analyze_pair(name, stereo_source, stereo_reference, SR))
    _assert_measurements_close(mono, stereo)


def test_pair_analyze_accepts_channel_buffer_sequences() -> None:
    source = [SOURCE.tolist(), SOURCE.tolist()]
    reference = [REFERENCE.tolist(), REFERENCE.tolist()]
    result = mastering_analyze_pair("match.referenceLoudness", source, reference, SR)
    assert result.source_lufs == pytest.approx(
        mastering_analyze_pair("match.referenceLoudness", SOURCE, REFERENCE, SR).source_lufs
        + 10.0 * np.log10(2.0),
        abs=1e-4,
    )


def test_equal_rates_match_the_omitted_rate_bit_for_bit() -> None:
    omitted = mastering_analyze_pair("match.referenceLoudness", SOURCE, REFERENCE, SR)
    stated = mastering_analyze_pair(
        "match.referenceLoudness", SOURCE, REFERENCE, SR, reference_sample_rate=SR
    )
    assert stated == omitted
    assert isinstance(stated, MatchReferenceLoudnessResult)


def test_ab_match_resamples_the_reference() -> None:
    source = sine(220.0, 1.0, sr=SR, amp=0.05)
    reference = _noise(48000, 5, 0.8)
    resampled = np.asarray(resample(reference, REF_SR, SR), dtype=np.float32)
    expected = mastering_ab_match_loudness(source, resampled, SR)
    actual = mastering_ab_match_loudness(source, reference, SR, reference_sample_rate=REF_SR)
    assert np.array_equal(actual.samples, expected.samples)
    assert actual.applied_gain_db == expected.applied_gain_db
    assert actual.reference_lufs == expected.reference_lufs
    misread = mastering_ab_match_loudness(source, reference, SR)
    assert misread.applied_gain_db != expected.applied_gain_db

    stereo = mastering_ab_match_loudness_stereo(
        source, -0.5 * source, reference, reference, SR, reference_sample_rate=REF_SR
    )
    expected_stereo = mastering_ab_match_loudness_stereo(
        source, -0.5 * source, resampled, resampled, SR
    )
    assert np.array_equal(stereo.left, expected_stereo.left)
    assert np.array_equal(stereo.right, expected_stereo.right)
    assert stereo.applied_gain_db == expected_stereo.applied_gain_db
    assert stereo.reference_lufs == expected_stereo.reference_lufs


def _eq_probe(**match_kwargs: object) -> np.ndarray:
    source = sine(1000.0, 1.0, sr=SR, amp=0.1)
    probe = _noise(4096, 99, 0.5)
    with StreamingEqualizer(sample_rate=SR, max_block_size=4096) as eq:
        reference = match_kwargs.pop("reference")
        eq.match(source, reference, max_bands=6, **match_kwargs)  # type: ignore[arg-type]
        return eq.process_mono(probe)


def test_eq_match_resamples_the_reference_and_accepts_unequal_lengths() -> None:
    reference = _noise(20000, 31, 0.7)
    resampled = np.asarray(resample(reference, REF_SR, SR), dtype=np.float32)
    expected = _eq_probe(reference=resampled)
    np.testing.assert_array_equal(
        _eq_probe(reference=reference, reference_sample_rate=REF_SR), expected
    )
    assert not np.array_equal(_eq_probe(reference=reference), expected)
    np.testing.assert_array_equal(
        _eq_probe(reference=reference, sample_rate=SR, reference_sample_rate=SR),
        _eq_probe(reference=reference),
    )


@pytest.mark.parametrize("bad_rate", [0, -1])
def test_a_non_positive_reference_rate_is_refused_like_the_sample_rate(bad_rate: int) -> None:
    with pytest.raises(SonareError) as by_sample_rate:
        mastering_analyze_pair("match.referenceLoudness", SOURCE, REFERENCE, bad_rate)
    with pytest.raises(type(by_sample_rate.value)):
        mastering_analyze_pair(
            "match.referenceLoudness", SOURCE, REFERENCE, SR, reference_sample_rate=bad_rate
        )
    with pytest.raises(type(by_sample_rate.value)):
        mastering_apply_pair_processor(
            "match.abCrossfade", SOURCE, REFERENCE, SR, reference_sample_rate=bad_rate
        )
    with pytest.raises(type(by_sample_rate.value)):
        mastering_ab_match_loudness(SOURCE, REFERENCE, SR, reference_sample_rate=bad_rate)
    with (
        StreamingEqualizer(sample_rate=SR, max_block_size=256) as eq,
        pytest.raises(type(by_sample_rate.value)),
    ):
        eq.match(SOURCE, REFERENCE, reference_sample_rate=bad_rate)


def _mix_tracks(rate_of_second: int | None, second: np.ndarray) -> list[MixAssistantTrack]:
    return [
        MixAssistantTrack("bass", [sine(110.0, 0.8, sr=SR, amp=0.3)], "bass"),
        MixAssistantTrack("keys", [second], "keys", rate_of_second),
    ]


def test_mixing_assistant_resamples_a_track_at_another_rate() -> None:
    keys = sine(660.0, 0.8, sr=REF_SR, amp=0.2)
    resampled = np.asarray(resample(keys, REF_SR, SR), dtype=np.float32)

    def scene(tracks: list[MixAssistantTrack]) -> object:
        return mixing_assistant_suggest(tracks, sample_rate=SR).scene

    expected = scene(_mix_tracks(None, resampled))
    assert scene(_mix_tracks(REF_SR, keys)) == expected
    assert scene(_mix_tracks(None, keys)) != expected
    assert scene(_mix_tracks(SR, keys)) == scene(_mix_tracks(None, keys))


@pytest.mark.parametrize("bad_rate", [0, -1])
def test_mixing_assistant_refuses_a_non_positive_track_rate(bad_rate: int) -> None:
    keys = sine(660.0, 0.8, sr=SR, amp=0.2)
    with pytest.raises(
        SonareValueError, match=r"tracks\[1\]\.sample_rate must be an integer in \[8000, 384000\]"
    ):
        mixing_assistant_suggest(_mix_tracks(bad_rate, keys), sample_rate=SR)
