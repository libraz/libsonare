"""Tests for the measurements added for what is not a stiff string.

Everything here runs on synthesised signals. The captured corpora cannot be
committed, so a test that needed one would be a test that never runs, and
synthesis is the only way to grade an estimator against an answer that is known
rather than measured.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from loss import (  # noqa: E402
    BAND_REFERENCE_FLOOR_DB,
    LOSS_TERMS,
    LossWeights,
    _mod_terms,
    _modes_terms,
    _pair_modes,
    loss_terms,
    percussion_terms,
)
from metrics import (  # noqa: E402
    THIRD_OCTAVE_CENTERS,
    a_weight_db,
    analyze_hit,
    analyze_note,
    audibility_weights,
    channel_correlation,
    hit_tone,
    ladder_present,
    measure_band_edge,
    modulation_note,
    pitch_drop,
    to_mono,
)
from patterns import LONG_DECAY_DRUM_NOTES, build_pattern, drum_gap_for  # noqa: E402
from smf import Note  # noqa: E402
from toneclass import ToneClass, default_weights, tone_class  # noqa: E402

SR = 48000


def _modal(f0: float, ratios, levels, decays, *, seconds: float = 3.5,
           seed: int = 0, noise: float = 1e-4) -> np.ndarray:
    """A struck bar or bell: partials at arbitrary ratios, each with its own decay."""
    n = int(SR * seconds)
    t = np.arange(n) / SR
    rng = np.random.default_rng(seed)
    y = np.zeros(n)
    for r, level, decay in zip(ratios, levels, decays):
        y += level * np.sin(2 * np.pi * f0 * r * t + rng.uniform(0, 6.28)) * np.exp(-t / decay)
    return y + noise * rng.standard_normal(n)


# --------------------------------------------------------------------------- #
# Which ruler an instrument gets
# --------------------------------------------------------------------------- #
def test_the_class_of_a_program_follows_the_instrument_not_the_family():
    assert tone_class(0) is ToneClass.STRUCK_STRING          # piano
    assert tone_class(9) is ToneClass.MODAL                  # glockenspiel
    assert tone_class(15) is ToneClass.STRUCK_STRING         # dulcimer, not a bar
    assert tone_class(40) is ToneClass.SUSTAINED             # violin
    assert tone_class(45) is ToneClass.PLUCKED_STRING        # pizzicato, not bowed
    assert tone_class(47) is ToneClass.MODAL                 # timpani
    assert tone_class(116) is ToneClass.MODAL                # taiko
    assert tone_class(120) is ToneClass.NOISE                # sound effect
    assert tone_class(0, drum_note=38) is ToneClass.MODAL    # the kit, whatever the program


def test_a_modal_voice_is_never_weighted_on_the_harmonic_ladder():
    """The ladder measures its noise floor, so weighting it is worse than useless."""
    assert "harm" not in default_weights(9)
    assert default_weights(9)["modes"] > 0.0
    # And a string still is, because there the ladder is the right ruler.
    assert default_weights(40)["harm"] > 0.0
    assert default_weights(0)["harm"] > 0.0


def test_every_program_has_a_register_inside_something_playable():
    """The old table covered three families; the rest fell to C3/C4/C5."""
    from patterns import registers_for_program

    for program in range(128):
        lo, mid, hi = registers_for_program(program)
        assert 0 <= lo <= mid <= hi <= 127
    # The families that used to fall through, each now in its own compass.
    assert registers_for_program(9)[0] >= 72      # glockenspiel is a treble instrument
    assert registers_for_program(24)[2] <= 67     # a guitar's top string is E5
    assert registers_for_program(116)[2] <= 55    # a taiko is not a piccolo


# --------------------------------------------------------------------------- #
# The measured partial series
# --------------------------------------------------------------------------- #
def test_a_bar_s_partials_are_found_where_they_are_rather_than_where_a_series_predicts():
    """1 : 2.756 : 5.404 : 8.933 — a celesta, and not one integer multiple among them."""
    y = _modal(261.626, [1, 2.756, 5.404, 8.933], [1, 0.55, 0.28, 0.14],
               [1.5, 1.0, 0.7, 0.5], seed=1)
    m = analyze_note(y, SR, Note(60, 100, 0.0, 2.0), 3.5)
    assert m.modal_ratio == pytest.approx([1.0, 2.756, 5.404, 8.933], rel=0.003)
    # Levels come back relative to the strongest mode, so the shape survives any
    # gain difference between two renders. They are the levels IN THE ANALYSIS
    # WINDOW rather than at the strike, so a mode that decays faster reads lower
    # than its initial amplitude — which is what makes the column comparable
    # between two renders measured over the same window.
    assert m.modal_db[0] == 0.0
    assert m.modal_db[1] > m.modal_db[2] > m.modal_db[3]
    assert -12.0 < m.modal_db[1] < -3.0


def test_the_harmonic_ladder_reports_that_it_found_nothing_on_a_bar():
    """The finding this exists for, and the one the -120 dB sentinel could not make.

    A bin that found no partial writes whatever the noise floor was, around
    -105 dB on a real render, which passes every `> -120` test there is. So a
    voice with no harmonic series scored a confident harmonic error made
    entirely of the difference between two noise floors.
    """
    y = _modal(261.626, [1, 2.756, 5.404, 8.933], [1, 0.55, 0.28, 0.14],
               [1.5, 1.0, 0.7, 0.5], seed=1)
    m = analyze_note(y, SR, Note(60, 100, 0.0, 2.0), 3.5)
    # Every bin is above the sentinel...
    assert all(v > -120.0 for v in m.harmonics_db)
    # ...and only the fundamental is a partial.
    assert m.ladder_partials <= 2
    assert sum(ladder_present(m.harmonics_db)) == m.ladder_partials


def test_two_noise_floors_no_longer_score_as_a_harmonic_difference():
    """The measurement that made `modes` necessary, as a regression test."""
    model = _modal(261.626, [1, 2.756, 5.404, 8.933], [1, 0.55, 0.28, 0.14],
                   [1.5, 1.0, 0.7, 0.5], seed=1)
    oracle = _modal(261.626, [1, 2.80, 5.50, 9.10], [1, 0.62, 0.24, 0.10],
                    [1.6, 1.1, 0.6, 0.45], seed=2)
    note = Note(60, 100, 0.0, 2.0)
    rows_m = [analyze_note(model, SR, note, 3.5).to_dict()]
    rows_o = [analyze_note(oracle, SR, note, 3.5).to_dict()]
    terms = loss_terms(rows_m, rows_o, n_harm=10)
    assert terms is not None
    # `harm` now says it measured almost nothing rather than producing a number.
    assert terms["harm_bins"] <= 1.0
    # And the term that CAN measure this reports a real difference: the modes are
    # 27 to 33 cents apart and their levels differ by a few dB.
    assert terms["modes"] > 0.5
    assert terms["modes_notes"] == 1.0


def test_a_model_whose_modes_match_scores_nothing_on_the_term():
    y = _modal(261.626, [1, 2.756, 5.404], [1, 0.55, 0.28], [1.5, 1.0, 0.7], seed=3)
    note = Note(60, 100, 0.0, 2.0)
    rows = [analyze_note(y, SR, note, 3.5).to_dict()]
    assert _modes_terms(rows, rows) == (0.0, 1)


def test_a_missing_mode_costs_more_than_a_mistuned_one():
    """Otherwise a fit can delete a partial it cannot place."""
    ref = {"modal_hz": [100.0, 276.0, 540.0], "modal_db": [0.0, -5.0, -11.0]}
    mistuned = {"modal_hz": [100.0, 290.0, 540.0], "modal_db": [0.0, -5.0, -11.0]}
    missing = {"modal_hz": [100.0, 540.0], "modal_db": [0.0, -11.0]}
    a, _ = _modes_terms([mistuned], [ref])
    b, _ = _modes_terms([missing], [ref])
    assert 0.0 < a < b


def test_modes_are_paired_nearest_first_rather_than_in_order():
    """One badly placed mode must not cascade into every pair after it."""
    pairs = _pair_modes([100.0, 400.0, 800.0], [100.0, 410.0, 810.0])
    assert sorted((i, j) for i, j, _ in pairs) == [(0, 0), (1, 1), (2, 2)]
    # Nothing within range of the second reference mode: it is an absence, not a
    # pairing with whatever happened to be nearest.
    assert [(i, j) for i, j, _ in _pair_modes([100.0], [100.0, 410.0])] == [(0, 0)]


def test_a_reference_with_no_modes_is_skipped_rather_than_matched():
    """A property of the reference, not of the voice — the `_absent_or` rule."""
    assert _modes_terms([{"modal_hz": [100.0], "modal_db": [0.0]}],
                        [{"modal_hz": [], "modal_db": []}]) == (0.0, 0)


# --------------------------------------------------------------------------- #
# Movement
# --------------------------------------------------------------------------- #
def _vibrato(f0: float, cents_pp: float, rate: float, seconds: float = 3.0,
             trem_db: float = 0.0) -> np.ndarray:
    n = int(SR * seconds)
    t = np.arange(n) / SR
    dev = 0.5 * (2.0 ** (cents_pp / 1200.0) - 1.0) * f0
    phase = 2 * np.pi * (f0 * t + (dev / (2 * np.pi * rate)) * np.sin(2 * np.pi * rate * t))
    am = 10.0 ** (0.5 * trem_db * np.sin(2 * np.pi * rate * t) / 20.0)
    return np.sin(phase) * am * np.exp(-t / 12.0)


def test_a_vibrato_is_measured_where_nothing_else_could_say_it():
    y = _vibrato(440.0, cents_pp=35.0, rate=5.5)
    got = modulation_note(y, SR, Note(69, 100, 0.0, 3.0), 440.0)
    assert got["vib_cents"] == pytest.approx(35.0, rel=0.2)
    assert got["vib_rate_hz"] == pytest.approx(5.5, abs=0.4)


def test_a_still_note_reports_no_movement_rather_than_no_measurement():
    n = int(SR * 3.0)
    t = np.arange(n) / SR
    got = modulation_note(np.sin(2 * np.pi * 440 * t), SR, Note(69, 100, 0.0, 3.0), 440.0)
    assert got["vib_cents"] is not None and got["vib_cents"] < 2.0
    assert got["trem_db"] is not None and got["trem_db"] < 0.5


def test_a_note_too_short_to_track_withholds_rather_than_reporting_zero():
    """Zero is this measurement's best value, so it must not also be its absent one."""
    n = int(SR * 0.2)
    t = np.arange(n) / SR
    got = modulation_note(np.sin(2 * np.pi * 440 * t), SR, Note(69, 100, 0.0, 0.2), 440.0)
    assert got["vib_cents"] is None


