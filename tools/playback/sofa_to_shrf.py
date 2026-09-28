"""Convert a SOFA HRIR file (SimpleFreeFieldHRIR) to a SHRF v1 blob.

The SHRF format is `sonare::playback`'s own (see `src/playback/shrf_format.h`):
a regular azimuth x elevation grid of minimum-phase HRIRs plus a per-direction
ITD table, little-endian, int16 or float32 quantized. SOFA carries an
irregular or denser grid and full-phase HRIRs, so this tool does the one-time
reduction: pick (or barycentrically blend) the grid points the target spec
asks for, extract each direction's ITD before the phase information needed to
measure it is discarded, then minimum-phase the HRIR and quantize.

Algorithm, in order:
  1. Resample every HRIR to the target sample rate (skipped if already there).
  2. ITD: low-pass each ear's HRIR at 1.5 kHz, cross-correlate the pair with
     parabolic sub-sample interpolation. Positive = the left ear lags.
  3. Minimum-phase reconstruction via the real cepstrum, computed on the
     original (not low-passed) HRIR, since ITD must be read off the phase
     the minimum-phase step destroys.
  4. Truncate to `taps` samples; taper the last `taper_taps` with a half-Hann
     fade so truncation adds no click.
  5. Resample onto the regular output grid: an exact grid point (within
     `grid_tolerance_deg`) is used directly; otherwise the nearest 3 source
     directions are blended with barycentric weights from a gnomonic
     projection onto the tangent plane at the target direction.
  6. Quantize to int16 with one scale for the whole set (max abs / 32767).

SOFA's spherical azimuth is counter-clockwise (0 = front, + = left); SHRF's is
clockwise (0 = front, + = right), so azimuths are negated (mod 360) on the way
in. `Data.IR`'s receiver axis is index 0 = left ear, 1 = right ear, per the
SimpleFreeFieldHRIR convention.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

import h5py
import numpy as np

_SHRF_MAGIC = b"SHRF"
_SHRF_VERSION = 1
_SHRF_HEADER_FORMAT = "<4sHBBIHHHHffff"
_SHRF_HEADER_BYTES = 36


@dataclass
class GridSpec:
    """The regular output grid: `n_az` columns spanning 360 degrees from
    azimuth 0, and `n_el` rows from `el_min_deg` in steps of `el_step_deg`."""

    n_az: int
    n_el: int
    el_min_deg: float
    el_step_deg: float
    taps: int = 128
    taper_taps: int = 16

    @property
    def az_step_deg(self) -> float:
        return 360.0 / self.n_az


@dataclass
class SofaHrirSet:
    """A loaded SOFA SimpleFreeFieldHRIR set, azimuths already in SHRF's
    clockwise convention."""

    sample_rate: int
    azimuth_deg: np.ndarray  # (M,), SHRF convention (0 front, + right)
    elevation_deg: np.ndarray  # (M,)
    ir: np.ndarray  # (M, 2, N), ear 0 = left


def load_sofa(path: Path) -> SofaHrirSet:
    """Reads the three SimpleFreeFieldHRIR datasets the design settled on:
    `Data.IR`, `SourcePosition`, `Data.SamplingRate`."""
    with h5py.File(path, "r") as f:
        sample_rate = round(float(np.asarray(f["Data.SamplingRate"])[0]))
        position = np.asarray(f["SourcePosition"], dtype=np.float64)
        ir = np.asarray(f["Data.IR"], dtype=np.float64)
    sofa_az = position[:, 0]
    el = position[:, 1]
    shrf_az = np.mod(360.0 - sofa_az, 360.0)
    return SofaHrirSet(sample_rate=sample_rate, azimuth_deg=shrf_az, elevation_deg=el, ir=ir)


def _fft_resample(x: np.ndarray, out_len: int) -> np.ndarray:
    """Bandlimited resampling by resizing the real spectrum (the technique
    `scipy.signal.resample` uses), so a short HRIR round-trips without a
    separately vendored resampler."""
    n = len(x)
    if out_len == n:
        return x.astype(np.float64)
    spectrum = np.fft.rfft(x)
    out_bins = out_len // 2 + 1
    if out_len > n:
        resized = np.zeros(out_bins, dtype=complex)
        resized[: len(spectrum)] = spectrum
    else:
        resized = spectrum[:out_bins].copy()
        if out_len % 2 == 0:
            resized[-1] = resized[-1].real
    y = np.fft.irfft(resized, n=out_len)
    return y * (out_len / n)


def resample_set(hrir_set: SofaHrirSet, target_sample_rate: int) -> SofaHrirSet:
    """Resamples every HRIR to `target_sample_rate`; a no-op if already there."""
    if hrir_set.sample_rate == target_sample_rate:
        return hrir_set
    ratio = target_sample_rate / hrir_set.sample_rate
    out_len = round(hrir_set.ir.shape[-1] * ratio)
    out = np.empty((hrir_set.ir.shape[0], 2, out_len), dtype=np.float64)
    for m in range(hrir_set.ir.shape[0]):
        for ear in range(2):
            out[m, ear] = _fft_resample(hrir_set.ir[m, ear], out_len)
    return SofaHrirSet(
        sample_rate=target_sample_rate,
        azimuth_deg=hrir_set.azimuth_deg,
        elevation_deg=hrir_set.elevation_deg,
        ir=out,
    )


def _design_lowpass(cutoff_hz: float, sample_rate: float, num_taps: int = 127) -> np.ndarray:
    """A windowed-sinc linear-phase FIR low-pass, unit DC gain."""
    n = np.arange(num_taps) - (num_taps - 1) / 2.0
    fc = cutoff_hz / sample_rate
    h = 2.0 * fc * np.sinc(2.0 * fc * n)
    h *= np.blackman(num_taps)
    return h / np.sum(h)


def _cross_correlate(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Full linear cross-correlation `c[lag] = sum_t a[t] * b[t - lag]`, lags
    ordered from `-(len(b)-1)` to `len(a)-1`."""
    n = len(a) + len(b) - 1
    fa = np.fft.rfft(a, n)
    fb = np.fft.rfft(b, n)
    c = np.fft.irfft(fa * np.conj(fb), n)
    c = np.concatenate((c[-(len(b) - 1) :], c[: len(a)]))
    lags = np.arange(-(len(b) - 1), len(a))
    return lags, c


