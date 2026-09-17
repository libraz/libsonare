"""Tests for the two stereo normalizers.

``normalize_stereo`` / ``normalize_rms_stereo`` are ctypes pass-throughs over
``sonare::normalize_stereo`` / ``sonare::normalize_rms_stereo``. What defines
them over calling the mono normalizer on each channel is that the level is
measured across the PAIR and one gain goes to both, so the stereo balance
survives. Every fixture here therefore holds two channels at deliberately
different levels: a per-channel implementation would satisfy any assertion that
only looks at the louder side, so each level assertion is paired with the mono
result on the same channel as its control.

The waveform is a ``±amp`` alternation rather than a tone because its peak and
its RMS are both exactly ``amp``, which lets the expected gains be written as
arithmetic rather than as a measurement of the fixture.
"""

from __future__ import annotations

import inspect

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare
from libsonare import SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050

# The louder channel and the gap to the quieter one. 12 dB is wide enough that a
# per-channel gain and a pair gain cannot be confused by float32 rounding: the
# quiet channel lands either at the target or a clean 12 dB below it.
_LOUD_AMP = 0.4
_GAP_DB = 12.0


def _alternating(amp: float, length: int = 1024) -> NDArray[np.float32]:
    """A ``±amp`` alternation, whose peak and RMS are both exactly ``amp``."""
    out = np.empty(length, dtype=np.float32)
    out[0::2] = amp
    out[1::2] = -amp
    return out


def _pair() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    """Left 12 dB under right, both otherwise identical."""
    return _alternating(_LOUD_AMP * 10.0 ** (-_GAP_DB / 20.0)), _alternating(_LOUD_AMP)


def _peak_db(samples: object) -> float:
    return float(20.0 * np.log10(np.max(np.abs(np.asarray(samples, dtype=np.float64)))))


def _rms_db(samples: object) -> float:
    return float(20.0 * np.log10(np.sqrt(np.mean(np.asarray(samples, dtype=np.float64) ** 2))))


class TestNormalizeStereoBalance:
    def test_one_gain_for_the_pair_keeps_the_channels_12_db_apart(self) -> None:
        left, right = _pair()
        result = libsonare.normalize_stereo(left, right, SR, target_db=-3.0)

        assert isinstance(result.left, list)
        assert isinstance(result.right, list)
        assert result.length == len(left)
        assert len(result.left) == len(left)
        assert len(result.right) == len(right)

        # The louder channel reaches the target; the quieter one keeps its
        # distance from it rather than being lifted to meet it.
        assert _peak_db(result.right) == pytest.approx(-3.0, abs=0.01)
        assert _peak_db(result.left) == pytest.approx(-15.0, abs=0.01)
        assert _peak_db(result.right) - _peak_db(result.left) == pytest.approx(_GAP_DB, abs=0.01)

        # One gain, and it is the one the louder channel needed.
        assert result.applied_gain_db == pytest.approx(-3.0 - _peak_db(right), abs=0.01)

    def test_the_mono_normalizer_puts_the_quiet_channel_somewhere_else(self) -> None:
        """Control: the same channel, normalized on its own gain, lands at the target.

        Without this the balance assertion above is untestable by inspection --
        it asserts a number, and a per-channel implementation returning -3.0 dB
        for both channels would only differ by which number. The mono call is
        exactly that implementation, so the two results must sit ``_GAP_DB``
        apart or the stereo entry is not doing what it claims.
        """
        left, right = _pair()
        stereo = libsonare.normalize_stereo(left, right, SR, target_db=-3.0)
        mono = libsonare.normalize(left, SR, target_db=-3.0)

        assert _peak_db(mono) == pytest.approx(-3.0, abs=0.01)
        assert _peak_db(mono) - _peak_db(stereo.left) == pytest.approx(_GAP_DB, abs=0.01)

    def test_rms_variant_shares_its_gain_the_same_way(self) -> None:
        left, right = _pair()
        result = libsonare.normalize_rms_stereo(left, right, SR, target_db=-20.0)
        mono = libsonare.normalize_rms(left, SR, target_db=-20.0)

        assert _rms_db(result.right) - _rms_db(result.left) == pytest.approx(_GAP_DB, abs=0.01)
        assert _rms_db(mono) == pytest.approx(-20.0, abs=0.01)
        assert _rms_db(mono) - _rms_db(result.left) > 1.0


