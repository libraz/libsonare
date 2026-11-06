"""Time-varying sinusoidal measurements for the per-partial decay skeleton."""

from __future__ import annotations

import numpy as np

SKELETON_FRAME_MIN_S = 0.05
SKELETON_FRAME_CYCLES = 2.5
SKELETON_FRAME_HOP_S = 0.01


def skeleton_frame_s(f0_hz: float) -> float:
    """Frame long enough to resolve low fundamentals, with a 50 ms minimum."""
    return max(SKELETON_FRAME_MIN_S, SKELETON_FRAME_CYCLES / max(float(f0_hz), 1e-9))


def sample_count(seconds: float, sr: int) -> int:
    """Convert a duration to the nearest sample, matching the rendered timeline."""
    return round(float(seconds) * sr)


def skeleton_frame_samples(
    frame_s: float, sr: int, *, hop_s: float = SKELETON_FRAME_HOP_S
) -> tuple[int, int]:
    """The measured window and hop in samples, shared with reachability checks."""
    return max(2, sample_count(frame_s, sr)), max(1, sample_count(hop_s, sr))


def partial_envelopes(
    seg: np.ndarray,
    sr: int,
    freqs_hz: list[float],
    *,
    frame_s: float,
    hop_s: float = SKELETON_FRAME_HOP_S,
) -> tuple[np.ndarray, np.ndarray]:
    """Jointly measure partial amplitudes and their local linear changes.

    One weighted sinusoid fit separates nearby partials in each frame. The
    local-slope columns let an amplitude move during the frame without forcing
    the fit to explain that movement as energy in a neighboring partial.
    Returns frame-center times and a ``(frames, partials)`` amplitude array.
    """
    win_n, hop = skeleton_frame_samples(frame_s, sr, hop_s=hop_s)
    if len(seg) < win_n + hop or not freqs_hz:
        return np.zeros(0, dtype=np.float64), np.zeros((0, len(freqs_hz)), dtype=np.float64)

    n_frames = (len(seg) - win_n) // hop
    frames = np.lib.stride_tricks.sliding_window_view(seg, win_n)[::hop][:n_frames]
    center = (np.arange(win_n, dtype=np.float64) - (win_n - 1) / 2.0) / sr
    u = center / (win_n / (2.0 * sr))
    phase = 2.0 * np.pi * np.outer(center, np.asarray(freqs_hz, dtype=np.float64))
    cos = np.cos(phase)
    sin = np.sin(phase)
    basis = np.concatenate((cos, sin, u[:, None] * cos, u[:, None] * sin), axis=1)
    sqrt_window = np.sqrt(np.hanning(win_n))
    weighted_basis = basis * sqrt_window[:, None]
    projector = np.linalg.pinv(weighted_basis).T * sqrt_window[:, None]
    coeff = frames @ projector
    n_partials = len(freqs_hz)
    envelopes = np.hypot(coeff[:, :n_partials], coeff[:, n_partials : 2 * n_partials])
    times = (np.arange(n_frames) * hop + (win_n - 1) / 2.0) / sr
    return times, envelopes