def estimate_itd_samples(left: np.ndarray, right: np.ndarray, sample_rate: float) -> float:
    """ITD in samples (positive: the left ear lags), via a 1.5 kHz low-pass
    ahead of cross-correlation and a parabolic peak refinement."""
    lpf = _design_lowpass(1500.0, sample_rate)
    l_filtered = np.convolve(left, lpf, mode="same")
    r_filtered = np.convolve(right, lpf, mode="same")
    lags, c = _cross_correlate(l_filtered, r_filtered)
    i = int(np.argmax(c))
    if 0 < i < len(c) - 1:
        y0, y1, y2 = c[i - 1], c[i], c[i + 1]
        denom = y0 - 2.0 * y1 + y2
        frac = 0.5 * (y0 - y2) / denom if denom != 0.0 else 0.0
    else:
        frac = 0.0
    return float(lags[i]) + frac


def minimum_phase(h: np.ndarray, out_len: int) -> np.ndarray:
    """Minimum-phase reconstruction of `h` via the real cepstrum (the
    homomorphic method), truncated to `out_len` samples."""
    n = len(h)
    n_fft = 1
    while n_fft < 8 * n:
        n_fft *= 2
    spectrum = np.fft.rfft(h, n_fft)
    log_mag = np.log(np.maximum(np.abs(spectrum), 1e-10))
    cepstrum = np.fft.irfft(log_mag, n_fft)
    window = np.zeros(n_fft)
    half = n_fft // 2
    window[0] = 1.0
    if n_fft % 2 == 0:
        window[1:half] = 2.0
        window[half] = 1.0
    else:
        window[1 : half + 1] = 2.0
    min_phase_spectrum = np.exp(np.fft.rfft(cepstrum * window, n_fft))
    h_min = np.fft.irfft(min_phase_spectrum, n_fft).real
    return h_min[:out_len]


def _taper_tail(h: np.ndarray, taper_taps: int) -> np.ndarray:
    """Fades the last `taper_taps` samples to 0 with the falling half of a
    Hann window, so truncation to `len(h)` taps adds no click."""
    out = h.copy()
    if taper_taps <= 0:
        return out
    fade = np.hanning(2 * taper_taps)[taper_taps:]
    out[-taper_taps:] *= fade
    return out


def _spherical_to_cartesian(az_deg: np.ndarray, el_deg: np.ndarray) -> np.ndarray:
    az = np.radians(az_deg)
    el = np.radians(el_deg)
    return np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], axis=-1)


def _barycentric_weights(target: np.ndarray, neighbors: np.ndarray) -> np.ndarray:
    """Barycentric weights of `target` (unit vector) in the triangle of the 3
    `neighbors` (unit vectors), via gnomonic projection onto the tangent
    plane at `target`. Falls back to equal weights if the triangle is
    degenerate (neighbors collinear through the target)."""
    # Any orthonormal basis of the tangent plane at `target` works; build one
    # from an arbitrary vector not parallel to `target`.
    helper = np.array([1.0, 0.0, 0.0]) if abs(target[0]) < 0.9 else np.array([0.0, 1.0, 0.0])
    u = np.cross(target, helper)
    u /= np.linalg.norm(u)
    v = np.cross(target, u)

    projected = np.empty((3, 2))
    for i, neighbor in enumerate(neighbors):
        cosine = float(np.dot(neighbor, target))
        scaled = neighbor / cosine if abs(cosine) > 1e-9 else neighbor
        projected[i] = [np.dot(scaled, u), np.dot(scaled, v)]

    a, b, c = projected
    denom = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(denom) < 1e-12:
        return np.full(3, 1.0 / 3.0)
    w_a = ((b[1] - c[1]) * (0.0 - c[0]) + (c[0] - b[0]) * (0.0 - c[1])) / denom
    w_b = ((c[1] - a[1]) * (0.0 - c[0]) + (a[0] - c[0]) * (0.0 - c[1])) / denom
    w_c = 1.0 - w_a - w_b
    return np.array([w_a, w_b, w_c])