class TestNormalizeRmsStereoLevel:
    def test_the_joint_level_is_the_quadratic_mean_not_an_average(self) -> None:
        """The pair's RMS is taken over both channels' samples together.

        The two averages a reader might expect instead -- the arithmetic mean of
        the per-channel dB figures, and the arithmetic mean of the per-channel
        linear RMS values -- are asserted to be measurably different answers, so
        this cannot pass by landing near any of the three.
        """
        left, right = _pair()
        result = libsonare.normalize_rms_stereo(left, right, SR, target_db=-20.0)

        left64 = left.astype(np.float64)
        right64 = right.astype(np.float64)
        left_rms = float(np.sqrt(np.mean(left64**2)))
        right_rms = float(np.sqrt(np.mean(right64**2)))
        joint_db = float(20.0 * np.log10(np.sqrt((left_rms**2 + right_rms**2) / 2.0)))

        assert result.applied_gain_db == pytest.approx(-20.0 - joint_db, abs=0.01)

        mean_of_db = (_rms_db(left) + _rms_db(right)) / 2.0
        mean_of_linear = float(20.0 * np.log10((left_rms + right_rms) / 2.0))
        assert abs(joint_db - mean_of_db) > 1.0
        assert abs(joint_db - mean_of_linear) > 1.0

    def test_an_rms_target_that_overshoots_full_scale_is_hard_clipped(self) -> None:
        """Peaks sit above the RMS, so a loud RMS target saturates -- as documented.

        ``applied_gain_db`` reports the gain that was applied, which is the
        unclipped figure; the achieved RMS is therefore below the target.
        """
        tone = sine(440.0, 0.1, sr=SR, amp=0.5)
        result = libsonare.normalize_rms_stereo(tone, tone, SR, target_db=0.0)

        unclipped_peak = 0.5 * 10.0 ** (result.applied_gain_db / 20.0)
        assert unclipped_peak > 1.0
        assert np.max(np.abs(result.left)) == pytest.approx(1.0, abs=1e-6)
        assert np.max(np.abs(result.right)) == pytest.approx(1.0, abs=1e-6)
        assert _rms_db(result.left) < -1.0


class TestNormalizeStereoSilence:
    @pytest.mark.parametrize(
        ("call", "target_db"),
        [
            (libsonare.normalize_stereo, -3.0),
            (libsonare.normalize_rms_stereo, -20.0),
        ],
    )
    def test_a_silent_pair_comes_back_untouched_at_zero_gain(
        self, call: object, target_db: float
    ) -> None:
        silence = np.zeros(512, dtype=np.float32)
        result = call(silence, silence, SR, target_db=target_db)  # type: ignore[operator]

        assert result.applied_gain_db == 0.0
        assert result.length == len(silence)
        assert np.array_equal(np.asarray(result.left, dtype=np.float32), silence)
        assert np.array_equal(np.asarray(result.right, dtype=np.float32), silence)

    @pytest.mark.parametrize(
        ("call", "target_db"),
        [
            (libsonare.normalize_stereo, -3.0),
            (libsonare.normalize_rms_stereo, -20.0),
        ],
    )
    def test_a_quiet_but_not_silent_pair_does_move(self, call: object, target_db: float) -> None:
        """Control for the silence case: 0.0 dB must mean silence, not "quiet".

        At -80 dBFS the pair is far below anything audible and still sits well
        above the silence epsilon, so a zero gain here would mean the
        short-circuit had swallowed a real signal.
        """
        quiet = _alternating(1e-4, 512)
        result = call(quiet, quiet, SR, target_db=target_db)  # type: ignore[operator]

        assert result.applied_gain_db == pytest.approx(target_db + 80.0, abs=0.01)
        assert result.applied_gain_db != 0.0


