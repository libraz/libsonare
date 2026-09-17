"""Restoration quality metrics: segmental SNR, log kurtosis ratio, log-spectral distance.

These three metrics exist in two languages. This module is the Python half; the C++ half is
``tests/mastering/repair_metrics.h``, and ``tests/mastering/repair_metrics_test.cpp`` pins the two
to the same values on the fixed vectors returned by :func:`_agreement_case`. Changing a definition
here means changing it there in the same edit, or the pin goes red.

STOI is Python-only and is not implemented here; see
:func:`short_time_objective_intelligibility`.

Definitions
-----------

**Common framing.** Frames are taken at a fixed length and hop with no centring and no padding; a
trailing partial frame is dropped. ``n_frames = 1 + (n - frame_length) // hop_length``, and zero
frames when the signal is shorter than one frame. Defaults are :data:`FRAME_LENGTH` and
:data:`HOP_LENGTH`, mirrored by the C++ constants of the same names.

**Sample resolution.** Inputs are taken at ``float32`` resolution -- the precision of the audio the
harness measures -- and every accumulation runs in ``float64``. The downcast is what lets the two
languages start from bit-identical samples.

**Spectra.** The spectral metrics window each frame with a periodic Hann window
(``w[n] = 0.5 - 0.5*cos(2*pi*n/N)``) and use the power spectrum ``|rfft(w*x)|**2`` over bins 1
through ``N/2`` inclusive. DC is excluded: it carries the frame's offset, not its spectrum. Powers
are floored by :data:`SPECTRUM_EPSILON` before the logarithm. The frame length must be a power of
two, which is what the C++ half's own radix-2 transform accepts.

**Segmental SNR.** Per frame, ``10*log10(sum(clean**2) / sum((clean - processed)**2))``, averaged
over frames. Frames whose clean RMS is below :data:`SEG_SNR_SILENCE_DBFS` are excluded, and each
frame's value is clipped to ``[SEG_SNR_FLOOR_DB, SEG_SNR_CEILING_DB]``: without both, a near-silent
or a bit-exact frame contributes an unbounded term and one such frame decides the file's average.
The metric is deliberately gain-sensitive -- its reference is the clean signal at the clean
signal's own level, so scaling the output is a real deviation from it and shows up as one. Higher
is better. Returns NaN when no frame is active. Because a clip that carries the value also hides
every change under it, :func:`segmental_snr_report` returns the same number with the clip counts
beside it; a caller comparing rows against a baseline reads that rather than the bare float.

**Log kurtosis ratio.** The raw kurtosis (``m4 / m2**2``, not excess) of the processed log power
spectrum divided by that of the unprocessed input's, after Uemura and Saruwatari. One scalar per
signal pair: the log powers of every (frame, bin) cell are pooled into one population rather than
reduced per band, because a per-band form needs an averaging rule across bands that no measurement
here sets. 1.0 means the processing created no new isolated spectral peaks; above 1.0 means it did.
The processed signal is RMS-matched to the input first: a kurtosis is already blind to the constant
log offset a gain produces, but the epsilon floor is not, and without the match a 6 dB gain moved
this metric in its third decimal. Returns NaN when either population has zero variance.

**Log-spectral distance.** Per frame, the RMS over bins of the difference between the two log power
spectra in dB, averaged over frames. Lower is better. The processed signal is scaled by one global
factor matching its RMS to the reference's before the comparison, so a pure gain reads 0 rather
than ``|20*log10(g)|``; a metric that moves under a gain change is reading loudness as quality.
Only the flat gain is removed -- a tilt still registers.

Run the cross-language pins with ``python3 -m doctest tools/mastering-eval/metrics_repair.py``.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import numpy.typing as npt

FRAME_LENGTH = 1024
"""Default frame length in samples. 1024 at 48 kHz is 21 ms, inside the 15-30 ms convention
segmental SNR is defined over, and a power of two for the spectral metrics."""

HOP_LENGTH = 512
"""Default hop in samples: half of :data:`FRAME_LENGTH`."""

SEG_SNR_FLOOR_DB = -10.0
"""Per-frame segmental SNR lower clip."""

SEG_SNR_CEILING_DB = 35.0
"""Per-frame segmental SNR upper clip, and the value a bit-exact frame contributes."""

SEG_SNR_SILENCE_DBFS = -50.0
"""A frame whose clean RMS is below this is excluded: its SNR is set by the noise floor of the
reference rather than by the processing."""

# Mirrors the C++ float constants::kSpectrumEpsilon widened to double, so both languages floor the
# power spectrum at the same bits.
SPECTRUM_EPSILON = float(np.float32(1e-8))

STOI_SAMPLE_RATE = 10000
"""The rate short-time objective intelligibility is defined at. Any other rate is resampled."""

STOI_FRAME_LENGTH = 256
"""STOI analysis frame: 25.6 ms at STOI_SAMPLE_RATE, hopped by half."""

STOI_FFT_SIZE = 512
"""Transform length the STOI frame is zero-padded to."""

STOI_BAND_COUNT = 15
"""One-third-octave bands, from STOI_LOWEST_BAND_HZ up."""

STOI_LOWEST_BAND_HZ = 150.0
"""Centre of the lowest one-third-octave band."""

STOI_SEGMENT_FRAMES = 30
"""Frames per intelligibility segment: 384 ms, the span the correlation is taken over."""

STOI_CLIP_DB = -15.0
"""Attenuation past which a band stops costing correlation, so an over-suppressed band is not
rewarded for staying quiet."""

STOI_DYNAMIC_RANGE_DB = 40.0
"""A clean frame this far below the loudest one is silence, and is dropped from both signals."""

# Double-precision machine epsilon, used where the reference chain guards a division by a norm.
_FLOAT_EPS = float(np.finfo(np.float64).eps)


def _as_samples(x: npt.ArrayLike, name: str) -> npt.NDArray[np.float64]:
    """Take a signal at float32 resolution and return it as float64."""
    arr = np.asarray(x, dtype=np.float32)
    if arr.ndim != 1:
        raise ValueError(f"{name} must be one-dimensional, got shape {arr.shape}")
    return arr.astype(np.float64)


def _check_pair(
    a: npt.NDArray[np.float64],
    b: npt.NDArray[np.float64],
    frame_length: int,
    hop_length: int,
) -> None:
    if a.shape != b.shape:
        raise ValueError(f"signals must be the same length, got {a.shape[0]} and {b.shape[0]}")
    if frame_length <= 0 or hop_length <= 0:
        raise ValueError("frame_length and hop_length must be positive")


def _frames(
    x: npt.NDArray[np.float64], frame_length: int, hop_length: int
) -> npt.NDArray[np.float64]:
    n = x.shape[0]
    if n < frame_length:
        return np.empty((0, frame_length), dtype=np.float64)
    count = 1 + (n - frame_length) // hop_length
    offsets = hop_length * np.arange(count)[:, None]
    return x[offsets + np.arange(frame_length)[None, :]]


def _hann_periodic(length: int) -> npt.NDArray[np.float64]:
    return 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(length, dtype=np.float64) / length)


def _log_power_spectra(
    x: npt.NDArray[np.float64], frame_length: int, hop_length: int
) -> npt.NDArray[np.float64]:
    """Return 10*log10 of the windowed power spectrum, bins 1..N/2, one row per frame."""
    if frame_length & (frame_length - 1):
        raise ValueError("frame_length must be a power of two")
    frames = _frames(x, frame_length, hop_length)
    if frames.shape[0] == 0:
        return np.empty((0, frame_length // 2), dtype=np.float64)
    spectrum = np.fft.rfft(frames * _hann_periodic(frame_length)[None, :], axis=1)[:, 1:]
    power = spectrum.real**2 + spectrum.imag**2
    return 10.0 * np.log10(power + SPECTRUM_EPSILON)


def _rms_matched(
    reference: npt.NDArray[np.float64], other: npt.NDArray[np.float64]
) -> npt.NDArray[np.float64]:
    """Scale `other` by one global factor so its RMS equals the reference's."""
    rms = float(np.sqrt(np.mean(other**2)))
    if rms <= 0.0:
        return other
    return other * (float(np.sqrt(np.mean(reference**2))) / rms)


