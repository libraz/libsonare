"""Tests for the stereo offline classical-denoise Python wrapper.

``mastering_repair_denoise_classical_stereo`` is a ctypes pass-through over
``mastering::repair::denoise_classical_stereo``. Unlike the declick, declip,
decrackle and dehum stereo wrappers, the result carries ONE ``report`` and not
a per-channel pair: the gain mask is built from the channel-summed power and
applied unchanged to both channels, so a pair would be two copies of one
measurement.

Two consequences the fixtures below are built to witness, neither of which a
default-config smoke test reaches:

* ``report.detected`` holds absolute dBFS measured on the SUMMED power, so it
  is the one part of the report that moves with the channel count. The Python
  mono entry returns no report, so the comparison here is against the same
  call with the right channel silenced -- which is exactly the summed power a
  mono pass would see.
* Which config fields are live depends on ``mode``. A test that only ever
  passes the default ``"logMmse"`` cannot tell a live ``over_subtraction``
  from a dead one.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050

# Length of SonareNoiseDetection.band_floor_dbfs -- SONARE_REPAIR_NOISE_BAND_COUNT
# in sonare_c_mastering.h.
_NOISE_BAND_COUNT = 32

# sonare::constants::kFloorDb, the value power_to_dbfs saturates at for a band
# whose bin range is empty at this n_fft / sample rate.
_FLOOR_DB = -120.0


def _noisy(freq: float, seed: int, *, duration: float = 0.5, noise_amp: float = 0.2) -> np.ndarray:
    """A tone plus a broadband bed, deterministic per ``seed``."""
    rng = np.random.default_rng(seed)
    tone = sine(freq, duration, sr=SR, amp=0.35)
    bed = (rng.standard_normal(tone.shape[0]) * noise_amp).astype(np.float32)
    return (tone + bed).astype(np.float32)


def _pair() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    return _noisy(440.0, seed=11), _noisy(660.0, seed=12)


class TestMasteringRepairDenoiseClassicalStereo:
    def test_default_options_return_matching_shapes(self) -> None:
        left, right = _pair()
        result = libsonare.mastering_repair_denoise_classical_stereo(left, right, SR)

        assert isinstance(result.left, list)
        assert isinstance(result.right, list)
        assert len(result.left) == len(left)
        assert len(result.right) == len(right)
        assert result.length == len(left)
        assert np.isfinite(result.left).all()
        assert np.isfinite(result.right).all()

        # Not the same data -- the two channels carry different tones, and one
        # shared mask applied to both does not erase that.
        assert result.left != result.right

    def test_result_carries_one_shared_report_not_a_pair(self) -> None:
        """The shape that differs from the four stereo repairs that came first.

        A wrapper written by pattern-matching the declick/declip/decrackle/dehum
        siblings would expose ``left_report`` / ``right_report``. The mask here
        is one measurement over the summed power, so the attributes those
        siblings carry must be absent rather than duplicated.
        """
        left, right = _pair()
        result = libsonare.mastering_repair_denoise_classical_stereo(left, right, SR)

        assert hasattr(result, "report")
        assert not hasattr(result, "left_report")
        assert not hasattr(result, "right_report")
        assert isinstance(result.report, libsonare.DenoiseReport)
        assert isinstance(result.report.detected, libsonare.NoiseDetection)

    def test_band_floor_array_is_full_length_and_measured(self) -> None:
        """All 32 entries, not just the first.

        A ctypes mirror that truncated or misordered ``band_floor_dbfs`` would
        still satisfy a test reading entry 0 alone. At 22050 Hz with the
        default 1024-point STFT the bin grid is 21.5 Hz wide, so the lowest
        geometric bands cover no bin at all and saturate at the dB floor
        exactly; the upper bands are measured and must sit well above it.
        """
        left, right = _pair()
        result = libsonare.mastering_repair_denoise_classical_stereo(left, right, SR)
        bands = result.report.detected.band_floor_dbfs

        assert len(bands) == _NOISE_BAND_COUNT
        assert all(np.isfinite(bands))
        assert all(b >= _FLOOR_DB for b in bands)

        measured = [b for b in bands if b > _FLOOR_DB]
        # A bed at 0.2 RMS puts the per-band floor tens of dB below unity but
        # nowhere near the -120 dB saturation point.
        assert len(measured) >= 12
        assert min(measured) > -100.0
        assert max(measured) < 0.0

        # The broadband figure sums the bands, so it sits above every one of
        # them and below unity.
        assert _FLOOR_DB < result.report.detected.floor_dbfs < 0.0
        assert result.report.detected.floor_dbfs >= max(measured)

    def test_detected_levels_are_pair_level_not_per_channel(self) -> None:
        """``detected`` is measured on the SUMMED power across both channels.

        Feeding the same material twice sums to double the power of feeding it
        once against silence -- and the silenced-right call is exactly what the
        mono entry would measure, since the summed power of ``(x, 0)`` is the
        power of ``x``. So the duplicated-channel floor must sit 10*log10(2)
        above it, in every band as well as broadband. An implementation that
        measured the left channel alone, or averaged the two, would put the
        difference at 0 dB and -3 dB respectively.
        """
        left = _noisy(440.0, seed=21)
        silence = np.zeros_like(left)

        both = libsonare.mastering_repair_denoise_classical_stereo(left, left, SR)
        one = libsonare.mastering_repair_denoise_classical_stereo(left, silence, SR)

        # Pin each side before relating them: a pair of saturated floors would
        # otherwise satisfy any difference assertion at 0 dB.
        assert -90.0 < both.report.detected.floor_dbfs < 0.0
        assert -90.0 < one.report.detected.floor_dbfs < 0.0

        delta = both.report.detected.floor_dbfs - one.report.detected.floor_dbfs
        assert 2.9 < delta < 3.1

        band_deltas = [
            b - m
            for b, m in zip(
                both.report.detected.band_floor_dbfs,
                one.report.detected.band_floor_dbfs,
                strict=True,
            )
            if m > _FLOOR_DB
        ]
        assert len(band_deltas) >= 12
        assert all(2.9 < d < 3.1 for d in band_deltas)

    def test_floor_limited_fraction_is_the_mode_in_spectral_subtraction(self) -> None:
        """Exactly 0 under ``"spectralSubtraction"``, which has no gain floor.

        That mode floors on ``spectral_floor`` instead, so 0 from it is the
        mode and not a measurement. Asserting only that would pass against an
        implementation that never populated the field at all, so the other half
        pins a nonzero fraction out of a mode that does have the floor: a
        shallow ``reduction_db`` puts the gain floor high enough that a large
        share of the cells land on it.
        """
        left, right = _pair()

        floored = libsonare.mastering_repair_denoise_classical_stereo(
            left, right, SR, mode="logMmse", reduction_db=3.0
        )
        assert floored.report.floor_limited_fraction > 0.05
        assert floored.report.floor_limited_fraction <= 1.0

        subtracted = libsonare.mastering_repair_denoise_classical_stereo(
            left, right, SR, mode="spectralSubtraction", reduction_db=3.0
        )
        assert subtracted.report.floor_limited_fraction == 0.0
        # The pass still ran and still attenuated, so the zero above is about
        # the floor and not about an inert call.
        assert subtracted.report.mean_reduction_db > 0.0

    def test_berouti_fields_are_read_only_in_spectral_subtraction(self) -> None:
        """``over_subtraction`` / ``spectral_floor`` do nothing at the default mode."""
        left, right = _pair()

        def run(mode: str, over_subtraction: float) -> list[float]:
            return libsonare.mastering_repair_denoise_classical_stereo(
                left, right, SR, mode=mode, over_subtraction=over_subtraction
            ).left

        assert run("logMmse", 1.0) == run("logMmse", 8.0)
        assert run("spectralSubtraction", 1.0) != run("spectralSubtraction", 8.0)

    def test_gain_shaping_fields_are_read_only_outside_spectral_subtraction(self) -> None:
        """The converse pair: ``speech_presence_gain`` / ``gain_smoothing``."""
        left, right = _pair()

        def run(mode: str, *, enabled: bool) -> list[float]:
            return libsonare.mastering_repair_denoise_classical_stereo(
                left,
                right,
                SR,
                mode=mode,
                speech_presence_gain=enabled,
                gain_smoothing=enabled,
            ).left

        assert run("spectralSubtraction", enabled=True) == run("spectralSubtraction", enabled=False)
        assert run("logMmse", enabled=True) != run("logMmse", enabled=False)

    def test_rejects_input_shorter_than_n_fft(self) -> None:
        """Denoise REJECTS a short input; dereverb pads one.

        Opposite behaviour behind a same-looking call, so this is the assertion
        that catches the stereo wrapper being wired to the wrong core function:
        ``dereverb_classical_stereo`` would return a result here.
        """
        left = _noisy(440.0, seed=31)[:256]
        right = _noisy(660.0, seed=32)[:256]

        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_denoise_classical_stereo(left, right, SR, n_fft=1024)

        # Exactly n_fft samples is the boundary and is accepted, so the refusal
        # above is about the length rule and not about a short buffer in
        # general.
        at_boundary = libsonare.mastering_repair_denoise_classical_stereo(
            _noisy(440.0, seed=31)[:1024], _noisy(660.0, seed=32)[:1024], SR, n_fft=1024
        )
        assert at_boundary.length == 1024

    def test_explicit_kwargs(self) -> None:
        left, right = _pair()
        result = libsonare.mastering_repair_denoise_classical_stereo(
            left,
            right,
            SR,
            mode="mmseStsa",
            noise_estimator="imcra",
            n_fft=512,
            hop_length=128,
            dd_alpha=0.95,
            reduction_db=18.0,
            over_subtraction=2.5,
            spectral_floor=0.02,
            noise_estimation_quantile=0.2,
            speech_presence_gain=False,
            gain_smoothing=False,
        )
        assert result.length == len(left)
        assert result.report.mean_reduction_db > 0.0
        assert result.report.max_reduction_db >= result.report.mean_reduction_db
        assert result.report.max_reduction_db <= 18.0 + 1e-3

    def test_rejects_mismatched_channel_lengths(self) -> None:
        left, right = _pair()
        with pytest.raises(ValueError, match="length"):
            libsonare.mastering_repair_denoise_classical_stereo(left, right[:-10], SR)

    def test_rejects_non_power_of_two_n_fft(self) -> None:
        left, right = _pair()
        with pytest.raises(ValueError, match="n_fft"):
            libsonare.mastering_repair_denoise_classical_stereo(left, right, SR, n_fft=1500)

    def test_rejects_non_positive_hop_length(self) -> None:
        left, right = _pair()
        with pytest.raises(ValueError, match="hop_length"):
            libsonare.mastering_repair_denoise_classical_stereo(left, right, SR, hop_length=0)

    def test_rejects_unresolvable_mode(self) -> None:
        left, right = _pair()
        with pytest.raises(ValueError):
            libsonare.mastering_repair_denoise_classical_stereo(left, right, SR, mode="not-a-mode")

    def test_rejects_unresolvable_noise_estimator(self) -> None:
        left, right = _pair()
        with pytest.raises(ValueError):
            libsonare.mastering_repair_denoise_classical_stereo(
                left, right, SR, noise_estimator=999
            )

    def test_rejects_empty_input(self) -> None:
        empty = np.zeros(0, dtype=np.float32)
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_denoise_classical_stereo(empty, empty, SR)

    def test_native_refusal_leaves_no_stale_allocation(self) -> None:
        """A refusal the Python layer does not pre-validate must not leak state.

        ``sample_rate`` is only validated natively, not by this binding, so this
        reaches the C call and must raise rather than return a result. A
        subsequent valid call on the same process must still succeed cleanly:
        the out-struct is cleared before the argument checks, so the refused
        call leaves NULL buffers and a zeroed report rather than anything to
        free twice or otherwise corrupt the next one.
        """
        left, right = _pair()
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_denoise_classical_stereo(left, right, sample_rate=0)

        result = libsonare.mastering_repair_denoise_classical_stereo(left, right, SR)
        assert result.length == len(left)
