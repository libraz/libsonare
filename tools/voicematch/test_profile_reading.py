"""What the summary reads off a spectrum: tone against mechanism noise, the
partial stack, and the velocity axis.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_profile_reading.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module

from metrics import _spectrum
from profile_test_fixtures import SR


def harmonic(f0: float, n_harm: int = 8, seconds: float = 1.0,
             noise: float = 0.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    x = sum(np.sin(2 * np.pi * f0 * n * t) / n for n in range(1, n_harm + 1))
    if noise:
        x = x + noise * np.random.default_rng(0).standard_normal(t.shape)
    return np.asarray(x, dtype=np.float64)


# --------------------------------------------------------------------------
# tone against mechanism noise


def test_a_clean_harmonic_series_reads_far_above_a_noisy_one():
    clean = _spectrum(harmonic(220.0), SR)
    noisy = _spectrum(harmonic(220.0, noise=0.30), SR)
    assert (profile_module.tone_to_noise_db(*clean, 220.0)
            > profile_module.tone_to_noise_db(*noisy, 220.0) + 20.0)


def test_broadband_noise_alone_is_not_reported_as_tonal():
    t = np.arange(SR) / SR
    noise = np.random.default_rng(1).standard_normal(t.shape)
    assert profile_module.tone_to_noise_db(*_spectrum(noise, SR), 220.0) < 0.0


@pytest.mark.parametrize("f0", [44.0, 55.0, 82.4])
def test_a_bass_note_is_not_scored_as_noise_by_the_analysis_window(f0):
    """The partial window has to stay wider than the FFT bin.

    At 44 Hz a +/-2 % window asks for +/-0.9 Hz out of a 1.7 Hz grid, so a
    relative-only window would put a clean bass note's own fundamental outside
    the band counted as tonal and report the string as a noise burst.
    """
    assert profile_module.tone_to_noise_db(*_spectrum(harmonic(f0), SR), f0) > 20.0


def test_an_empty_spectrum_is_not_a_measurement():
    assert not np.isfinite(
        profile_module.tone_to_noise_db(np.zeros(0), np.zeros(0), 220.0)
    )


# --------------------------------------------------------------------------
# the partial stack, and the bins that hold no partial


def test_a_real_partial_stack_is_the_mean_of_its_partials():
    assert profile_module.partial_balance_db(
        [0.0, -6.0, -12.0, -18.0, -24.0, -30.0]) == pytest.approx(-18.0)


def test_a_ladder_with_nothing_above_the_fundamental_is_unscorable():
    """A bar or a bell puts its modes off the ladder, so h2-h6 hold the floor.

    Averaged in, four noise-floor bins read as a partial stack 85 dB too weak,
    which is a finite number for a voice that has no stack on this ruler at all.
    """
    assert profile_module.partial_balance_db(
        [0.0, -95.0, -99.0, -101.0, -97.0, -103.0]) is None


def test_a_floor_bin_does_not_drag_the_mean_of_a_real_partial():
    """One partial and four empty bins is one partial, not a fifth of it."""
    assert profile_module.partial_balance_db(
        [0.0, -6.0, -99.0, -101.0, -97.0, -103.0]) == pytest.approx(-6.0)


# --------------------------------------------------------------------------
# the velocity axis


def rows(peaks: dict[int, float], note: int = 60, timbre: str = "a") -> list[dict]:
    return [{"timbre": timbre, "note": note, "velocity": v, "peak_dbfs": p}
            for v, p in peaks.items()]


def test_the_range_spans_the_whole_captured_axis():
    got = profile_module.velocity_response(rows({24: -30.0, 88: -18.0, 120: -12.0}))
    assert got["60"]["range_db"] == pytest.approx(18.0)
    assert got["60"]["monotonic"]


def test_a_non_monotonic_instrument_is_reported_rather_than_smoothed():
    """On a plucked instrument this is a property, not a fault.

    A model that rises monotonically would otherwise pass on the range alone
    while being wrong about the one thing the axis was captured to settle.
    """
    got = profile_module.velocity_response(rows({24: -20.0, 88: -14.0, 120: -17.0}))
    assert got["60"]["range_db"] == pytest.approx(6.0)
    assert not got["60"]["monotonic"]


def test_one_velocity_is_no_range_at_all():
    assert profile_module.velocity_response(rows({88: -18.0})) == {}


def test_the_summary_carries_the_velocity_response_per_timbre():
    measured = rows({24: -30.0, 120: -12.0}, timbre="dry") + \
        rows({24: -20.0, 120: -18.0}, timbre="wet")
    summary = profile_module.summarize(measured)
    assert summary["dry"]["velocity_response"]["60"]["range_db"] == pytest.approx(18.0)
    assert summary["wet"]["velocity_response"]["60"]["range_db"] == pytest.approx(2.0)
