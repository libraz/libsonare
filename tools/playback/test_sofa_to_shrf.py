"""Tests for `sofa_to_shrf.py`: ITD recovery (native rate and resampled),
the minimum-phase reconstruction's energy-compaction property, and a SHRF v1
header round-trip."""

from __future__ import annotations

import struct
from pathlib import Path

import h5py
import numpy as np
import pytest
import sofa_to_shrf as s2s


def _bandlimited_impulse(length: int, center: float) -> np.ndarray:
    """A windowed sinc peaking at fractional sample `center`, standing in for
    a measured HRIR onset with a known, sub-sample-accurate delay."""
    t = np.arange(length, dtype=np.float64)
    x = np.sinc(t - center)
    x *= np.blackman(length)
    return x


def _write_synthetic_sofa(
    path: Path, sofa_azimuths_deg: list[float], sample_rate: int, itd_of_shrf_az_samples
) -> None:
    """A minimal SimpleFreeFieldHRIR file: `Data.IR`, `SourcePosition`,
    `Data.SamplingRate` only, matching what `load_sofa` reads. All directions
    are on the horizontal plane (elevation 0). `itd_of_shrf_az_samples(az)`
    gives the desired ITD (positive: left lags) once `az` is converted to
    SHRF's clockwise convention.
    """
    length = 200
    center = 100.0
    n = len(sofa_azimuths_deg)
    ir = np.zeros((n, 2, length), dtype=np.float64)
    position = np.zeros((n, 3), dtype=np.float64)
    for i, sofa_az in enumerate(sofa_azimuths_deg):
        shrf_az = (360.0 - sofa_az) % 360.0
        itd = itd_of_shrf_az_samples(shrf_az)
        # Positive ITD: the left ear lags, i.e. the left impulse sits later.
        ir[i, 0] = _bandlimited_impulse(length, center + itd)
        ir[i, 1] = _bandlimited_impulse(length, center)
        position[i] = [sofa_az, 0.0, 1.2]

    with h5py.File(path, "w") as f:
        f.create_dataset("Data.IR", data=ir)
        f.create_dataset("SourcePosition", data=position)
        f.create_dataset("Data.SamplingRate", data=np.array([float(sample_rate)]))


def _itd_model(shrf_az_deg: float) -> float:
    """Known-ground-truth ITD used by the synthetic fixtures: 20 samples at
    the horizontal extremes, zero on the median plane."""
    return 20.0 * np.sin(np.radians(shrf_az_deg))


def test_itd_recovery_at_native_rate(tmp_path: Path) -> None:
    sofa_azimuths = [0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0]
    sofa_path = tmp_path / "synthetic.sofa"
    _write_synthetic_sofa(
        sofa_path, sofa_azimuths, sample_rate=48000, itd_of_shrf_az_samples=_itd_model
    )

    hrir_set = s2s.load_sofa(sofa_path)
    for i in range(len(sofa_azimuths)):
        expected = _itd_model(hrir_set.azimuth_deg[i])
        measured = s2s.estimate_itd_samples(
            hrir_set.ir[i, 0], hrir_set.ir[i, 1], hrir_set.sample_rate
        )
        assert measured == pytest.approx(expected, abs=0.1)


def test_itd_recovery_with_resampling(tmp_path: Path) -> None:
    """The delay is authored in samples at 44.1 kHz; after resampling to
    48 kHz the same physical delay in samples scales by the rate ratio."""
    source_rate = 44100
    target_rate = 48000
    sofa_azimuths = [0.0, 60.0, 90.0, 120.0, 180.0, 270.0]
    sofa_path = tmp_path / "synthetic_44k1.sofa"
    _write_synthetic_sofa(
        sofa_path, sofa_azimuths, sample_rate=source_rate, itd_of_shrf_az_samples=_itd_model
    )

    hrir_set = s2s.load_sofa(sofa_path)
    resampled = s2s.resample_set(hrir_set, target_rate)
    ratio = target_rate / source_rate
    for i in range(len(sofa_azimuths)):
        expected = _itd_model(resampled.azimuth_deg[i]) * ratio
        measured = s2s.estimate_itd_samples(
            resampled.ir[i, 0], resampled.ir[i, 1], resampled.sample_rate
        )
        assert measured == pytest.approx(expected, abs=0.1)


