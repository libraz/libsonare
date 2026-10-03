"""Native integration coverage for the stereo mastering Python surface."""

from __future__ import annotations

import math

import numpy as np
import pytest

from libsonare import (
    LoudnessMatch,
    MasteringStereoResult,
    SonareError,
    SonareValueError,
)

from ._helpers import sine


def test_public_stereo_pair_and_loudness_functions_exist() -> None:
    import libsonare

    assert callable(getattr(libsonare, "mastering_pair_process_stereo", None))
    assert callable(getattr(libsonare, "mastering_ab_match_loudness_stereo", None))


def test_stereo_pair_rejects_nul_c_strings_before_library_dispatch() -> None:
    import libsonare

    with pytest.raises(SonareValueError, match="processor_name must not contain NUL"):
        libsonare.mastering_pair_process_stereo(
            "match.abCrossfade\x00junk",
            [0.1],
            [-0.1],
            [0.2],
            [-0.2],
            sample_rate=48000,
        )
    with pytest.raises(SonareValueError, match="parameter key must not contain NUL"):
        libsonare.mastering_pair_process_stereo(
            "match.abCrossfade",
            [0.1],
            [-0.1],
            [0.2],
            [-0.2],
            sample_rate=48000,
            params={"mix\x00junk": 0.5},
        )


def test_stereo_pair_uses_independent_lengths_and_shared_crossfade() -> None:
    import libsonare

    source_left = [1.0] * 8
    source_right = [-1.0] * 8
    reference_left = [3.0] * 5
    reference_right = [-3.0] * 5

    mixed = libsonare.mastering_pair_process_stereo(
        "match.abCrossfade",
        source_left,
        source_right,
        reference_left,
        reference_right,
        sample_rate=48000,
        params={"mix": 0.25},
    )
    assert isinstance(mixed, MasteringStereoResult)
    assert len(mixed.left) == len(reference_left)
    assert len(mixed.right) == len(reference_right)
    assert mixed.left[0] == pytest.approx(1.5, abs=1e-6)
    assert mixed.right[0] == pytest.approx(-1.5, abs=1e-6)

    at_source = libsonare.mastering_pair_process_stereo(
        "match.abCrossfade",
        source_left,
        source_right,
        reference_left,
        reference_right,
        48000,
        {"mix": 0.0},
    )
    at_reference = libsonare.mastering_pair_process_stereo(
        "match.abCrossfade",
        source_left,
        source_right,
        reference_left,
        reference_right,
        sample_rate=48000,
        params={"mix": 1.0},
    )
    assert at_source.left[0] == pytest.approx(source_left[0], abs=1e-6)
    assert at_reference.left[0] == pytest.approx(reference_left[0], abs=1e-6)


def test_stereo_pair_rejects_bad_plane_shape_and_nonfinite_audio() -> None:
    import libsonare

    with pytest.raises(SonareValueError, match="source_left and source_right"):
        libsonare.mastering_pair_process_stereo(
            "match.abCrossfade",
            [0.1, 0.2],
            [-0.1],
            [0.4],
            [-0.4],
            sample_rate=48000,
            params={"mix": 0.5},
        )
    with pytest.raises(SonareValueError, match="reference_left and reference_right"):
        libsonare.mastering_pair_process_stereo(
            "match.abCrossfade",
            [0.1],
            [-0.1],
            [0.4, 0.5],
            [-0.4],
            sample_rate=48000,
            params={"mix": 0.5},
        )
    with pytest.raises(SonareValueError, match="source_left"):
        libsonare.mastering_pair_process_stereo(
            "match.abCrossfade",
            [math.nan],
            [0.1],
            [0.4],
            [-0.4],
            sample_rate=48000,
            params={"mix": 0.5},
        )


