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

import metrics_modulation
import metrics_partials
from metrics import (
    Tremolo,
    Vibrato,
    _spectrum,
    am_sideband_reach,
    analyze_note,
    fm_sideband_reach,
    held_pitch,
    pitch_excursion,
    pitch_motion,
    tremolo_extent,
    vibrato_extent,
)
from profile_measure import find_partials
from profile_test_fixtures import SR
from smf import Note


def harmonic(f0: float, n_harm: int = 8, seconds: float = 1.0, noise: float = 0.0) -> np.ndarray:
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
    assert (
        profile_module.tone_to_noise_db(*clean, 220.0)
        > profile_module.tone_to_noise_db(*noisy, 220.0) + 20.0
    )


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


def test_partials_above_the_sixteenth_are_tone_not_noise():
    """A bright string's ladder runs far past h16, and all of it is the string."""
    f0 = 110.0
    t = np.arange(SR) / SR
    x = sum(np.sin(2 * np.pi * f0 * n * t) / np.sqrt(n) for n in range(1, 101))
    assert profile_module.tone_to_noise_db(*_spectrum(x, SR), f0) > 40.0


def test_energy_below_the_fundamental_is_left_to_the_body_reading():
    """Sub-f0 energy is what `body_below_f0_db` measures; counted here it is counted twice."""
    f0 = 110.0
    t = np.arange(SR) / SR
    body = 0.5 * np.sin(2 * np.pi * 55.0 * t) + 0.5 * np.sin(2 * np.pi * 70.0 * t)
    assert profile_module.tone_to_noise_db(*_spectrum(harmonic(f0) + body, SR), f0) > 40.0


def test_a_stiff_string_is_masked_where_its_partials_actually_sit():
    """Placed at integer multiples, a stiff string's upper partials fall between the windows."""
    f0, b = 110.0, 2e-4
    t = np.arange(SR) / SR
    x = sum(
        np.sin(2 * np.pi * f0 * n * np.sqrt(1.0 + b * n * n) * t) / np.sqrt(n) for n in range(1, 81)
    )
    spectrum = _spectrum(x, SR)
    assert profile_module.tone_to_noise_db(*spectrum, f0, b) > 40.0
    assert profile_module.tone_to_noise_db(*spectrum, f0, b) > (
        profile_module.tone_to_noise_db(*spectrum, f0) + 20.0
    )


def vibrato_tone(
    f0: float, cents: float, rate: float = 5.0, noise: float = 1e-3, n_harm: int = 8
) -> np.ndarray:
    """`harmonic` under a sinusoidal vibrato of `cents` peak to peak."""
    t = np.arange(SR) / SR
    beta = f0 * (2.0 ** (0.5 * cents / 1200.0) - 1.0) / rate
    phase = 2 * np.pi * f0 * t - beta * np.cos(2 * np.pi * rate * t)
    x = sum(np.sin(n * phase) / n for n in range(1, n_harm + 1))
    return np.asarray(x + noise * np.random.default_rng(0).standard_normal(t.shape))


def tnr_with_vibrato(x: np.ndarray, f0: float) -> float:
    """The ratio as both callers take it: vibrato measured on the segment the spectrum covers."""
    return profile_module.tone_to_noise_db(*_spectrum(x, SR), f0, 0.0, vibrato_extent(x, SR, f0))


def test_a_vibrato_low_note_reads_as_clean_as_the_same_note_held_still():
    """At 147 Hz a 5 Hz vibrato's first sidebands sit outside the still window."""
    still = tnr_with_vibrato(vibrato_tone(147.0, 0.0), 147.0)
    moving = vibrato_tone(147.0, 6.0)
    assert tnr_with_vibrato(moving, 147.0) == pytest.approx(still, abs=2.0)
    assert profile_module.tone_to_noise_db(*_spectrum(moving, SR), 147.0) < still - 20.0