def build_grid(
    hrir_set: SofaHrirSet, grid: GridSpec, grid_tolerance_deg: float = 0.05
) -> tuple[np.ndarray, np.ndarray]:
    """Resamples `hrir_set` onto `grid`.

    Returns `(itd, hrir)`: `itd` shaped `(n_el, n_az)`, `hrir` shaped
    `(n_el, n_az, 2, grid.taps)`.
    """
    source_xyz = _spherical_to_cartesian(hrir_set.azimuth_deg, hrir_set.elevation_deg)

    itd = np.zeros((grid.n_el, grid.n_az))
    hrir = np.zeros((grid.n_el, grid.n_az, 2, grid.taps))

    for el_index in range(grid.n_el):
        target_el = grid.el_min_deg + el_index * grid.el_step_deg
        for az_index in range(grid.n_az):
            target_az = az_index * grid.az_step_deg
            target_xyz = _spherical_to_cartesian(np.array([target_az]), np.array([target_el]))[0]

            angular_distance_deg = np.degrees(
                np.arccos(np.clip(source_xyz @ target_xyz, -1.0, 1.0))
            )
            nearest = np.argsort(angular_distance_deg)

            if angular_distance_deg[nearest[0]] <= grid_tolerance_deg:
                indices = [int(nearest[0])]
                weights = np.array([1.0])
            else:
                indices = [int(i) for i in nearest[:3]]
                weights = _barycentric_weights(target_xyz, source_xyz[indices])

            direction_itd = 0.0
            direction_hrir = np.zeros((2, hrir_set.ir.shape[-1]))
            for index, weight in zip(indices, weights):
                left_raw = hrir_set.ir[index, 0]
                right_raw = hrir_set.ir[index, 1]
                direction_itd += weight * estimate_itd_samples(
                    left_raw, right_raw, hrir_set.sample_rate
                )
                direction_hrir[0] += weight * left_raw
                direction_hrir[1] += weight * right_raw

            itd[el_index, az_index] = direction_itd
            for ear in range(2):
                min_phase = minimum_phase(direction_hrir[ear], grid.taps)
                hrir[el_index, az_index, ear] = _taper_tail(min_phase, grid.taper_taps)

    return itd, hrir


def quantize_int16(hrir: np.ndarray) -> tuple[np.ndarray, float]:
    """int16 samples and the one scale for the whole set: max abs / 32767."""
    peak = float(np.max(np.abs(hrir)))
    scale = peak / 32767.0 if peak > 0.0 else 1.0
    quantized = np.clip(np.round(hrir / scale), -32768, 32767).astype(np.int16)
    return quantized, scale


def write_shrf(
    path: Path,
    sample_rate: int,
    grid: GridSpec,
    itd: np.ndarray,
    hrir_i16: np.ndarray,
    scale: float,
) -> None:
    """Writes SHRF v1 bytes matching `src/playback/shrf_format.h` exactly."""
    header = struct.pack(
        _SHRF_HEADER_FORMAT,
        _SHRF_MAGIC,
        _SHRF_VERSION,
        1,  # quant = int16
        0,  # reserved
        sample_rate,
        grid.taps,
        grid.n_az,
        grid.n_el,
        0,  # reserved
        grid.az_step_deg,
        grid.el_min_deg,
        grid.el_step_deg,
        scale,
    )
    assert len(header) == _SHRF_HEADER_BYTES
    with open(path, "wb") as f:
        f.write(header)
        f.write(itd.astype("<f4").tobytes())
        f.write(hrir_i16.astype("<i2").tobytes())


def convert(
    sofa_path: Path,
    shrf_path: Path,
    grid: GridSpec,
    target_sample_rate: int = 48000,
) -> None:
    hrir_set = load_sofa(sofa_path)
    hrir_set = resample_set(hrir_set, target_sample_rate)
    itd, hrir = build_grid(hrir_set, grid)
    hrir_i16, scale = quantize_int16(hrir)
    write_shrf(shrf_path, target_sample_rate, grid, itd, hrir_i16, scale)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sofa_path", type=Path)
    parser.add_argument("shrf_path", type=Path)
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--n-az", type=int, default=72)
    parser.add_argument("--n-el", type=int, default=7)
    parser.add_argument("--el-min-deg", type=float, default=-30.0)
    parser.add_argument("--el-step-deg", type=float, default=15.0)
    parser.add_argument("--taps", type=int, default=128)
    parser.add_argument("--taper-taps", type=int, default=16)
    args = parser.parse_args()

    grid = GridSpec(
        n_az=args.n_az,
        n_el=args.n_el,
        el_min_deg=args.el_min_deg,
        el_step_deg=args.el_step_deg,
        taps=args.taps,
        taper_taps=args.taper_taps,
    )
    convert(args.sofa_path, args.shrf_path, grid, args.sample_rate)


if __name__ == "__main__":
    main()
