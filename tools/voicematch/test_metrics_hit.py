"""The two percussion measurements that are not read off the band profile.

Every other column of the percussion set is a level or a first moment over
1/3-octave bands, so a comb of lines and a continuum with the same band profile
are one instrument to all of them. Flatness is the column that separates those
two, and an image is not in a mono mix at all.

Both are written against the same failure: returning a plausible number where
there was nothing to read. A flat spectrum and a window too short to transform
are different answers, and so are a centred source and a render with one channel.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from metrics import (
    THIRD_OCTAVE_CENTERS,
    THIRD_OCTAVE_RATIO,
    _band_power,
    analyze_hit,
    channel_width,
    spectral_flatness_db,
)
from metrics_hit import HIT_TONE_WINDOW_S
from metrics_signal import _spectrum
from smf import Note

SR = 48000


@pytest.fixture
def rng():
    return np.random.default_rng(7)


def _flatness(sig: np.ndarray, max_band_hz: float | None = None) -> float:
    freqs, mag = _spectrum(np.asarray(sig, dtype=np.float64), SR)
    return spectral_flatness_db(freqs, mag, max_band_hz)


def _tone(hz: float, seconds: float = HIT_TONE_WINDOW_S, amp: float = 1.0) -> np.ndarray:
    t = np.arange(int(seconds * SR)) / SR
    return amp * np.sin(2 * np.pi * hz * t)


# --------------------------------------------------------------------------
# Flatness: what it separates


def test_a_tone_reads_far_under_noise_and_the_gap_is_worth_having(rng):
    """The separation is the whole point, so the size of it is the assertion.

    A test that only asserted an ordering would pass on a measure that moves by
    a decibel between a triangle and a shaker, which is inside what two
    recordings of one kit disagree by and would gate nothing.
    """
    noise = rng.standard_normal(int(HIT_TONE_WINDOW_S * SR))
    assert _flatness(noise) > -6.0
    assert _flatness(_tone(1000.0)) < _flatness(noise) - 20.0


def _band_profile(sig: np.ndarray) -> np.ndarray:
    """The 1/3-octave profile every other percussion column is read off."""
    freqs, mag = _spectrum(np.asarray(sig, dtype=np.float64), SR)
    bands = _band_power(freqs, mag**2, THIRD_OCTAVE_CENTERS, THIRD_OCTAVE_RATIO)
    db = 10.0 * np.log10(np.maximum(bands, 1e-30))
    return db - db.max()


def test_a_comb_of_lines_and_a_continuum_of_one_profile_are_not_one_instrument():
    """The failure this exists for: two hits the band profile cannot tell apart.

    A tone per 1/3-octave band, each carrying that band's energy, has the SAME
    profile as the noise it was built from — to a hundredth of a decibel — so
    `bands_db`, `band_tilt`, `band_shape` and the centroid read the two as one
    instrument, and there is nothing else in the percussion set that could
    separate them. One is a struck bar and the other is a filtered shaker.
    """
    rng = np.random.default_rng(3)
    noise = rng.standard_normal(int(HIT_TONE_WINDOW_S * SR))
    freqs, mag = _spectrum(noise, SR)
    bands = _band_power(freqs, mag**2, THIRD_OCTAVE_CENTERS, THIRD_OCTAVE_RATIO)
    t = np.arange(len(noise)) / SR
    comb = np.zeros(len(noise))
    for centre, power in zip(THIRD_OCTAVE_CENTERS, bands):
        comb += np.sin(2 * np.pi * centre * t + rng.uniform(0.0, 2 * np.pi)) * np.sqrt(power)
    assert np.max(np.abs(_band_profile(comb) - _band_profile(noise))) < 0.1
    assert _flatness(comb) < _flatness(noise) - 40.0


def test_within_one_band_it_answers_coarsely_and_the_size_of_that_is_the_point():
    """What this column does NOT buy, asserted so the limit cannot drift shut.

    Flatness says whether a spectrum is made of lines or of a continuum across
    the whole measured range. It is nearly blind to where inside one 1/3-octave
    band the energy sits: a line and a filled quarter-octave carrying the same
    band energy, over the same broadband floor, come back about a decibel apart,
    which is under what two recordings of one kit disagree by. Narrowness within
    a band stays unmeasured, and reading a small `tonality` delta as evidence
    about it would be reading noise.
    """
    def in_the_2k_band(sig):
        freqs = np.fft.rfftfreq(len(sig), 1.0 / SR)
        spectrum = np.fft.rfft(sig)
        spectrum[(freqs < 1781.0) | (freqs > 2245.0)] = 0.0
        return np.fft.irfft(spectrum, len(sig))

    rng = np.random.default_rng(3)
    floor = rng.standard_normal(int(HIT_TONE_WINDOW_S * SR)) * 0.05
    plateau = in_the_2k_band(rng.standard_normal(len(floor)))
    plateau /= float(np.sqrt(np.mean(plateau**2)))
    spike = _tone(2000.0)
    spike /= float(np.sqrt(np.mean(spike**2)))
    assert abs(_flatness(floor + spike) - _flatness(floor + plateau)) < 3.0


def test_flatness_is_blind_to_level():
    """A ratio of two means of one spectrum, so a gain divides out of both."""
    quiet, loud = _tone(800.0, amp=0.001), _tone(800.0, amp=1.0)
    assert _flatness(quiet) == pytest.approx(_flatness(loud), abs=0.01)


def test_content_above_the_capture_s_ceiling_does_not_make_a_hit_read_noisy():
    """Above the edge the spectrum is the recording chain, which is smooth.

    A model with a real wash above what the reference could record would
    otherwise be charged for it here, in the one dimension that cannot be
    normalised away.
    """
    tone = _tone(800.0)
    rng = np.random.default_rng(5)
    hiss = rng.standard_normal(len(tone))
    freqs = np.fft.rfftfreq(len(hiss), 1.0 / SR)
    spectrum = np.fft.rfft(hiss)
    spectrum[freqs < 8000.0] = 0.0
    wash = tone + np.fft.irfft(spectrum, len(hiss))
    assert _flatness(wash, 5000.0) == pytest.approx(_flatness(tone, 5000.0), abs=0.5)
    # And the control: uncut, the same wash moves the reading a long way.
    assert _flatness(wash) > _flatness(tone) + 10.0


def test_a_window_too_short_to_transform_has_no_flatness_rather_than_a_flat_one():
    """Zero dB is white noise. An absence has to read as one."""
    freqs, mag = _spectrum(np.zeros(8), SR)
    assert spectral_flatness_db(freqs, mag, None) is None
    silent = np.zeros(int(HIT_TONE_WINDOW_S * SR))
    assert _flatness(silent) is None


def test_one_cancelled_bin_does_not_decide_the_whole_reading():
    """A geometric mean is decided by its smallest terms, hence the floor.

    Notching a single bin out of a noise spectrum is a change of nothing
    audible, and without the floor it takes the reading to negative infinity.
    """
    rng = np.random.default_rng(11)
    noise = rng.standard_normal(int(HIT_TONE_WINDOW_S * SR))
    spectrum = np.fft.rfft(noise)
    spectrum[400] = 0.0
    notched = np.fft.irfft(spectrum, len(noise))
    assert _flatness(notched) == pytest.approx(_flatness(noise), abs=0.2)


# --------------------------------------------------------------------------
# The image


def test_a_render_with_one_channel_has_no_width_rather_than_no_spread():
    """`None` and 0.0 are different answers and only one of them is a measurement."""
    assert channel_width(None) is None
    assert channel_width(np.zeros(1024)) is None


def test_bit_identical_channels_read_as_mono_and_independent_ones_as_wide(rng):
    mono = rng.standard_normal(4096)
    assert channel_width(np.stack([mono, mono], axis=1)) == pytest.approx(0.0, abs=1e-3)
    wide = np.stack([mono, rng.standard_normal(4096)], axis=1)
    assert channel_width(wide) > 0.9


def test_the_window_decides_the_answer_which_is_why_the_caller_supplies_it(rng):
    """A capture is a hit followed by whatever else is in the file.

    Measured across the whole recording a wide strike is averaged with its tail,
    so a hit whose ring collapses to the middle reads narrower than the strike
    was — which is a fact about the two parts rather than about the instrument.
    `analyze_hit` reads the image over the same window it reads everything else
    over for that reason.
    """
    n = 4096
    strike = np.stack([rng.standard_normal(n), rng.standard_normal(n)], axis=1)
    centred = rng.standard_normal(n * 4)
    whole = np.concatenate([strike, np.stack([centred, centred], axis=1)])
    assert channel_width(whole, 0, n) == pytest.approx(channel_width(strike))
    assert channel_width(whole) < channel_width(strike) - 0.5


def test_a_hit_measured_without_its_channels_reports_no_image(rng):
    """`measure_hit` passes them only when the render has two, so this is the
    ordinary case for a mono corpus and must not read as a centred source."""
    hit = rng.standard_normal(int(0.4 * SR)) * np.exp(
        -np.arange(int(0.4 * SR)) / (0.05 * SR))
    note = Note(38, 100, 0.0, 0.05)
    assert analyze_hit(hit, SR, note, 0.4).stereo_width is None
    two = np.stack([hit, rng.standard_normal(len(hit))], axis=1)
    assert analyze_hit(hit, SR, note, 0.4, stereo=two).stereo_width > 0.9
