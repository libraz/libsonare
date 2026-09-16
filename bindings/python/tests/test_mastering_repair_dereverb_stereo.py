"""Tests for the stereo offline classical-dereverb Python wrapper.

``mastering_repair_dereverb_classical_stereo`` is a ctypes pass-through over
``mastering::repair::dereverb_classical_stereo``. Like the stereo denoiser and
unlike the four stereo repairs that came before it, the result carries ONE
``report``: the subtraction mask is built from the channel-summed power and the
WPE stage accumulates its covariance over both channels, so neither stage can
produce a per-channel measurement.

Every field of that report is a ratio or a fraction, so unlike the denoiser's
``detected`` nothing here shifts with the channel count -- a claim the fixtures
below check rather than restate.

Two more things a default-config test cannot witness, and both are covered:
``wpe_enabled`` is clear by default, which leaves ``late_predictability`` and
``wpe_predictor_norm`` at exactly zero; and a short input is PADDED here where
the denoise pair rejects it.
"""

from __future__ import annotations

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050

# The default late-reverb onset. Echoes planted past it are what the module
# calls a late tail.
_LATE_DELAY_MS = 50.0


def _bed(n: int, seed: int) -> NDArray[np.float32]:
    """A quiet broadband bed.

    Not decoration: ``late_decay_ratio_db`` skips any cell whose power sits
    under its 1e-18 floor, so a dry signal with digitally silent gaps would
    have its most negative ratios discarded and would not read as dry at all.
    """
    rng = np.random.default_rng(seed)
    return (rng.standard_normal(n) * 1e-3).astype(np.float32)


def _bursts(freq: float, seed: int) -> NDArray[np.float32]:
    """Tone bursts separated by silence: a signal that stops, not one that decays."""
    tone = sine(freq, 0.5, sr=SR, amp=0.4)
    gate = np.zeros(tone.shape[0], dtype=np.float32)
    burst = int(0.06 * SR)
    period = int(0.16 * SR)
    for start in range(0, tone.shape[0], period):
        gate[start : start + burst] = 1.0
    return (tone * gate + _bed(tone.shape[0], seed)).astype(np.float32)


def _reverberate(dry: NDArray[np.float32]) -> NDArray[np.float32]:
    """Adds decaying echoes spaced past the default late-reverb onset.

    Each tap lands beyond ``_LATE_DELAY_MS``, so what they add is energy that
    sustains across the lag the module measures -- the thing a dry offset does
    not do.
    """
    out = dry.astype(np.float64).copy()
    for delay_ms, gain in ((60.0, 0.6), (120.0, 0.36), (180.0, 0.22)):
        delay = int(delay_ms * 1e-3 * SR)
        out[delay:] += gain * dry[: dry.shape[0] - delay]
    return out.astype(np.float32)


def _steady_pair() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    """Material that neither decays nor grows across the late lag.

    The gate compares the lagged cell against the current one, so a steady tone
    puts every cell's ratio near unity -- which is what makes the two ends of
    the ``threshold`` sweep separate by construction rather than by luck.
    """
    left = (sine(440.0, 0.5, sr=SR, amp=0.4) + _bed(int(0.5 * SR), seed=51)).astype(np.float32)
    right = (sine(660.0, 0.5, sr=SR, amp=0.4) + _bed(int(0.5 * SR), seed=52)).astype(np.float32)
    return left, right


def _dry_pair() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    return _bursts(440.0, seed=41), _bursts(660.0, seed=42)


def _wet_pair() -> tuple[NDArray[np.float32], NDArray[np.float32]]:
    left, right = _dry_pair()
    return _reverberate(left), _reverberate(right)


