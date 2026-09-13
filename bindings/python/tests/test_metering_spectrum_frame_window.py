"""Window-scoped validation and dB-parameter domain for the spectrum meters.

``metering_spectrum_frame`` reads exactly ``[frame_offset, frame_offset + n_fft)``
and that frame is the only span it validates: a non-finite sample inside the
frame is refused, one outside it neither reaches the FFT nor refuses the call.
The emptiness and ``sample_rate`` checks still cover the whole buffer, and the
index quoted by a refusal is absolute -- an index into the caller's buffer, not
into the frame.

``db_ref`` and ``db_amin`` take exactly ``0.0`` as "use the library default";
every other value outside ``[0, inf)`` -- negative, NaN or infinite -- is refused
rather than promoted to the default or applied as a real level. Both
:func:`libsonare.metering_spectrum` and :func:`libsonare.metering_spectrum_frame`
share that domain.
"""

from __future__ import annotations

import math

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare
from libsonare._features_metering import _DEFAULT_SPECTRUM_N_FFT
from libsonare._runtime import SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
LENGTH = 8192
# The frame sits away from both ends of the buffer so an off-by-start bug in the
# scan is visible: a scan anchored at 0 and a scan anchored at the frame both
# cover a different span than this one.
FRAME_OFFSET = 1024
N_FFT = 512
FRAME_END = FRAME_OFFSET + N_FFT  # 1536, the first index past the frame


def _tone(length: int = LENGTH) -> NDArray[np.float32]:
    """Return a float32 sine of an exact sample count, for index arithmetic."""
    n = np.arange(length, dtype=np.float64)
    return (0.5 * np.sin(2.0 * np.pi * 440.0 * n / SR)).astype(np.float32)


def _with(index: int, value: float, length: int = LENGTH) -> NDArray[np.float32]:
    buf = _tone(length)
    buf[index] = value
    return buf


def _assert_same_spectrum(left: libsonare.SpectrumReport, right: libsonare.SpectrumReport) -> None:
    assert left.n_fft == right.n_fft
    assert left.sample_rate == right.sample_rate
    for field in ("frequencies", "magnitude", "power", "db"):
        assert np.array_equal(getattr(left, field), getattr(right, field)), field


# ---------------------------------------------------------------------------
# The analysis frame is the validated span
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
def test_non_finite_outside_the_frame_is_neither_refused_nor_read(bad: float) -> None:
    """A non-finite sample outside the frame leaves the result untouched.

    Equality is exact rather than approximate: the sample never enters the FFT,
    so the frame is byte-identical to the one a clean buffer yields.
    """
    clean = _tone()
    dirty = _with(4000, bad)
    expected = libsonare.metering_spectrum_frame(clean, SR, FRAME_OFFSET, N_FFT)
    actual = libsonare.metering_spectrum_frame(dirty, SR, FRAME_OFFSET, N_FFT)
    _assert_same_spectrum(actual, expected)
    assert float(np.max(actual.magnitude)) > 0.0


@pytest.mark.parametrize("bad", [math.nan, math.inf])
def test_non_finite_inside_the_frame_is_refused_by_absolute_index(bad: float) -> None:
    """A refusal quotes the index into the caller's buffer, not into the frame.

    This is the assertion a deleted scan cannot survive, and the exact index
    separates an absolute report from a frame-relative one: the offending sample
    sits 176 samples into a frame that starts at 1024.
    """
    bad_index = FRAME_OFFSET + 176
    with pytest.raises(
        SonareValueError,
        match=rf"^metering_spectrum_frame: samples contains NaN or Inf at index {bad_index}$",
    ):
        libsonare.metering_spectrum_frame(_with(bad_index, bad), SR, FRAME_OFFSET, N_FFT)


def test_frame_boundaries_are_half_open() -> None:
    """``frame_offset`` is inside, ``frame_offset + n_fft`` is outside.

    The two inside cases fail if the scan is deleted; the two outside cases fail
    if it is widened back to the whole buffer, so neither edge can drift without
    a red test.
    """
    for inside in (FRAME_OFFSET, FRAME_END - 1):
        with pytest.raises(SonareValueError, match=rf"at index {inside}$"):
            libsonare.metering_spectrum_frame(_with(inside, math.nan), SR, FRAME_OFFSET, N_FFT)

    for outside in (FRAME_OFFSET - 1, FRAME_END):
        report = libsonare.metering_spectrum_frame(
            _with(outside, math.nan), SR, FRAME_OFFSET, N_FFT
        )
        assert np.all(np.isfinite(report.magnitude))


