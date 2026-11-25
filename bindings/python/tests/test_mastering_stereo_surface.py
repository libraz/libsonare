"""Native integration coverage for the stereo mastering Python surface."""

from __future__ import annotations

import math

import numpy as np
import pytest

from libsonare import (
    ErrorCode,
    LoudnessMatchStereoResult,
    MasteringStereoResult,
    SonareError,
    SonareValueError,
)

from ._helpers import raises_coded, sine


def test_public_stereo_pair_and_loudness_functions_exist() -> None:
    import libsonare

    assert callable(getattr(libsonare, "mastering_apply_pair_processor_stereo", None))
    assert callable(getattr(libsonare, "mastering_ab_match_loudness_stereo", None))


def test_stereo_pair_rejects_nul_c_strings_before_library_dispatch() -> None:
    import libsonare

    with pytest.raises(SonareValueError, match="processor must not contain NUL"):
        libsonare.mastering_apply_pair_processor_stereo(
            "match.abCrossfade\x00junk",
            [0.1],
            [-0.1],
            [0.2],
            [-0.2],
            sample_rate=48000,
        )
    with pytest.raises(SonareValueError, match="parameter key must not contain NUL"):
        libsonare.mastering_apply_pair_processor_stereo(
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

    mixed = libsonare.mastering_apply_pair_processor_stereo(
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

    at_source = libsonare.mastering_apply_pair_processor_stereo(
        "match.abCrossfade",
        source_left,
        source_right,
        reference_left,
        reference_right,
        48000,
        {"mix": 0.0},
    )
    at_reference = libsonare.mastering_apply_pair_processor_stereo(
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


def test_stereo_pair_control_rejects_unsupported_processor_before_audio_domains() -> None:
    import libsonare

    with raises_coded(
        ErrorCode.NOT_SUPPORTED,
        match=r": processor does not support planar stereo pairs",
    ):
        libsonare.mastering_apply_pair_processor_stereo(
            "match.abSwitch",
            [0.1, 0.2],
            [-0.1, -0.2],
            [0.4, 0.5],
            [-0.4, -0.5],
            sample_rate=0,
        )


@pytest.mark.parametrize(
    ("call_kwargs", "error_type", "message", "exact"),
    [
        pytest.param(
            {
                "processor": "match.unknown",
                "source_left": [0.1, 0.2],
                "source_right": [-0.1],
                "reference_left": [0.2, -0.2],
                "reference_right": [0.2, -0.2],
                "sample_rate": 0,
            },
            SonareValueError,
            "mastering_apply_pair_processor_stereo: processor must be one of ",
            False,
            id="processor membership precedes unequal plane lengths",
        ),
        pytest.param(
            {
                "processor": "match.unknown",
                "source_left": [math.nan, 0.2],
                "source_right": [0.1, -0.1],
                "reference_left": [0.2, -0.2],
                "reference_right": [0.2, -0.2],
                "sample_rate": 0,
                "params": 1,
            },
            TypeError,
            "mastering_apply_pair_processor_stereo: params must be an object",
            True,
            id="params bag shape precedes processor membership and domains",
        ),
        pytest.param(
            {
                "processor": "match.unknown",
                "source_left": [math.nan, 0.2],
                "source_right": [0.1, -0.1],
                "reference_left": [0.2, -0.2],
                "reference_right": [0.2, -0.2],
                "sample_rate": 0,
                "params": {"mix": "bad"},
            },
            TypeError,
            "mastering_apply_pair_processor_stereo: Parameter 'mix' must be a number or boolean "
            "(got an unsupported type)",
            True,
            id="parameter value shape precedes processor membership and domains",
        ),
        pytest.param(
            {
                "processor": "match.abCrossfade",
                "source_left": [math.nan, 0.2],
                "source_right": [0.1, -0.1],
                "reference_left": [0.2, -0.2],
                "reference_right": [0.2, -0.2],
                "sample_rate": 48_000.5,
                "reference_sample_rate": 0,
            },
            SonareValueError,
            (
                "mastering_apply_pair_processor_stereo: sample_rate must be an integer in "
                "[8000, 384000]"
            ),
            True,
            id="sampleRate domain precedes source samples",
        ),
        pytest.param(
            {
                "processor": "match.abCrossfade",
                "source_left": [0.1, -0.1],
                "source_right": [0.1, -0.1],
                "reference_left": [0.2, -0.2],
                "reference_right": [0.2, -0.2],
                "sample_rate": 48_000,
                "reference_sample_rate": 0,
                "params": {"mix": 2},
            },
            SonareValueError,
            (
                "mastering_apply_pair_processor_stereo: reference_sample_rate must be an integer "
                "in [8000, 384000]"
            ),
            True,
            id="control: referenceSampleRate domain precedes parameter domain",
        ),
        pytest.param(
            {
                "processor": "match.abCrossfade",
                "source_left": [math.nan, 0.2],
                "source_right": [0.1, -0.1],
                "reference_left": [0.2, -0.2],
                "reference_right": [0.2, -0.2],
                "sample_rate": 48_000,
                "reference_sample_rate": 48_000.5,
            },
            SonareValueError,
            (
                "mastering_apply_pair_processor_stereo: source_left contains a non-finite "
                "sample at index 0"
            ),
            True,
            id="control: source samples precede referenceSampleRate domain",
        ),
    ],
)
def test_stereo_pair_uses_shape_first_and_c_field_order_for_compound_refusals(
    call_kwargs: dict[str, object],
    error_type: type[BaseException],
    message: str,
    exact: bool,
) -> None:
    import libsonare

    with pytest.raises(error_type) as excinfo:
        libsonare.mastering_apply_pair_processor_stereo(**call_kwargs)
    if exact:
        assert str(excinfo.value) == message
    else:
        assert str(excinfo.value).startswith(message)
    if error_type is SonareValueError:
        assert excinfo.value.code == ErrorCode.INVALID_PARAMETER


def test_stereo_pair_rejects_bad_plane_shape_and_nonfinite_audio() -> None:
    import libsonare

    with pytest.raises(SonareValueError, match="source_left and source_right"):
        libsonare.mastering_apply_pair_processor_stereo(
            "match.abCrossfade",
            [0.1, 0.2],
            [-0.1],
            [0.4],
            [-0.4],
            sample_rate=48000,
            params={"mix": 0.5},
        )
    with pytest.raises(SonareValueError, match="source_left and source_right"):
        libsonare.mastering_apply_pair_processor_stereo(
            "match.abCrossfade",
            [math.nan, 0.2],
            [-0.1],
            [0.4],
            [-0.4],
            sample_rate=48000,
            params={"mix": 0.5},
        )
    with pytest.raises(SonareValueError, match="reference_left and reference_right"):
        libsonare.mastering_apply_pair_processor_stereo(
            "match.abCrossfade",
            [0.1],
            [-0.1],
            [0.4, 0.5],
            [-0.4],
            sample_rate=48000,
            params={"mix": 0.5},
        )
    with pytest.raises(SonareValueError, match="source_left"):
        libsonare.mastering_apply_pair_processor_stereo(
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

    match = libsonare.mastering_ab_match_loudness_stereo(
        source_left,
        source_right,
        reference_left,
        reference_right,
        sample_rate=sample_rate,
    )
    assert isinstance(match, LoudnessMatchStereoResult)
    with pytest.raises(TypeError):
        iter(match)
    matched_left, matched_right = match.left, match.right
    assert match.sample_rate == sample_rate
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


def test_stereo_loudness_match_rejects_bad_planes() -> None:
    import libsonare

    source_left = sine(220.0, 0.8, sr=48000, amp=0.05)
    source_right = -sine(220.0, 0.8, sr=48000, amp=0.05)
    reference_left = sine(330.0, 0.6, sr=48000, amp=0.2)
    reference_right = -sine(330.0, 0.6, sr=48000, amp=0.2)
    with pytest.raises(SonareValueError, match="reference_left and reference_right"):
        libsonare.mastering_ab_match_loudness_stereo(
            source_left,
            source_right,
            reference_left,
            reference_right[:-1],
            sample_rate=48000,
        )


def test_streaming_set_parameter_rejects_nonfinite() -> None:
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain(
        {"eq": {"tilt": {"tiltDb": 0.0}}}, sample_rate=48000, num_channels=1
    )
    with pytest.raises(SonareValueError, match="key must not be empty"):
        chain.set_parameter("", 3.0)
    with pytest.raises(SonareValueError, match="value must be finite"):
        chain.set_parameter("eq.tilt.tiltDb", math.inf)
    chain.set_parameter("eq.tilt.tiltDb", 3.0)
    chain.close()
    with raises_coded(ErrorCode.INVALID_STATE, match="closed"):
        chain.set_parameter("eq.tilt.tiltDb", 3.0)


def test_streaming_set_parameter_rejects_nul_key_before_library_dispatch() -> None:
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain(sample_rate=48000, num_channels=1)
    try:
        with pytest.raises(SonareValueError, match="key must not contain NUL"):
            chain.set_parameter("maximizer.truePeakLimiter.ceilingDbtp\x00junk", -3.0)
    finally:
        chain.close()


def test_streaming_rejected_structural_update_preserves_processing_state() -> None:
    from libsonare import StreamingMasteringChain

    config = {"maximizer": {"truePeakLimiter": {"enabled": 1.0, "ceilingDbtp": -1.0}}}
    chain = StreamingMasteringChain(config, sample_rate=48000, num_channels=1)
    control = StreamingMasteringChain(config, sample_rate=48000, num_channels=1)
    try:
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


def test_stereo_log_band_params_name_the_caller_keys_and_keep_their_defaults() -> None:
    import libsonare

    sample_rate = 48000
    left = sine(220.0, 0.5, sr=sample_rate, amp=0.2)
    right = sine(440.0, 0.5, sr=sample_rate, amp=0.35)
    analysis = "stereo.monoCompatCheckLogBands"

    defaults = libsonare.mastering_analyze_stereo(analysis, left, right, sample_rate)
    spelled_defaults = libsonare.mastering_analyze_stereo(
        analysis,
        left,
        right,
        sample_rate,
        {"bandsPerOctave": 3, "lowHz": 20.0, "highHz": 20000.0},
    )
    assert len(defaults.bands) == len(spelled_defaults.bands)
    for actual, expected in zip(defaults.bands, spelled_defaults.bands, strict=True):
        assert actual.low_hz == pytest.approx(expected.low_hz)
        assert actual.high_hz == pytest.approx(expected.high_hz)
        assert actual.side_rms == pytest.approx(expected.side_rms)
        if actual.correlation is None:
            assert expected.correlation is None
        else:
            assert actual.correlation == pytest.approx(expected.correlation)

    near_boundary = libsonare.mastering_analyze_stereo(
        analysis,
        left,
        right,
        sample_rate,
        {"bandsPerOctave": 1, "lowHz": 1.0, "highHz": sample_rate / 2},
    )
    assert near_boundary.bands

    with raises_coded(match=r"params\.bandsPerOctave"):
        libsonare.mastering_analyze_stereo(
            analysis, left, right, sample_rate, {"bandsPerOctave": 0}
        )
    with raises_coded(match=r"params\.lowHz"):
        libsonare.mastering_analyze_stereo(analysis, left, right, sample_rate, {"lowHz": 0.0})
    with raises_coded(match=r"params\.highHz"):
        libsonare.mastering_analyze_stereo(analysis, left, right, sample_rate, {"highHz": 0.0})
