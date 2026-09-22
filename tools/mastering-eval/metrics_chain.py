"""Mastering chain metrics: integrated loudness, true peak, short-term loudness spread, per-band
energy delta.

Every level these four report is measured by the library's own meters, reached through the Python
binding. What this module adds is the two things the binding does not publish: the spread reduction
over the short-term series, and the resample of a long-term spectrum onto the 32 logarithmic band
centres. None of it is a second implementation of a meter.

Unlike ``metrics_repair`` this module has no C++ twin pinned to it, because the arithmetic that
could drift already lives inside the library and both languages call it.
``tests/mastering/mastering_metrics_test.cpp`` measures the same four quantities through the same
library entry points and pins each one to its own degradation.

Definitions
-----------

**Feed shape.** A signal is ``(frames,)`` for a mono feed or ``(frames, channels)`` for a
multi-channel one, exactly as the harness holds it. Integrated loudness and true peak are
channel-dependent by definition, so nothing here collapses a feed before measuring it.

**Integrated loudness.** ITU-R BS.1770-4 / EBU R128 integrated loudness in LUFS, straight from
``libsonare.lufs_interleaved``, which applies the BS.1770 channel weighting and the two-stage
gate. Mono goes through the same call with one channel, which is what the library's own mono entry
point does. This is the level, so it moves by exactly an applied gain; that is the check rather
than the failure.

**True peak.** Inter-sample peak in dBTP at :data:`TRUE_PEAK_OVERSAMPLE`, the maximum over the
channels of ``libsonare.metering_true_peak_db``. Taking the maximum of the per-channel dB values is
the same number as the library's own stereo planar meter takes, which maximizes in the linear
domain and converts once: the conversion is monotonic and both channels share one silence floor.
**This is a level, not a level-independent reading** -- it moves by exactly an applied gain, the
same way integrated loudness does. It is in the set to be compared against a ceiling, and a
movement other than the applied gain is what it catches.

**Short-term loudness spread.** The population standard deviation, in LU, of the BS.1770-4
short-term loudness series -- 3 s windows on a 100 ms hop -- over the blocks that survive the
absolute gate at :data:`ABSOLUTE_GATE_LUFS`. The series comes from
``libsonare.lufs_series_interleaved``, which sums the K-weighted per-channel block energies the way
the standard specifies, so the channel weighting and the LFE exclusion of a surround layout are the
library's rather than this module's. Fewer than two surviving blocks gives NaN, which is what a
signal shorter than 3.1 s yields: short-term emits no partial window.

*Why this is not the loudness range.* LRA is an inter-percentile range (P95 - P10) over a
distribution a second, relative gate has already trimmed 20 LU below its own mean, so it reads two
order statistics and deliberately discards the quiet material a compressor lifts. This is the
second moment of the whole absolutely-gated distribution, so it moves when processing redistributes
loudness between those two percentiles and LRA does not. Both are reported by the library and they
are not interchangeable.

**Per-band energy delta.** The change in long-term spectral level, in dB, at the
:data:`BAND_COUNT` logarithmically spaced band centres running from :data:`BAND_LOW_HZ` to Nyquist:
``f[i] = low * (nyquist / low) ** ((i + 0.5) / BAND_COUNT)``. Those are the same centres the C ABI's
``SonareMasteringReport.band_energy_delta_db`` reports, so a harness row and a chain's own report
describe the same bands. The spectrum is the library's Welch-averaged long-term spectrum with
third-octave smoothing, read per channel and combined in the power domain, and a band's level is
linearly interpolated between the two neighbouring FFT bins.

The output is scaled by one global factor matching its RMS to the input's before the comparison, so
a flat gain reads 0 in every band rather than ``20*log10(g)`` in all of them; only the flat gain is
removed, and a tilt still registers. That is where this differs from the C ABI report field, which
keeps the chain's applied gain in it: the report's band values are this module's plus the flat
level change, and over a broadband item the two agree band for band to within hundredths of a dB
once that one offset is taken out.

A band where both spectra sit on the library's dB floor reads 0, which is the same number a band
that genuinely did not change reads. Above the floor the two cases are distinguishable; at it they
are not, so a run over material with no content in a band says nothing about that band.

Run the definition pins with
``rye run python -m doctest ../../tools/mastering-eval/metrics_chain.py`` from ``bindings/python``.
"""

