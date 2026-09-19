"""Take alignment on the Python surface: the anchors, the config, and the refusals.

Mirrors the C-ABI coverage (tests/arrangement/c_abi_edit_ops_test.cpp) rather than
restating it: what is Python's own is the marshalling of a heap-owned anchor array
and of the alignment metadata, and that the keyword configuration reaches the
measurement instead of being read off the caller and dropped.

The signals are continuous pitch glides, not held tones. A held tone leaves the
alignment path unconstrained -- every frame matches every other frame equally
well -- so its anchors say nothing about the orientation or the rate difference
the assertions here are about.
"""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import Project, SonareError, SonareValueError, align_take_to_reference

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def glide(seconds: float, sr: int = SR, f0: float = 261.63, semitones: float = 11.0) -> np.ndarray:
    """A phase-continuous two-partial pitch glide, as float32 at ``sr``.

    The frequency rises by ``semitones`` over the whole signal, so the chroma
    sequence advances monotonically and the alignment path is pinned by the
    content rather than free to wander.
    """
    n = int(sr * seconds)
    frac = np.arange(n) / n
    freq = f0 * np.power(2.0, semitones * frac / 12.0)
    value = np.zeros(n)
    for partial, amplitude in ((1, 1.0), (2, 0.5)):
        phase = np.cumsum(2.0 * np.pi * freq * partial / sr)
        value += amplitude * np.sin(phase)
    return (0.4 * value).astype(np.float32)


@pytest.fixture(scope="module")
def pair() -> tuple[np.ndarray, np.ndarray]:
    """A reference and a take of the same glide at different rates."""
    return glide(0.5), glide(0.75)


def test_anchors_place_a_longer_take_under_the_reference(
    pair: tuple[np.ndarray, np.ndarray],
) -> None:
    reference, take = pair
    anchors, _ = align_take_to_reference(reference, take, SR)

    assert len(anchors) >= 2
    for i in range(1, len(anchors)):
        assert anchors[i][0] > anchors[i - 1][0], f"warp axis not increasing at {i}: {anchors}"
        assert anchors[i][1] > anchors[i - 1][1], f"source axis not increasing at {i}: {anchors}"

    # Orientation: the first coordinate is on the REFERENCE timeline and the
    # second in the TAKE. The take runs half again as long, so the two axes
    # cannot be confused -- the last source position is past the last warp
    # position, and each sits inside its own signal.
    last_warp, last_source = anchors[-1]
    assert last_source > last_warp
    assert last_warp < len(reference)
    assert last_source < len(take)


def test_swapping_the_two_signals_inverts_the_orientation(
    pair: tuple[np.ndarray, np.ndarray],
) -> None:
    """The control for the assertion above, which a fixed axis order would pass.

    Handing the long signal over as the reference has to move the inequality, or
    the relation asserted there is a property of these two buffers rather than of
    the direction the anchors were built for.
    """
    reference, take = pair
    anchors, alignment = align_take_to_reference(take, reference, SR)

    last_warp, last_source = anchors[-1]
    assert last_source < last_warp
    assert alignment.take_frames < alignment.reference_frames


def test_the_hop_reaches_the_measurement(pair: tuple[np.ndarray, np.ndarray]) -> None:
    """Two runs differing only in ``hop_length`` must answer differently.

    A domain check would only prove the value was accepted. A hop read off the
    keyword and dropped gives the same anchor count both times, so the count
    itself is the assertion.
    """
    reference, take = pair
    coarse, _ = align_take_to_reference(reference, take, SR, hop_length=1024)
    fine, _ = align_take_to_reference(reference, take, SR, hop_length=256)
    assert len(fine) > len(coarse), (
        f"hop did not reach the measurement: {len(fine)} vs {len(coarse)}"
    )


def test_the_bins_per_octave_reaches_the_measurement(
    pair: tuple[np.ndarray, np.ndarray],
) -> None:
    """The chroma resolution moves the path, so the anchors are the assertion here.

    It does not change the frame count the way the hop does, and 24 lands on the
    same path as the default for this glide -- so proving reach takes a value that
    moves the anchors, not merely one that is accepted.
    """
    reference, take = pair
    default, _ = align_take_to_reference(reference, take, SR)
    finer, _ = align_take_to_reference(reference, take, SR, bins_per_octave=36)
    assert finer != default


def test_zero_asks_for_the_library_value_as_omitting_it_does(
    pair: tuple[np.ndarray, np.ndarray],
) -> None:
    reference, take = pair
    omitted, _ = align_take_to_reference(reference, take, SR)
    zeroed, _ = align_take_to_reference(reference, take, SR, hop_length=0, bins_per_octave=0)
    assert zeroed == omitted


