"""Tests for the offline stereo / phase-scope / spectrum Python wrappers.

These are thin pass-throughs over sonare_c_editing.cpp entries: this suite
verifies the ctypes marshaling and that heap arrays come back as numpy float32
without leaking the underlying C buffer.
"""

from __future__ import annotations

import ctypes
import math
from typing import Any

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare
from libsonare import ErrorCode

from ._helpers import LIB_AVAILABLE, raises_coded

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")


SR = 22050


def _sine(freq: float, duration: float) -> NDArray[np.float32]:
    t = np.linspace(0.0, duration, int(SR * duration), endpoint=False, dtype=np.float32)
    return np.asarray(0.5 * np.sin(2 * math.pi * freq * t), dtype=np.float32)


# ---------------------------------------------------------------------------
# Stereo wrappers
# ---------------------------------------------------------------------------


def test_metering_stereo_correlation_in_phase_and_inverted() -> None:
    left = _sine(440.0, 0.5)
    right = left.copy()
    inverted = -left
    assert libsonare.metering_stereo_correlation(left, right, SR) == pytest.approx(1.0, abs=1e-3)
    assert libsonare.metering_stereo_correlation(left, inverted, SR) == pytest.approx(
        -1.0, abs=1e-3
    )


def test_metering_stereo_width_mono_vs_inverted() -> None:
    left = _sine(440.0, 0.5)
    width_mono = libsonare.metering_stereo_width(left, left, SR)
    width_inv = libsonare.metering_stereo_width(left, -left, SR)
    assert abs(width_mono) < 1e-3
    assert width_inv > width_mono


def test_metering_vectorscope_returns_float32_arrays() -> None:
    left = _sine(440.0, 0.1)
    report = libsonare.metering_vectorscope(left, left, SR)
    assert isinstance(report, libsonare.VectorscopeReport)
    assert report.mid.dtype == np.float32
    assert report.side.dtype == np.float32
    assert report.mid.shape == left.shape
    assert report.side.shape == left.shape
    # In-phase: side ≈ 0 everywhere.
    assert float(np.max(np.abs(report.side))) < 1e-3


def test_metering_scope_max_points_folds_into_primary() -> None:
    # Passing max_points bounds the point count; omitting it is the per-sample call.
    left = _sine(440.0, 0.1)
    max_points = 64
    vs = libsonare.metering_vectorscope(left, left, SR, max_points)
    assert vs.mid.shape[0] <= max_points

    ps = libsonare.metering_phase_scope(left, left, SR, max_points)
    assert ps.radius.shape[0] <= max_points

    # An omitted max_points is the per-sample call: one point per input sample.
    full_vs = libsonare.metering_vectorscope(left, left, SR)
    full_ps = libsonare.metering_phase_scope(left, left, SR)
    assert full_vs.mid.shape == left.shape
    assert full_ps.radius.shape == left.shape
    # A max_points of the buffer length or more is the same per-sample result.
    big_vs = libsonare.metering_vectorscope(left, left, SR, left.shape[0] + 10)
    big_ps = libsonare.metering_phase_scope(left, left, SR, left.shape[0] + 10)
    assert np.array_equal(big_vs.mid, full_vs.mid)
    assert np.array_equal(big_vs.side, full_vs.side)
    assert np.array_equal(big_ps.radius, full_ps.radius)


@pytest.mark.parametrize(
    "fn",
    [
        libsonare.metering_vectorscope,
        libsonare.metering_phase_scope,
    ],
)
@pytest.mark.parametrize("max_points", [0, -1])
def test_a_non_positive_max_points_is_refused(fn: Any, max_points: int) -> None:
    """Full resolution is spelled by omission; the C entry's 0 is not a point count."""
    left = _sine(440.0, 0.1)
    with pytest.raises(libsonare.SonareValueError, match="max_points"):
        fn(left, left, SR, max_points)


def test_metering_phase_scope_populates_summary_stats() -> None:
    left = _sine(440.0, 0.1)
    report = libsonare.metering_phase_scope(left, left, SR)
    assert isinstance(report, libsonare.PhaseScopeReport)
    assert report.mid.shape == left.shape
    assert report.side.shape == left.shape
    assert report.radius.shape == left.shape
    assert report.angle_rad.shape == left.shape
    assert report.correlation == pytest.approx(1.0, abs=1e-3)
    assert report.max_radius > 0


