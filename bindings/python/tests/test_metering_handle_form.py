"""Equivalence tests for the handle-form metering methods on ``Audio``.

Each ``Audio.<meter>`` method reads the handle's own validated samples
instead of re-scanning and copying a caller buffer, and is expected to
compute bit-identical results to its ``metering_*`` free-function twin over
the same samples. Buffers here are all-finite by construction -- the one
documented divergence (``spectrum_frame`` accepting a non-finite sample
outside its analysis window) cannot be expressed by a ``SonareAudio`` handle
at all, since construction scans and rejects the whole buffer.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare
from libsonare import Audio

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _sine(freq: float, duration: float, sr: int = SR) -> np.ndarray:
    t = np.linspace(0.0, duration, int(sr * duration), endpoint=False, dtype=np.float32)
    return (0.5 * np.sin(2 * math.pi * freq * t)).astype(np.float32)


def test_peak_rms_dc_crest_match_the_buffer_form() -> None:
    samples = _sine(440.0, 0.5)
    with Audio.from_buffer(samples, sample_rate=SR) as audio:
        peak = audio.peak_db()
        rms = audio.rms_db()
        dc = audio.dc_offset()
        crest = audio.crest_factor_db()

    # Non-vacuity: a sine at 0.5 amplitude has a real peak/rms gap and a
    # near-zero (not exactly-zero) DC offset -- none of these degenerate.
    assert math.isfinite(peak)
    assert peak > rms > -100.0
    assert crest > 0.0

    assert peak == libsonare.metering_peak_db(samples, SR)
    assert rms == libsonare.metering_rms_db(samples, SR)
    assert dc == libsonare.metering_dc_offset(samples, SR)
    assert crest == libsonare.metering_crest_factor_db(samples, SR)


def test_silence_ratio_matches_the_buffer_form() -> None:
    samples = np.concatenate([np.zeros(1024, dtype=np.float32), np.ones(1024, dtype=np.float32)])
    with Audio.from_buffer(samples, sample_rate=SR) as audio:
        ratio = audio.silence_ratio(threshold_db=-45.0, frame_length=1024, hop_length=1024)

    assert ratio == pytest.approx(0.5)
    assert ratio == libsonare.metering_silence_ratio(
        samples, SR, threshold_db=-45.0, frame_length=1024, hop_length=1024
    )


def test_true_peak_db_matches_the_buffer_form() -> None:
    samples = _sine(440.0, 0.5)
    with Audio.from_buffer(samples, sample_rate=SR) as audio:
        handle_value = audio.true_peak_db(oversample_factor=4)

    buffer_value = libsonare.metering_true_peak_db(samples, SR, oversample_factor=4)
    assert math.isfinite(handle_value)
    assert handle_value >= libsonare.metering_peak_db(samples, SR)
    assert handle_value == buffer_value


def test_ebur128_loudness_range_matches_the_buffer_form() -> None:
    # LRA needs enough audio to form more than one gated block.
    loud = _sine(440.0, 2.0) * 0.8
    quiet = _sine(440.0, 2.0) * 0.05
    samples = np.concatenate([loud, quiet, loud]).astype(np.float32)
    with Audio.from_buffer(samples, sample_rate=SR) as audio:
        handle_lra = audio.ebur128_loudness_range()

    buffer_lra = libsonare.ebur128_loudness_range(samples, SR)
    assert handle_lra > 0.0
    assert handle_lra == buffer_lra


def test_detect_clipping_matches_the_buffer_form_field_by_field() -> None:
    samples = np.full(8000, 0.1, dtype=np.float32)
    samples[1000:1064] = 1.0
    with Audio.from_buffer(samples, sample_rate=SR) as audio:
        handle_report = audio.detect_clipping(threshold=0.999, min_region_samples=1)

    buffer_report = libsonare.metering_detect_clipping(samples, SR, threshold=0.999)

    # Non-vacuity: the injected run must actually be found before the field
    # comparison below means anything.
    assert handle_report.clipped_samples >= 1
    assert len(handle_report.regions) >= 1

    assert handle_report.clipped_samples == buffer_report.clipped_samples
    assert handle_report.clipping_ratio == buffer_report.clipping_ratio
    assert handle_report.max_clipped_peak == buffer_report.max_clipped_peak
    assert len(handle_report.regions) == len(buffer_report.regions)
    for handle_region, buffer_region in zip(
        handle_report.regions, buffer_report.regions, strict=True
    ):
        assert handle_region.start_sample == buffer_region.start_sample
        assert handle_region.end_sample == buffer_region.end_sample
        assert handle_region.length == buffer_region.length
        assert handle_region.peak == buffer_region.peak


def test_dynamic_range_zero_percentile_is_a_real_request_not_the_default() -> None:
    """0.0 selects the 0th percentile; only a NEGATIVE value selects the default."""
    sr = 22050
    loud = _sine(440.0, 3.5) * 0.8
    quiet = _sine(440.0, 3.5) * 0.05
    samples = np.concatenate([loud, quiet, loud]).astype(np.float32)

    with Audio.from_buffer(samples, sample_rate=sr) as audio:
        default_report = audio.dynamic_range()
        zero_low_report = audio.dynamic_range(low_percentile=0.0, high_percentile=0.95)

    # Positive control for the sentinel pitfall: an explicit 0.0 percentile
    # must differ from the default (-1.0 sentinel), not silently fall back to it.
    assert zero_low_report.low_percentile_db != default_report.low_percentile_db

    assert (
        default_report.dynamic_range_db
        == libsonare.metering_dynamic_range(samples, sr).dynamic_range_db
    )
    buffer_zero_low = libsonare.metering_dynamic_range(
        samples, sr, low_percentile=0.0, high_percentile=0.95
    )
    assert zero_low_report.low_percentile_db == buffer_zero_low.low_percentile_db
    assert zero_low_report.high_percentile_db == buffer_zero_low.high_percentile_db
    assert zero_low_report.dynamic_range_db == buffer_zero_low.dynamic_range_db
    assert list(zero_low_report.window_rms_db) == list(buffer_zero_low.window_rms_db)


def test_spectrum_matches_the_buffer_form_field_by_field() -> None:
    samples = _sine(440.0, 0.5)
    with Audio.from_buffer(samples, sample_rate=SR) as audio:
        handle_spectrum = audio.spectrum(n_fft=1024)

    buffer_spectrum = libsonare.metering_spectrum(samples, SR, n_fft=1024)

    assert handle_spectrum.n_fft == 1024
    assert len(handle_spectrum.frequencies) > 0
    assert np.max(handle_spectrum.magnitude) > 0.0

    assert handle_spectrum.n_fft == buffer_spectrum.n_fft
    assert handle_spectrum.sample_rate == buffer_spectrum.sample_rate
    np.testing.assert_array_equal(handle_spectrum.frequencies, buffer_spectrum.frequencies)
    np.testing.assert_array_equal(handle_spectrum.magnitude, buffer_spectrum.magnitude)
    np.testing.assert_array_equal(handle_spectrum.power, buffer_spectrum.power)
    np.testing.assert_array_equal(handle_spectrum.db, buffer_spectrum.db)


def test_spectrum_frame_offset_changes_the_result_and_matches_the_buffer_form() -> None:
    # A chirp so different frame offsets genuinely see a different spectrum.
    t = np.linspace(0.0, 1.0, SR, endpoint=False, dtype=np.float32)
    samples = (0.5 * np.sin(2 * math.pi * (200.0 + 4000.0 * t) * t)).astype(np.float32)

    with Audio.from_buffer(samples, sample_rate=SR) as audio:
        first_frame = audio.spectrum_frame(frame_offset=0, n_fft=1024)
        later_frame = audio.spectrum_frame(frame_offset=20000, n_fft=1024)

        # Moving frame_offset must actually change what is measured -- the
        # positive control for the "cost is set by n_fft, not length" claim.
        assert not np.array_equal(first_frame.magnitude, later_frame.magnitude)

        buffer_first = libsonare.metering_spectrum_frame(samples, SR, frame_offset=0, n_fft=1024)
        buffer_later = libsonare.metering_spectrum_frame(
            samples, SR, frame_offset=20000, n_fft=1024
        )

    np.testing.assert_array_equal(first_frame.magnitude, buffer_first.magnitude)
    np.testing.assert_array_equal(first_frame.db, buffer_first.db)
    np.testing.assert_array_equal(later_frame.magnitude, buffer_later.magnitude)
    np.testing.assert_array_equal(later_frame.db, buffer_later.db)


def test_handle_measures_samples_at_creation_time_not_at_call_time() -> None:
    """Mutating the source buffer after handle creation changes nothing.

    ``Audio.from_buffer`` copies the samples into the handle at construction
    time, so nothing downstream observes a later mutation of the caller's array.
    """
    samples = _sine(440.0, 0.25)
    audio = Audio.from_buffer(samples, sample_rate=SR)
    try:
        before = audio.peak_db()
        samples[:] = 0.0
        after = audio.peak_db()
        assert after == before
        # The buffer form over the now-mutated buffer sees the mutation.
        assert libsonare.metering_peak_db(samples, SR) != before
    finally:
        audio.close()


def test_metering_methods_reject_use_after_close() -> None:
    audio = Audio.from_buffer(_sine(440.0, 0.1), sample_rate=SR)
    audio.close()
    with pytest.raises(RuntimeError):
        audio.peak_db()
    with pytest.raises(RuntimeError):
        audio.detect_clipping()
    with pytest.raises(RuntimeError):
        audio.spectrum_frame()
