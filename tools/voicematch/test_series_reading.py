"""The partial-series reading: the h1-free second start, stationary lines, and the unwarped keep rule.

Every signal is built with a known stiffness, line or motion, and the reading has to
recover the number it was built from; none is checked against its own output.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import metrics_lines
import metrics_warp
from metrics_partials import fit_partial_series, partial_hz, pitch_motion
from metrics_signal import _peak_near, _spectrum, midi_to_hz
from profile_measure import find_partials, measure_note, read_partials
from toneclass import ToneClass

SR = 48000
PREROLL = 0.1


def stiff_series(
    f0: float,
    b: float,
    *,
    seconds: float = 1.5,
    partials: int = 24,
    rolloff_db: float = 1.5,
    h1_cents: float = 0.0,
    h1_db: float = 0.0,
    cents: float = 0.0,
    rate: float = 5.5,
    detune_cents: float = 0.0,
    noise_db: float | None = None,
) -> np.ndarray:
    """Partials at n*f0*sqrt(1+B n^2), levels falling `rolloff_db` per partial.

    `h1_cents` and `h1_db` move the first partial off its series and down. `cents` is a
    vibrato's peak deviation at `rate`, `detune_cents` splits the voice in two lines.
    """
    t = np.arange(int(seconds * SR)) / SR
    # A vibrato moves every partial by the same fraction: one warped clock for the whole series.
    clock = t + cents / 1200.0 * np.log(2.0) / (2.0 * np.pi * rate) * np.sin(2 * np.pi * rate * t)
    x = np.zeros_like(t)
    for shift in (
        (2.0 ** (-detune_cents / 1200.0), 2.0 ** (detune_cents / 1200.0))
        if detune_cents
        else (1.0,)
    ):
        for n in range(1, partials + 1):
            fn = partial_hz(f0, n, b) * shift * (2.0 ** (h1_cents / 1200.0) if n == 1 else 1.0)
            if fn > 0.45 * SR:
                break
            level = 10.0 ** ((-rolloff_db * (n - 1) + (h1_db if n == 1 else 0.0)) / 20.0)
            x += level * np.sin(2 * np.pi * fn * clock)
    if noise_db is not None:
        x += 10.0 ** (noise_db / 20.0) * np.random.default_rng(0).standard_normal(t.size)
    return x


def read_fit(x: np.ndarray, f0_nominal: float) -> tuple[float, float, int]:
    freqs, mag = _spectrum(x, SR)
    f0, h1 = _peak_near(freqs, mag, f0_nominal, 120.0)
    return fit_partial_series(freqs, mag, f0, h1, SR)


# --------------------------------------------------------------------------
# the h1-free second start


@pytest.mark.parametrize(
    "f0,kwargs",
    [
        (27.5, {"h1_cents": 10.0, "rolloff_db": 1.0}),
        (41.2, {"h1_cents": 6.0}),
        (55.0, {"h1_cents": 8.0, "h1_db": -20.0, "rolloff_db": 0.7, "partials": 40}),
    ],
)
def test_a_first_partial_off_its_series_does_not_bias_the_stiffness(f0, kwargs):
    """A grand's bass note whose h1 sits cents off the law reads the law's B, not h1's pull.

    The first start trusts h1 and reads 0.9-1.7e-4 for the 4e-4 these are built with.
    """
    _, got, fitted = read_fit(stiff_series(f0, 4e-4, **kwargs), f0)
    assert fitted >= 10
    assert got == pytest.approx(4e-4, rel=0.1)


def test_a_clean_series_is_read_as_the_first_start_reads_it():
    """No h1 offset: the second start puts no more level on its series, so nothing changes."""
    x = stiff_series(110.0, 2e-4)
    _, got, _ = read_fit(x, 110.0)
    assert got == pytest.approx(2e-4, rel=0.05)


# --------------------------------------------------------------------------
# stationary lines


def note_with_lines(note: int, lines_hz: list[float]) -> np.ndarray:
    f0 = midi_to_hz(note)
    t = np.arange(int(1.2 * SR)) / SR
    x = sum(np.sin(2 * np.pi * n * f0 * t) / n for n in range(1, 40) if n * f0 < 0.45 * SR)
    x = x + 1e-7 * np.random.default_rng(note).standard_normal(t.size)
    for hz in lines_hz:
        x = x + 3e-5 * np.sin(2 * np.pi * hz * t)
    return np.concatenate([np.zeros(int(PREROLL * SR)), 0.3 * x])


def test_lines_present_at_every_note_are_found_and_the_notes_partials_are_not():
    lines = [10885.8, 18210.1]
    takes = []
    # Two of the eight notes put a partial on a line, which leaves it standing in six takes.
    for note in (36, 43, 50, 57, 64, 71, 78, 85):
        got = metrics_lines.held_lines(note_with_lines(note, lines), SR, 0.8, PREROLL)
        assert got is not None
        takes.append(got[0])
    found = metrics_lines.stationary_lines(takes)
    assert [ln.hz for ln in found] == pytest.approx(lines, abs=0.2)


def test_too_few_takes_say_nothing():
    takes = [np.array([10885.8])] * (metrics_lines.LINE_MIN_TAKES - 1)
    assert metrics_lines.stationary_lines(takes) == []


def test_a_masked_line_leaves_the_median_of_its_surroundings():
    x = note_with_lines(60, [10885.8])[int(PREROLL * SR) :]
    freqs, mag = _spectrum(x, SR)
    k = int(np.argmin(np.abs(freqs - 10885.8)))
    masked = metrics_lines.mask_lines(freqs, mag, [10885.8])
    assert masked[k] < 0.05 * mag[k]
    assert metrics_lines.mask_lines(freqs, mag, []) is mag


# --------------------------------------------------------------------------
# the unwarped reading and its keep rule


def test_a_vibrato_one_source_made_is_kept():
    x = stiff_series(220.0, 0.0, cents=50.0, partials=8, rolloff_db=6.0)
    motion = pitch_motion(x, SR, 220.0)
    assert motion is not None
    got = metrics_warp.line_structure(x, SR, 220.0, motion)
    assert got.state is metrics_warp.CoherenceState.SINGLE_SOURCE and got.coherence > 0.9
    assert got.width_ratio is not None and got.width_ratio < 0.4
    assert got.unwarped is not None and not got.broad


def test_a_unison_is_not_one_source_and_is_read_raw_and_broad():
    x = stiff_series(220.0, 0.0, detune_cents=8.0, partials=8, rolloff_db=6.0)
    motion = pitch_motion(x, SR, 220.0)
    got = metrics_warp.line_structure(x, SR, 220.0, motion)
    assert got.unwarped is None
    assert got.broad and got.broad_ratio >= metrics_warp.BROAD_RATIO


def test_a_still_tone_is_neither_broad_nor_warped():
    x = stiff_series(220.0, 0.0, partials=8, rolloff_db=6.0, noise_db=-40.0)
    got = metrics_warp.line_structure(x, SR, 220.0, None)
    assert got.unwarped is None and got.width_ratio is None and not got.broad


@pytest.mark.parametrize("f0,b,cents", [(110.0, 4e-4, 30.0), (220.0, 2e-4, 50.0)])
def test_a_stiff_string_under_vibrato_keeps_its_stiffness_through_the_warp(f0, b, cents):
    x = stiff_series(f0, b, cents=cents, rolloff_db=2.0, partials=20)
    note = round(69 + 12 * np.log2(f0 / 440.0))
    row = find_partials(x, SR, note)
    assert row["unwarp"]["kept"]
    assert row["inharmonicity_b"] == pytest.approx(b, rel=0.2)


@pytest.mark.parametrize("f0", [110.0, 220.0, 440.0])
def test_a_harmonic_tone_under_vibrato_reads_no_stiffness(f0):
    x = stiff_series(f0, 0.0, cents=50.0, rolloff_db=2.0, partials=20)
    note = round(69 + 12 * np.log2(f0 / 440.0))
    row = find_partials(x, SR, note)
    assert 1200.0 * np.log2(np.sqrt(1.0 + row["inharmonicity_b"] * 144.0)) < 5.0


def held(x: np.ndarray) -> np.ndarray:
    return np.concatenate([np.zeros(int(PREROLL * SR)), 0.3 * x / np.abs(x).max()]).astype(
        np.float32
    )


def test_a_steady_note_carries_no_motion_fields():
    x = stiff_series(261.63, 0.0, partials=8, rolloff_db=6.0, noise_db=-40.0)
    row = measure_note(held(x), SR, 60, preroll_s=PREROLL, gate_s=1.8)
    assert not {
        "unwarp",
        "broad_lines",
        "b_unreliable",
        "stationary_lines_hz",
        "modal_profile",
    } & set(row)


def test_b_unreliable_names_the_dimensions_that_read_through_it():
    """Two lines per partial hop under the fit: the row says which readings that touches."""
    flagged = None
    for detune in (8.0, 12.0, 16.0):
        x = stiff_series(220.0, 3e-4, detune_cents=detune, partials=20, rolloff_db=1.0)
        row = find_partials(x, SR, 57)
        if "b_unreliable" in row:
            flagged = row["b_unreliable"]
            break
    assert flagged is not None
    assert flagged["dimensions"] == ["stretch", "balance", "partial_tilt", "tnr"]
    assert "off one series" in flagged["reason"]


# --------------------------------------------------------------------------
# modal captures


def test_a_modal_capture_fits_no_stiffness_and_carries_its_measured_partials():
    x = stiff_series(261.63, 6e-4, partials=10, rolloff_db=3.0)
    row = measure_note(held(x), SR, 60, preroll_s=PREROLL, gate_s=1.8, modal=True)
    assert row["inharmonicity_b"] == 0.0
    assert row["partials_fit"] == 0 and row["inharmonicity_reliable"] is False
    assert row["modal_profile"]["modal_hz"]
    assert "modal_profile" not in measure_note(held(x), SR, 60, preroll_s=PREROLL, gate_s=1.8)


def test_only_the_modal_class_fits_no_stiffness():
    assert [c for c in ToneClass if not c.stiffness_fit] == [ToneClass.MODAL]


def test_read_partials_reports_the_structure_it_read_the_row_on():
    x = stiff_series(220.0, 2e-4, cents=50.0, rolloff_db=2.0, partials=20)
    row, structure, masked = read_partials(x, SR, 57)
    assert masked == []
    assert structure is not None and structure.unwarped is not None
    assert row["unwarp"]["width_ratio"] == pytest.approx(structure.width_ratio, abs=1e-3)


def test_a_stationary_line_that_is_also_a_partial_of_the_note_is_not_masked():
    f0 = 369.994
    x = stiff_series(f0, 0.0, partials=20, rolloff_db=2.0)
    on_series, off_series = 16 * f0, 7777.0
    _, _, masked = read_partials(x, SR, 66, stationary_lines_hz=[on_series, off_series])
    assert masked == [off_series]


def test_a_still_ladder_reads_the_same_level_wherever_its_partials_fall_in_a_bin():
    # A fixed 1/k ladder at f0s a fraction of a bin apart: a bare Hann peak scallops by up to
    # ~1 dB with the offset, a lobe's power does not. The truth is -20 log10(k) per partial.
    bin_hz = 1.0 / 1.5
    f0s = [220.0 + bin_hz * frac for frac in (0.0, 0.17, 0.33, 0.5, 0.67, 0.83)]
    truth = np.array([-20.0 * np.log10(n) for n in range(1, 9)])
    for f0 in f0s:
        t = np.arange(int(1.5 * SR)) / SR
        x = sum(np.sin(2 * np.pi * n * f0 * t) / n for n in range(1, 21))
        row, _, _ = read_partials(x, SR, 57)
        got = np.array(row["partials_db"][:8])
        assert got == pytest.approx(truth, abs=0.2), f0


def _ladder_balance(cents_spread=0.0, snr_db=None, power=1, seed=3):
    """partial_balance_db of a 1/k^power ladder off-bin, each partial detuned off the series."""
    from profile_summary import partial_balance_db

    rng = np.random.default_rng(seed)
    t = np.arange(int(1.5 * SR)) / SR
    f0 = 220.0 + 0.37 / 1.5
    dev = rng.uniform(-cents_spread, cents_spread, 21)
    dev[1] = 0.0
    x = sum(
        np.sin(2 * np.pi * n * f0 * 2 ** (dev[n] / 1200) * t + rng.uniform(0, 2 * np.pi)) / n**power
        for n in range(1, 21)
    )
    if snr_db is not None:
        x = x + rng.normal(0.0, np.std(x) * 10 ** (-snr_db / 20), x.size)
    row, _, _ = read_partials(x, SR, 57)
    truth = partial_balance_db([-20.0 * power * np.log10(n) for n in range(1, 9)])
    return partial_balance_db(row["partials_db"]), truth


@pytest.mark.parametrize("cents", [5.0, 10.0, 20.0])
def test_a_still_partial_off_its_series_is_read_at_the_peak_found(cents):
    # Partials a few cents off the fitted series: a lobe centred on the series misses the
    # peak by more than its own width and reads the noise beside it, 9 dB dark at 10 cents.
    got, truth = _ladder_balance(cents_spread=cents, snr_db=60.0)
    assert got == pytest.approx(truth, abs=0.2)


@pytest.mark.parametrize("snr_db", [40.0, 10.0, 0.0])
def test_a_still_ladder_over_white_noise_keeps_its_balance(snr_db):
    got, truth = _ladder_balance(snr_db=snr_db, power=2, seed=11)
    assert got == pytest.approx(truth, abs=0.4)