def test_the_movement_term_asks_for_vibrato_the_noise_term_never_could():
    """`tnr` charges the model only for being NOISIER, so a dead note is free there."""
    still = analyze_note(_vibrato(440.0, 0.0, 5.5), SR, Note(69, 100, 0.0, 3.0), 3.0).to_dict()
    moving = analyze_note(_vibrato(440.0, 40.0, 5.5), SR, Note(69, 100, 0.0, 3.0), 3.0).to_dict()
    charged, notes = _mod_terms([still], [moving])
    assert notes == 1 and charged > 1.0
    assert _mod_terms([moving], [moving])[0] < charged


def test_an_absent_movement_reading_on_the_model_side_is_charged_not_skipped():
    """A note too dead to track is the defect, not the absence of evidence."""
    ref = {"vib_cents": 30.0, "vib_rate_hz": 5.5, "trem_db": 0.0, "beat_db": 0.0,
           "f0_width_cents": 8.0}
    dead = {"vib_cents": None, "vib_rate_hz": None, "trem_db": None,
            "beat_db": None, "f0_width_cents": None}
    assert _mod_terms([dead], [ref])[0] > _mod_terms([ref], [ref])[0]


def test_a_unison_pair_is_wider_and_beats_where_one_string_does_neither():
    """The one property of an ensemble patch a single note can carry."""
    n = int(SR * 3.0)
    t = np.arange(n) / SR
    one = np.sin(2 * np.pi * 220.0 * t)
    pair = one + np.sin(2 * np.pi * 220.0 * 2 ** (6 / 1200.0) * t)
    a = analyze_note(one, SR, Note(57, 100, 0.0, 3.0), 3.0)
    b = analyze_note(pair, SR, Note(57, 100, 0.0, 3.0), 3.0)
    assert b.f0_width_cents > a.f0_width_cents
    # 6 cents at 220 Hz is a beat under 1 Hz, which is the band this watches.
    assert b.beat_db > a.beat_db


