"""Tests for ``detect_boundaries``, the unlabelled layer under ``analyze_sections``.

What separates this from the section analyzer is the novelty curve and the two
thresholds applied to it, so that is what these measure: a section list cannot
express "how close was this to being a boundary", and a caller that wants its own
cutoff has nothing to work from without the curve.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

_SR = 22050
_SECTION_SEC = 4.0


def _tone(n: int, frequency: float) -> np.ndarray:
    return 0.5 * np.sin(2.0 * np.pi * frequency * np.arange(n) / _SR)


@pytest.fixture(scope="module")
def three_sections() -> list[float]:
    """Tone, then noise, then a different tone: two transitions, at 4 s and 8 s."""
    rng = np.random.default_rng(5)
    n = int(_SR * _SECTION_SEC)
    signal = np.concatenate([_tone(n, 220.0), rng.standard_normal(n) * 0.3, _tone(n, 660.0)])
    return [float(x) for x in signal]


@pytest.fixture(scope="module")
def stationary() -> list[float]:
    """Steady noise: nothing changes, so nothing should be reported."""
    rng = np.random.default_rng(11)
    return [float(x) for x in rng.standard_normal(int(_SR * 12)) * 0.3]


def test_boundaries_land_on_the_transitions(three_sections) -> None:
    result = libsonare.detect_boundaries(three_sections, sample_rate=_SR)

    times = [b.time for b in result.boundaries]
    assert len(times) == 2
    # Within one analysis hop of the two joins, not merely "some boundaries".
    hop_sec = 512 / _SR
    assert abs(times[0] - _SECTION_SEC) < 2 * hop_sec
    assert abs(times[1] - 2 * _SECTION_SEC) < 2 * hop_sec

    # `frame` indexes the analysis grid; on an unpooled input that is time / hop.
    for boundary in result.boundaries:
        assert boundary.frame == pytest.approx(boundary.time * _SR / 512, abs=1.0)


def test_the_novelty_curve_is_returned_and_scaled_by_its_own_peak(three_sections) -> None:
    result = libsonare.detect_boundaries(three_sections, sample_rate=_SR)

    assert len(result.novelty_curve) == result.n_frames
    assert all(0.0 <= value <= 1.0 for value in result.novelty_curve)
    # Scaled by its own maximum, so the top of the curve is exactly 1.
    assert max(result.novelty_curve) == pytest.approx(1.0, abs=1e-6)
    # And `novelty_peak` is the factor back to the raw response the absolute
    # threshold reads, which is a different number -- asserting it is non-zero
    # and distinct from 1.0 is what stops this passing on a curve that was never
    # normalized at all.
    assert result.novelty_peak > 0.0
    assert not math.isclose(result.novelty_peak, 1.0)


def test_the_absolute_threshold_reaches_the_detector(three_sections) -> None:
    """Raising the floor past the raw peak must remove every boundary.

    A threshold argument that never arrived would leave the count unchanged, and
    the default case alone cannot tell that apart from a threshold that arrived
    and was satisfied.
    """
    default = libsonare.detect_boundaries(three_sections, sample_rate=_SR)
    assert len(default.boundaries) == 2

    above_peak = libsonare.detect_boundaries(
        three_sections, sample_rate=_SR, absolute_threshold=default.novelty_peak * 2.0
    )
    assert above_peak.boundaries == []
    # The curve is still computed; only the picking changed.
    assert len(above_peak.novelty_curve) == default.n_frames


def test_the_absolute_threshold_is_what_keeps_stationary_input_unsegmented(stationary) -> None:
    """The floor's whole reason for existing, asserted in both directions.

    The relative threshold is compared against a curve scaled by its own maximum,
    so a signal that never changes still produces peaks of 1.0. Disabling the
    floor must therefore segment steady noise -- if it does not, the floor is not
    what is holding this back and the default case proves nothing about it.
    """
    with_floor = libsonare.detect_boundaries(stationary, sample_rate=_SR)
    assert with_floor.boundaries == []

    without_floor = libsonare.detect_boundaries(stationary, sample_rate=_SR, absolute_threshold=0.0)
    assert len(without_floor.boundaries) > 0


def test_analysis_grid_is_reported_and_is_not_the_source_rate() -> None:
    """A 44.1 kHz input is analyzed at 22.05 kHz, and says so.

    `frame` is uninterpretable without this: it indexes the analysis grid, so a
    caller mapping it back through the source rate lands in the wrong place.
    """
    rng = np.random.default_rng(3)
    n = int(44100 * 4)
    signal = np.concatenate([_tone(n, 220.0), rng.standard_normal(n) * 0.3])
    result = libsonare.detect_boundaries([float(x) for x in signal], sample_rate=44100)

    assert result.sample_rate == 22050
    assert result.hop_length == 512
    # Unpooled: the band budget is several hours at the default kernel.
    assert result.frame_stride == 1


def test_disabling_both_feature_streams_is_refused(three_sections) -> None:
    """Neither stream leaves nothing to combine, so the novelty curve is undefined.

    Asserted rather than left to produce an empty result: an empty boundary list
    is also what a stationary input gives, and the two must not look alike.
    """
    with pytest.raises(libsonare.SonareError):
        libsonare.detect_boundaries(
            three_sections, sample_rate=_SR, use_mfcc=False, use_chroma=False
        )

    # Either one alone is accepted, so the refusal is about the combination.
    assert (
        libsonare.detect_boundaries(three_sections, sample_rate=_SR, use_chroma=False).n_frames > 0
    )
    assert libsonare.detect_boundaries(three_sections, sample_rate=_SR, use_mfcc=False).n_frames > 0


@pytest.mark.parametrize("option", ["n_fft", "hop_length", "kernel_size", "n_mfcc", "n_chroma"])
def test_non_positive_grid_options_are_refused(three_sections, option) -> None:
    with pytest.raises(libsonare.SonareError):
        libsonare.detect_boundaries(three_sections[:_SR], sample_rate=_SR, **{option: 0})