@pytest.mark.parametrize("f0", [47.5, 73.4, 104.6, 147.0, 440.0])
def test_a_still_note_keeps_the_mask_it_had_without_the_vibrato_reading(f0):
    """47.5 and 104.6 Hz are where a 50 ms frame tracker reads a still tone as vibrato."""
    for noise in (1e-3, 3e-2):
        x = harmonic(f0, noise=noise)
        assert vibrato_extent(x, SR, f0) is None
        assert held_pitch(x, SR, f0) == (f0, None)
        assert tnr_with_vibrato(x, f0) == profile_module.tone_to_noise_db(*_spectrum(x, SR), f0)


@pytest.mark.parametrize("glide", [0.01, 0.02, 0.04])
def test_a_struck_pitch_glide_cut_short_by_its_decay_is_not_a_vibrato(glide):
    """The track ends after about a quarter second, so its band holds one cycle of drift."""
    t = np.arange(int(1.5 * SR)) / SR
    f0 = 92.8
    phase = 2 * np.pi * np.cumsum(f0 * (1.0 + glide * np.exp(-t / 0.15))) / SR
    x = sum(np.sin(n * phase) / n for n in range(1, 6)) * np.exp(-t / 0.05)
    x = x + 1e-6 * np.random.default_rng(0).standard_normal(t.shape)
    assert vibrato_extent(x, SR, f0) is None


def test_noise_between_the_partials_of_a_vibrato_note_is_still_counted():
    """The widened windows stay under the quarter-f0 cap, so the gaps still hold noise."""
    noisy = vibrato_tone(147.0, 30.0, noise=0.03)
    still_noisy = vibrato_tone(147.0, 0.0, noise=0.03)
    assert tnr_with_vibrato(noisy, 147.0) == pytest.approx(
        tnr_with_vibrato(still_noisy, 147.0), abs=2.0
    )
    t = np.arange(SR) / SR
    between = vibrato_tone(147.0, 30.0) + 0.1 * np.sin(2 * np.pi * 1.5 * 147.0 * t)
    assert tnr_with_vibrato(between, 147.0) < 25.0
    # A vibrato too deep to resolve still leaves half of white noise outside the mask.
    noise = _spectrum(np.random.default_rng(1).standard_normal(SR), SR)
    assert profile_module.tone_to_noise_db(*noise, 220.0, 0.0, Vibrato(200.0, 5.0, 5.0)) < 1.0


def moving_tone(
    f0: float,
    cents: float,
    deviation: str = "sine",
    rate: float = 6.0,
    seconds: float = 1.5,
    noise: float = 1e-3,
    seed: int = 0,
) -> np.ndarray:
    """`harmonic` with its pitch moving `cents` peak to peak about `f0`.

    `wander` steps the rate to a new value in 4-6 Hz every 250 ms, a player's
    vibrato rather than an oscillator's.
    """
    t = np.arange(int(SR * seconds)) / SR
    cycle = 2 * np.pi * rate * t
    if deviation == "wander":
        knots = np.random.default_rng(seed).uniform(4.0, 6.0, int(seconds * 4) + 2)
        cycle = 2 * np.pi * np.cumsum(np.interp(t, np.linspace(0, seconds, len(knots)), knots)) / SR
    shape = 2 / np.pi * np.arcsin(np.sin(cycle)) if deviation == "triangle" else np.sin(cycle)
    phase = 2 * np.pi * np.cumsum(f0 * 2.0 ** (0.5 * cents * shape / 1200.0)) / SR
    x = sum(np.sin(n * phase) / n for n in range(1, 9))
    return np.asarray(x + noise * np.random.default_rng(0).standard_normal(t.shape))


# Bin-centred for the 1.5 s window, so the still reading carries no scalloping of its own.
ON_BIN_F0 = (110.0, 220.0, 440.0)