# --------------------------------------------------------------------------- #
# Audibility
# --------------------------------------------------------------------------- #
def test_a_weighting_matches_the_standard_at_its_reference_points():
    assert a_weight_db(1000.0) == pytest.approx(0.0, abs=0.05)
    assert a_weight_db(100.0) == pytest.approx(-19.1, abs=0.3)
    assert a_weight_db(31.5) == pytest.approx(-39.4, abs=0.5)


def test_a_partial_nobody_can_hear_still_counts_for_something():
    """Zero would license an arbitrarily loud model wherever the reference is quiet."""
    w = audibility_weights([1000.0] * 3, [0.0, -20.0, -90.0])
    assert w[0] > w[1] > w[2] > 0.0


def test_the_bottom_octave_no_longer_outvotes_the_partials_that_carry_its_timbre():
    w = audibility_weights([27.5, 220.0], [0.0, 0.0])
    assert w[0] < w[1]


def test_flat_weighting_restores_the_raw_difference():
    dark = {"harmonics_db": [0.0, -8.0], "f0_cents_err": 0.0, "tnr_db": 20.0,
            "sustain_slope_db_s": 0.0, "release_ms": 0.0, "attack_ms": 0.0}
    bright = {"harmonics_db": [0.0, -3.0], "f0_cents_err": 0.0, "tnr_db": 20.0,
              "sustain_slope_db_s": 0.0, "release_ms": 0.0, "attack_ms": 0.0}
    assert loss_terms([dark], [bright], n_harm=2, audibility=False)["harm"] == pytest.approx(5.0)
    assert loss_terms([dark], [bright], n_harm=2)["harm"] < 5.0


# --------------------------------------------------------------------------- #
# The attack, on a grid that can resolve one
# --------------------------------------------------------------------------- #
def test_a_struck_attack_is_resolved_rather_than_quantised():
    """Measured on the coarse grid, 0.5 ms and 5 ms both report the same number."""
    def struck(rise_ms: float) -> np.ndarray:
        n = int(SR * 2.5)
        t = np.arange(n) / SR
        env = (1 - np.exp(-t / (rise_ms / 1000.0))) * np.exp(-t / 1.5)
        return sum(np.sin(2 * np.pi * 261.6 * k * t) / k for k in range(1, 9)) * env

    coarse = [analyze_note(struck(r), SR, Note(60, 100, 0.0, 2.0), 2.5).attack_ms
              for r in (0.5, 2.0)]
    fine = [analyze_note(struck(r), SR, Note(60, 100, 0.0, 2.0), 2.5).attack_fine_ms
            for r in (0.5, 2.0)]
    assert coarse[0] == coarse[1] or abs(coarse[0] - coarse[1]) >= 5.0
    assert fine[1] > fine[0]
    assert fine[0] < 5.0