def test_stereo_loudness_match_uses_one_gain_for_antiphase_asymmetric_planes() -> None:
    import libsonare

    sample_rate = 48000
    source_left = sine(440.0, 1.0, sr=sample_rate, amp=0.05)
    source_right = -sine(660.0, 1.0, sr=sample_rate, amp=0.15)
    reference_left = sine(440.0, 0.75, sr=sample_rate, amp=0.25)
    reference_right = -sine(660.0, 0.75, sr=sample_rate, amp=0.25)

    (matched_left, matched_right), match = libsonare.mastering_ab_match_loudness_stereo(
        source_left,
        source_right,
        reference_left,
        reference_right,
        sample_rate=sample_rate,
    )
    assert isinstance(match, LoudnessMatch)
    assert matched_left.dtype == np.float32
    assert matched_right.dtype == np.float32
    assert matched_left.shape == source_left.shape
    assert matched_right.shape == source_right.shape
    assert np.max(np.abs(matched_right)) > np.max(np.abs(matched_left))

    # Shared gain preserves the ratio between the two source planes, even when
    # their phase differs and one has a substantially larger peak.
    nonzero = np.flatnonzero(np.abs(source_left) > 1e-3)
    assert len(nonzero) > 0
    index = int(nonzero[len(nonzero) // 2])
    source_ratio = float(source_right[index] / source_left[index])
    matched_ratio = float(matched_right[index] / matched_left[index])
    assert matched_ratio == pytest.approx(source_ratio, rel=1e-5, abs=1e-5)
    assert math.isfinite(match.matched_true_peak_dbtp)


def test_stereo_loudness_match_supports_without_match_and_rejects_bad_planes() -> None:
    import libsonare

    source_left = sine(220.0, 0.8, sr=48000, amp=0.05)
    source_right = -sine(220.0, 0.8, sr=48000, amp=0.05)
    reference_left = sine(330.0, 0.6, sr=48000, amp=0.2)
    reference_right = -sine(330.0, 0.6, sr=48000, amp=0.2)
    (left, right), match = libsonare.mastering_ab_match_loudness_stereo(
        source_left,
        source_right,
        reference_left,
        reference_right,
        sample_rate=48000,
        with_match=False,
    )
    assert match is None
    assert left.shape == source_left.shape
    assert right.shape == source_right.shape

    with pytest.raises(SonareValueError, match="reference_left and reference_right"):
        libsonare.mastering_ab_match_loudness_stereo(
            source_left,
            source_right,
            reference_left,
            reference_right[:-1],
            sample_rate=48000,
        )


def test_streaming_set_parameter_is_prepared_only_and_rejects_nonfinite() -> None:
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain({"eq.tilt.tiltDb": 0.0})
    with pytest.raises(RuntimeError, match="prepared"):
        chain.set_parameter("eq.tilt.tiltDb", 3.0)
    chain.prepare(sample_rate=48000, max_block_size=128, num_channels=1)
    with pytest.raises(SonareValueError, match="value must be a finite number"):
        chain.set_parameter("eq.tilt.tiltDb", math.inf)
    chain.set_parameter("eq.tilt.tiltDb", 3.0)
    chain.close()
    with pytest.raises(RuntimeError, match="closed"):
        chain.set_parameter("eq.tilt.tiltDb", 3.0)


def test_streaming_set_parameter_rejects_nul_key_before_library_dispatch() -> None:
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain()
    try:
        chain.prepare(sample_rate=48000, max_block_size=128, num_channels=1)
        with pytest.raises(SonareValueError, match="key must not contain NUL"):
            chain.set_parameter("maximizer.truePeakLimiter.ceilingDb\x00junk", -3.0)
    finally:
        chain.close()


def test_streaming_rejected_structural_update_preserves_processing_state() -> None:
    from libsonare import StreamingMasteringChain

    config = {
        "maximizer.truePeakLimiter.enabled": 1.0,
        "maximizer.truePeakLimiter.ceilingDb": -1.0,
    }
    chain = StreamingMasteringChain(config)
    control = StreamingMasteringChain(config)
    try:
        chain.prepare(sample_rate=48000, max_block_size=128, num_channels=1)
        control.prepare(sample_rate=48000, max_block_size=128, num_channels=1)
        assert chain.stage_names() == control.stage_names()
        with pytest.raises(SonareError):
            chain.set_parameter("maximizer.truePeakLimiter.enabled", 0.0)
        with pytest.raises(SonareError):
            chain.set_parameter("maximizer.truePeakLimiter.noSuchParameter", 0.0)
        block = [0.8] * 128
        assert chain.process_mono(block) == pytest.approx(control.process_mono(block), abs=1e-6)
    finally:
        chain.close()
        control.close()
