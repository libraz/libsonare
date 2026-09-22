"""The multi-scale spectral distance: the loss term taken over the whole timeline."""

from __future__ import annotations

import numpy as np

MSS_FFT_SIZES = (512, 1024, 2048, 4096)


def _stft_mag(x: np.ndarray, n_fft: int, hop: int) -> np.ndarray:
    """Magnitude STFT of a mono signal; (frames, bins)."""
    if len(x) < n_fft:
        x = np.concatenate([x, np.zeros(n_fft - len(x))])
    n_frames = (len(x) - n_fft) // hop + 1
    frames = np.lib.stride_tricks.sliding_window_view(x, n_fft)[::hop][:n_frames]
    return np.abs(np.fft.rfft(frames * np.hanning(n_fft), axis=1))


# How the multi-scale distance weights the spectrum, and why not equally.
#
# A linear-frequency bin grid puts half its bins above a quarter of the sample
# rate. At 48 kHz that is 12 kHz up, where a sampled reference is usually
# reporting its own capture bandwidth, and only a handful of bins cover the
# octave from 100 to 200 Hz where a bass note's whole identity lives. So an
# unweighted mean over bins is a mean over the wrong measure: it is dominated by
# the top two octaves, which carry the least musical information per bin and the
# most measurement artefact.
#
# Weighting each bin by 1/f makes every octave contribute equally, which is the
# scale pitch is heard on and the scale `shape/spectro.py` already compares its
# spectrograms on. The two halves of this harness were reading the same renders
# through different frequency scales.
MSS_LOG_WEIGHTING = True
#: Below this a bin is weighted as if it sat here. Without the clamp the DC and
#: first few bins take weights running to infinity.
MSS_MIN_HZ = 30.0


def _log_bin_weights(n_fft: int, sr: float = 48000.0) -> np.ndarray:
    """Per-bin weights that make each octave contribute equally, summing to 1."""
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    w = 1.0 / np.maximum(freqs, MSS_MIN_HZ)
    return w / w.sum()


# How far under ITS OWN loudest bin a side still carries signal, and how many
# bins clearing that on both sides the gain below needs before it is a
# measurement. A bin one side has fallen silent in IS the duration difference,
# so reading the gain there would put that difference straight back into the
# quantity the gain exists to separate from it. Sixty dB is the span the band
# metrics already treat as the reference's usable range; sixty-four bins is a
# handful of frames at the smallest scale, under which a median is a guess.
#
# Each side against its own peak rather than both against the pair's, because a
# floor set by the louder side moves when either side's gain does — which would
# make the set the median is taken over depend on the gain, and so make the
# correction only approximately undo it. Measured on a spectrum against a
# brighter one, a 4x gain then moved the term from 326 to 1299.
MSS_GAIN_FLOOR_DB = 60.0
MSS_GAIN_MIN_BINS = 64


def _common_gain(model_mag: np.ndarray, oracle_mag: np.ndarray) -> float:
    """What the model must be multiplied by to sit at the oracle's level.

    Both renders reach this term already scaled to a common RMS over the whole
    timeline, and that is a duration weighting rather than a level match: a
    model that decays faster than its reference holds less total energy, so
    equalising the total lifts the part of it that is still sounding above the
    reference's. The harmonic and band terms are ratios against each side's own
    reference bin and cancel that lift; this one does not — a gain `g` adds
    `|log g|` to every bin of the log term, and the linear term's division by
    the oracle's mean scales the oracle rather than matching the two.

    So the lift is estimated where both sides still sound and divided out,
    which leaves the shape difference — the thing this term is for — and takes
    out the level difference, which `level` already scores on its own.

    The median rather than the mean because the term is an L1 distance, whose
    minimiser over a constant offset is the median of the offsets.
    """
    span = 10.0 ** (-MSS_GAIN_FLOOR_DB / 20.0)
    model_floor = float(model_mag.max(initial=0.0)) * span
    oracle_floor = float(oracle_mag.max(initial=0.0)) * span
    if model_floor <= 0.0 or oracle_floor <= 0.0:
        return 1.0
    both = (model_mag > model_floor) & (oracle_mag > oracle_floor)
    if int(both.sum()) < MSS_GAIN_MIN_BINS:
        return 1.0
    return float(np.exp(np.median(np.log(oracle_mag[both]) - np.log(model_mag[both]))))


def mss_distance(model: np.ndarray, oracle: np.ndarray) -> float:
    """Multi-scale STFT distance between two mono renders.

    The per-note metric set models what is known to matter for a physical
    voice — harmonic ladder, intonation, noise floor, envelope. This term sees
    the rest: inharmonicity, formant structure between the harmonics, attack
    detail, anything the sustain window misses. Log and linear magnitude are
    both summed at each scale, the linear term normalised by the oracle's own
    mean so the scales stay comparable. Phase is deliberately ignored — two
    renders of the same note are never phase-aligned.

    Level is ignored too, which it was not before `_common_gain`: measured on
    two renders separated by nothing but a gain, this term charged 0.80 at 1.5x
    and 4.06 at 4x, and now charges zero at both, while a spectral difference
    taken at matched level reads what it read.
    """
    n = min(len(model), len(oracle))
    if n < MSS_FFT_SIZES[-1]:
        return 0.0
    a = np.asarray(model[:n], dtype=np.float64)
    b = np.asarray(oracle[:n], dtype=np.float64)
    total = 0.0
    for n_fft in MSS_FFT_SIZES:
        sa = _stft_mag(a, n_fft, n_fft // 4)
        sb = _stft_mag(b, n_fft, n_fft // 4)
        m = min(len(sa), len(sb))
        sa, sb = sa[:m], sb[:m]
        # The RMS normalisation upstream equalises total energy, which weights
        # by duration. Taken out here, at this scale's own resolution, so what
        # is left is the spectral shape. See `_common_gain`.
        sa = sa * _common_gain(sa, sb)
        # Each octave weighted equally rather than each bin — see
        # MSS_LOG_WEIGHTING. `np.average` over the bin axis, then a plain mean
        # over frames, so the weighting changes which frequencies count and not
        # which moments do.
        w = _log_bin_weights(n_fft) if MSS_LOG_WEIGHTING else None
        scale = max(float(np.mean(sb @ w) if w is not None else np.mean(sb)), 1e-9)
        lin = np.abs(sa - sb)
        log = np.abs(np.log(sa + 1e-5) - np.log(sb + 1e-5))
        if w is None:
            total += float(np.mean(lin)) / scale + float(np.mean(log))
        else:
            total += float(np.mean(lin @ w)) / scale + float(np.mean(log @ w))
    return total / len(MSS_FFT_SIZES)