# --------------------------------------------------------------------------- #
# Drums
# --------------------------------------------------------------------------- #
def _tom(f0: float = 100.0, drop: float = 0.25, tau: float = 0.040,
         seconds: float = 2.0, seed: int = 0) -> np.ndarray:
    n = int(SR * seconds)
    t = np.arange(n) / SR
    rng = np.random.default_rng(seed)
    phase = np.cumsum(2 * np.pi * f0 * (1 + drop * np.exp(-t / tau)) / SR)
    y = np.zeros(n)
    for r, level, decay in zip([1, 1.59, 2.14, 2.30], [1, 0.5, 0.3, 0.2],
                               [0.45, 0.3, 0.22, 0.18]):
        y += level * np.sin(phase * r) * np.exp(-t / decay)
    return y + 0.05 * rng.standard_normal(n) * np.exp(-t / 0.01)


def test_a_drum_with_a_pitch_has_it_measured():
    """A 1/3-octave band is four semitones wide, so `band` cannot see this."""
    tone = hit_tone(_tom(f0=100.0), SR)
    assert tone["tone_f0_hz"] == pytest.approx(100.0, rel=0.03)
    assert tone["modal_ratio"][:4] == pytest.approx([1.0, 1.59, 2.14, 2.30], rel=0.01)


def test_a_tom_two_semitones_out_of_tune_is_charged_where_the_band_profile_shrugs():
    note = Note(45, 100, 0.0, 0.05)
    right = analyze_hit(_tom(f0=100.0), SR, note, 2.0).to_dict()
    flat = analyze_hit(_tom(f0=100.0 * 2 ** (-2 / 12)), SR, note, 2.0).to_dict()
    assert percussion_terms([flat], [right])["modes"] > 1.0
    # And the band profile barely moves, which is the point.
    band_shift = percussion_terms([flat], [right])["band"]
    band_same = percussion_terms([right], [right])["band"]
    assert band_shift - band_same < percussion_terms([flat], [right])["modes"] * 25.0


def test_the_strike_s_pitch_overshoot_is_measured():
    """`pitch_drop` is a patch field a fit can move, and nothing scored it."""
    static = pitch_drop(_tom(drop=0.0), SR, 100.0)
    falling = pitch_drop(_tom(drop=0.25), SR, 100.0)
    assert falling["pitch_drop_ratio"] > static["pitch_drop_ratio"]
    assert falling["pitch_drop_ratio"] > 1.1
    assert falling["pitch_drop_ms"] is not None


def test_a_cymbal_reports_no_pitch_rather_than_a_pitch_of_zero():
    n = int(SR * 2.0)
    t = np.arange(n) / SR
    rng = np.random.default_rng(7)
    wash = rng.standard_normal(n) * np.exp(-t / 1.2)
    got = pitch_drop(wash, SR, None)
    assert got["pitch_drop_ratio"] is None


def test_a_band_the_reference_floored_is_not_charged_to_the_model():
    """The capture cannot resolve it, so any charge levied there is fabricated.

    Measured on the shipped kit reference, the 12.5 kHz band is at the floor on
    98 % of rows and the 10 kHz band on 60 %, while a model with a real cymbal
    wash is nowhere near it — so those two bands charged up to 48 units of a
    term whose whole value is around a hundred, reducible only by dulling the
    model.
    """
    def hit(top_db: float) -> dict:
        bands = [-6.0] * 22 + [top_db] * 3
        return {"bands_db": bands, "band_decay_db_s": [None] * 8,
                "attack_ms": 3.0, "decay_ms": 200.0, "crest_db": 20.0,
                "note": 49, "velocity": 100}

    dead = hit(BAND_REFERENCE_FLOOR_DB - 1.0)
    bright = hit(-12.0)
    out = percussion_terms([bright], [dead])
    assert out["band"] == pytest.approx(0.0)
    assert out["band_bins"] == 22.0
    # A band the reference DOES have is still scored, in both directions.
    assert percussion_terms([bright], [hit(-30.0)])["band"] > 0.0


def test_a_decay_rate_is_compared_as_a_ratio_so_the_cap_means_the_same_everywhere():
    """A difference cannot be scaled: the same estimator returns -20 dB/s for a
    shell and -800 for the stick click on top of it, both correctly."""
    def hit(rates: list[float]) -> dict:
        return {"bands_db": [-6.0] * 25, "band_decay_db_s": rates,
                "attack_ms": 3.0, "decay_ms": 200.0, "crest_db": 20.0,
                "note": 38, "velocity": 100}

    slow_pair = percussion_terms([hit([-20.0] * 8)], [hit([-40.0] * 8)])["bdecay"]
    fast_pair = percussion_terms([hit([-400.0] * 8)], [hit([-800.0] * 8)])["bdecay"]
    # Both are a factor of two, so both cost the same — which a dB/s difference
    # could never do. The term is a sum over the eight octave bands, as `band`
    # is a sum over its twenty-five, so one octave each comes to eight.
    assert slow_pair == pytest.approx(fast_pair)
    assert slow_pair == pytest.approx(8.0)