from __future__ import annotations

import libsonare
import numpy as np
import numpy.typing as npt

BAND_COUNT = 32
"""Number of logarithmic band centres. Mirrors ``SONARE_MASTERING_REPORT_BAND_COUNT`` so a harness
row and a chain's own report can be read against each other band for band."""

BAND_LOW_HZ = 20.0
"""Low end of the band sweep, clamped to Nyquist for a rate whose Nyquist is below it. Mirrors the
mastering report's own low edge."""

SPECTRUM_N_FFT = 2048
"""Transform size of the long-term spectrum the bands are resampled from."""

SPECTRUM_OCTAVE_FRACTION = 3
"""Fractional-octave smoothing applied to that spectrum: third-octave, matching the report."""

TRUE_PEAK_OVERSAMPLE = 4
"""Oversampling ratio of the true-peak meter. Mirrors the library's own default."""

ABSOLUTE_GATE_LUFS = -70.0
"""Blocks below this are not measurements. Mirrors ``metering::kLufsAbsoluteGate``."""

# Sentinel the Python binding reads as "use the library's own default" for the spectrum's dB
# reference and floor.
_LIBRARY_DEFAULT = 0.0


def _as_feed(x: npt.ArrayLike, name: str) -> npt.NDArray[np.float64]:
    """Take a feed as ``(frames, channels)``, widening a mono feed to one column."""
    arr = np.asarray(x, dtype=np.float64)
    if arr.ndim == 1:
        arr = arr[:, None]
    if arr.ndim != 2:
        raise ValueError(f"{name} must be (frames,) or (frames, channels), got shape {arr.shape}")
    if arr.shape[0] == 0 or arr.shape[1] == 0:
        raise ValueError(f"{name} must not be empty, got shape {arr.shape}")
    return arr


def _channel(feed: npt.NDArray[np.float64], index: int) -> npt.NDArray[np.float32]:
    return np.ascontiguousarray(feed[:, index], dtype=np.float32)


def _rms_matched(
    reference: npt.NDArray[np.float64], other: npt.NDArray[np.float64]
) -> npt.NDArray[np.float64]:
    """Scale `other` by one global factor so its RMS equals the reference's."""
    rms = float(np.sqrt(np.mean(other**2)))
    if rms <= 0.0:
        return other
    return other * (float(np.sqrt(np.mean(reference**2))) / rms)


def band_center_frequencies(sample_rate: float) -> npt.NDArray[np.float64]:
    """The :data:`BAND_COUNT` band centres in Hz, from :data:`BAND_LOW_HZ` to Nyquist.

    Args:
        sample_rate: Rate the bands are laid out for; Nyquist is the high end.

    Returns:
        Band centres in ascending order, one per band.

    >>> centres = band_center_frequencies(48000.0)
    >>> centres.shape
    (32,)
    >>> f"{centres[0]:.4f} {centres[-1]:.4f}"
    '22.3430 21483.2031'
    >>> ratios = centres[1:] / centres[:-1]
    >>> bool(np.allclose(ratios, ratios[0]))
    True
    """
    high_hz = 0.5 * float(sample_rate)
    low_hz = min(BAND_LOW_HZ, high_hz)
    if low_hz <= 0.0:
        raise ValueError(f"sample_rate must be positive, got {sample_rate}")
    position = (np.arange(BAND_COUNT, dtype=np.float64) + 0.5) / BAND_COUNT
    return low_hz * (high_hz / low_hz) ** position