@pytest.mark.parametrize("deviation", ["sine", "triangle"])
@pytest.mark.parametrize("f0", ON_BIN_F0)
def test_a_vibrato_reads_the_tuning_and_stack_of_the_note_held_still(f0, deviation):
    """6 Hz x 35 cents: the peak is whichever FM line won, and the upper partials' peaks fall."""
    note = round(69 + 12 * np.log2(f0 / 440.0))
    still = find_partials(moving_tone(f0, 0.0), SR, note)
    moving = find_partials(moving_tone(f0, 35.0, deviation), SR, note)
    assert moving["cents_vs_et"] == pytest.approx(still["cents_vs_et"], abs=1.0)
    assert profile_module.partial_balance_db(moving["partials_db"]) == pytest.approx(
        profile_module.partial_balance_db(still["partials_db"]), abs=0.5
    )


@pytest.mark.parametrize("deviation", ["sine", "triangle"])
@pytest.mark.parametrize("f0", ON_BIN_F0)
def test_the_fit_reads_a_vibrato_with_the_reference_profile_s_ruler(f0, deviation):
    """`analyze_note` is what the loss compares, so it has to agree with `find_partials`."""
    note = Note(round(69 + 12 * np.log2(f0 / 440.0)), 100, 0.0, 2.0)
    pad = np.zeros(int(0.5 * SR))
    still = analyze_note(np.concatenate([moving_tone(f0, 0.0, seconds=2.0), pad]), SR, note, 2.5)
    moving = analyze_note(
        np.concatenate([moving_tone(f0, 35.0, deviation, seconds=2.0), pad]), SR, note, 2.5
    )
    assert moving.f0_cents_err == pytest.approx(still.f0_cents_err, abs=1.0)
    assert np.mean(moving.harmonics_db[1:6]) == pytest.approx(
        np.mean(still.harmonics_db[1:6]), abs=0.5
    )


def test_a_vibrato_off_its_centre_is_read_at_its_mean_pitch():
    """Demodulated around a peak 20 cents high, the track's mean still finds the centre."""
    f0 = 220.0
    x = moving_tone(f0, 35.0)
    pitch, vibrato = held_pitch(x, SR, f0 * 2.0 ** (20.0 / 1200.0))
    assert vibrato is not None
    assert 1200.0 * np.log2(pitch / f0) == pytest.approx(0.0, abs=1.0)


def test_the_held_pitch_does_not_follow_a_component_beside_the_fundamental(monkeypatch):
    """A fourth below, a dipping fundamental's phase follows the neighbour in a wide passband."""
    f0 = 104.0
    t = np.arange(int(1.5 * SR)) / SR
    phase = 2 * np.pi * np.cumsum(f0 * 2.0 ** (6.0 * np.sin(2 * np.pi * 6.0 * t) / 1200.0)) / SR
    dipping = 1.0 - 0.7 * (0.5 - 0.5 * np.cos(2 * np.pi * 0.8 * t))
    x = dipping * np.sin(phase) + sum(np.sin(n * phase) / n for n in range(2, 6))
    x = x + 0.4 * np.sin(2 * np.pi * 0.75 * f0 * t)
    monkeypatch.setattr(metrics_partials, "vibrato_extent", lambda *_: Vibrato(12.0, 6.0, 6.0))
    pitch, _ = metrics_partials.held_pitch(x, SR, f0)
    assert 1200.0 * np.log2(pitch / f0) == pytest.approx(0.0, abs=1.0)
    wide = metrics_modulation.demodulated_pitch(
        x,
        SR,
        f0,
        metrics_modulation.FM_LOWPASS_F0_FRACTION * f0,
        metrics_modulation.FM_STOP_F0_FRACTION * f0,
    )
    assert abs(float(np.mean(wide))) > 20.0