def test_metering_scope_columns_come_from_one_shared_bulk_copy() -> None:
    """All four scope read-outs marshal through the same packed-record helper.

    The per-point attribute walk they each used to carry is one Python-level
    access per input sample per field at the ``max_points=0`` default. The
    replacement reads the C array as a float32 matrix, which is only valid while
    every field is a ``c_float`` and the record has no padding -- so that
    property is asserted here rather than assumed, and the column names are
    taken from the ctypes mirror so a new field needs no second edit there.
    """
    from libsonare._features_metering import _scope_point_columns
    from libsonare._ffi import SonarePhaseScopePoint, SonareVectorscopePoint

    for point_type, expected in (
        (SonareVectorscopePoint, ["mid", "side"]),
        (SonarePhaseScopePoint, ["mid", "side", "radius", "angle_rad"]),
    ):
        names = [name for name, _ in point_type._fields_]
        assert names == expected
        assert ctypes.sizeof(point_type) == 4 * len(names)
        assert set(_scope_point_columns(None, 0, point_type)) == set(expected)

    # A record the helper cannot read as a matrix is refused rather than
    # silently mis-split.
    class _Padded(ctypes.Structure):
        _fields_ = [("mid", ctypes.c_float), ("flag", ctypes.c_double)]

    with raises_coded(ErrorCode.UNKNOWN):
        _scope_point_columns(None, 1, _Padded)

    # Every entry point returns the helper's columns.
    left = _sine(440.0, 0.05)
    right = _sine(660.0, 0.05)
    vector = libsonare.metering_vectorscope(left, right, SR)
    phase = libsonare.metering_phase_scope(left, right, SR)

    assert vector.mid.dtype == np.float32
    assert phase.angle_rad.dtype == np.float32
    assert vector.mid.shape == phase.mid.shape
    # Mid/side are the same quantity on both read-outs, so the shared helper has
    # to split both records into the same columns.
    assert np.allclose(vector.mid, phase.mid)
    assert np.allclose(vector.side, phase.side)
    # The phase-scope columns are the polar form of those two, which pins the
    # column ORDER rather than only the names.
    assert np.allclose(phase.radius, np.hypot(phase.mid, phase.side), atol=1e-5)
    # Both sines start at zero, so the first point is silent and has no angle.
    silent = (phase.mid == 0.0) & (phase.side == 0.0)
    assert silent[0]
    expected_angle = np.where(silent, np.nan, np.arctan2(phase.side, phase.mid))
    assert np.allclose(phase.angle_rad, expected_angle, atol=1e-5, equal_nan=True)

    # The arrays outlive the C result, which is freed inside the call.
    assert float(np.max(np.abs(vector.mid))) > 0.0


def test_phase_scope_over_silence_reports_no_angle() -> None:
    silence = np.zeros(256, dtype=np.float32)
    phase = libsonare.metering_phase_scope(silence, silence, SR)
    # The angle array keeps one element per point, NaN where a point has no angle.
    assert phase.angle_rad.shape == phase.mid.shape
    assert bool(np.all(np.isnan(phase.angle_rad)))
    assert phase.average_abs_angle_rad is None
    assert phase.correlation is None
    assert phase.max_radius == 0.0


def test_metering_stereo_rejects_mismatched_lengths() -> None:
    left = _sine(440.0, 0.1)
    right = _sine(440.0, 0.05)
    with pytest.raises(ValueError):
        libsonare.metering_stereo_correlation(left, right, SR)
    with pytest.raises(ValueError):
        libsonare.metering_vectorscope(left, right, SR)


_A = [0.1] * 4800
_B = [0.1] * 4799