def test_alignment_metadata_is_named_for_the_arguments_the_caller_passed(
    pair: tuple[np.ndarray, np.ndarray],
) -> None:
    """The C entry inverts its own arguments internally; the counts must not follow it."""
    reference, take = pair
    _, alignment = align_take_to_reference(reference, take, SR)

    assert alignment.reference_frames > 0
    # The take is the longer signal, so a swap of the two counts shows up here.
    assert alignment.take_frames > alignment.reference_frames
    assert np.isfinite(alignment.mean_residual_frames)
    assert alignment.mean_residual_frames >= 0.0


def test_the_anchors_are_accepted_as_a_project_warp_map(
    pair: tuple[np.ndarray, np.ndarray],
) -> None:
    """The composition the entry point exists for, asserted with its own control.

    The raw alignment path repeats a coordinate wherever one signal carries more
    frames than the other, and ``set_warp_map`` refuses that shape -- which the
    tied array below shows on the same project, so the acceptance above cannot
    pass merely because the check stopped running.
    """
    reference, take = pair
    anchors, _ = align_take_to_reference(reference, take, SR)

    with Project() as project:
        project.set_warp_map(101, anchors, "aligned take")
        with pytest.raises(SonareError):
            project.set_warp_map(102, [(0.0, 0.0), (100.0, 50.0), (100.0, 90.0)], "unreduced")


@pytest.mark.parametrize("argument", ["reference", "take"])
def test_an_empty_buffer_is_refused_by_name(
    pair: tuple[np.ndarray, np.ndarray], argument: str
) -> None:
    reference, take = pair
    empty = np.zeros(0, dtype=np.float32)
    buffers = {"reference": reference, "take": take, argument: empty}
    # Anchored on the rest of the message: a bare "take" would also match inside
    # the function's own name and pass for either argument.
    with pytest.raises(SonareValueError, match=rf"\b{argument} must not be empty"):
        align_take_to_reference(buffers["reference"], buffers["take"], SR)


@pytest.mark.parametrize("sample_rate", [0, -1])
def test_a_non_positive_sample_rate_is_refused(
    pair: tuple[np.ndarray, np.ndarray], sample_rate: int
) -> None:
    reference, take = pair
    with pytest.raises(SonareError):
        align_take_to_reference(reference, take, sample_rate)


def test_an_unalignable_pair_is_reported_rather_than_answered(
    pair: tuple[np.ndarray, np.ndarray],
) -> None:
    """Too short to yield two distinct anchors is what an unalignable pair looks like."""
    reference, take = pair
    with pytest.raises(SonareError):
        align_take_to_reference(reference[:64], take[:64], SR)


@pytest.mark.parametrize("bins_per_octave", [-1, 6, 13, 18])
def test_a_bins_per_octave_off_the_twelve_bin_grid_is_refused(
    pair: tuple[np.ndarray, np.ndarray], bins_per_octave: int
) -> None:
    """Only a positive multiple of 12 is in domain -- the chroma folds onto 12 classes.

    13, 18 and 6 are also the cheapest proof that the field reaches the
    measurement: they are positive, so nothing between the keyword and the chroma
    grid has a reason to object, and a value read off the caller and dropped would
    be answered with the default resolution's anchors instead of a refusal.
    """
    reference, take = pair
    with pytest.raises(SonareError):
        align_take_to_reference(reference, take, SR, bins_per_octave=bins_per_octave)


def test_the_rate_a_resolution_needs_rises_with_it() -> None:
    """The docstring's sample-rate claim, pinned on both sides.

    8 kHz carries the grid at the default resolution and does not at 24 bins per
    octave, because the top CQT bin rises with the resolution and has to stay
    under Nyquist. The pair is the accept and the refuse, so neither half can be
    read as "this rate never works" or "the rate is never checked".
    """
    reference = glide(0.5, sr=8000)
    take = glide(0.75, sr=8000)

    anchors, _ = align_take_to_reference(reference, take, 8000)
    assert len(anchors) >= 2
    with pytest.raises(SonareError):
        align_take_to_reference(reference, take, 8000, bins_per_octave=24)


@pytest.mark.parametrize("hop_length", [512.5, 2**32 + 8])
def test_a_config_value_the_c_field_would_fold_is_refused(
    pair: tuple[np.ndarray, np.ndarray], hop_length: float
) -> None:
    """``2**32 + 8`` lands on 8 as an int32, which is a hop a caller could have asked for."""
    reference, take = pair
    with pytest.raises(SonareValueError, match="hop_length"):
        align_take_to_reference(reference, take, SR, hop_length=hop_length)  # type: ignore[arg-type]