def test_frame_offset_at_or_past_the_end_yields_a_silent_frame() -> None:
    """An out-of-range ``frame_offset`` clamps to an empty frame instead of erroring.

    The frame is zero-padded, so the result is the spectrum of silence at the
    requested FFT size -- and because the clamped frame covers no samples, a
    non-finite value anywhere in the buffer is outside it.
    """
    for offset in (LENGTH, LENGTH + 1, LENGTH * 4):
        report = libsonare.metering_spectrum_frame(_tone(), SR, offset, N_FFT)
        assert report.n_fft == N_FFT
        assert report.sample_rate == SR
        assert report.frequencies.shape == (N_FFT // 2 + 1,)
        assert np.array_equal(report.magnitude, np.zeros(N_FFT // 2 + 1, dtype=np.float32))
        assert np.array_equal(report.power, np.zeros(N_FFT // 2 + 1, dtype=np.float32))
        # Silence lands on the dB floor rather than on -inf.
        assert np.all(report.db == -120.0)

    past_end = libsonare.metering_spectrum_frame(_with(8000, math.nan), SR, LENGTH, N_FFT)
    assert float(np.max(past_end.magnitude)) == 0.0


def test_a_fractional_frame_offset_is_refused_as_an_offset() -> None:
    """A non-index offset is named as the offset, not as a sample that is not there.

    The scan is sized from ``frame_offset``, so an unchecked fractional value
    would slice the buffer with it and refuse the call at the wrong layer.
    """
    with pytest.raises(Exception) as excinfo:  # noqa: PT011 - the class is the assertion
        libsonare.metering_spectrum_frame(_tone(), SR, 100.5, N_FFT)  # type: ignore[arg-type]
    assert "NaN or Inf" not in str(excinfo.value)
    assert "frame_offset" in str(excinfo.value)


def test_empty_buffer_is_refused_whatever_the_frame() -> None:
    """The emptiness check covers the whole buffer and is not narrowed by the window."""
    with pytest.raises(SonareValueError, match=r"metering_spectrum_frame: samples must not be"):
        libsonare.metering_spectrum_frame(np.empty(0, dtype=np.float32), SR, 99999, N_FFT)


def test_zero_n_fft_scans_the_span_the_library_actually_reads() -> None:
    """With ``n_fft=0`` the scanned span is the library's own default FFT size.

    The reported ``n_fft`` pins the facade's mirrored default against the
    library, and the two index probes pin the scan against the same value
    behaviourally -- a mirror that drifted would scan a span the call does not
    read, which neither the constant comparison alone nor the scan alone can see.
    """
    assert libsonare.metering_spectrum_frame(_tone(), SR, FRAME_OFFSET, 0).n_fft == (
        _DEFAULT_SPECTRUM_N_FFT
    )

    last_inside = FRAME_OFFSET + _DEFAULT_SPECTRUM_N_FFT - 1
    first_outside = FRAME_OFFSET + _DEFAULT_SPECTRUM_N_FFT
    assert first_outside < LENGTH
    with pytest.raises(SonareValueError, match=rf"at index {last_inside}$"):
        libsonare.metering_spectrum_frame(_with(last_inside, math.nan), SR, FRAME_OFFSET, 0)

    report = libsonare.metering_spectrum_frame(_with(first_outside, math.nan), SR, FRAME_OFFSET, 0)
    assert report.n_fft == _DEFAULT_SPECTRUM_N_FFT
    assert np.all(np.isfinite(report.magnitude))


def test_validate_false_skips_the_facade_scan_but_not_the_core_guard() -> None:
    """``validate=False`` drops the facade scan; the C layer still refuses the frame.

    The two refusals are distinguishable: the facade raises
    :class:`SonareValueError` naming the index, the core returns an invalid-
    parameter error with no index. Outside the frame the core agrees with the
    facade and accepts the buffer.
    """
    inside = FRAME_OFFSET + 200
    with pytest.raises(SonareError) as excinfo:
        libsonare.metering_spectrum_frame(
            _with(inside, math.nan), SR, FRAME_OFFSET, N_FFT, validate=False
        )
    assert not isinstance(excinfo.value, SonareValueError)
    assert "Invalid parameter" in str(excinfo.value)
    assert str(inside) not in str(excinfo.value)

    unchecked = libsonare.metering_spectrum_frame(
        _with(4000, math.nan), SR, FRAME_OFFSET, N_FFT, validate=False
    )
    _assert_same_spectrum(
        unchecked, libsonare.metering_spectrum_frame(_tone(), SR, FRAME_OFFSET, N_FFT)
    )


# ---------------------------------------------------------------------------
# dB conversion parameters
# ---------------------------------------------------------------------------


def _spectrum(samples: NDArray[np.float32], **kwargs: float) -> libsonare.SpectrumReport:
    return libsonare.metering_spectrum(samples, SR, N_FFT, False, 0, **kwargs)


def _spectrum_frame(samples: NDArray[np.float32], **kwargs: float) -> libsonare.SpectrumReport:
    return libsonare.metering_spectrum_frame(samples, SR, FRAME_OFFSET, N_FFT, False, 0, **kwargs)


@pytest.mark.parametrize("entry", [_spectrum, _spectrum_frame])
@pytest.mark.parametrize("name", ["db_ref", "db_amin"])
@pytest.mark.parametrize("value", [math.nan, math.inf])
def test_non_finite_db_parameters_are_refused(entry, name: str, value: float) -> None:
    """NaN and +Inf are outside ``[0, inf)`` and are refused, not substituted.

    Neither may reach the ``> 0`` default substitution: NaN would be promoted to
    the library default and ``+inf`` would be applied as a real reference level.
    """
    with pytest.raises(SonareError):
        entry(_tone(), **{name: value})


@pytest.mark.parametrize("entry", [_spectrum, _spectrum_frame])
def test_a_valid_db_ref_still_changes_the_result(entry) -> None:
    """The control for the refusals above: an in-domain ``db_ref`` is applied.

    A reference of 2.0 divides every linear magnitude by two, which is
    ``20 * log10(2)`` dB down. Without this the refusal tests would pass against
    an entry point that rejected every ``db_ref``.
    """
    samples = _tone()
    default_db = entry(samples).db
    scaled_db = entry(samples, db_ref=2.0).db
    assert np.all(np.isfinite(default_db))
    # Both curves saturate at the same dB floor, and a quiet bin that clears it
    # unscaled can land on it once scaled, so require both sides off the floor.
    off_floor = (default_db > -119.0) & (scaled_db > -119.0)
    assert int(np.count_nonzero(off_floor)) > 0
    delta = default_db[off_floor] - scaled_db[off_floor]
    assert delta == pytest.approx(20.0 * math.log10(2.0), abs=1e-3)