@pytest.mark.parametrize(
    ("entry", "call", "pair"),
    [
        (
            "metering_stereo_correlation",
            lambda: libsonare.metering_stereo_correlation(_A, _B, 48000),
            "left and right",
        ),
        (
            "metering_vectorscope",
            lambda: libsonare.metering_vectorscope(_A, _B, 48000),
            "left and right",
        ),
        (
            "metering_phase_scope",
            lambda: libsonare.metering_phase_scope(_A, _B, 48000),
            "left and right",
        ),
        (
            "metering_crest_factor_db_stereo",
            lambda: libsonare.metering_crest_factor_db_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_repair_declick_stereo",
            lambda: libsonare.mastering_repair_declick_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_repair_declip_stereo",
            lambda: libsonare.mastering_repair_declip_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_repair_decrackle_stereo",
            lambda: libsonare.mastering_repair_decrackle_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_repair_denoise_classical_stereo",
            lambda: libsonare.mastering_repair_denoise_classical_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_repair_dehum_stereo",
            lambda: libsonare.mastering_repair_dehum_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_repair_dereverb_classical_stereo",
            lambda: libsonare.mastering_repair_dereverb_classical_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_repair_detect_trim_range_stereo",
            lambda: libsonare.mastering_repair_detect_trim_range_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_repair_trim_silence_stereo",
            lambda: libsonare.mastering_repair_trim_silence_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "master_audio_stereo",
            lambda: libsonare.master_audio_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "streaming_loudness_gain_stereo",
            lambda: libsonare.streaming_loudness_gain_stereo(_A, _B, 48000),
            "left and right",
        ),
        (
            "mastering_ab_match_loudness_stereo",
            lambda: libsonare.mastering_ab_match_loudness_stereo(_A, _B, _A, _A, 48000),
            "source_left and source_right",
        ),
        (
            "mastering_ab_match_loudness_stereo",
            lambda: libsonare.mastering_ab_match_loudness_stereo(_A, _A, _A, _B, 48000),
            "reference_left and reference_right",
        ),
        (
            "mix_stereo",
            lambda: libsonare.mix_stereo([(_A, _A), (_A, _B)], 48000),
            "strips[1][0] and strips[1][1]",
        ),
    ],
)
def test_a_stereo_pair_of_unequal_lengths_names_both_channels(
    entry: str, call: Any, pair: str
) -> None:
    with pytest.raises(libsonare.SonareValueError) as refused:
        call()
    assert str(refused.value) == f"{entry}: {pair} must have the same length"


def test_metering_stereo_rejects_invalid_sample_rates() -> None:
    left = _sine(440.0, 0.1)
    with raises_coded():
        libsonare.metering_stereo_correlation(left, left, 0)
    with raises_coded():
        libsonare.metering_phase_scope(left, left, -1)


# ---------------------------------------------------------------------------
# Spectrum wrapper
# ---------------------------------------------------------------------------


def test_metering_spectrum_returns_expected_bins_and_peak() -> None:
    samples = _sine(1000.0, 0.5)
    n_fft = 2048
    report = libsonare.metering_spectrum(samples, SR, n_fft=n_fft)
    assert isinstance(report, libsonare.SpectrumReport)
    assert report.frequencies.shape == (n_fft // 2 + 1,)
    assert report.magnitude.shape == (n_fft // 2 + 1,)
    assert report.power.shape == (n_fft // 2 + 1,)
    assert report.db.shape == (n_fft // 2 + 1,)
    assert report.n_fft == n_fft
    assert report.sample_rate == SR
    peak_bin = int(np.argmax(report.magnitude))
    assert report.frequencies[peak_bin] == pytest.approx(1000.0, abs=60.0)
    expected_power = float(report.magnitude[peak_bin]) ** 2
    # Large-magnitude bins drift in the last sig figs; check ratio.
    assert abs(float(report.power[peak_bin]) - expected_power) / expected_power < 1e-5


def test_metering_spectrum_uses_defaults() -> None:
    samples = _sine(440.0, 0.5)
    report = libsonare.metering_spectrum(samples, SR)
    assert report.n_fft == 2048
    assert report.frequencies.shape == (2048 // 2 + 1,)


def test_metering_spectrum_rejects_non_power_of_two_n_fft() -> None:
    samples = _sine(440.0, 0.1)
    with raises_coded():
        libsonare.metering_spectrum(samples, SR, n_fft=1500)