def _kurtosis(values: npt.NDArray[np.float64]) -> float:
    """Raw (non-excess) kurtosis; NaN when the population has no spread."""
    if values.size == 0:
        return float("nan")
    centred = values - values.mean()
    squared = centred * centred
    m2 = float(np.mean(squared))
    if m2 <= 0.0:
        return float("nan")
    m4 = float(np.mean(squared * squared))
    return m4 / (m2 * m2)


@dataclass(frozen=True)
class SegmentalSnrReport:
    """A segmental SNR together with how much of it came off the clip.

    A row whose frames all sat on the ceiling reports 35.00 dB however the processing changed, so
    it reads as "not worse" against any baseline and lets every regression through. The clip stays
    where the convention puts it; this is what lets a caller see that it is carrying the value.

    Attributes:
        value: Exactly what :func:`segmental_snr` returns.
        active_frames: Frames above the silence threshold, the population `value` averages.
        ceiling_frames: Active frames whose SNR was taken at SEG_SNR_CEILING_DB.
        floor_frames: Active frames whose SNR was taken at SEG_SNR_FLOOR_DB.
    """

    value: float
    active_frames: int
    ceiling_frames: int
    floor_frames: int

    @property
    def ceiling_fraction(self) -> float:
        """Share of the averaged frames that hit the ceiling; 0.0 when no frame was active."""
        if self.active_frames == 0:
            return 0.0
        return self.ceiling_frames / self.active_frames

    @property
    def floor_fraction(self) -> float:
        """Share of the averaged frames that hit the floor; 0.0 when no frame was active."""
        if self.active_frames == 0:
            return 0.0
        return self.floor_frames / self.active_frames

    @property
    def saturated(self) -> bool:
        """Whether the reported value is itself a clip bound, which only happens when every
        averaged frame hit that bound. Such a row cannot report a change in either direction."""
        return self.value in (SEG_SNR_CEILING_DB, SEG_SNR_FLOOR_DB)