def test_a_cymbal_is_given_room_to_decay_and_a_snare_is_not():
    """Every cymbal's measured decay sat within 40 ms of the old 1.8 s ceiling."""
    assert drum_gap_for((38,)) == 2.0
    assert drum_gap_for((49,)) > 6.0
    assert 49 in LONG_DECAY_DRUM_NOTES and 38 not in LONG_DECAY_DRUM_NOTES
    snare = build_pattern("drum", 0, notes=[38])
    crash = build_pattern("drum", 0, notes=[49])
    assert crash.notes[1].start - crash.notes[0].start > snare.notes[1].start - snare.notes[0].start


def test_the_mute_group_finally_gets_a_probe_that_fires_it():
    """42, 44 and 46 share an exclusive class and no single-hit probe ever hit it."""
    seq = build_pattern("drum-sequence", 0, sequence="hh-choke")
    assert seq.channel == 9 and seq.percussive
    assert {n.note for n in seq.notes} == {42, 44, 46}
    gaps = [b.start - a.start for a, b in zip(seq.notes, seq.notes[1:])]
    assert max(gaps) < 1.5     # close enough that the choke is what is heard
    # Nothing is an analysis note: every per-hit measurement assumes isolation.
    assert seq.analysis_notes == []


def test_a_drum_sequence_names_what_it_takes_rather_than_raising_a_typeerror():
    with pytest.raises(ValueError, match="has no notes axis"):
        build_pattern("drum-sequence", 0, notes=(38,))


# --------------------------------------------------------------------------- #
# Stereo
# --------------------------------------------------------------------------- #
def test_a_stereo_sum_stays_the_default_and_one_channel_is_offered():
    a = np.stack([np.array([1.0, 0.0, -1.0]), np.array([0.0, 1.0, 0.0])], axis=1)
    assert to_mono(a) == pytest.approx([0.5, 0.5, -0.5])
    assert to_mono(a, "left") == pytest.approx([1.0, 0.0, -1.0])
    assert to_mono(a, "loudest") == pytest.approx([1.0, 0.0, -1.0])
    with pytest.raises(ValueError, match="unknown mono mode"):
        to_mono(a, "middle")


def test_decorrelated_channels_are_reported_before_they_are_summed():
    n = 4096
    rng = np.random.default_rng(3)
    same = rng.standard_normal(n)
    assert channel_correlation(np.stack([same, same], axis=1)) == pytest.approx(1.0)
    apart = np.stack([same, rng.standard_normal(n)], axis=1)
    assert abs(channel_correlation(apart)) < 0.2
    assert channel_correlation(np.zeros(n)) is None


# --------------------------------------------------------------------------- #
# What the capture can measure, and what it cannot
# --------------------------------------------------------------------------- #
def _bandlimited_hit(cutoff_hz: float, tilt_db_per_octave: float, *,
                     seconds: float = 1.0, seed: int = 11) -> np.ndarray:
    """One instrument of a kit, through a chain that stops at `cutoff_hz`.

    The tilt is what makes it a different object from its neighbours: a kit's
    band profile separates a cowbell from a cabasa by tens of dB, and that
    separation is the signal `measure_band_edge` looks for the end of.
    """
    n = int(SR * seconds)
    rng = np.random.default_rng(seed)
    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    spec = np.fft.rfft(rng.standard_normal(n))
    octaves = np.log2(np.maximum(freqs, 20.0) / 1000.0)
    spec *= 10.0 ** (tilt_db_per_octave * octaves / 20.0)
    spec[freqs > cutoff_hz] = 0.0
    t = np.arange(n) / SR
    return np.fft.irfft(spec, n) * np.exp(-t / 0.25)


def _kit(cutoff_hz: float) -> list[dict]:
    """Ten instruments differing in spectral tilt, sharing one capture chain."""
    note = Note(38, 100, 0.0, 0.05)
    return [analyze_hit(_bandlimited_hit(cutoff_hz, tilt, seed=i), SR, note,
                        1.0).to_dict()
            for i, tilt in enumerate(range(-12, 13, 3))]


def test_a_capture_s_own_ceiling_is_measured_rather_than_assumed():
    """The rows say where the reference stops telling instruments apart."""
    edge = measure_band_edge(_kit(7000.0))
    assert edge is not None
    # The band centres straddling 7 kHz are 6300 and 8000, so either is the
    # honest answer; what must not happen is the analysis range being reported.
    assert edge <= 8000.0
    # A reference that carries its whole range says so by returning None, and
    # everything downstream then behaves exactly as it did before the edge.
    assert measure_band_edge(_kit(20000.0)) is None