@pytest.mark.parametrize("f0", ON_BIN_F0)
def test_a_wandering_vibrato_reads_as_clean_as_the_same_note_held_still(f0):
    """Masked at its line's one rate, the sidebands of the 6 Hz end read as noise."""
    still = tnr_with_vibrato(moving_tone(f0, 0.0), f0)
    moving = moving_tone(f0, 15.0, "wander", seed=6)
    vibrato = vibrato_extent(moving, SR, f0)
    assert vibrato is not None and vibrato.rate_hi_hz > vibrato.rate_hz
    assert tnr_with_vibrato(moving, f0) == pytest.approx(still, abs=3.0)
    assert profile_module.tone_to_noise_db(*_spectrum(moving, SR), f0) < still - 20.0


def tnr_with_motion(x: np.ndarray, f0: float) -> float:
    """The ratio as both callers take it: `pitch_motion` measured on the segment the spectrum covers."""
    return profile_module.tone_to_noise_db(*_spectrum(x, SR), f0, 0.0, pitch_motion(x, SR, f0))


def ensemble_tone(f0: float, seed: int, players: int = 6, seconds: float = 1.5) -> np.ndarray:
    """`harmonic` played by `players` copies at unit RMS, -60 dB noise added.

    Each copy has its own 5-7 Hz vibrato of 10-20 cents peak to peak, its own
    detune within ±10 cents and its own phases, as a section's players do.
    """
    rng = np.random.default_rng(seed)
    t = np.arange(int(SR * seconds)) / SR
    x = np.zeros_like(t)
    for _ in range(players):
        rate, depth, detune = rng.uniform(5.0, 7.0), rng.uniform(10.0, 20.0), rng.uniform(-10, 10)
        cents = detune + 0.5 * depth * np.sin(2 * np.pi * rate * t + rng.uniform(0, 2 * np.pi))
        phase = 2 * np.pi * np.cumsum(f0 * 2.0 ** (cents / 1200.0)) / SR + rng.uniform(0, 2 * np.pi)
        x += sum(np.sin(n * phase) / n for n in range(1, 9))
    return with_noise(x)


def with_noise(x: np.ndarray) -> np.ndarray:
    """`x` at unit RMS with white noise 60 dB under it."""
    noise = 1e-3 * np.random.default_rng(1).standard_normal(x.shape)
    return np.asarray(x / np.std(x) + noise)


def still_tone(f0: float, seconds: float = 1.5) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    return with_noise(sum(np.sin(2 * np.pi * f0 * n * t) / n for n in range(1, 9)))


#: Sections the held-part excursion does not read as clean as one still player, and why.
ENSEMBLE_SHORTFALLS = {
    (110.0, 1): "the six vibratos cancel in the mean pitch: 7 c against 11-20 c",
    (110.0, 4): "the held part's rate reads 5.3 Hz for the whole track's 5.7: 5.5 dB short",
    (440.0, 3): "the fundamental cancels 40 dB down at 0.5 s, leaving under 0.2 s held",
}
ENSEMBLE_CASES = [
    pytest.param(
        f0, seed, marks=pytest.mark.xfail(strict=True, reason=ENSEMBLE_SHORTFALLS[f0, seed])
    )
    if (f0, seed) in ENSEMBLE_SHORTFALLS
    else (f0, seed)
    for f0 in ON_BIN_F0
    for seed in range(6)
]


@pytest.mark.parametrize(("f0", "seed"), ENSEMBLE_CASES)
def test_an_ensemble_reads_as_clean_as_one_player_held_still(f0, seed):
    """Six players' aperiodic spread is tone, read off the mean pitch's excursion."""
    assert tnr_with_motion(ensemble_tone(f0, seed), f0) == pytest.approx(
        tnr_with_motion(still_tone(f0), f0), abs=3.0
    )


