"""Tests for the opt-in per-beat tempo curve of ``analyze()``.

These cover ``compute_tempo_curve`` reaching the core and ``beat_local_bpm``
coming back beat-indexed, plus the interaction that the option's documentation
warns about: the curve describes the beat grid it was decoded from, so it stays
nearly flat on moving material unless ``adaptive_tempo`` is set as well.
"""

from __future__ import annotations

import math

import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

SR = 22050


def _sweeping_clicks(
    bpm0: float, bpm1: float, duration: float, sample_rate: int = SR
) -> list[float]:
    """Generate a click track whose tempo sweeps linearly from bpm0 to bpm1."""
    n = int(sample_rate * duration)
    samples = [0.0] * n
    click_length = sample_rate // 100
    t = 0.0
    while t < duration:
        start = int(t * sample_rate)
        for i in range(click_length):
            if start + i >= n:
                break
            samples[start + i] = (1.0 - i / click_length) * 0.9
        t += 60.0 / (bpm0 + (bpm1 - bpm0) * (t / duration))
    return samples


def _span(curve: list[float]) -> float:
    return (max(curve) - min(curve)) / min(curve)


@pytest.fixture(scope="module")
def steady() -> list[float]:
    return _sweeping_clicks(120.0, 120.0, 6.0)


def test_tempo_curve_is_withheld_until_asked_for(steady: list[float]) -> None:
    without = libsonare.analyze(steady, SR)
    assert without.beat_local_bpm == []

    with_curve = libsonare.analyze(steady, SR, compute_tempo_curve=True)
    assert len(with_curve.beat_local_bpm) == len(with_curve.beat_times)
    assert with_curve.beat_local_bpm
    for bpm in with_curve.beat_local_bpm:
        assert math.isfinite(bpm)
        assert bpm > 0.0


def test_tempo_curve_adds_an_output_without_moving_the_analysis(steady: list[float]) -> None:
    without = libsonare.analyze(steady, SR)
    with_curve = libsonare.analyze(steady, SR, compute_tempo_curve=True)

    assert with_curve.bpm == without.bpm
    assert len(with_curve.beat_times) == len(without.beat_times)
    assert with_curve.time_signature.numerator == without.time_signature.numerator
    assert with_curve.time_signature.denominator == without.time_signature.denominator


def test_camel_case_alias_reads_the_same_curve(steady: list[float]) -> None:
    result = libsonare.analyze(steady, SR, compute_tempo_curve=True)
    assert result.beatLocalBpm == result.beat_local_bpm


@pytest.mark.slow
def test_tempo_curve_follows_a_sweep_only_when_beat_tracking_does() -> None:
    # The curve is faithful to the beat grid it was decoded from, so with beat
    # tracking holding a fixed prior it reports a nearly flat tempo on material
    # that is not. That is the trap the option documents; pin it as behaviour
    # rather than leaving it to the prose.
    samples = _sweeping_clicks(90.0, 150.0, 25.0)

    held = libsonare.analyze(samples, SR, compute_tempo_curve=True)
    tracked = libsonare.analyze(samples, SR, compute_tempo_curve=True, adaptive_tempo=True)

    assert _span(tracked.beat_local_bpm) > 0.25
    assert _span(tracked.beat_local_bpm) > _span(held.beat_local_bpm) * 2.0
    assert 80.0 < tracked.beat_local_bpm[0] < 105.0
    assert tracked.beat_local_bpm[-1] > 125.0
