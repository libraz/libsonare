"""A reference (or track) at another rate is resampled onto the source rate.

Each case compares a call given the reference at its own rate against the same
call given the reference pre-resampled by this surface's ``resample``. Both go
through one resampler, so the two must agree exactly. The control reads the raw
reference at the source rate, which a call ignoring the rate would also do.
"""

from __future__ import annotations

import json

import numpy as np
import pytest

from libsonare import (
    MixTrackInput,
    SonareError,
    SonareValueError,
    StreamingEqualizer,
    mastering_ab_match_loudness,
    mastering_ab_match_loudness_stereo,
    mastering_pair_analyze,
    mastering_pair_process,
    mastering_pair_process_stereo,
    resample,
    suggest_mix_scene_json,
)

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
    expected = mastering_pair_process("match.abCrossfade", SOURCE, RESAMPLED, SR, params)
    actual = mastering_pair_process(
        "match.abCrossfade", SOURCE, REFERENCE, SR, params, reference_sample_rate=REF_SR
    )
    assert actual.sample_rate == SR
    assert np.array_equal(actual.samples, expected.samples)
    misread = mastering_pair_process("match.abCrossfade", SOURCE, REFERENCE, SR, params)
    assert not np.array_equal(misread.samples, expected.samples)


def test_pair_process_stereo_resamples_the_reference() -> None:
    params = {"mix": 0.25}
    expected = mastering_pair_process_stereo(
        "match.abCrossfade", SOURCE, SOURCE, RESAMPLED, RESAMPLED, SR, params
    )
    actual = mastering_pair_process_stereo(
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
    misread = mastering_pair_process_stereo(
        "match.abCrossfade", SOURCE, SOURCE, REFERENCE, REFERENCE, SR, params
    )
    assert not np.array_equal(misread.left, expected.left)


@pytest.mark.parametrize("name", ["match.referenceLoudness", "match.matchEqCurve"])
def test_pair_analyze_resamples_the_reference(name: str) -> None:
    expected = mastering_pair_analyze(name, SOURCE, RESAMPLED, SR)
    actual = mastering_pair_analyze(name, SOURCE, REFERENCE, SR, reference_sample_rate=REF_SR)
    assert actual == expected
    assert mastering_pair_analyze(name, SOURCE, REFERENCE, SR) != expected


def test_pair_delay_is_in_source_rate_samples() -> None:
    signal = _noise(16000, 23, 0.5)
    reference = np.concatenate([np.zeros(100, dtype=np.float32), signal])
    source = np.asarray(resample(signal, REF_SR, SR), dtype=np.float32)
    delay = json.loads(
        mastering_pair_analyze(
            "match.estimateReferenceDelaySamples",
            source,
            reference,
            SR,
            reference_sample_rate=REF_SR,
        )
    )["delaySamples"]
    assert abs(delay - 100 * SR / REF_SR) <= 1.0


def test_equal_rates_match_the_omitted_rate_bit_for_bit() -> None:
    omitted = mastering_pair_analyze("match.referenceLoudness", SOURCE, REFERENCE, SR)
    stated = mastering_pair_analyze(
        "match.referenceLoudness", SOURCE, REFERENCE, SR, reference_sample_rate=SR
    )
    assert stated == omitted


def test_ab_match_resamples_the_reference() -> None:
    source = sine(220.0, 1.0, sr=SR, amp=0.05)
    reference = _noise(48000, 5, 0.8)
    resampled = np.asarray(resample(reference, REF_SR, SR), dtype=np.float32)
    expected, expected_match = mastering_ab_match_loudness(source, resampled, SR)
    actual, actual_match = mastering_ab_match_loudness(
        source, reference, SR, reference_sample_rate=REF_SR
    )
    assert np.array_equal(actual, expected)
    assert actual_match == expected_match
    _, misread_match = mastering_ab_match_loudness(source, reference, SR)
    assert misread_match != expected_match

    (left, right), stereo_match = mastering_ab_match_loudness_stereo(
        source, -0.5 * source, reference, reference, SR, reference_sample_rate=REF_SR
    )
    (expected_left, expected_right), expected_stereo = mastering_ab_match_loudness_stereo(
        source, -0.5 * source, resampled, resampled, SR
    )
    assert np.array_equal(left, expected_left)
    assert np.array_equal(right, expected_right)
    assert stereo_match == expected_stereo


def _eq_probe(**match_kwargs: object) -> list[float]:
    source = sine(1000.0, 1.0, sr=SR, amp=0.1)
    probe = _noise(4096, 99, 0.5)
    with StreamingEqualizer(sample_rate=SR, max_block_size=4096) as eq:
        reference = match_kwargs.pop("reference")
        eq.match(source, reference, 6, **match_kwargs)  # type: ignore[arg-type]
        return eq.process_mono(probe)


def test_eq_match_resamples_the_reference_and_accepts_unequal_lengths() -> None:
    reference = _noise(20000, 31, 0.7)
    resampled = np.asarray(resample(reference, REF_SR, SR), dtype=np.float32)
    expected = _eq_probe(reference=resampled)
    assert _eq_probe(reference=reference, reference_sample_rate=REF_SR) == expected
    assert _eq_probe(reference=reference) != expected
    assert _eq_probe(reference=reference, sample_rate=SR, reference_sample_rate=SR) == _eq_probe(
        reference=reference
    )


@pytest.mark.parametrize("bad_rate", [0, -1])
def test_a_non_positive_reference_rate_is_refused_like_the_sample_rate(bad_rate: int) -> None:
    with pytest.raises(SonareError) as by_sample_rate:
        mastering_pair_analyze("match.referenceLoudness", SOURCE, REFERENCE, bad_rate)
    with pytest.raises(type(by_sample_rate.value)):
        mastering_pair_analyze(
            "match.referenceLoudness", SOURCE, REFERENCE, SR, reference_sample_rate=bad_rate
        )
    with pytest.raises(type(by_sample_rate.value)):
        mastering_pair_process(
            "match.abCrossfade", SOURCE, REFERENCE, SR, reference_sample_rate=bad_rate
        )
    with pytest.raises(type(by_sample_rate.value)):
        mastering_ab_match_loudness(SOURCE, REFERENCE, SR, reference_sample_rate=bad_rate)
    with (
        StreamingEqualizer(sample_rate=SR, max_block_size=256) as eq,
        pytest.raises(type(by_sample_rate.value)),
    ):
        eq.match(SOURCE, REFERENCE, reference_sample_rate=bad_rate)


def _mix_tracks(rate_of_second: int | None, second: np.ndarray) -> list[MixTrackInput]:
    return [
        MixTrackInput("bass", sine(110.0, 0.8, sr=SR, amp=0.3), None, "bass"),
        MixTrackInput("keys", second, None, "keys", rate_of_second),
    ]


def test_mixing_assistant_resamples_a_track_at_another_rate() -> None:
    keys = sine(660.0, 0.8, sr=REF_SR, amp=0.2)
    resampled = np.asarray(resample(keys, REF_SR, SR), dtype=np.float32)
    expected = suggest_mix_scene_json(_mix_tracks(None, resampled), sample_rate=SR)
    assert suggest_mix_scene_json(_mix_tracks(REF_SR, keys), sample_rate=SR) == expected
    assert suggest_mix_scene_json(_mix_tracks(None, keys), sample_rate=SR) != expected
    assert suggest_mix_scene_json(_mix_tracks(SR, keys), sample_rate=SR) == suggest_mix_scene_json(
        _mix_tracks(None, keys), sample_rate=SR
    )


@pytest.mark.parametrize("bad_rate", [0, -1])
def test_mixing_assistant_refuses_a_non_positive_track_rate(bad_rate: int) -> None:
    keys = sine(660.0, 0.8, sr=SR, amp=0.2)
    with pytest.raises(SonareValueError, match=r"tracks\[1\]\.sample_rate must be positive"):
        suggest_mix_scene_json(_mix_tracks(bad_rate, keys), sample_rate=SR)