@pytest.mark.parametrize("f0", ON_BIN_F0)
def test_an_interferer_between_an_ensemble_s_partials_is_still_counted(f0):
    """Halfway between two partials, past the quarter-f0 cap, it is noise as for a still tone."""
    t = np.arange(int(1.5 * SR)) / SR
    interferer = 0.1 * np.sin(2 * np.pi * 1.5 * f0 * t)
    ensemble = ensemble_tone(f0, 0)
    assert tnr_with_motion(ensemble + interferer, f0) < tnr_with_motion(ensemble, f0) - 30.0
    assert tnr_with_motion(ensemble + interferer, f0) == pytest.approx(
        tnr_with_motion(still_tone(f0) + interferer, f0), abs=2.0
    )


@pytest.mark.parametrize("f0", ON_BIN_F0)
def test_a_slow_drift_keeps_the_still_mask(f0):
    """20 cents at 0.5 Hz completes under a cycle: it stays inside each partial's still window."""
    t = np.arange(int(1.5 * SR)) / SR
    phase = 2 * np.pi * np.cumsum(f0 * 2.0 ** (10.0 * np.sin(2 * np.pi * 0.5 * t) / 1200.0)) / SR
    drifting = with_noise(sum(np.sin(n * phase) / n for n in range(1, 9)))
    assert pitch_motion(drifting, SR, f0) is None
    assert held_pitch(drifting, SR, f0) == (f0, None)


@pytest.mark.parametrize("f0", [47.5, 73.4, 104.6, 147.0, 440.0])
def test_a_still_note_has_no_pitch_motion(f0):
    for noise in (1e-3, 3e-2):
        x = harmonic(f0, noise=noise)
        assert pitch_excursion(x, SR, f0) is None
        assert pitch_motion(x, SR, f0) is None


@pytest.mark.parametrize("f0", ON_BIN_F0)
def test_a_sine_vibrato_reads_as_its_line_alone(f0):
    """Where the track holds a vibrato, its line decides, though an excursion reads too."""
    x = moving_tone(f0, 35.0)
    assert pitch_excursion(x, SR, f0) is not None
    assert pitch_motion(x, SR, f0) == vibrato_extent(x, SR, f0)


def test_a_layer_a_fourth_below_is_not_read_as_the_fundamental_s_excursion():
    """The excursion's passband stops at a quarter of f0, where the layer sits."""
    f0 = 104.0
    t = np.arange(int(1.5 * SR)) / SR
    phase = 2 * np.pi * np.cumsum(f0 * 2.0 ** (6.0 * np.sin(2 * np.pi * 6.0 * t) / 1200.0)) / SR
    dipping = 1.0 - 0.7 * (0.5 - 0.5 * np.cos(2 * np.pi * 0.8 * t))
    x = dipping * np.sin(phase) + sum(np.sin(n * phase) / n for n in range(2, 6))
    x = x + 0.4 * np.sin(2 * np.pi * 0.75 * f0 * t)
    excursion = pitch_excursion(x, SR, f0)
    assert excursion is not None and excursion.cents < 20.0


@pytest.mark.parametrize("f0", [82.4, 110.0, 220.0, 440.0])
def test_an_onset_glide_that_then_holds_is_no_excursion(f0, monkeypatch):
    """70 cents falling to pitch over 0.25 s, then 6.6 c of jitter: the held part is still."""
    t = np.arange(int(1.5 * SR)) / SR
    jitter = sum(1.5 * np.sin(2 * np.pi * r * t + k) for k, r in enumerate((6.3, 9.1, 13.7)))
    cents = 70.0 * np.clip(1.0 - t / 0.25, 0.0, 1.0) + jitter
    phase = 2 * np.pi * np.cumsum(f0 * 2.0 ** (cents / 1200.0)) / SR
    x = with_noise(np.exp(-t / 1.5) * sum(np.sin(n * phase) / n for n in range(1, 9)))
    assert pitch_excursion(x, SR, f0) is None
    assert pitch_motion(x, SR, f0) is None
    monkeypatch.setattr(metrics_modulation, "EXCURSION_SKIP_S", 0.0)
    monkeypatch.setattr(metrics_modulation, "EXCURSION_TAIL_S", 0.0)
    assert pitch_excursion(x, SR, f0).cents > 70.0