class TestNormalizeStereoPairRefusals:
    """The pair's own preconditions, each against a control that the same call
    shape is otherwise accepted -- so a refusal cannot be read as the entry
    point being unreachable."""

    @pytest.mark.parametrize("call", [libsonare.normalize_stereo, libsonare.normalize_rms_stereo])
    def test_a_valid_pair_is_accepted(self, call: object) -> None:
        left, right = _pair()
        result = call(left, right, SR)  # type: ignore[operator]
        assert result.length == len(left)

    @pytest.mark.parametrize("call", [libsonare.normalize_stereo, libsonare.normalize_rms_stereo])
    def test_unequal_lengths_are_refused(self, call: object) -> None:
        left, right = _pair()
        with pytest.raises(SonareValueError, match="lengths must match"):
            call(left[:-1], right, SR)  # type: ignore[operator]
        with pytest.raises(SonareValueError, match="lengths must match"):
            call(left, right[:-1], SR)  # type: ignore[operator]

    @pytest.mark.parametrize("call", [libsonare.normalize_stereo, libsonare.normalize_rms_stereo])
    def test_an_empty_channel_is_refused_by_name(self, call: object) -> None:
        left, right = _pair()
        empty = np.zeros(0, dtype=np.float32)
        with pytest.raises(SonareValueError, match="left must not be empty"):
            call(empty, right, SR)  # type: ignore[operator]
        with pytest.raises(SonareValueError, match="right must not be empty"):
            call(left, empty, SR)  # type: ignore[operator]

    @pytest.mark.parametrize("call", [libsonare.normalize_stereo, libsonare.normalize_rms_stereo])
    def test_the_two_channels_cannot_disagree_on_the_sample_rate(self, call: object) -> None:
        """The pair shares one rate by construction, so there is nothing to refuse.

        ``require_stereo_pair`` rejects a sample-rate mismatch in the core, but
        the C entry takes one ``sample_rate`` for the pair and this facade
        mirrors it, so the mismatch is not expressible here. Asserting the
        parameter list is what keeps that true: a second per-channel rate
        parameter added later would reopen the case with no refusal behind it.
        """
        parameters = inspect.signature(call).parameters  # type: ignore[arg-type]
        rate_parameters = [name for name in parameters if "sample_rate" in name]
        assert rate_parameters == ["sample_rate"]

    @pytest.mark.parametrize("call", [libsonare.normalize_stereo, libsonare.normalize_rms_stereo])
    def test_a_non_positive_sample_rate_is_refused_by_the_c_layer(self, call: object) -> None:
        """And it is the C layer refusing it, not a Python-side guard.

        The negative type assertion matters because ``SonareValueError``
        subclasses ``SonareError``: a bare ``pytest.raises(SonareError)`` would
        stay green if this ever started being caught in the facade, which would
        mean the C validation had stopped being exercised at all.
        """
        left, right = _pair()
        with pytest.raises(SonareError) as caught:
            call(left, right, 0)  # type: ignore[operator]
        assert not isinstance(caught.value, SonareValueError)

    @pytest.mark.parametrize("call", [libsonare.normalize_stereo, libsonare.normalize_rms_stereo])
    def test_a_positive_target_is_refused_by_name(self, call: object) -> None:
        left, right = _pair()
        with pytest.raises(SonareValueError, match="at or below 0 dBFS"):
            call(left, right, SR, target_db=3.0)  # type: ignore[operator]

    @pytest.mark.parametrize("call", [libsonare.normalize_stereo, libsonare.normalize_rms_stereo])
    def test_a_non_finite_sample_is_refused_naming_its_channel(self, call: object) -> None:
        left, right = _pair()
        spoiled = left.copy()
        spoiled[7] = np.nan
        with pytest.raises(SonareValueError, match="left contains NaN or Inf at index 7"):
            call(spoiled, right, SR)  # type: ignore[operator]

    @pytest.mark.parametrize("call", [libsonare.normalize_stereo, libsonare.normalize_rms_stereo])
    def test_validate_false_skips_the_scan_but_not_the_refusal(self, call: object) -> None:
        """``validate=False`` drops the facade's scan onto the C layer's own.

        The scan the flag skips is the one that reports an index; the C entry
        runs its own finite check, so a NaN is still refused -- without the
        index, and as the base ``SonareError``. The negative type assertion is
        what proves the flag was honoured: were the facade still scanning, this
        would arrive as ``SonareValueError`` and the test would pass anyway.
        """
        left, right = _pair()
        spoiled = right.copy()
        spoiled[7] = np.nan
        with pytest.raises(SonareError) as caught:
            call(left, spoiled, SR, validate=False)  # type: ignore[operator]
        assert not isinstance(caught.value, SonareValueError)

        # Control: the same call on a clean pair is accepted with the flag off,
        # so the refusal above is about the NaN rather than about validate=False.
        assert call(left, right, SR, validate=False).length == len(left)  # type: ignore[operator]
