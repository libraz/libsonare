"""Tests for the offline metering + scale-quantizer Python wrappers.

These functions are thin pass-throughs over the C API entry points added
in sonare_c_editing.cpp. The goal is to verify the marshaling: result
fields are correctly typed, heap arrays are freed by the wrapper, and the
documented defaults match the C side.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")


SR = 22050


def _sine(freq: float, duration: float) -> np.ndarray:
    t = np.linspace(0.0, duration, int(SR * duration), endpoint=False, dtype=np.float32)
    return (0.5 * np.sin(2 * math.pi * freq * t)).astype(np.float32)


def test_metering_basic_meters_agree() -> None:
    samples = _sine(440.0, 1.0)
    peak = libsonare.metering_peak_db(samples, SR)
    rms = libsonare.metering_rms_db(samples, SR)
    crest = libsonare.metering_crest_factor_db(samples, SR)
    dc = libsonare.metering_dc_offset(samples, SR)
    assert math.isfinite(peak)
    assert math.isfinite(rms)
    assert peak >= rms
    assert math.isclose(crest, peak - rms, abs_tol=1e-3)
    assert abs(dc) < 1e-2


def test_metering_true_peak_db_default_oversample() -> None:
    samples = _sine(440.0, 1.0)
    tp = libsonare.metering_true_peak_db(samples, SR)
    # True peak >= sample peak; differences should be small for a 440 Hz sine.
    assert math.isfinite(tp)
    assert tp >= libsonare.metering_peak_db(samples, SR) - 0.1


def test_metering_true_peak_rejects_non_power_of_two() -> None:
    samples = _sine(440.0, 0.1)
    with pytest.raises(RuntimeError):
        libsonare.metering_true_peak_db(samples, SR, oversample_factor=3)


def test_metering_detect_clipping_reports_runs_and_frees_heap() -> None:
    samples = np.full(8000, 0.1, dtype=np.float32)
    samples[1000:1064] = 1.0
    report = libsonare.metering_detect_clipping(samples, SR, threshold=0.999)
    assert report.clipped_samples >= 1
    assert len(report.regions) >= 1
    assert report.max_clipped_peak >= 1.0
    region = report.regions[0]
    assert region.start_sample <= region.end_sample
    assert region.length >= 1
    assert math.isclose(region.peak, 1.0, abs_tol=1e-6)


def test_metering_detect_clipping_clean_signal() -> None:
    samples = (_sine(440.0, 0.5) * 0.3).astype(np.float32)
    report = libsonare.metering_detect_clipping(samples, SR)
    assert report.clipped_samples == 0
    assert report.regions == []


def test_metering_dynamic_range_returns_positive_dr_for_varying_signal() -> None:
    # Default config has window=3 s / hop=1 s, so we need more than 3 s of audio
    # with varying level for high - low percentiles to differ.
    sr = 22050
    loud = _sine(440.0, 3.5) * 0.8
    quiet = _sine(440.0, 3.5) * 0.05
    samples = np.concatenate([loud, quiet, loud]).astype(np.float32)
    report = libsonare.metering_dynamic_range(samples, sr)
    assert len(report.window_rms_db) >= 2
    assert report.dynamic_range_db > 0.0


def test_metering_dynamic_range_rejects_inverted_percentiles() -> None:
    samples = _sine(440.0, 1.0)
    with pytest.raises(RuntimeError):
        libsonare.metering_dynamic_range(samples, SR, low_percentile=0.9, high_percentile=0.1)


C_MAJOR_MASK = 0b101010110101


def test_scale_quantize_midi_snaps_off_scale_to_nearest_in_scale() -> None:
    # C# (61) → nearest in C major is C (60) or D (62).
    quantized = libsonare.scale_quantize_midi(0, C_MAJOR_MASK, 61.0)
    assert quantized in (pytest.approx(60.0, abs=0.01), pytest.approx(62.0, abs=0.01))


def test_scale_quantize_midi_passes_through_in_scale_notes() -> None:
    quantized = libsonare.scale_quantize_midi(0, C_MAJOR_MASK, 60.0)
    assert math.isclose(quantized, 60.0, abs_tol=0.01)


def test_scale_correction_semitones_agrees_with_quantize_midi() -> None:
    q = libsonare.scale_quantize_midi(0, C_MAJOR_MASK, 61.4)
    c = libsonare.scale_correction_semitones(0, C_MAJOR_MASK, 61.4)
    assert math.isclose(c, q - 61.4, abs_tol=0.01)


def test_scale_pitch_class_enabled_reflects_mode_mask() -> None:
    assert libsonare.scale_pitch_class_enabled(0, C_MAJOR_MASK, 0) is True
    assert libsonare.scale_pitch_class_enabled(0, C_MAJOR_MASK, 1) is False


def test_scale_quantize_rejects_bad_args() -> None:
    with pytest.raises(RuntimeError):
        libsonare.scale_quantize_midi(-1, C_MAJOR_MASK, 60.0)
    with pytest.raises(RuntimeError):
        libsonare.scale_quantize_midi(0, 0, 60.0)
    with pytest.raises(RuntimeError):
        libsonare.scale_pitch_class_enabled(0, C_MAJOR_MASK, 12)