@pytest.mark.parametrize("f0", [349.2, 440.0, 523.3])
def test_a_tremolo_s_phase_leak_on_a_weak_fundamental_is_no_excursion(f0, monkeypatch):
    """Sidebands 16 dB down at +4.1 and -3.9 Hz, as a vibraphone's motor puts them, leak 5-9 c."""
    t = np.arange(int(1.5 * SR)) / SR
    carrier = (
        np.sin(2 * np.pi * f0 * t)
        + 0.16 * np.sin(2 * np.pi * (f0 + 4.1) * t)
        + 0.16 * np.sin(2 * np.pi * (f0 - 3.9) * t)
    )
    upper = sum(np.sin(2 * np.pi * n * f0 * t) / n for n in range(2, 6))
    x = with_noise(
        np.exp(-t) * (0.3 * carrier + upper * (1.0 + 0.32 * np.cos(2 * np.pi * 4.0 * t)))
    )
    assert pitch_excursion(x, SR, f0) is None
    monkeypatch.setattr(metrics_modulation, "EXCURSION_MIN_CENTS", 3.0)
    assert pitch_excursion(x, SR, f0) is not None


@pytest.mark.parametrize("f0", ON_BIN_F0)
def test_a_two_line_beat_widens_the_mask_but_leaves_the_pitch(f0):
    """A component 0.7 of the fundamental 12 Hz above it: tone, but no line to relocate."""
    t = np.arange(int(1.5 * SR)) / SR
    x = sum(np.sin(2 * np.pi * n * f0 * t) / n for n in range(1, 9))
    x = with_noise(x + 0.7 * np.sin(2 * np.pi * (f0 + 12.0) * t))
    motion = pitch_motion(x, SR, f0)
    assert vibrato_extent(x, SR, f0) is None and motion is not None
    assert held_pitch(x, SR, f0) == (f0, motion)
    still = profile_module.tone_to_noise_db(*_spectrum(x, SR), f0)
    assert tnr_with_motion(x, f0) > still + 30.0


def fm_tone(f0: float, components: list[tuple[float, float]], seconds: float = 1.5) -> np.ndarray:
    """`harmonic` under a pitch deviation summed from (peak-to-peak cents, rate Hz) sines."""
    t = np.arange(int(SR * seconds)) / SR
    shape = sum(0.5 * c * np.sin(2 * np.pi * r * t + r) for c, r in components)
    phase = 2 * np.pi * np.cumsum(f0 * 2.0 ** (shape / 1200.0)) / SR
    x = sum(np.sin(n * phase) / n for n in range(1, 9))
    return np.asarray(x + 1e-3 * np.random.default_rng(0).standard_normal(t.shape))


def test_a_vibrato_sharing_its_track_with_fast_fm_is_still_a_vibrato():
    """A second component inside the demodulation passband puts its beat far above the band."""
    x = fm_tone(330.0, [(6.0, 5.0), (8.0, 39.0)])
    vibrato = vibrato_extent(x, SR, 330.0)
    assert vibrato is not None
    assert vibrato.cents == pytest.approx(6.0, abs=1.0)
    assert vibrato.rate_hz <= 5.0 <= vibrato.rate_hi_hz


@pytest.mark.parametrize(
    "components",
    [
        [(20.0, 1.5), (6.0, 0.7)],  # slow drift under the band
        [(12.0, 39.0)],  # fast FM alone
        [(1.5, 6.0), (12.0, 13.0)],  # a shallow line under a beat's second harmonic
        [(2.0, 5.0)],  # a periodic line too shallow for the band shape, kept by the line test
    ],
)
def test_motion_that_is_not_a_wandering_vibrato_keeps_the_line_test(components):
    x = fm_tone(220.0, components)
    vibrato = vibrato_extent(x, SR, 220.0)
    if components == [(2.0, 5.0)]:
        assert vibrato is not None and vibrato.cents == pytest.approx(2.0, abs=0.5)
    else:
        assert vibrato is None