def segmental_snr(
    clean: npt.ArrayLike,
    processed: npt.ArrayLike,
    sample_rate: float,
    frame_length: int = FRAME_LENGTH,
    hop_length: int = HOP_LENGTH,
) -> float:
    """Frame-averaged segmental SNR in dB between a clean reference and a processed signal.

    Thin wrapper over :func:`segmental_snr_report`, which carries the arithmetic and the
    saturation counts; the two cannot disagree about the value.

    Args:
        clean: Clean reference signal, one channel.
        processed: Processed signal of the same length.
        sample_rate: Rate the signals were measured at. The framing is in samples and the
            arithmetic does not read this; it is in the signature so that every metric is called
            the same way and each recorded row carries the rate its frames were taken at.
        frame_length: Frame length in samples.
        hop_length: Hop in samples.

    Returns:
        Mean per-frame SNR in dB over the active frames, or NaN when no frame is active.

    >>> clean, processed = _agreement_case()
    >>> f"{segmental_snr(clean, processed, 8000.0, 16, 8):.12f}"
    '26.831584635411'
    >>> f"{segmental_snr(clean, clean, 8000.0, 16, 8):.12f}"
    '35.000000000000'
    """
    return segmental_snr_report(clean, processed, sample_rate, frame_length, hop_length).value