def test_minimum_phase_compacts_energy_and_preserves_magnitude() -> None:
    # A symmetric (linear-phase) low-pass kernel: energy spread around its
    # center tap, none of it concentrated near sample 0.
    length = 64
    n = np.arange(length) - (length - 1) / 2.0
    fc = 4000.0 / 48000.0
    linear_phase = 2.0 * fc * np.sinc(2.0 * fc * n) * np.blackman(length)

    min_phase = s2s.minimum_phase(linear_phase, length)

    def energy_fraction_in_first_k(x: np.ndarray, k: int) -> float:
        energy = x.astype(np.float64) ** 2
        return float(energy[:k].sum() / energy.sum())

    # Minimum phase front-loads the energy the linear-phase kernel spreads
    # across its full symmetric support.
    assert energy_fraction_in_first_k(min_phase, 8) > 10 * energy_fraction_in_first_k(
        linear_phase, 8
    )
    assert energy_fraction_in_first_k(min_phase, 16) > 0.8

    # The cepstral reconstruction changes phase only: magnitude, and thus
    # total energy (Parseval), is preserved.
    total_energy_ratio = np.sum(min_phase**2) / np.sum(linear_phase**2)
    assert total_energy_ratio == pytest.approx(1.0, abs=0.01)


def _read_shrf_header(path: Path) -> dict:
    with open(path, "rb") as f:
        raw = f.read(s2s._SHRF_HEADER_BYTES)
    (
        magic,
        version,
        quant,
        _reserved_u8,
        sample_rate,
        taps,
        n_az,
        n_el,
        _reserved_u16,
        az_step_deg,
        el_min_deg,
        el_step_deg,
        scale,
    ) = struct.unpack(s2s._SHRF_HEADER_FORMAT, raw)
    return {
        "magic": magic,
        "version": version,
        "quant": quant,
        "sample_rate": sample_rate,
        "taps": taps,
        "n_az": n_az,
        "n_el": n_el,
        "az_step_deg": az_step_deg,
        "el_min_deg": el_min_deg,
        "el_step_deg": el_step_deg,
        "scale": scale,
    }


def test_header_round_trip(tmp_path: Path) -> None:
    sofa_azimuths = [0.0, 90.0, 180.0, 270.0]
    sofa_path = tmp_path / "synthetic.sofa"
    _write_synthetic_sofa(
        sofa_path, sofa_azimuths, sample_rate=48000, itd_of_shrf_az_samples=_itd_model
    )

    grid = s2s.GridSpec(n_az=4, n_el=1, el_min_deg=0.0, el_step_deg=15.0, taps=32, taper_taps=8)
    shrf_path = tmp_path / "out.shrf"
    s2s.convert(sofa_path, shrf_path, grid, target_sample_rate=48000)

    header = _read_shrf_header(shrf_path)
    assert header["magic"] == b"SHRF"
    assert header["version"] == 1
    assert header["quant"] == 1  # int16
    assert header["sample_rate"] == 48000
    assert header["taps"] == 32
    assert header["n_az"] == 4
    assert header["n_el"] == 1
    assert header["az_step_deg"] == pytest.approx(90.0)
    assert header["el_min_deg"] == pytest.approx(0.0)
    assert header["el_step_deg"] == pytest.approx(15.0)
    assert header["scale"] > 0.0

    expected_size = (
        s2s._SHRF_HEADER_BYTES
        + grid.n_el * grid.n_az * 4
        + grid.n_el * grid.n_az * 2 * grid.taps * 2
    )
    assert shrf_path.stat().st_size == expected_size