def test_the_sideband_reach_follows_the_modulation_index():
    assert fm_sideband_reach(0.0, 10) == 0
    assert fm_sideband_reach(1e-4, 10) == 0
    assert fm_sideband_reach(0.05, 10) == 1
    assert fm_sideband_reach(5.0, 40) > 6
    assert fm_sideband_reach(5.0, 3) == 3


def tremolo_tone(
    f0: float, db_pp: float, rate: float = 4.3, noise: float = 1e-3, seconds: float = 1.5
) -> np.ndarray:
    """`harmonic` under a level swinging `db_pp` peak to peak in dB, at the still tone's mean power."""
    t = np.arange(int(SR * seconds)) / SR
    swing = 10.0 ** (0.5 * db_pp * np.sin(2 * np.pi * rate * t + 1.0) / 20.0)
    swing /= np.sqrt(np.mean(swing**2))
    x = swing * sum(np.sin(2 * np.pi * f0 * n * t) / n for n in range(1, 9))
    return np.asarray(x + noise * np.random.default_rng(0).standard_normal(t.shape))


def tnr_as_measured(x: np.ndarray, f0: float) -> float:
    """The ratio as both callers take it: pitch and level motion read over the spectrum's segment."""
    return profile_module.tone_to_noise_db(
        *_spectrum(x, SR), f0, 0.0, pitch_motion(x, SR, f0), tremolo_extent(x, SR, f0)
    )


@pytest.mark.parametrize("db_pp", [5.8, 18.0])
def test_a_tremolo_low_note_reads_as_clean_as_the_same_note_held_still(db_pp):
    """At 130 Hz a 4.3 Hz tremolo's first sidebands sit outside the still window.

    The still tone sits 45 dB clean: sidebands under `TNR_SIDEBAND_DB` stay noise,
    which caps a tremolo note's ratio near 60 dB.
    """
    still = tnr_as_measured(tremolo_tone(130.0, 0.0, noise=1e-2), 130.0)
    moving = tremolo_tone(130.0, db_pp, noise=1e-2)
    tremolo = tremolo_extent(moving, SR, 130.0)
    assert tremolo is not None
    assert tremolo.rate_hz == pytest.approx(4.3, abs=0.05)
    assert tremolo.db == pytest.approx(db_pp, abs=0.1 * db_pp)
    assert tnr_as_measured(moving, 130.0) == pytest.approx(still, abs=1.0)
    assert profile_module.tone_to_noise_db(*_spectrum(moving, SR), 130.0) < still - 10.0


@pytest.mark.parametrize("decay_db_s", [6.0, 20.0, 60.0])
@pytest.mark.parametrize("knee_s", [None, 0.4])
def test_a_decaying_struck_tone_reads_no_tremolo(decay_db_s, knee_s):
    """A strike, then a fall whose rate may break at a knee: a level trend, not a modulation."""
    f0 = 130.0
    t = np.arange(int(1.5 * SR)) / SR
    struck = t >= 0.1
    after = np.maximum(t - 0.1, 0.0)
    fall = decay_db_s * after
    if knee_s is not None:
        fall = np.where(after < knee_s, 3.0 * fall, 3.0 * decay_db_s * knee_s + fall)
    level = np.where(struck, 10.0 ** (-fall / 20.0), 0.0)
    x = level * sum(np.sin(2 * np.pi * f0 * n * t) / n for n in range(1, 9))
    x = x + 1e-4 * np.random.default_rng(0).standard_normal(t.shape)
    assert tremolo_extent(x, SR, f0) is None