def test_a_band_with_energy_but_no_separation_is_past_the_edge():
    """The criterion is discrimination, not level — an energy test cannot find this.

    57 % of `reference/drums.json` is still above the band floor at 8 kHz and
    36 % at 10 kHz, so a floor count puts the edge wherever its threshold was
    chosen. Written as band profiles rather than as rendered audio, because what
    is under test is the rule and the rule reads profiles: the top three bands
    here sit far above the floor on every row and read the SAME on every row,
    which is one transfer function and not ten instruments.
    """
    rng = np.random.default_rng(2)
    n_bands = len(THIRD_OCTAVE_CENTERS)
    rows = []
    for _ in range(12):
        bands = list(rng.uniform(-40.0, 0.0, n_bands - 3))
        rows.append({"bands_db": bands + [-20.0, -25.0, -30.0]})
    assert all(r["bands_db"][-1] > BAND_REFERENCE_FLOOR_DB for r in rows)
    assert measure_band_edge(rows) == THIRD_OCTAVE_CENTERS[n_bands - 4]


def test_too_few_rows_to_read_a_spread_from_reports_no_edge():
    """A pair of instruments differ or they do not; that is not a spread."""
    assert measure_band_edge(_kit(7000.0)[:3]) is None
    assert measure_band_edge([]) is None


def test_content_above_the_edge_does_not_move_the_bands_below_it():
    """The reason the edge cannot be left to the loss to skip.

    The band profile is normalised to its own loudest band. A model with a
    bright wash the reference could never have recorded takes that normalisation
    with it, and every band the reference CAN measure reads low by however far
    the wash stood above them — an error the loss then charges across the whole
    profile.
    """
    note = Note(49, 100, 0.0, 0.05)
    quiet_top = _bandlimited_hit(7000.0, 0.0, seed=3)
    n = len(quiet_top)
    t = np.arange(n) / SR
    rng = np.random.default_rng(4)
    # The same hit plus a loud 12.5 kHz band, well above a 7 kHz capture.
    wash = 6.0 * np.sin(2 * np.pi * 12500.0 * t) * np.exp(-t / 0.25)
    bright = quiet_top + wash + 1e-6 * rng.standard_normal(n)

    free = analyze_hit(bright, SR, note, 1.0).to_dict()["bands_db"]
    held = analyze_hit(bright, SR, note, 1.0, max_band_hz=8000.0).to_dict()["bands_db"]
    plain = analyze_hit(quiet_top, SR, note, 1.0, max_band_hz=8000.0).to_dict()["bands_db"]

    # Unbounded, the audible bands are dragged down by the inaudible one.
    assert min(free[:10]) < min(held[:10]) - 10.0
    # Bounded, they read the same as the hit without the wash at all.
    assert held[:10] == pytest.approx(plain[:10], abs=0.5)
    # And the band above the edge is reported as unmeasurable, which is what
    # `percussion_terms` already skips on.
    assert held[-1] <= BAND_REFERENCE_FLOOR_DB


def test_a_kick_hot_in_the_bottom_is_charged_as_a_region_not_as_one_band_of_25():
    """`band` averages 25 bands, so the loudest error in a kit barely moved it."""
    note = Note(36, 100, 0.0, 0.05)
    n = int(SR * 1.0)
    t = np.arange(n) / SR
    rng = np.random.default_rng(5)

    def kick(low_gain: float) -> np.ndarray:
        body = low_gain * np.sin(2 * np.pi * 55.0 * t) * np.exp(-t / 0.18)
        # The shell and the beater: what the bottom is heard against.
        shell = 0.6 * np.sin(2 * np.pi * 400.0 * t) * np.exp(-t / 0.08)
        beater = 0.3 * rng.standard_normal(n) * np.exp(-t / 0.010)
        return body + shell + beater

    ref = analyze_hit(kick(1.0), SR, note, 1.0).to_dict()
    hot = analyze_hit(kick(8.0), SR, note, 1.0).to_dict()   # +18 dB of bottom
    terms = percussion_terms([hot], [ref])
    same = percussion_terms([ref], [ref])
    assert same["lf"] == pytest.approx(0.0, abs=0.01)
    # Priced in dB of imbalance, so the 18 dB put in comes back out. The
    # dilution this term exists to undo is NOT asserted here: `band` spreads
    # the same evidence over however many bands carry content, and a hit built
    # from three components has almost none to spread over, so a comparison
    # against it on this fixture would be measuring the fixture.
    assert terms["lf"] == pytest.approx(18.0, abs=3.0)
    assert terms["lf_notes"] == 1.0


def test_the_low_end_is_measured_as_a_balance_so_a_gain_change_alone_is_not_it():
    """`level` is the term for loudness; `lf` must not double as a second one."""
    note = Note(36, 100, 0.0, 0.05)
    n = int(SR * 1.0)
    t = np.arange(n) / SR
    hit = (np.sin(2 * np.pi * 55.0 * t) * np.exp(-t / 0.18)
           + 0.6 * np.sin(2 * np.pi * 400.0 * t) * np.exp(-t / 0.08))
    quiet = analyze_hit(hit, SR, note, 1.0).to_dict()
    loud = analyze_hit(hit * 4.0, SR, note, 1.0).to_dict()
    # Same instrument, 12 dB louder. The balance between its bottom and its
    # body has not changed, so this term has nothing to say about it.
    assert percussion_terms([loud], [quiet])["lf"] == pytest.approx(0.0, abs=0.05)