def segmental_snr_report(
    clean: npt.ArrayLike,
    processed: npt.ArrayLike,
    sample_rate: float,
    frame_length: int = FRAME_LENGTH,
    hop_length: int = HOP_LENGTH,
) -> SegmentalSnrReport:
    """Segmental SNR with the clip counts the value alone cannot show.

    Args:
        clean: Clean reference signal, one channel.
        processed: Processed signal of the same length.
        sample_rate: See :func:`segmental_snr`.
        frame_length: Frame length in samples.
        hop_length: Hop in samples.

    Returns:
        A :class:`SegmentalSnrReport`. With no active frame the value is NaN, the counts are 0 and
        `saturated` is False -- an empty population is not a saturated one.

    >>> clean, processed = _agreement_case()
    >>> report = segmental_snr_report(clean, clean, 8000.0, 16, 8)
    >>> report.saturated, report.ceiling_fraction, report.active_frames
    (True, 1.0, 7)
    >>> report = segmental_snr_report(clean, processed, 8000.0, 16, 8)
    >>> report.saturated, report.ceiling_frames, report.floor_frames
    (False, 0, 0)
    """
    ref = _as_samples(clean, "clean")
    out = _as_samples(processed, "processed")
    _check_pair(ref, out, frame_length, hop_length)

    empty = SegmentalSnrReport(float("nan"), 0, 0, 0)
    ref_frames = _frames(ref, frame_length, hop_length)
    err_frames = _frames(ref - out, frame_length, hop_length)
    if ref_frames.shape[0] == 0:
        return empty

    signal = np.sum(ref_frames**2, axis=1)
    error = np.sum(err_frames**2, axis=1)
    active = signal / frame_length >= 10.0 ** (SEG_SNR_SILENCE_DBFS / 10.0)
    if not np.any(active):
        return empty

    signal = signal[active]
    error = error[active]
    snr = np.where(
        error > 0.0,
        10.0 * np.log10(np.divide(signal, error, out=np.ones_like(signal), where=error > 0.0)),
        SEG_SNR_CEILING_DB,
    )
    clipped = np.clip(snr, SEG_SNR_FLOOR_DB, SEG_SNR_CEILING_DB)
    return SegmentalSnrReport(
        value=float(np.mean(clipped)),
        active_frames=int(clipped.size),
        ceiling_frames=int(np.count_nonzero(clipped == SEG_SNR_CEILING_DB)),
        floor_frames=int(np.count_nonzero(clipped == SEG_SNR_FLOOR_DB)),
    )


def log_kurtosis_ratio(
    unprocessed: npt.ArrayLike,
    processed: npt.ArrayLike,
    sample_rate: float,
    frame_length: int = FRAME_LENGTH,
    hop_length: int = HOP_LENGTH,
) -> float:
    """Ratio of the processed log-spectral kurtosis to the unprocessed input's.

    Args:
        unprocessed: Signal as it entered the processing, one channel.
        processed: Signal as it left, same length.
        sample_rate: See :func:`segmental_snr`.
        frame_length: Frame length in samples.
        hop_length: Hop in samples.

    Returns:
        1.0 when the processing created no new isolated spectral peaks, above 1.0 when it did,
        or NaN when either log spectrum has zero spread.

    >>> clean, processed = _agreement_case()
    >>> f"{log_kurtosis_ratio(clean, processed, 8000.0, 16, 8):.12f}"
    '1.003027174030'
    >>> f"{log_kurtosis_ratio(clean, clean, 8000.0, 16, 8):.12f}"
    '1.000000000000'
    >>> halved = (processed * np.float32(0.5)).astype(np.float32)
    >>> log_kurtosis_ratio(clean, halved, 8000.0, 16, 8) == log_kurtosis_ratio(
    ...     clean, processed, 8000.0, 16, 8
    ... )
    True
    """
    src = _as_samples(unprocessed, "unprocessed")
    out = _as_samples(processed, "processed")
    _check_pair(src, out, frame_length, hop_length)

    src_log = _log_power_spectra(src, frame_length, hop_length)
    out_log = _log_power_spectra(_rms_matched(src, out), frame_length, hop_length)
    if src_log.shape[0] == 0:
        return float("nan")

    src_kurtosis = _kurtosis(src_log.reshape(-1))
    if not np.isfinite(src_kurtosis) or src_kurtosis == 0.0:
        return float("nan")
    return _kurtosis(out_log.reshape(-1)) / src_kurtosis


