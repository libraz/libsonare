"""The streaming mastering chain and equalizer take the shapes the other surfaces use.

Construction builds and prepares in one step with a required sample rate,
results are float32 arrays (a stereo block is a ``StereoBlock``), plain counters
are properties, and the phase mode and band placement are exact lowerCamel tokens.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from libsonare import (
    ErrorCode,
    SonareValueError,
    StereoBlock,
    StreamingEqualizer,
    StreamingMasteringChain,
)

from ._helpers import LIB_AVAILABLE, raises_coded

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000
IMAGER = {"stereo": {"imager": {"enabled": 1, "width": 1.5}}}


def _tone(n: int) -> np.ndarray:
    return (0.25 * np.sin(2 * math.pi * 440.0 * np.arange(n) / SR)).astype(np.float32)


def test_the_sample_rate_is_required() -> None:
    with pytest.raises(TypeError):
        StreamingMasteringChain({"eq": {"tilt": {"tiltDb": 1.0}}}, num_channels=1)  # type: ignore[call-arg]
    with pytest.raises(TypeError):
        StreamingEqualizer()  # type: ignore[call-arg]
    with pytest.raises(TypeError):
        StreamingEqualizer(SR)  # type: ignore[misc]


def test_the_chain_is_ready_once_constructed() -> None:
    with StreamingMasteringChain(
        {"eq": {"tilt": {"tiltDb": 3.0}}}, sample_rate=SR, num_channels=1
    ) as chain:
        assert not hasattr(chain, "prepare")
        assert "eq.tilt" in chain.stage_names()
        out = chain.process_mono(_tone(128))
        assert isinstance(out, np.ndarray) and out.dtype == np.float32
        assert out.shape == (128,)
        assert isinstance(chain.flush_mono(), np.ndarray)


def test_the_channel_count_given_at_construction_selects_the_stereo_stages() -> None:
    with StreamingMasteringChain(IMAGER, sample_rate=SR, num_channels=1) as mono:
        assert "stereo.imager" not in mono.stage_names()
    with StreamingMasteringChain(IMAGER, sample_rate=SR, num_channels=2) as stereo:
        assert "stereo.imager" in stereo.stage_names()
        left, right = stereo.process_stereo(_tone(128), _tone(128) * 0.5)
        assert isinstance(left, np.ndarray) and isinstance(right, np.ndarray)
        assert left.dtype == right.dtype == np.float32
        flushed = stereo.flush_stereo()
        assert all(isinstance(channel, np.ndarray) for channel in flushed)


def test_explicit_zero_block_size_is_refused_before_channel_prepare() -> None:
    with pytest.raises(SonareValueError, match="max_block_size"):
        StreamingMasteringChain(sample_rate=SR, max_block_size=0, num_channels=3)
    with pytest.raises(SonareValueError, match="sample_rate"):
        StreamingMasteringChain(sample_rate=7999, max_block_size=0, num_channels=3)


@pytest.mark.parametrize(
    ("field", "value"),
    [
        pytest.param("loudness_static_gain_db", math.nan, id="control: gain NaN"),
        pytest.param("loudness_static_gain_peak_dbtp", math.nan, id="peak NaN without gain"),
        pytest.param(
            "loudness_static_gain_peak_dbtp",
            1e100,
            id="peak beyond float32 without gain",
        ),
    ],
)
def test_each_written_loudness_option_is_checked_before_float_narrowing(
    field: str, value: float
) -> None:
    with pytest.raises(SonareValueError) as excinfo:
        StreamingMasteringChain(
            sample_rate=SR,
            num_channels=1,
            **{field: value},
        )
    assert excinfo.value.code == ErrorCode.INVALID_PARAMETER
    assert str(excinfo.value) == (
        f"StreamingMasteringChain: {field} must be finite and in [-3.4028235e38, 3.4028235e38]"
    )


def test_loudness_option_absence_and_silent_peak_are_not_sentinels() -> None:
    with StreamingMasteringChain(
        sample_rate=SR,
        num_channels=1,
        loudness_static_gain_db=None,
        loudness_static_gain_peak_dbtp=None,
    ):
        pass
    with StreamingMasteringChain(
        sample_rate=SR,
        num_channels=1,
        loudness_static_gain_db=0.0,
        loudness_static_gain_peak_dbtp=-math.inf,
    ):
        pass


def test_configuration_refusal_precedes_a_nonfinite_static_gain() -> None:
    config = {"repair": {"declick": {"enabled": 1}}}
    with raises_coded(ErrorCode.NOT_SUPPORTED, match="repair.declick"):
        StreamingMasteringChain(
            config,
            sample_rate=SR,
            num_channels=1,
            loudness_static_gain_db=math.inf,
        )


def test_the_block_size_given_at_construction_bounds_a_block() -> None:
    with StreamingMasteringChain(
        {"eq": {"tilt": {"tiltDb": 1.0}}}, sample_rate=SR, max_block_size=64, num_channels=1
    ) as chain:
        chain.process_mono(_tone(64))
        with pytest.raises(SonareValueError):
            chain.process_mono(_tone(65))


@pytest.mark.parametrize("value", [0, 3])
def test_a_channel_count_other_than_one_or_two_is_refused(value: int) -> None:
    with pytest.raises(SonareValueError):
        StreamingMasteringChain(
            {"eq": {"tilt": {"tiltDb": 1.0}}}, sample_rate=SR, num_channels=value
        )


def test_counters_are_properties() -> None:
    with StreamingMasteringChain(
        {"eq": {"tilt": {"tiltDb": 1.0}}}, sample_rate=SR, num_channels=1
    ) as chain:
        assert chain.non_finite_substitution_count == 0
        assert chain.non_finite_discard_count == 0
        assert isinstance(chain.latency_samples, int)
    with StreamingEqualizer(sample_rate=SR) as eq:
        assert eq.non_finite_discard_count == 0
        assert isinstance(eq.latency_samples, int)


@pytest.mark.parametrize("mode", ["zero", "natural", "linear"])
def test_each_phase_mode_token_is_accepted(mode: str) -> None:
    with StreamingEqualizer(sample_rate=SR) as eq:
        eq.set_phase_mode(mode)  # type: ignore[arg-type]


@pytest.mark.parametrize("mode", ["zero-latency", "linear_phase", "Linear", "natural-phase"])
def test_a_phase_mode_outside_the_tokens_is_refused(mode: str) -> None:
    with StreamingEqualizer(sample_rate=SR) as eq, pytest.raises(SonareValueError, match="zero"):
        eq.set_phase_mode(mode)  # type: ignore[arg-type]


@pytest.mark.parametrize("mode", [1, 3, True])
def test_a_phase_mode_ordinal_is_a_type_error(mode: object) -> None:
    with StreamingEqualizer(sample_rate=SR) as eq, pytest.raises(TypeError):
        eq.set_phase_mode(mode)  # type: ignore[arg-type]


def test_the_band_count_of_a_match_is_keyword_only() -> None:
    source = _tone(4096)
    with StreamingEqualizer(sample_rate=SR) as eq:
        eq.match(source, source * 0.5, max_bands=4)
        with pytest.raises(TypeError):
            eq.match(source, source * 0.5, 4)  # type: ignore[misc]


def test_equalizer_results_are_arrays_with_snake_case_fields() -> None:
    with StreamingEqualizer(sample_rate=SR) as eq:
        out = eq.process_mono(_tone(256))
        assert isinstance(out, np.ndarray) and out.dtype == np.float32
        left, right = eq.process_stereo(_tone(256), _tone(256))
        assert isinstance(left, np.ndarray) and isinstance(right, np.ndarray)
        curve = eq.magnitude_response([100.0, 1000.0])
        assert isinstance(curve, np.ndarray) and curve.shape == (2,)
        snapshot = eq.spectrum()
    for field in ("pre_left", "pre_right", "post_left", "post_right", "band_gain_db", "profile_db"):
        value = getattr(snapshot, field)
        assert isinstance(value, np.ndarray) and value.dtype == np.float32
    for alias in ("preLeft", "postRight", "bandGainDb", "profileDb", "lastAutoGainDb"):
        assert not hasattr(snapshot, alias)


def test_a_stereo_block_names_its_channels_and_still_unpacks() -> None:
    with StreamingMasteringChain(IMAGER, sample_rate=SR, num_channels=2) as chain:
        block = chain.process_stereo(_tone(64), _tone(64))
        assert isinstance(block, StereoBlock)
        left, right = block
        np.testing.assert_array_equal(left, block.left)
        np.testing.assert_array_equal(right, block.right)
        assert isinstance(chain.flush_stereo(), StereoBlock)
    with StreamingEqualizer(sample_rate=SR) as eq:
        assert isinstance(eq.process_stereo(_tone(64), _tone(64)), StereoBlock)


@pytest.mark.parametrize("placement", ["stereo", "left", "right", "mid", "side"])
def test_each_placement_token_is_accepted(placement: str) -> None:
    with StreamingEqualizer(sample_rate=SR) as eq:
        eq.magnitude_response([1000.0], placement=placement)  # type: ignore[arg-type]


@pytest.mark.parametrize("placement", ["Stereo", "LEFT", "Mid", "rear"])
def test_a_placement_outside_the_tokens_is_refused(placement: str) -> None:
    with StreamingEqualizer(sample_rate=SR) as eq, pytest.raises(SonareValueError, match="stereo"):
        eq.magnitude_response([1000.0], placement=placement)  # type: ignore[arg-type]


def test_a_placement_ordinal_is_a_type_error() -> None:
    with StreamingEqualizer(sample_rate=SR) as eq, pytest.raises(TypeError):
        eq.magnitude_response([1000.0], placement=1)  # type: ignore[arg-type]