# --------------------------------------------------------------------------- #
# A term that stops being measurable
# --------------------------------------------------------------------------- #
def _terms(**over) -> dict:
    row = {name: 0.0 for name in LOSS_TERMS}
    row.update({"modes_notes": 3.0, "harm_bins": 30.0, "mod_notes": 3.0,
                "stiff_notes": 3.0, "dyn_groups": 1.0, "comparable": 1.0})
    row.update(over)
    return row


def test_a_term_that_goes_unmeasurable_is_charged_rather_than_scored_perfect():
    """Every averaged term skips what it cannot use and divides by the rest.

    That is right until there is nothing left: the sum is then 0.0 over 0
    points, and 0.0 is the term's best possible score. A render whose modes
    vanished reads as the render that fixed the term, and a search takes that
    trade every time it is offered.
    """
    weights = LossWeights({"modes": 1.0})
    weights.calibrate(_terms(modes=4.0))
    assert weights.combine(_terms(modes=4.0)) == pytest.approx(1.0)
    # Genuinely better: fewer cents of error over the same three notes.
    assert weights.combine(_terms(modes=1.0, modes_notes=3.0)) < 1.0
    # Not better: no modes were found at all, so the term measured nothing.
    blind = weights.combine(_terms(modes=0.0, modes_notes=0.0))
    assert blind > 1.0


def test_a_term_that_never_had_data_is_not_charged_for_still_not_having_it():
    """A probe with no velocity axis cannot score `dyn`, and that is not a fault."""
    weights = LossWeights({"modes": 1.0, "dyn": 1.0})
    weights.calibrate(_terms(modes=4.0, dyn=0.0, dyn_groups=0.0))
    assert weights.combine(_terms(modes=4.0, dyn=0.0, dyn_groups=0.0)) == pytest.approx(1.0)


def test_the_guard_stays_out_of_the_way_of_a_raw_loss():
    """`--raw-loss` never calibrates, so there is no start point to compare against."""
    weights = LossWeights({"modes": 1.0})
    assert weights.combine(_terms(modes=0.0, modes_notes=0.0)) == pytest.approx(0.0)


# --------------------------------------------------------------------------- #
# The relations inside a kit
# --------------------------------------------------------------------------- #
#: The reference kit's six toms and libsonare's, in Hz, as they were measured.
#: The reference's are the shipped capture's medians; the model's key-track, so
#: they run over about half as much range.
REFERENCE_TOMS = (71.1, 79.3, 92.3, 97.0, 170.6, 184.1)
MODEL_TOMS = (100.0, 112.0, 125.0, 137.0, 152.0, 167.0)
TOM_NOTES = (41, 43, 45, 47, 48, 50)
KIT_GROUPS = {"toms": TOM_NOTES}


def _drum_row(note: int, *, f0: float | None = None, decay_ms: float = 300.0,
              centroid: float = 500.0, level: float = -30.0,
              capped: bool = False, velocity: int = 100) -> dict:
    """One percussion row carrying only what the kit relations read."""
    return {
        "note": note, "velocity": velocity,
        "bands_db": [-6.0] * 25, "band_decay_db_s": [None] * 8,
        "attack_ms": 3.0, "decay_ms": decay_ms, "decay_capped": capped,
        "crest_db": 20.0, "tone_f0_hz": f0, "centroid_hz": centroid,
        "peak_dbfs": level,
    }


def _tom_rows(pitches, **over) -> list[dict]:
    return [_drum_row(n, f0=f, **over) for n, f in zip(TOM_NOTES, pitches)]


def test_a_tom_series_half_as_wide_as_the_reference_s_is_charged_for_being_so():
    """No per-hit term can say this, and past a point none of them can even move.

    The six toms are one series of sizes. Every other percussion term scores a
    hit against its own reference row, so a compressed series reads as six
    separate moderate errors — and `modes` caps at 200 cents, which every one of
    these six exceeds, so widening the series changes that term by nothing at
    all until the members come back under the cap.
    """
    oracle = _tom_rows(REFERENCE_TOMS)
    narrow = percussion_terms(_tom_rows(MODEL_TOMS), oracle, groups=KIT_GROUPS)
    # A mean over the six members, in doublings. Four of them are within 0.03
    # and the two smallest toms are 0.6 out, which is where the whole missing
    # 0.6 doublings of spread ends up.
    assert narrow["kit"] == pytest.approx(0.23, abs=0.02)
    assert narrow["kit_notes"] == 6.0
    # Widening the model's series towards the reference's spread reduces it,
    # which is the gradient the capped per-hit terms do not have.
    wider = percussion_terms(
        _tom_rows([100.0 * (f / 116.0) ** 1.6 for f in MODEL_TOMS]), oracle,
        groups=KIT_GROUPS)
    assert wider["kit"] < narrow["kit"]
    # And a series with the reference's own shape is free, however far the whole
    # group sits from it — that part is `modes`, not this.
    matched = percussion_terms(_tom_rows([f * 1.5 for f in REFERENCE_TOMS]), oracle,
                               groups=KIT_GROUPS)
    assert matched["kit"] == pytest.approx(0.0, abs=1e-9)