def log_spectral_distance(
    reference: npt.ArrayLike,
    processed: npt.ArrayLike,
    sample_rate: float,
    frame_length: int = FRAME_LENGTH,
    hop_length: int = HOP_LENGTH,
) -> float:
    """Frame-averaged log-spectral distance in dB, after matching the two signals' RMS.

    Args:
        reference: Reference signal, one channel.
        processed: Processed signal of the same length.
        sample_rate: See :func:`segmental_snr`.
        frame_length: Frame length in samples.
        hop_length: Hop in samples.

    Returns:
        Mean per-frame RMS log-spectral difference in dB, or NaN when there is no frame.

    >>> clean, processed = _agreement_case()
    >>> f"{log_spectral_distance(clean, processed, 8000.0, 16, 8):.12f}"
    '10.275617136869'
    >>> f"{log_spectral_distance(clean, clean, 8000.0, 16, 8):.12f}"
    '0.000000000000'
    >>> halved = (processed * np.float32(0.5)).astype(np.float32)
    >>> log_spectral_distance(clean, halved, 8000.0, 16, 8) == log_spectral_distance(
    ...     clean, processed, 8000.0, 16, 8
    ... )
    True
    """
    ref = _as_samples(reference, "reference")
    out = _as_samples(processed, "processed")
    _check_pair(ref, out, frame_length, hop_length)

    ref_log = _log_power_spectra(ref, frame_length, hop_length)
    out_log = _log_power_spectra(_rms_matched(ref, out), frame_length, hop_length)
    if ref_log.shape[0] == 0:
        return float("nan")

    per_frame = np.sqrt(np.mean((ref_log - out_log) ** 2, axis=1))
    return float(np.mean(per_frame))


def _kaiser_sinc_resample_filter(up: int, down: int) -> npt.NDArray[np.float64]:
    """Anti-aliasing FIR for a rational resample: a windowed sinc, Kaiser-apodised.

    The filter, not the polyphase arithmetic, is what decides whether a resampled STOI agrees with
    the reference implementation's, so its parameters follow the reference chain exactly: a 60 dB
    stopband, a roll-off a tenth of the stopband edge, and the Kaiser beta the rejection implies.
    """
    divisor = int(np.gcd(up, down))
    p = up / divisor
    q = down / divisor
    stopband_cutoff = 1.0 / (2.0 * max(p, q))
    roll_off_width = stopband_cutoff / 10.0
    rejection_db = 60.0
    half_length = np.ceil((rejection_db - 8.0) / (28.714 * roll_off_width))
    taps = np.arange(-half_length, half_length + 1)
    ideal = 2.0 * p * stopband_cutoff * np.sinc(2.0 * stopband_cutoff * taps)
    if 21.0 <= rejection_db <= 50.0:
        beta = 0.5842 * (rejection_db - 21.0) ** 0.4 + 0.07886 * (rejection_db - 21.0)
    elif rejection_db > 50.0:
        beta = 0.1102 * (rejection_db - 8.7)
    else:
        beta = 0.0
    return np.kaiser(int(2 * half_length + 1), beta) * ideal


def _upfirdn(h: npt.NDArray[np.float64], x: npt.NDArray[np.float64], up: int, down: int):
    """Upsample by `up`, filter with `h`, downsample by `down`.

    Decomposed into `up` polyphase branches, which is the same sum as convolving the zero-stuffed
    signal but costs a factor of `up` less: branch b of the interleaved output is x convolved with
    every up-th tap of h starting at b.
    """
    length = (x.size - 1) * up + h.size
    full = np.zeros(length, dtype=np.float64)
    for phase in range(up):
        branch = np.convolve(x, h[phase::up])
        full[phase::up][: branch.size] = branch[: full[phase::up].size]
    return full[::down]