@pytest.mark.parametrize("f0", [47.5, 73.4, 104.6, 130.0, 147.0, 440.0])
def test_a_still_note_keeps_the_mask_it_had_without_the_tremolo_reading(f0):
    for noise in (1e-3, 3e-2):
        x = harmonic(f0, noise=noise)
        assert tremolo_extent(x, SR, f0) is None
        assert tnr_as_measured(x, f0) == profile_module.tone_to_noise_db(
            *_spectrum(x, SR), f0, 0.0, pitch_motion(x, SR, f0)
        )


def test_noise_between_the_partials_of_a_tremolo_note_is_still_counted():
    """The widened windows stay under the quarter-f0 cap, so the gaps still hold noise."""
    noisy = tremolo_tone(130.0, 18.0, noise=0.03)
    assert tremolo_extent(noisy, SR, 130.0) is not None
    assert tnr_as_measured(noisy, 130.0) == pytest.approx(
        tnr_as_measured(tremolo_tone(130.0, 0.0, noise=0.03), 130.0), abs=2.0
    )
    assert tnr_as_measured(noisy, 130.0) < tnr_as_measured(tremolo_tone(130.0, 18.0), 130.0) - 20.0
    # A tremolo deep enough to reach the cap still leaves half of white noise outside the mask.
    noise = _spectrum(np.random.default_rng(1).standard_normal(SR), SR)
    assert profile_module.tone_to_noise_db(*noise, 220.0, 0.0, None, Tremolo(60.0, 8.0)) < 1.0


def test_the_am_sideband_reach_follows_the_depth():
    """5.8 dB peak to peak, a vibraphone's motor, keeps its -37 dB second pair and not its third."""
    nepers = np.log(10.0) / 40.0
    assert am_sideband_reach(0.0, 10) == 0
    assert am_sideband_reach(1.0 * nepers, 10) == 1
    assert am_sideband_reach(5.8 * nepers, 10) == 2
    assert am_sideband_reach(18.0 * nepers, 10) == 4
    assert am_sideband_reach(18.0 * nepers, 3) == 3


def test_an_empty_spectrum_is_not_a_measurement():
    assert not np.isfinite(profile_module.tone_to_noise_db(np.zeros(0), np.zeros(0), 220.0))


# --------------------------------------------------------------------------
# the partial stack, and the bins that hold no partial


def test_a_real_partial_stack_is_the_mean_of_its_partials():
    assert profile_module.partial_balance_db(
        [0.0, -6.0, -12.0, -18.0, -24.0, -30.0]
    ) == pytest.approx(-18.0)


def test_a_ladder_with_nothing_above_the_fundamental_is_unscorable():
    """A bar or a bell puts its modes off the ladder, so h2-h6 hold the floor.

    Averaged in, four noise-floor bins read as a partial stack 85 dB too weak,
    which is a finite number for a voice that has no stack on this ruler at all.
    """
    assert profile_module.partial_balance_db([0.0, -95.0, -99.0, -101.0, -97.0, -103.0]) is None


def test_a_floor_bin_does_not_drag_the_mean_of_a_real_partial():
    """One partial and four empty bins is one partial, not a fifth of it."""
    assert profile_module.partial_balance_db(
        [0.0, -6.0, -99.0, -101.0, -97.0, -103.0]
    ) == pytest.approx(-6.0)


# --------------------------------------------------------------------------
# the velocity axis


def rows(peaks: dict[int, float], note: int = 60, timbre: str = "a") -> list[dict]:
    return [
        {"timbre": timbre, "note": note, "velocity": v, "peak_dbfs": p} for v, p in peaks.items()
    ]


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
    measured = rows({24: -30.0, 120: -12.0}, timbre="dry") + rows(
        {24: -20.0, 120: -18.0}, timbre="wet"
    )
    summary = profile_module.summarize(measured)
    assert summary["dry"]["velocity_response"]["60"]["range_db"] == pytest.approx(18.0)
    assert summary["wet"]["velocity_response"]["60"]["range_db"] == pytest.approx(2.0)