def test_a_kit_relation_needs_no_note_map_because_a_family_is_a_set():
    """This capture lays its six toms out as 45, 47, 48, 50, 41, 43.

    The term compares sorted contrasts, so which key holds which drum cannot
    reach it — which is what lets a kit whose layout disagrees with GM's be
    scored at all without correcting one side into the other.
    """
    oracle = _tom_rows(REFERENCE_TOMS)
    straight = percussion_terms(_tom_rows(MODEL_TOMS), oracle, groups=KIT_GROUPS)
    shuffled = percussion_terms(_tom_rows(list(reversed(MODEL_TOMS))), oracle,
                                groups=KIT_GROUPS)
    assert shuffled["kit"] == pytest.approx(straight["kit"])
    assert shuffled["kit_notes"] == straight["kit_notes"]


def test_a_relation_the_reference_does_not_hold_is_not_one_to_reproduce():
    """The shipped kit's six toms are level-matched to within 1.2 dB.

    There is no level relation among them, so scoring one would charge the model
    for the reference's own strike-to-strike variation. Which relations a family
    holds is read off the rows rather than declared, for the same reason the
    capture's band edge is measured rather than chosen.
    """
    flat = [-29.4, -29.3, -29.4, -29.0, -28.5, -28.2]     # the shipped medians
    oracle = [_drum_row(n, f0=f, level=v)
              for n, f, v in zip(TOM_NOTES, REFERENCE_TOMS, flat)]
    # A model whose toms are spread over 20 dB, and identical to the reference
    # in every other relation.
    spread = [_drum_row(n, f0=f, level=v)
              for n, f, v in zip(TOM_NOTES, REFERENCE_TOMS,
                                 [-40.0, -36.0, -32.0, -28.0, -24.0, -20.0])]
    assert percussion_terms(spread, oracle, groups=KIT_GROUPS)["kit"] == pytest.approx(0.0)
    # The same 20 dB against a reference that DOES hold a level relation — the
    # hi-hat trio spans 10 dB — is charged.
    hats = {"hats": (42, 44, 46)}
    ref_hats = [_drum_row(n, level=v) for n, v in zip((42, 44, 46), (-34.8, -38.1, -27.9))]
    model_hats = [_drum_row(n, level=v) for n, v in zip((42, 44, 46), (-30.0, -30.0, -30.0))]
    assert percussion_terms(model_hats, ref_hats, groups=hats)["kit"] > 0.5


def test_a_decay_the_capture_ran_out_of_room_for_is_not_a_relation_to_match():
    """A capped reference decay is the analysis window, not the instrument."""
    notes = (49, 51, 57)
    ref = [_drum_row(n, decay_ms=d, capped=c)
           for n, d, c in zip(notes, (1618.0, 1800.0, 1958.0), (False, True, False))]
    model = [_drum_row(n, decay_ms=d) for n, d in zip(notes, (1618.0, 4000.0, 1958.0))]
    cymbals = {"cymbals": notes}
    # The capped member is dropped, so the model's wildly longer ride costs
    # nothing here — there was no reference number to compare it against.
    assert percussion_terms(model, ref, groups=cymbals)["kit"] == pytest.approx(0.0)
    # Uncap it and the same render is charged.
    honest = [_drum_row(n, decay_ms=d) for n, d in zip(notes, (1618.0, 1800.0, 1958.0))]
    assert percussion_terms(model, honest, groups=cymbals)["kit"] > 0.0


def test_a_probe_with_no_families_scores_no_kit_relations_rather_than_a_perfect_one():
    rows = _tom_rows(MODEL_TOMS)
    out = percussion_terms(rows, _tom_rows(REFERENCE_TOMS))
    assert out["kit"] == 0.0
    assert out["kit_notes"] == 0.0
    # One member is not a family, whatever the capture declared.
    one = percussion_terms(rows[:1], _tom_rows(REFERENCE_TOMS)[:1], groups=KIT_GROUPS)
    assert one["kit_notes"] == 0.0


def test_a_kit_term_that_stops_being_measurable_is_charged_like_every_other():
    """The likeliest of them all to go blind: a relation is dropped whenever the
    reference stops holding it or a member stops supplying a value, so a render
    whose toms lost their pitch takes every tom relation with it."""
    weights = LossWeights({"kit": 1.0})
    weights.calibrate(_terms(kit=0.4, kit_notes=18.0))
    assert weights.combine(_terms(kit=0.4, kit_notes=18.0)) == pytest.approx(1.0)
    assert weights.combine(_terms(kit=0.1, kit_notes=18.0)) < 1.0
    assert weights.combine(_terms(kit=0.0, kit_notes=0.0)) > 1.0