def _resample(x: npt.NDArray[np.float64], up: int, down: int) -> npt.NDArray[np.float64]:
    """Rational resampling by up/down, zero-padded at both ends and delay-compensated."""
    divisor = int(np.gcd(up, down))
    up //= divisor
    down //= divisor
    if up == down == 1:
        return x.copy()

    window = _kaiser_sinc_resample_filter(up, down)
    half_length = (window.size - 1) // 2
    h = window / np.sum(window) * up

    # Zero-pad the filter so the kept output samples sit at the centre of its response.
    n_out = -(-(x.size * up) // down)
    pre_pad = down - half_length % down
    post_pad = 0
    pre_remove = (half_length + pre_pad) // down
    while (h.size + pre_pad + post_pad - 1 + (x.size - 1) * up) // down + 1 < n_out + pre_remove:
        post_pad += 1
    h = np.concatenate((np.zeros(pre_pad), h, np.zeros(post_pad)))
    return _upfirdn(h, x, up, down)[pre_remove : pre_remove + n_out]


def _stoi_window(length: int) -> npt.NDArray[np.float64]:
    """The Hann window the reference chain uses: the interior of a symmetric one two taps longer."""
    return np.hanning(length + 2)[1:-1]


def _stoi_frames(x: npt.NDArray[np.float64], frame_length: int, hop: int):
    """Windowed frames. The last whole frame is dropped -- the reference chain's frame bound is
    exclusive, and a measure that must agree with it inherits its framing rather than its own."""
    window = _stoi_window(frame_length)
    starts = range(0, x.size - frame_length, hop)
    frames = [window * x[i : i + frame_length] for i in starts]
    if not frames:
        return np.zeros((0, frame_length))
    return np.array(frames)


def _remove_silent_frames(
    clean: npt.NDArray[np.float64], processed: npt.NDArray[np.float64], hop: int
):
    """Drop the frames where the clean signal sits more than STOI_DYNAMIC_RANGE_DB below its own
    loudest frame, from both signals, and overlap-add what is left."""
    clean_frames = _stoi_frames(clean, STOI_FRAME_LENGTH, hop)
    processed_frames = _stoi_frames(processed, STOI_FRAME_LENGTH, hop)
    if clean_frames.size == 0:
        return np.zeros(0), np.zeros(0)

    energies = 20.0 * np.log10(np.linalg.norm(clean_frames, axis=1) + _FLOAT_EPS)
    keep = (np.max(energies) - STOI_DYNAMIC_RANGE_DB - energies) < 0
    clean_frames = clean_frames[keep]
    processed_frames = processed_frames[keep]
    if clean_frames.shape[0] == 0:
        return np.zeros(0), np.zeros(0)

    length = (clean_frames.shape[0] - 1) * hop + STOI_FRAME_LENGTH
    clean_out = np.zeros(length)
    processed_out = np.zeros(length)
    for i in range(clean_frames.shape[0]):
        clean_out[i * hop : i * hop + STOI_FRAME_LENGTH] += clean_frames[i]
        processed_out[i * hop : i * hop + STOI_FRAME_LENGTH] += processed_frames[i]
    return clean_out, processed_out


def _third_octave_matrix() -> npt.NDArray[np.float64]:
    """Band matrix mapping rfft bins onto the 15 one-third-octave bands from 150 Hz up.

    Each band spans the bins between the rfft frequencies nearest its edges, the upper edge
    exclusive, which is the reference chain's own band assignment.
    """
    frequencies = np.linspace(0, STOI_SAMPLE_RATE, STOI_FFT_SIZE + 1)[: STOI_FFT_SIZE // 2 + 1]
    index = np.arange(STOI_BAND_COUNT, dtype=np.float64)
    low = STOI_LOWEST_BAND_HZ * np.power(2.0, (2 * index - 1) / 6)
    high = STOI_LOWEST_BAND_HZ * np.power(2.0, (2 * index + 1) / 6)
    matrix = np.zeros((STOI_BAND_COUNT, frequencies.size))
    for band in range(STOI_BAND_COUNT):
        first = int(np.argmin(np.square(frequencies - low[band])))
        last = int(np.argmin(np.square(frequencies - high[band])))
        matrix[band, first:last] = 1.0
    return matrix


def _band_envelopes(x: npt.NDArray[np.float64]) -> npt.NDArray[np.float64]:
    """Per-band RMS envelope, bands by frames."""
    frames = _stoi_frames(x, STOI_FRAME_LENGTH, STOI_FRAME_LENGTH // 2)
    spectrum = np.fft.rfft(frames, n=STOI_FFT_SIZE).transpose()
    return np.sqrt(np.matmul(_third_octave_matrix(), np.square(np.abs(spectrum))))


def _stoi_segments(bands: npt.NDArray[np.float64]) -> npt.NDArray[np.float64]:
    """Every run of STOI_SEGMENT_FRAMES consecutive frames, as segments by bands by frames."""
    ends = range(STOI_SEGMENT_FRAMES, bands.shape[1] + 1)
    return np.array([bands[:, end - STOI_SEGMENT_FRAMES : end] for end in ends])


def short_time_objective_intelligibility(
    clean: npt.ArrayLike,
    processed: npt.ArrayLike,
    sample_rate: float,
) -> float:
    """Short-time objective intelligibility between a clean reference and a processed signal.

    The fourth restoration metric and the only one that lives on the Python side alone, after Taal
    et al. (2011). Carried here rather than taken from a package because the harness may not grow a
    runtime dependency; the only piece the reference implementation takes from scipy is a polyphase
    resampler, and that is a windowed-sinc FIR, reproduced above.

    The measure is defined at 10 kHz, so any other rate is resampled first. **The resampler decides
    whether this agrees with the reference**: everything after it is deterministic arithmetic, while
    the filter design and the polyphase phasing are where an independent implementation drifts.

    Args:
        clean: Clean reference signal, one channel.
        processed: Processed signal of the same length.
        sample_rate: Rate of both signals, in Hz. Must be a whole number of Hz; unlike the other
            three metrics, this one reads it, because the measure is only defined at 10 kHz.

    Returns:
        Intelligibility in roughly 0..1, higher better. NaN when what survives silent-frame removal
        is shorter than one analysis segment, which is a population too small to score rather than
        a low score.

    >>> rng = np.random.default_rng(7)
    >>> clean = np.sin(2 * np.pi * 300.0 * np.arange(16000) / 16000.0)
    >>> noisy = clean + 0.05 * rng.standard_normal(16000)
    >>> round(short_time_objective_intelligibility(clean, clean, 16000.0), 6)
    1.0
    >>> short_time_objective_intelligibility(clean, noisy, 16000.0) < 1.0
    True
    """
    reference = _as_samples(clean, "clean")
    degraded = _as_samples(processed, "processed")
    if reference.shape != degraded.shape:
        raise ValueError(
            f"signals must be the same length, got {reference.shape[0]} and {degraded.shape[0]}"
        )
    if sample_rate != int(sample_rate):
        raise ValueError(f"sample_rate must be a whole number of Hz, got {sample_rate}")

    if int(sample_rate) != STOI_SAMPLE_RATE:
        reference = _resample(reference, STOI_SAMPLE_RATE, int(sample_rate))
        degraded = _resample(degraded, STOI_SAMPLE_RATE, int(sample_rate))

    reference, degraded = _remove_silent_frames(reference, degraded, STOI_FRAME_LENGTH // 2)
    if reference.size <= STOI_FRAME_LENGTH:
        return float("nan")

    reference_bands = _band_envelopes(reference)
    degraded_bands = _band_envelopes(degraded)
    segments = reference_bands.shape[1] - STOI_SEGMENT_FRAMES + 1
    if segments < 1:
        return float("nan")

    reference_segments = _stoi_segments(reference_bands)
    degraded_segments = _stoi_segments(degraded_bands)

    # Match each segment's level to the clean one's, then clip: past this much attenuation the
    # correlation stops falling, which is what keeps STOI from rewarding an over-suppressed band.
    normalisation = np.linalg.norm(reference_segments, axis=2, keepdims=True) / (
        np.linalg.norm(degraded_segments, axis=2, keepdims=True) + _FLOAT_EPS
    )
    clipped = np.minimum(
        degraded_segments * normalisation,
        reference_segments * (1.0 + 10.0 ** (-STOI_CLIP_DB / 20.0)),
    )

    clipped = clipped - np.mean(clipped, axis=2, keepdims=True)
    centred_reference = reference_segments - np.mean(reference_segments, axis=2, keepdims=True)
    clipped = clipped / (_FLOAT_EPS + np.linalg.norm(clipped, axis=2, keepdims=True))
    centred_reference = centred_reference / (
        _FLOAT_EPS + np.linalg.norm(centred_reference, axis=2, keepdims=True)
    )
    return float(np.sum(clipped * centred_reference) / (segments * STOI_BAND_COUNT))


def _agreement_case() -> tuple[npt.NDArray[np.float32], npt.NDArray[np.float32]]:
    """The fixed vectors the C++ half is pinned against.

    The same 64 samples are embedded as literals in ``tests/mastering/repair_metrics_test.cpp``.
    Nine significant digits round-trip through float32, so both languages read the same samples.
    """
    clean = np.array(
        [
            0.0973545834, 0.448020369, 0.56344223, 0.47747317,
            0.364068449, 0.375386655, 0.504074693, 0.589327276,
            0.461526036, 0.0999566093, -0.333829224, -0.617625177,
            -0.642323256, -0.490006953, -0.350897968, -0.352267146,
            -0.447759628, -0.462734759, -0.255865842, 0.14372994,
            0.543952942, 0.737424135, 0.664114833, 0.450502753,
            0.292962402, 0.286892831, 0.349193305, 0.300413966,
            0.0344605222, -0.372338444, -0.708984494, -0.795319438,
            -0.62443471, -0.360641122, -0.195695758, -0.187579736,
            -0.220440716, -0.119085349, 0.182488576, 0.565995574,
            0.814145982, 0.784704745, 0.524722576, 0.227377817,
            0.0689707547, 0.0662896782, 0.0763010159, -0.0632447079,
            -0.375648975, -0.708166659, -0.849993348, -0.705163658,
            -0.372234195, -0.0623646826, 0.0738869458, 0.0626645535,
            0.0672626123, 0.229060575, 0.528344214, 0.78714025,
            0.813293815, 0.562591016, 0.179469377, -0.119114019,
        ],
        dtype=np.float32,
    )  # fmt: skip
    processed = np.array(
        [
            0.0981206074, 0.4555628, 0.566838264, 0.457478046,
            0.356876701, 0.352674395, 0.462118, 0.567563117,
            0.439797133, 0.104728535, -0.325172186, -0.588777661,
            -0.612557292, -0.463409185, -0.334250271, -0.349844068,
            -0.423265785, -0.419098258, -0.23194252, 0.149016172,
            0.529353619, 0.721015871, 0.630402327, 0.438146919,
            0.281121731, 0.310348839, 0.382469416, 0.31563431,
            0.0406418927, -0.349331141, -0.678894639, -0.749402463,
            -0.607337296, -0.34372589, -0.177198187, -0.171282947,
            -0.205833301, -0.101602025, 0.201786071, 0.57286793,
            0.797753453, 0.763524115, 0.503403604, 0.221326634,
            0.0705853254, 0.0294230841, 0.0722776428, -0.0742331371,
            -0.378703684, -0.700963557, -0.84074825, -0.691183209,
            -0.37364161, -0.0581244342, 0.0889330357, 0.0252746921,
            0.0420315862, 0.198966727, 0.496846795, 0.749709249,
            0.77744627, 0.563504279, 0.17052041, -0.115976125,
        ],
        dtype=np.float32,
    )  # fmt: skip
    return clean, processed