def _band_levels_db(feed: npt.NDArray[np.float64], sample_rate: float) -> npt.NDArray[np.float64]:
    """Long-term level in dB at each band centre, channels combined in the power domain."""
    channel_power = np.zeros((feed.shape[1], BAND_COUNT), dtype=np.float64)
    centres = band_center_frequencies(sample_rate)
    for index in range(feed.shape[1]):
        report = libsonare.metering_spectrum(
            _channel(feed, index),
            int(sample_rate),
            SPECTRUM_N_FFT,
            True,
            SPECTRUM_OCTAVE_FRACTION,
            _LIBRARY_DEFAULT,
            _LIBRARY_DEFAULT,
        )
        db = np.interp(centres, np.asarray(report.frequencies), np.asarray(report.db))
        channel_power[index] = 10.0 ** (db / 10.0)
    return 10.0 * np.log10(np.mean(channel_power, axis=0))


def _short_term_series(
    feed: npt.NDArray[np.float64], sample_rate: float
) -> npt.NDArray[np.float64]:
    """The channel-summed BS.1770-4 short-term loudness series in LUFS."""
    interleaved = np.ascontiguousarray(feed.reshape(-1), dtype=np.float32)
    _, short_term = libsonare.lufs_series_interleaved(interleaved, feed.shape[1], int(sample_rate))
    return np.asarray(short_term, dtype=float)


def integrated_loudness(x: npt.ArrayLike, sample_rate: float) -> float:
    """Integrated loudness of one feed in LUFS (ITU-R BS.1770-4 / EBU R128).

    Args:
        x: The feed, ``(frames,)`` or ``(frames, channels)``.
        sample_rate: Rate the feed was measured at. K-weighting is rate-dependent, so a wrong rate
            yields a wrong loudness.

    Returns:
        Gated integrated loudness in LUFS, or negative infinity when no block passes the gate.
    """
    feed = _as_feed(x, "x")
    interleaved = np.ascontiguousarray(feed.reshape(-1), dtype=np.float32)
    result = libsonare.lufs_interleaved(interleaved, feed.shape[1], int(sample_rate))
    return float(result.integrated_lufs)


def true_peak_dbtp(x: npt.ArrayLike, sample_rate: float) -> float:
    """Inter-sample true peak of one feed in dBTP.

    Args:
        x: The feed, ``(frames,)`` or ``(frames, channels)``.
        sample_rate: Rate the feed was measured at.

    Returns:
        The largest true peak over the channels, in dBTP.
    """
    feed = _as_feed(x, "x")
    return max(
        libsonare.metering_true_peak_db(
            _channel(feed, index), int(sample_rate), TRUE_PEAK_OVERSAMPLE
        )
        for index in range(feed.shape[1])
    )


def short_term_spread(x: npt.ArrayLike, sample_rate: float) -> float:
    """Width of the short-term loudness distribution of one feed, in LU.

    Args:
        x: The feed, ``(frames,)`` or ``(frames, channels)``.
        sample_rate: Rate the feed was measured at.

    Returns:
        Population standard deviation of the gated short-term loudness series, or NaN when fewer
        than two blocks survive the gate -- which includes every feed shorter than 3.1 s, since
        short-term emits no partial window.
    """
    feed = _as_feed(x, "x")
    series = _short_term_series(feed, sample_rate)
    gated = series[np.isfinite(series) & (series >= ABSOLUTE_GATE_LUFS)]
    if gated.size < 2:
        return float("nan")
    return float(np.std(gated))


def band_energy_delta(
    source: npt.ArrayLike, processed: npt.ArrayLike, sample_rate: float
) -> npt.NDArray[np.float64]:
    """Per-band change in long-term spectral level between a chain's input and its output, in dB.

    Args:
        source: The feed as it entered the chain.
        processed: The feed as it left, same shape.
        sample_rate: Rate both were measured at.

    Returns:
        One value per band, ascending in frequency, after minus before. Positive means the band
        gained level relative to the rest of the spectrum.
    """
    before = _as_feed(source, "source")
    after = _as_feed(processed, "processed")
    if before.shape != after.shape:
        raise ValueError(f"feeds must be the same shape, got {before.shape} and {after.shape}")
    return _band_levels_db(_rms_matched(before, after), sample_rate) - _band_levels_db(
        before, sample_rate
    )