class TestMasteringRepairDereverbClassicalStereo:
    def test_default_options_return_matching_shapes(self) -> None:
        left, right = _wet_pair()
        result = libsonare.mastering_repair_dereverb_classical_stereo(left, right, SR)

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
        siblings would expose ``left_report`` / ``right_report``. The mask and
        the WPE predictors here are each one measurement over both channels, so
        the attributes those siblings carry must be absent rather than
        duplicated.
        """
        left, right = _wet_pair()
        result = libsonare.mastering_repair_dereverb_classical_stereo(left, right, SR)

        assert hasattr(result, "report")
        assert not hasattr(result, "left_report")
        assert not hasattr(result, "right_report")
        assert isinstance(result.report, libsonare.DereverbReport)
        assert isinstance(result.report.detected, libsonare.ReverbDetection)

    def test_reverberant_material_reads_higher_than_the_same_material_dry(self) -> None:
        """``late_decay_ratio_db``: less negative means the material sustains.

        The two fixtures share one bed and one burst pattern and differ only by
        the echo taps, so the comparison isolates the tail. A dry burst that
        has stopped leaves the lagged cell far above the current one, which is
        a deeply negative ratio; a tail fills that gap and pulls the figure up.
        Both sides are pinned into a plausible range first, so a pair of
        degenerate zeros cannot satisfy the relation.
        """
        dry_left, dry_right = _dry_pair()
        wet_left, wet_right = _wet_pair()

        dry = libsonare.mastering_repair_dereverb_classical_stereo(
            dry_left, dry_right, SR
        ).report.detected.late_decay_ratio_db
        wet = libsonare.mastering_repair_dereverb_classical_stereo(
            wet_left, wet_right, SR
        ).report.detected.late_decay_ratio_db

        assert -80.0 < dry < 0.0
        assert -80.0 < wet < 20.0
        assert wet > dry + 1.0

    def test_report_does_not_shift_with_the_channel_count(self) -> None:
        """Every always-live field is a ratio or a fraction.

        Feeding the same material twice doubles the summed power the mask is
        built from; the denoiser's absolute ``floor_dbfs`` moves about 3 dB
        under exactly that change, and nothing here may. The two WPE fields are
        left out: their covariance carries an absolute regularization term, so
        they are invariant only to within it, and they are zero here anyway
        because the stage is off by default.
        """
        left, _ = _wet_pair()
        silence = np.zeros_like(left)

        both = libsonare.mastering_repair_dereverb_classical_stereo(left, left, SR).report
        one = libsonare.mastering_repair_dereverb_classical_stereo(left, silence, SR).report

        # Pin each figure away from zero before relating the two, so an
        # implementation reporting nothing at all cannot pass.
        assert -80.0 < both.detected.late_decay_ratio_db < 20.0
        assert both.mean_reduction_db > 0.01
        assert both.suppressed_fraction > 0.5

        assert both.detected.late_decay_ratio_db == pytest.approx(
            one.detected.late_decay_ratio_db, rel=1e-5, abs=1e-5
        )
        assert both.mean_reduction_db == pytest.approx(one.mean_reduction_db, rel=1e-5)
        assert both.suppressed_fraction == pytest.approx(one.suppressed_fraction, rel=1e-5)

    def test_wpe_fields_are_zero_unless_the_stage_is_enabled(self) -> None:
        """``wpe_enabled`` is clear by default, so both WPE fields read exactly 0.

        That zero is the measurement, not an unset field -- but asserting it
        alone would pass against a binding where the WPE stage does not exist
        at all, so the other half turns the stage on and pins both figures
        above zero.
        """
        left, right = _wet_pair()

        off = libsonare.mastering_repair_dereverb_classical_stereo(left, right, SR).report
        assert off.detected.late_predictability == 0.0
        assert off.wpe_predictor_norm == 0.0
        # The pass still ran, so the two zeros above are about the stage and
        # not about an inert call.
        assert off.mean_reduction_db > 0.01

        on = libsonare.mastering_repair_dereverb_classical_stereo(
            left, right, SR, wpe_enabled=True, wpe_iterations=2, wpe_taps=3
        ).report
        assert on.detected.late_predictability > 0.0
        assert on.wpe_predictor_norm > 0.0
        # after the clamp <= before the clamp, by construction.
        assert on.wpe_predictor_norm <= on.detected.late_predictability

    def test_threshold_gate_is_the_only_observation_of_suppressed_fraction(self) -> None:
        """``threshold`` is relative: a late cell must beat it times the current one.

        At the default 0 the gate admits every cell with any lagged energy; at
        1.0 it demands the tail exceed the present frame, which steady material
        never does. Both ends are pinned, so neither a gate stuck open nor one
        stuck shut passes.
        """
        left, right = _steady_pair()

        wide = libsonare.mastering_repair_dereverb_classical_stereo(
            left, right, SR, threshold=0.0
        ).report
        assert 0.5 < wide.suppressed_fraction <= 1.0

        narrow = libsonare.mastering_repair_dereverb_classical_stereo(
            left, right, SR, threshold=1.0
        ).report
        assert narrow.suppressed_fraction < wide.suppressed_fraction
        assert narrow.suppressed_fraction < 0.1

    def test_attenuation_scales_what_the_mask_removes(self) -> None:
        """``attenuation`` 0 blends the mask fully back out, 1 applies it whole."""
        left, right = _wet_pair()

        full = libsonare.mastering_repair_dereverb_classical_stereo(
            left, right, SR, attenuation=1.0
        ).report
        none = libsonare.mastering_repair_dereverb_classical_stereo(
            left, right, SR, attenuation=0.0
        ).report

        assert full.mean_reduction_db > 0.01
        assert none.mean_reduction_db == 0.0
        # The gate ran either way: only how much of the mask was applied moved.
        assert none.suppressed_fraction == pytest.approx(full.suppressed_fraction, rel=1e-5)

    def test_pads_an_input_shorter_than_n_fft(self) -> None:
        """Dereverb PADS a short input; the denoise pair rejects one.

        Opposite behaviour behind a same-looking call, so this is the assertion
        that catches the stereo wrapper being wired to the wrong core function:
        ``denoise_classical_stereo`` would raise here. The output keeps the
        caller's length, not the padded analysis length.
        """
        left, right = _wet_pair()
        short_left = left[:256]
        short_right = right[:256]

        result = libsonare.mastering_repair_dereverb_classical_stereo(
            short_left, short_right, SR, n_fft=1024
        )
        assert result.length == 256
        assert len(result.left) == 256
        assert len(result.right) == 256
        assert np.isfinite(result.left).all()

    def test_explicit_kwargs(self) -> None:
        left, right = _wet_pair()
        result = libsonare.mastering_repair_dereverb_classical_stereo(
            left,
            right,
            SR,
            threshold=0.05,
            attenuation=0.8,
            n_fft=512,
            hop_length=128,
            t60_sec=0.6,
            late_delay_ms=_LATE_DELAY_MS,
            over_subtraction=1.5,
            spectral_floor=0.05,
            wpe_enabled=True,
            wpe_iterations=1,
            wpe_taps=2,
            wpe_strength=0.5,
        )
        assert result.length == len(left)
        assert result.report.mean_reduction_db > 0.0
        assert result.report.wpe_predictor_norm > 0.0

    def test_rejects_mismatched_channel_lengths(self) -> None:
        left, right = _wet_pair()
        with pytest.raises(ValueError, match="length"):
            libsonare.mastering_repair_dereverb_classical_stereo(left, right[:-10], SR)

    def test_rejects_non_power_of_two_n_fft(self) -> None:
        left, right = _wet_pair()
        with pytest.raises(ValueError, match="n_fft"):
            libsonare.mastering_repair_dereverb_classical_stereo(left, right, SR, n_fft=1500)

    def test_rejects_hop_length_outside_the_window(self) -> None:
        """Narrower than the denoise pair, which only requires ``hop_length`` positive."""
        left, right = _wet_pair()
        with pytest.raises(ValueError, match="hop_length"):
            libsonare.mastering_repair_dereverb_classical_stereo(left, right, SR, hop_length=0)
        with pytest.raises(ValueError, match="hop_length"):
            libsonare.mastering_repair_dereverb_classical_stereo(
                left, right, SR, n_fft=512, hop_length=1024
            )

    def test_rejects_empty_input(self) -> None:
        empty = np.zeros(0, dtype=np.float32)
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_dereverb_classical_stereo(empty, empty, SR)

    def test_native_refusal_leaves_no_stale_allocation(self) -> None:
        """A refusal the Python layer does not pre-validate must not leak state.

        ``sample_rate`` is only validated natively, not by this binding, so this
        reaches the C call and must raise rather than return a result. A
        subsequent valid call on the same process must still succeed cleanly:
        the out-struct is cleared before the argument checks, so the refused
        call leaves NULL buffers and a zeroed report rather than anything to
        free twice or otherwise corrupt the next one.
        """
        left, right = _wet_pair()
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_dereverb_classical_stereo(left, right, sample_rate=0)

        result = libsonare.mastering_repair_dereverb_classical_stereo(left, right, SR)
        assert result.length == len(left)
