"""Tests for the N-channel linked denoise / dereverb Python wrappers.

``mastering_repair_denoise_classical_linked`` and
``..._dereverb_classical_linked`` generalise the stereo pair to any channel
count, and they are the only repair entries whose output planes are
caller-owned: nothing is heap-allocated on the C side, so the buffers handed
in are the ones that come back. What that costs a wrapper is a pointer table
per direction; what it costs a test is that a plane written to the wrong slot
is invisible in any shape assertion.

Three properties the fixtures below are built to witness, none of which a
default-config smoke test reaches:

* The channel count is a real parameter of the measurement for denoise and
  not for dereverb. ``NoiseDetection`` carries absolute levels referred to
  the channel-summed power, so N identical channels read ``10*log10(N)``
  above one of them, while every dereverb report field is a ratio.
* The two entries are the same call shape with opposite behaviour on a short
  buffer -- denoise refuses one, dereverb pads it -- so a wrapper written by
  copying one onto the other passes everything except that.
* Routing. A silent channel contributes exactly zero to the summed power, so
  the same signal in slot 0 and in slot 1 produces the same mask; only the
  slot the output lands in separates the two runs.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any, cast

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
DURATION = 0.3


def _noisy(freq: float, seed: int, *, duration: float = DURATION) -> NDArray[np.float32]:
    """A tone plus a broadband bed, deterministic per ``seed``."""
    rng = np.random.default_rng(seed)
    tone = sine(freq, duration, sr=SR, amp=0.35)
    bed = (rng.standard_normal(tone.shape[0]) * 0.2).astype(np.float32)
    return (tone + bed).astype(np.float32)


@pytest.fixture(scope="module")
def signal() -> NDArray[np.float32]:
    return _noisy(440.0, seed=11)


@pytest.fixture(scope="module")
def other_signal() -> NDArray[np.float32]:
    """A second channel that differs from ``signal`` at every sample.

    Both the tone and the noise bed differ, so a wrapper that routed one
    input to both output planes -- or swapped the pair -- cannot agree with
    the stereo entry by accident.
    """
    return _noisy(660.0, seed=12)


@pytest.fixture(scope="module")
def silence() -> NDArray[np.float32]:
    return np.zeros(int(SR * DURATION), dtype=np.float32)


def _as_plane(values: list[float]) -> NDArray[np.float32]:
    """The stereo entries return Python floats; compare in the C side's type.

    ``float32 -> float -> float32`` is exact, so this stays an equality
    comparison rather than a tolerance one.
    """
    return np.asarray(values, dtype=np.float32)


class TestDenoiseLinkedAgreesWithTheNarrowerEntries:
    def test_one_channel_reproduces_the_mono_entry(self, signal: NDArray[np.float32]) -> None:
        linked = libsonare.mastering_repair_denoise_classical_linked([signal], SR)
        mono = libsonare.mastering_repair_denoise_classical(signal, SR)

        assert len(linked.channels) == 1
        assert linked.channels[0].dtype == np.float32
        assert np.array_equal(linked.channels[0], mono)

    def test_two_channels_reproduce_the_stereo_entry_plane_for_plane(
        self, signal: NDArray[np.float32], other_signal: NDArray[np.float32]
    ) -> None:
        linked = libsonare.mastering_repair_denoise_classical_linked([signal, other_signal], SR)
        stereo = libsonare.mastering_repair_denoise_classical_stereo(signal, other_signal, SR)

        assert len(linked.channels) == 2
        assert np.array_equal(linked.channels[0], _as_plane(stereo.left))
        assert np.array_equal(linked.channels[1], _as_plane(stereo.right))
        # The pair is not interchangeable: a swapped result would satisfy a
        # set comparison but not this one.
        assert not np.array_equal(linked.channels[0], _as_plane(stereo.right))


class TestDereverbLinkedAgreesWithTheNarrowerEntries:
    def test_one_channel_reproduces_the_mono_entry(self, signal: NDArray[np.float32]) -> None:
        linked = libsonare.mastering_repair_dereverb_classical_linked([signal], SR)
        mono = libsonare.mastering_repair_dereverb_classical(signal, SR)

        assert len(linked.channels) == 1
        assert linked.channels[0].dtype == np.float32
        assert np.array_equal(linked.channels[0], mono)

    def test_two_channels_reproduce_the_stereo_entry_plane_for_plane(
        self, signal: NDArray[np.float32], other_signal: NDArray[np.float32]
    ) -> None:
        linked = libsonare.mastering_repair_dereverb_classical_linked([signal, other_signal], SR)
        stereo = libsonare.mastering_repair_dereverb_classical_stereo(signal, other_signal, SR)

        assert len(linked.channels) == 2
        assert np.array_equal(linked.channels[0], _as_plane(stereo.left))
        assert np.array_equal(linked.channels[1], _as_plane(stereo.right))
        assert not np.array_equal(linked.channels[0], _as_plane(stereo.right))


class TestWhatTheChannelCountMoves:
    def test_denoise_floor_rises_by_ten_log_ten_n(self, signal: NDArray[np.float32]) -> None:
        """The one report field that is a level rather than a ratio.

        The estimator runs on the channel-summed power, so N copies of one
        channel put N times the power under the same spectrum.
        """
        one = libsonare.mastering_repair_denoise_classical_linked([signal], SR)
        three = libsonare.mastering_repair_denoise_classical_linked([signal] * 3, SR)

        delta = three.report.detected.floor_dbfs - one.report.detected.floor_dbfs
        assert delta == pytest.approx(10.0 * np.log10(3.0), abs=1e-3)

        per_band = np.asarray(three.report.detected.band_floor_dbfs) - np.asarray(
            one.report.detected.band_floor_dbfs
        )
        # Bands whose bin range is empty saturate at the floor sentinel and
        # cannot move; every band that measured anything moves by the same
        # amount as the broadband figure.
        live = per_band[np.asarray(one.report.detected.band_floor_dbfs) > -120.0]
        assert live.size > 0
        assert np.allclose(live, 10.0 * np.log10(3.0), atol=1e-3)

    def test_denoise_attenuation_figures_are_fractions_and_stay_put(
        self, signal: NDArray[np.float32]
    ) -> None:
        one = libsonare.mastering_repair_denoise_classical_linked([signal], SR)
        three = libsonare.mastering_repair_denoise_classical_linked([signal] * 3, SR)

        assert three.report.mean_reduction_db == pytest.approx(one.report.mean_reduction_db)
        assert three.report.max_reduction_db == pytest.approx(one.report.max_reduction_db)
        assert three.report.floor_limited_fraction == pytest.approx(
            one.report.floor_limited_fraction
        )

    def test_dereverb_report_does_not_move_with_the_channel_count(
        self, signal: NDArray[np.float32]
    ) -> None:
        """Every field is a ratio or a fraction, so every field compares.

        Compared at a relative tolerance rather than for equality:
        ``late_decay_ratio_db`` is a ratio of two accumulations that the
        linked pass runs over N times as many terms, and on some inputs that
        lands one float32 ULP away. The tolerance is three orders of
        magnitude below the ``10*log10(3)`` = 4.77 dB the denoise floor
        moves by over the same change, so it cannot absorb a figure that
        actually tracks the channel count.
        """
        one = libsonare.mastering_repair_dereverb_classical_linked([signal], SR)
        three = libsonare.mastering_repair_dereverb_classical_linked([signal] * 3, SR)

        assert three.report.mean_reduction_db == pytest.approx(
            one.report.mean_reduction_db, rel=1e-6
        )
        assert three.report.suppressed_fraction == pytest.approx(
            one.report.suppressed_fraction, rel=1e-6
        )
        assert three.report.wpe_predictor_norm == pytest.approx(
            one.report.wpe_predictor_norm, abs=1e-9
        )
        assert three.report.detected.late_decay_ratio_db == pytest.approx(
            one.report.detected.late_decay_ratio_db, rel=1e-6
        )
        assert three.report.detected.late_predictability == pytest.approx(
            one.report.detected.late_predictability, abs=1e-9
        )

    @pytest.mark.parametrize(
        "entry",
        [
            "mastering_repair_denoise_classical_linked",
            "mastering_repair_dereverb_classical_linked",
        ],
    )
    def test_identical_channels_come_back_identical(
        self, entry: str, signal: NDArray[np.float32]
    ) -> None:
        """One shared mask over the set, so three copies stay three copies."""
        result = getattr(libsonare, entry)([signal] * 3, SR)

        assert len(result.channels) == 3
        assert np.array_equal(result.channels[0], result.channels[1])
        assert np.array_equal(result.channels[0], result.channels[2])
        # Not vacuous: the pass did something, so the equality is between
        # processed planes rather than between untouched copies of the input.
        assert not np.array_equal(result.channels[0], signal)


class TestPlaneRouting:
    """Where an output lands, held apart from what it contains.

    A silent channel adds exactly zero to the summed power, so moving the
    only live channel between slots leaves the mask bit-identical. Anything
    that differs between the two runs is routing.
    """

    @pytest.mark.parametrize(
        "entry",
        [
            "mastering_repair_denoise_classical_linked",
            "mastering_repair_dereverb_classical_linked",
        ],
    )
    def test_the_live_channel_comes_back_in_the_slot_it_went_in(
        self, entry: str, signal: NDArray[np.float32], silence: NDArray[np.float32]
    ) -> None:
        call = getattr(libsonare, entry)
        first = call([signal, silence, silence], SR)
        second = call([silence, signal, silence], SR)

        assert np.array_equal(first.channels[0], second.channels[1])
        assert not np.all(first.channels[0] == 0.0)

        for index in (1, 2):
            assert np.all(first.channels[index] == 0.0)
        for index in (0, 2):
            assert np.all(second.channels[index] == 0.0)


class TestShortInputSplit:
    def test_denoise_refuses_a_short_buffer_and_dereverb_pads_one(
        self, signal: NDArray[np.float32]
    ) -> None:
        """The pair most likely to be written by copying one onto the other.

        Identical call shape, opposite behaviour below ``n_fft``.
        """
        short = signal[:512]
        channels = [short, short]

        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_denoise_classical_linked(channels, SR, n_fft=1024)

        padded = libsonare.mastering_repair_dereverb_classical_linked(channels, SR, n_fft=1024)
        assert len(padded.channels) == 2
        assert all(plane.shape[0] == short.shape[0] for plane in padded.channels)


class TestRejections:
    @pytest.fixture(
        params=[
            "mastering_repair_denoise_classical_linked",
            "mastering_repair_dereverb_classical_linked",
        ]
    )
    def call(self, request: pytest.FixtureRequest) -> Callable[..., Any]:
        """Both entries, so a rejection added to only one of them fails here."""
        return cast(Callable[..., Any], getattr(libsonare, request.param))

    def test_empty_channel_sequence(self, call: Callable[..., Any]) -> None:
        with pytest.raises(libsonare.SonareValueError, match="channels must not be empty"):
            call([], SR)

    def test_channels_of_different_lengths(
        self, call: Callable[..., Any], signal: NDArray[np.float32]
    ) -> None:
        with pytest.raises(libsonare.SonareValueError, match="same length"):
            call([signal, signal[:512]], SR)

    def test_non_finite_sample_past_the_first_channel(
        self, call: Callable[..., Any], signal: NDArray[np.float32]
    ) -> None:
        """Every channel is validated, not just the one a loop would reach first."""
        bad = signal.copy()
        bad[17] = np.nan
        with pytest.raises(libsonare.SonareValueError, match=r"channels\[2\].*index 17"):
            call([signal, signal, bad], SR)

    def test_bad_sample_rate(self, call: Callable[..., Any], signal: NDArray[np.float32]) -> None:
        with pytest.raises(libsonare.SonareError):
            call([signal], 0)

    def test_n_fft_that_is_not_a_power_of_two(
        self, call: Callable[..., Any], signal: NDArray[np.float32]
    ) -> None:
        with pytest.raises(libsonare.SonareValueError, match="power of two"):
            call([signal], SR, n_fft=1000)
