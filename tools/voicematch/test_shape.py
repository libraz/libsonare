"""Tests for the spectrogram-shape comparison.

Everything here runs on synthesised signals. The captured corpus cannot be
committed -- a sample library's licence covers what is rendered from it -- so a
test that needed it would be a test that never runs. Synthesis is also the only
way to check an estimator against an answer that is known rather than measured:
a partial put at a chosen frequency with a chosen decay is the one case where
"the tracker reported 6 dB per second" can be graded.

The bed tests are the load-bearing ones. That estimator asserts something strong
about the reference -- that one source sits under every note -- and it is used to
delete data from the comparison, so a version that accepts a floor which is not
shared would quietly remove real signal.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shape import terms  # noqa: E402
from shape.bed import Bed  # noqa: E402
from shape.density import (  # noqa: E402
    RAYLEIGH_CV,
    band_snr_db,
    diffuse_floor,
    envelope_diffuseness,
    modal_density,
)
from shape import attack, struck  # noqa: E402
from shape.__main__ import holdout  # noqa: E402
from shape.search import split_velocities  # noqa: E402
from shape.loss import (  # noqa: E402
    DEFAULT_WEIGHTS, STRUCK_WEIGHTS, ShapeLoss, Terms,
)
from shape.partials import (  # noqa: E402
    Track, band_envelope, decay_db_s, fit_inharmonicity, harmonic_rows, note_hz,
)
from shape.probes import decay_bins, sustain_colour, tail_residue  # noqa: E402
from shape.render import read_overrides, write_overrides  # noqa: E402
from shape.search import split_notes  # noqa: E402
from shape.spectro import DEFAULT_SCALES, Spectro, rows_hz  # noqa: E402

SR = 48000


def synth(note, seconds=4.0, B=0.0, decay=(), level=(), sr=SR, n_partials=8,
          noise_db=None, extra=()):
    """A struck-string stand-in: inharmonic partials with per-partial decay."""
    t = np.arange(int(seconds * sr)) / sr
    f0 = note_hz(note)
    x = np.zeros_like(t)
    rng = np.random.default_rng(note)
    for k in range(1, n_partials + 1):
        f = k * f0 * np.sqrt(1.0 + B * k * k)
        if f > 0.45 * sr:
            break
        d = decay[k - 1] if k - 1 < len(decay) else 4.0 + 1.5 * k
        a = 10 ** ((level[k - 1] if k - 1 < len(level) else -6.0 * (k - 1)) / 20.0)
        x += a * np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28)) * 10 ** (-d * t / 20.0)
    for f, a, d in extra:
        x += a * np.sin(2 * np.pi * f * t) * 10 ** (-d * t / 20.0)
    if noise_db is not None:
        x += 10 ** (noise_db / 20.0) * rng.standard_normal(len(t))
    return x


# --- spectrogram ---------------------------------------------------------

def test_rows_are_geometric_and_ordered():
    hz = rows_hz(64, 27.5, 16000.0)
    assert hz[0] > 27.5 and hz[-1] < 16000.0
    ratios = hz[1:] / hz[:-1]
    assert np.allclose(ratios, ratios[0])


def test_a_tone_lands_on_its_own_row():
    sp = Spectro(seconds=1.0)
    t = np.arange(SR) / SR
    G = sp(np.sin(2 * np.pi * 1000.0 * t))[0]
    hz = sp.rows_hz(0)
    assert abs(hz[int(np.argmax(G[:, G.shape[1] // 2]))] / 1000.0 - 1.0) < 0.03


def test_a_signal_shorter_than_the_window_is_padded_not_truncated():
    sp = Spectro(seconds=2.0)
    G = sp(np.ones(SR // 2))
    assert G[0].shape[1] == (int(2.0 * SR) - 8192) // 2048 + 1


# --- partials ------------------------------------------------------------

def test_inharmonicity_is_recovered_from_a_stretched_series():
    B = 4e-4
    got = fit_inharmonicity(synth(60, B=B), note_hz(60))
    assert abs(got - B) < 1e-4


def test_a_pure_harmonic_series_fits_no_stiffness():
    assert fit_inharmonicity(synth(60, B=0.0), note_hz(60)) < 5e-5


def test_too_few_partials_returns_zero_rather_than_a_fit_through_noise():
    assert fit_inharmonicity(np.zeros(2 * SR), note_hz(60)) == 0.0


def test_harmonic_rows_mark_the_partials_and_not_the_gaps():
    hz = rows_hz(240, 27.5, 16000.0)
    m = harmonic_rows(hz, 440.0, 0.0)
    assert m[int(np.argmin(np.abs(hz - 880.0)))]
    assert not m[int(np.argmin(np.abs(hz - 660.0)))]


def test_decay_rate_matches_the_rate_it_was_synthesised_with():
    sig = synth(60, decay=(6.0,), n_partials=1)
    env = band_envelope(sig, note_hz(60), 60.0)
    assert abs(decay_db_s(env, 0.3, 2.0) + 6.0) < 0.6


def test_a_rate_is_withheld_when_the_partial_is_under_the_floor():
    sig = synth(60, decay=(40.0,), n_partials=1)
    env = band_envelope(sig, note_hz(60), 60.0)
    assert decay_db_s(env, 0.3, 3.0, floor_db=-20.0) is None


def test_a_partial_buried_in_noise_is_not_reported_as_clear():
    quiet = synth(60, decay=(40.0,), n_partials=2, noise_db=-40.0)
    track = Track(quiet, 60)
    assert not track.clear(1, 3.0)


# --- bed -----------------------------------------------------------------

def _floor(n_samples, tilt, seed, sr=SR):
    """Noise with a chosen spectral tilt: the thing a recording session leaves."""
    rng = np.random.default_rng(seed)
    S = np.fft.rfft(rng.standard_normal(n_samples))
    f = np.fft.rfftfreq(n_samples, 1.0 / sr)
    S *= (np.maximum(f, 20.0) / 1000.0) ** tilt
    x = np.fft.irfft(S, n_samples)
    return x / np.std(x)


def _bed_corpus(shared: bool, seconds=3.0):
    """Notes over one shared floor, or over a different floor each.

    The shared case also varies the floor's gain per note, because that is what
    a velocity-layered sampler does and the estimator has to align it out before
    the shapes can be compared at all.
    """
    sp = Spectro(seconds=seconds)
    n = int(seconds * SR)
    sigs = {}
    for i, note in enumerate(range(36, 36 + 12 * 4, 4)):
        f = _floor(n, -1.2, 7) if shared else _floor(n, -1.2 - 0.6 * i, 100 + i)
        gain = 10 ** ((-46 - 1.5 * i) / 20.0)
        sigs[(note, 24)] = synth(note, seconds=seconds, n_partials=6) + gain * f
    return sp, sigs


def test_one_floor_under_every_note_is_accepted():
    sp, sigs = _bed_corpus(shared=True)
    assert Bed.measure(sp, sigs).usable


def test_a_different_floor_per_note_is_refused():
    sp, sigs = _bed_corpus(shared=False)
    bed = Bed.measure(sp, sigs)
    assert not bed.usable
    assert bed.grid(sp, sp(next(iter(sigs.values())))[0], 0) is None


def test_subtracting_the_floor_lowers_the_gaps_and_leaves_the_partials():
    sp, sigs = _bed_corpus(shared=True)
    bed = Bed.measure(sp, sigs)
    key = next(iter(sigs))
    R = sp(sigs[key])[0]
    clean, _ = bed.clean(sp, R, 0)
    hz = sp.rows_hz(0)
    hm = harmonic_rows(hz, note_hz(key[0]), 0.0)
    col = R.shape[1] // 2
    # The loudest partials are untouched; the gaps between them come down. Rows
    # where this particular note happened to BE the across-note minimum are
    # subtracted to nothing, which is correct -- there the capture never held an
    # instrument level -- and is why the claim is about the strong partials and
    # the median gap rather than about every row.
    loud = hm & (R[:, col] > R[:, col].max() - 30.0)
    assert (clean[loud, col] > R[loud, col] - 1.0).all()
    assert np.median(clean[~hm, col]) < np.median(R[~hm, col]) - 3.0


def test_a_frozen_bed_round_trips(tmp_path):
    sp, sigs = _bed_corpus(shared=True)
    bed = Bed.measure(sp, sigs)
    bed.save(tmp_path / "b.npz")
    back = Bed.load(tmp_path / "b.npz", sp.scales)
    assert np.allclose(back.shapes[0], bed.shapes[0])
    assert back.usable == bed.usable


# --- terms ---------------------------------------------------------------

def test_a_slower_attack_reads_as_a_longer_rise():
    t = np.arange(int(0.5 * SR)) / SR
    tone = np.sin(2 * np.pi * 2000.0 * t)
    fast = tone * np.minimum(t / 0.002, 1.0)
    slow = tone * np.minimum(t / 0.05, 1.0)
    _, rf = terms.onset_stats(fast, start=0.0)
    _, rs = terms.onset_stats(slow, start=0.0)
    assert rs[2] > rf[2] * 3


def test_the_rise_is_not_fooled_by_a_beat_maximum():
    """Two partials a few hertz apart peak late; the rise is still immediate."""
    t = np.arange(int(0.3 * SR)) / SR
    beat = np.sin(2 * np.pi * 2000.0 * t) + np.sin(2 * np.pi * 2004.0 * t)
    _, rise = terms.onset_stats(beat * np.minimum(t / 0.001, 1.0), start=0.0)
    assert rise[2] < 10.0


def test_residue_ratio_separates_a_clean_series_from_a_stray_tone():
    sp = Spectro(seconds=4.0)
    hm = harmonic_rows(sp.rows_hz(0), note_hz(60), 0.0)
    clean = terms.residue_ratio(sp, sp(synth(60))[0], hm)
    rung = terms.residue_ratio(
        sp, sp(synth(60, extra=((1451.0, 2.0, 0.5),)))[0], hm)
    assert (rung > clean + 6.0).all()


def test_invariant_floor_finds_a_tone_that_answers_every_note():
    sp = Spectro(seconds=4.0)
    curves = []
    # 1451 Hz is not within a quarter tone of any partial of any of these
    # notes; a probe tone that collides with one is masked out for that note
    # and the across-note minimum then reports the collision, not the ring.
    for n in (48, 55, 60, 67):
        sig = synth(n, extra=((1451.0, 1.0, 0.5),))
        hm = harmonic_rows(sp.rows_hz(0), note_hz(n), 0.0)
        curves.append(terms.residue_curve(sp, sp(sig)[0], hm))
    floor = terms.invariant_floor(curves)
    hz = sp.rows_hz(0)
    assert abs(hz[int(np.argmax(floor))] / 1451.0 - 1.0) < 0.04


def test_invariant_floor_is_quiet_when_nothing_is_shared():
    sp = Spectro(seconds=4.0)
    curves = []
    for i, n in enumerate((48, 55, 60, 67)):
        sig = synth(n, extra=((1451.0 + 310.0 * i, 1.0, 0.5),))
        hm = harmonic_rows(sp.rows_hz(0), note_hz(n), 0.0)
        curves.append(terms.residue_curve(sp, sp(sig)[0], hm))
    shared = terms.invariant_floor(curves)
    assert shared.max() < -20.0


def test_release_is_measured_against_the_note_peak():
    t = np.arange(int(10.0 * SR)) / SR
    held = np.sin(2 * np.pi * 440.0 * t) * np.where(t < 8.0, 1.0, 0.001)
    pre, post = terms.release_stats(held, (7.6, 8.0), (9.2, 9.6))
    assert -4.0 < pre < 0.0
    assert post < -50.0


# --- loss ----------------------------------------------------------------

class _Fixed:
    """A signal source that answers from a table, standing in for a renderer."""

    def __init__(self, ref, mod):
        self.ref, self.mod = ref, mod

    def __call__(self, pairs, ov="", ref=False):
        src = self.ref if ref else self.mod
        return {tuple(p): src[tuple(p)] for p in pairs}


def _pair_tables(mod_fn):
    notes, vels = (48, 60, 72), (88,)
    ref = {(n, v): synth(n, seconds=10.0, B=3e-4) for n in notes for v in vels}
    mod = {k: mod_fn(k, v) for k, v in ref.items()}
    return notes, vels, ref, mod


def test_an_identical_render_scores_near_zero():
    notes, vels, ref, mod = _pair_tables(lambda k, v: v.copy())
    loss = ShapeLoss(signals=_Fixed(ref, mod), spectro=Spectro(seconds=10.0),
                     velocities=vels)
    assert loss.score(notes=notes).total < 0.5


def test_a_gain_offset_alone_is_removed():
    notes, vels, ref, mod = _pair_tables(lambda k, v: v * 4.0)
    loss = ShapeLoss(signals=_Fixed(ref, mod), spectro=Spectro(seconds=10.0),
                     velocities=vels)
    assert loss.score(notes=notes).total < 0.5
    assert 11.0 < loss.score(notes=notes).gain_db < 13.0


def test_an_added_ring_is_scored_worse_than_the_clean_render():
    notes, vels, ref, _ = _pair_tables(lambda k, v: v)
    clean = {k: v.copy() for k, v in ref.items()}
    rung = {k: v + synth(k[0], seconds=10.0, n_partials=0,
                         extra=((1451.0, 1.0, 0.4),)) for k, v in ref.items()}
    sp = Spectro(seconds=10.0)
    a = ShapeLoss(signals=_Fixed(ref, clean), spectro=sp, velocities=vels)
    b = ShapeLoss(signals=_Fixed(ref, rung), spectro=sp, velocities=vels)
    assert b.score(notes=notes).total > a.score(notes=notes).total + 0.5
    assert b.score(notes=notes).parts["invariance"] > 1.0


def test_the_recurrence_term_charges_a_ring_that_answers_every_note():
    """The term the other six could not be, on the shape that defeated them.

    One set of fixed frequencies added under every note, scaled so it is a large
    share of one note and a small share of another. That level pattern is what
    makes a row-wise minimum blind -- the bank has no weak appearance for a
    minimum to find -- and it is exactly the pattern a resonator bank produces
    against a keyboard whose notes are not equally loud.
    """
    notes, vels, ref, _ = _pair_tables(lambda k, v: v)
    sp = Spectro(seconds=10.0)
    fixed = ((1451.0, 1.0, 3.0), (2137.0, 0.8, 3.0), (3299.0, 0.6, 3.0))
    share = {48: 0.02, 60: 0.15, 72: 1.2}
    clean = {k: v.copy() for k, v in ref.items()}
    rung = {k: v + synth(k[0], seconds=10.0, n_partials=0, extra=fixed) * share[k[0]]
            for k, v in ref.items()}
    a = ShapeLoss(signals=_Fixed(ref, clean), spectro=sp, velocities=vels)
    b = ShapeLoss(signals=_Fixed(ref, rung), spectro=sp, velocities=vels)
    ra = a.score(notes=notes).parts["recurrence"]
    rb = b.score(notes=notes).parts["recurrence"]
    assert rb > ra + 2.0, (ra, rb)


def test_the_recurrence_term_charges_the_model_far_more_than_the_reference():
    """Asymmetric, which is what it needs to be -- and not strictly one-sided.

    The clip is one-sided, but the detector underneath is contextual: a peak is
    a peak relative to its own neighbourhood's median, so putting a ring into
    EITHER side changes what is detected around it on that side, and the
    reference having a ring the model lacks does not score exactly zero. What
    must hold is that the direction the term exists to punish costs clearly more
    than its opposite. Measured here it is about two and a half times; what is
    left on the other side is a mild bias against a model that lacks the
    instrument's own body ring, which `residue` already charges, so it is
    double-counted rather than wrong-signed. It is not a reward for planting
    resonances, and only that would break the term.
    """
    notes, vels, ref, _ = _pair_tables(lambda k, v: v)
    sp = Spectro(seconds=10.0)
    fixed = ((1451.0, 0.05, 3.0), (2137.0, 0.04, 3.0))
    plain = {k: v.copy() for k, v in ref.items()}
    rung = {k: v + synth(k[0], seconds=10.0, n_partials=0, extra=fixed)
            for k, v in ref.items()}
    model_rings = ShapeLoss(signals=_Fixed(plain, rung), spectro=sp,
                            velocities=vels).score(notes=notes).parts["recurrence"]
    ref_rings = ShapeLoss(signals=_Fixed(rung, plain), spectro=sp,
                          velocities=vels).score(notes=notes).parts["recurrence"]
    assert model_rings > 2.0 * ref_rings, (model_rings, ref_rings)


def test_a_render_missing_its_tail_is_scored_by_the_balance_term():
    """The defect the ear found and the cell comparison could not.

    A tail with its upper bands taken away is weighted at the cell floor
    everywhere it differs, so `spectrum` barely moves. `balance` is the term
    that charges for it, and it charges the same for too little as for too
    much.
    """
    notes, vels, ref, _ = _pair_tables(lambda k, v: v)
    sp = Spectro(seconds=10.0)

    def darken(sig):
        out = sig.copy()
        i = int(2.5 * SR)
        S = np.fft.rfft(out[i:])
        fr = np.fft.rfftfreq(len(out) - i, 1.0 / SR)
        S[fr > 2000.0] *= 10 ** (-40.0 / 20.0)
        out[i:] = np.fft.irfft(S, len(out) - i)
        return out

    clean = {k: v.copy() for k, v in ref.items()}
    dark = {k: darken(v) for k, v in ref.items()}
    a = ShapeLoss(signals=_Fixed(ref, clean), spectro=sp, velocities=vels)
    b = ShapeLoss(signals=_Fixed(ref, dark), spectro=sp, velocities=vels)
    assert b.score(notes=notes).parts["balance"] > \
        a.score(notes=notes).parts["balance"] + 2.0


def test_the_balance_term_is_blind_to_a_pure_gain():
    notes, vels, ref, mod = _pair_tables(lambda k, v: v * 8.0)
    loss = ShapeLoss(signals=_Fixed(ref, mod), spectro=Spectro(seconds=10.0),
                     velocities=vels)
    assert loss.score(notes=notes).parts["balance"] < 0.2


def test_the_parts_are_reported_separately():
    notes, vels, ref, mod = _pair_tables(lambda k, v: v.copy())
    loss = ShapeLoss(signals=_Fixed(ref, mod), spectro=Spectro(seconds=10.0),
                     velocities=vels)
    assert set(loss.score(notes=notes).parts) == {
        "spectrum", "onset", "residue", "invariance", "release", "balance",
        "recurrence"}


# --- probes --------------------------------------------------------------

def test_sustain_colour_rises_when_the_middle_partials_are_lifted():
    dull = synth(48, seconds=6.0, level=(0, -8, -20, -26, -32, -38, -44, -50))
    hard = synth(48, seconds=6.0, level=(0, -8, -6, -8, -10, -12, -14, -16))
    track = Track(dull, 48)
    assert sustain_colour(track, hard) > sustain_colour(track, dull) + 8.0


def test_tail_residue_sees_a_ring_the_string_cannot_account_for():
    sp = Spectro(seconds=8.0)
    clean = synth(60, seconds=8.0)
    rung = clean + synth(60, seconds=8.0, n_partials=0,
                         extra=((1451.0, 1.0, 0.4),))
    assert tail_residue(sp, rung, 60) > tail_residue(sp, clean, 60) + 10.0


def test_decay_bins_pool_across_notes_and_count_their_samples():
    prof = [[(100.0, -5.0), (900.0, -9.0)], [(120.0, -7.0)]]
    got = decay_bins(prof, bins=((60, 250), (700, 2000)))
    assert got[(60, 250)] == (-6.0, 2)
    assert got[(700, 2000)] == (-9.0, 1)


def test_an_empty_band_reports_no_samples_rather_than_a_number():
    got = decay_bins([[(100.0, -5.0)]], bins=((5000, 15500),))
    assert got[(5000, 15500)] == (None, 0)


# --- density -------------------------------------------------------------

SR = 48000
DUR = 3.5


def _noise(seed=7):
    return np.random.default_rng(seed).standard_normal(int(DUR * SR))


def _tones(freqs, seed=7):
    t = np.arange(int(DUR * SR)) / SR
    rng = np.random.default_rng(seed)
    return sum(np.sin(2 * np.pi * f * t + rng.uniform(0, 2 * np.pi)) for f in freqs)


def test_the_diffuse_floor_is_measured_and_is_nowhere_near_rayleighs_figure():
    """The whole reason the function exists: the constant does not apply here.

    A reading compared against 0.52 would call a field diffuse while it is still
    several times the spread a diffuse field actually produces through this
    path, which is the error the statistic was added to prevent.
    """
    for band in ((60, 125), (500, 1000), (2000, 4000)):
        floor = diffuse_floor(band, window=(0.0, DUR))
        assert 0.1 < floor < 0.5
        assert floor < RAYLEIGH_CV * 0.9


def test_noise_lands_on_the_measured_floor_and_two_tones_land_far_above_it():
    band = (500.0, 1000.0)
    floor = diffuse_floor(band, window=(0.0, DUR))
    # Same process as the floor, an independent draw: within a few percent.
    assert abs(envelope_diffuseness(_noise(), band, (0.0, DUR)) - floor) < 0.1
    # Two equal partials beating to a full null. Its value is arithmetic rather
    # than a threshold: the envelope is |cos|, whose spread over its own mean is
    # sqrt(pi^2/8 - 1) = 0.483, and it is the landmark the middle of the scale
    # is read against.
    beating = envelope_diffuseness(_tones([620.0, 623.0]), band, (0.0, DUR))
    assert abs(beating - 0.483) < 0.05


def test_a_decaying_tail_is_not_reported_as_sparse():
    """The confound the detrend exists to remove, and it is a large one.

    An exponential decay's envelope spreads wide with no beating in it at all,
    so before the detrend a band that merely decayed faster measured as though
    it held fewer resonances. Two renders being compared decay at different
    rates by construction -- that is usually the thing under test -- so the
    error does not average out, it takes whichever side decays slower.
    """
    band = (500.0, 1000.0)
    t = np.arange(int(DUR * SR)) / SR
    decaying = _tones([700.0]) * np.exp(-t / 1.2)
    # Three time constants across the window, and still nothing is beating.
    assert envelope_diffuseness(decaying, band, (0.0, DUR)) < 0.06
    # The same decay imposed on noise leaves the floor where it was.
    floor = diffuse_floor(band, window=(0.0, DUR))
    got = envelope_diffuseness(_noise() * np.exp(-t / 1.2), band, (0.0, DUR))
    assert abs(got - floor) < 0.1


def test_one_steady_tone_has_almost_no_envelope_spread():
    """The other end of the scale, and the one that catches a sign error.

    A single sinusoid's envelope is flat, so anything that leaks the carrier
    into the statistic shows up here as a number that is not near zero.
    """
    assert envelope_diffuseness(_tones([700.0]), (500.0, 1000.0), (0.0, DUR)) < 0.05


def test_a_note_that_stops_decaying_into_a_floor_is_caught_by_the_gate():
    """The failure the gate exists for, built to the shape the corpus has.

    A tail that decays for a while and then sits flat on a recorded floor gives
    the measurement window noise, and noise reads as the dense, diffuse,
    instrument-like texture the comparison is looking for. Judged against the
    same note's own post-release floor, which is what a sampler leaves behind.
    """
    band = (4000.0, 8000.0)
    n = int(10.0 * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(3)
    floor = rng.standard_normal(n) * 1e-3
    # Decays 60 dB in two seconds, then the floor is all that is left.
    plateaued = np.sin(2 * np.pi * 5500.0 * t) * np.exp(-t / 0.29) + floor
    assert band_snr_db(plateaued, band, (2.5, 6.0), (9.0, 10.0)) < 10.0
    # A tail that is still well above its own floor over the window is not.
    alive = np.sin(2 * np.pi * 5500.0 * t) * np.exp(-t / 3.0) + floor
    assert band_snr_db(alive, band, (2.5, 6.0), (9.0, 10.0)) > 10.0


def test_recurrence_finds_a_ring_that_answers_every_note():
    """The detector the row-wise minimum could not be.

    Three notes, each with its own partial series, plus one fixed set of
    resonances present under all of them at a level that is a large share of the
    quiet note and a small share of the loud one. That level pattern is exactly
    what defeats a minimum, and a count does not care about it.
    """
    from shape.density import bell_score, recurrence
    from shape.partials import note_hz
    sp = Spectro(seconds=DUR)
    # Halfway between the partials of the lowest note, whose grid the octaves
    # above it are subsets of. A resonance that lands ON a note's partial is
    # notched out for that note and cannot be counted there, which is a real
    # limit of the statistic and not something to hide in a fixture.
    fixed = [note_hz(48) * (k + 0.5) for k in (12, 17, 24, 29)]
    loud = {48: 1.0, 60: 0.3, 72: 0.05}
    with_bell, without = {}, {}
    for i, (n, amp) in enumerate(loud.items()):
        own = _tones([note_hz(n) * k for k in range(1, 9)], seed=i) * amp
        ring = _tones(fixed, seed=99) * 0.05
        with_bell[(n, 88)] = own + ring
        without[(n, 88)] = own
    hz, frac = recurrence(sp, with_bell, tuple(loud), 88, window=(0.2, 3.0))
    # A majority rather than all three, and the shortfall is the statistic
    # telling the truth: under the loudest note the ring sits far enough below
    # the surrounding partials that it does not stand clear of the local median.
    # Detection weakens exactly where the note's own energy crowds the row, so a
    # fixture tuned until every frequency scores 1.0 would be testing the
    # fixture. What must hold is that the ring is found in most notes and that
    # the pooled figure moves.
    for f in fixed:
        near = np.argmin(np.abs(hz - f))
        assert frac[near - 1:near + 2].max() >= 2.0 / 3.0, f
    assert bell_score(sp, with_bell, tuple(loud), 88, window=(0.2, 3.0)) > \
        bell_score(sp, without, tuple(loud), 88, window=(0.2, 3.0))


def test_recurrence_does_not_count_each_note_s_own_partials():
    """Otherwise every render reports itself as a bell.

    Notes an octave apart share most of their series, so without the notch the
    shared partials would recur in every note by construction and the statistic
    would be a description of the note grid.
    """
    from shape.density import recurrence
    from shape.partials import note_hz
    sp = Spectro(seconds=DUR)
    sigs = {(n, 88): _tones([note_hz(n) * k for k in range(1, 13)], seed=n)
            for n in (48, 60, 72)}
    _hz, frac = recurrence(sp, sigs, (48, 60, 72), 88, window=(0.2, 3.0))
    assert float(np.mean(frac >= 0.99)) < 0.02


def test_modal_density_counts_planted_resonances_and_not_the_played_partials():
    """Peaks per octave, with the note's own series notched out.

    The notch is the point of the estimator: a string put through it must not
    be counted as the structure's modes, or every render reports the density of
    whatever note it was asked about.
    """
    f0 = 261.6256  # C4, the note the signal is claimed to be
    partials = [f0 * k for k in range(1, 16)]
    planted = [432.0, 517.0, 655.0, 881.0]
    sig = _tones(partials + planted)
    got = modal_density(sig, 60, window=(0.0, DUR), sr=SR, bands=((250, 1000),))
    runs = got[(250, 1000)][0]
    # The four planted resonances, and none of the six partials in the band.
    assert 3 <= runs <= 6


def test_a_band_with_nothing_in_it_reports_zero_rather_than_noise():
    quiet = np.zeros(int(DUR * SR))
    got = modal_density(quiet, 60, window=(0.0, DUR), sr=SR, bands=((250, 1000),))
    assert got[(250, 1000)] == (0, 0.0)


# --- purity --------------------------------------------------------------

def _partials(note, count=12, b=0.0, seconds=DUR, decay=None, seed=1):
    """A synthesised note: `count` partials on the stiff-string grid."""
    from shape.partials import note_hz, partial_hz
    t = np.arange(int(seconds * SR)) / SR
    rng = np.random.default_rng(seed)
    out = np.zeros_like(t)
    for k in range(1, count + 1):
        f = partial_hz(note_hz(note), b, k)
        if f > 0.45 * SR:
            break
        env = np.exp(-t / decay) if decay else 1.0
        out += (np.sin(2 * np.pi * f * t + rng.uniform(0, 2 * np.pi)) / k) * env
    return out


def test_a_pure_partial_stack_is_all_string_and_noise_alone_is_none_of_it():
    from shape.purity import purity_db
    sp = Spectro(seconds=DUR)
    clean = purity_db(sp, _partials(60), 60, (0.2, 3.0))
    noisy = purity_db(sp, np.random.default_rng(2).standard_normal(int(DUR * SR)),
                      60, (0.2, 3.0))
    # The separation is the property; neither endpoint is. A synthesised stack
    # does not read as infinitely pure because the analysis rows are wider than
    # the mask, and noise does not read as zero because the mask covers a
    # note-dependent share of the band -- which is why every comparison built on
    # this is between two renders of the SAME note.
    assert clean - noisy > 8.0
    assert clean > noisy


def test_adding_noise_to_a_note_lowers_its_purity_monotonically():
    """The property the measure is used for: more junk, lower number.

    Monotone rather than a threshold, because the absolute value depends on the
    analysis band and on how many partials the note has, and a test pinned to
    one number would be pinned to this synthesis rather than to the measure.
    """
    from shape.purity import purity_db
    sp = Spectro(seconds=DUR)
    base = _partials(60)
    rms = float(np.sqrt(np.mean(base ** 2)))
    rng = np.random.default_rng(5)
    got = [purity_db(sp, base + rng.standard_normal(len(base)) * rms * a, 60,
                     (0.2, 3.0)) for a in (0.0, 0.05, 0.2, 0.8)]
    assert all(a > b for a, b in zip(got, got[1:])), got


def test_a_tail_sitting_on_its_own_floor_is_flagged_by_the_floor_share():
    """The check that keeps a model from being tuned to match hiss.

    A reference whose tail has decayed onto its session noise reads as richly
    non-harmonic, and matching that figure means generating noise. Priced by
    the same recording after its damper has landed.
    """
    from shape.purity import FLOOR_SHARE_LIMIT, floor_share
    sp = Spectro(seconds=10.0)
    t = np.arange(int(10.0 * SR)) / SR
    floor = np.random.default_rng(4).standard_normal(len(t)) * 3e-4
    dead = _partials(60, seconds=10.0, decay=0.35) + floor
    alive = _partials(60, seconds=10.0, decay=4.0) + floor
    assert floor_share(sp, dead, 60, (3.5, 6.5), (9.0, 10.0)) > FLOOR_SHARE_LIMIT
    assert floor_share(sp, alive, 60, (3.5, 6.5), (9.0, 10.0)) < FLOOR_SHARE_LIMIT


def test_the_profile_keeps_the_notes_rather_than_averaging_them():
    """Not a style point. The pooled figure changed sign with its convention on
    real data, which meant the notes disagreed, and the disagreement was the
    finding -- so the reduction is the caller's to choose."""
    from shape.purity import profile
    sp = Spectro(seconds=DUR)
    sigs = {(n, 88): _partials(n) for n in (48, 60, 72)}
    got = profile(sp, sigs, (48, 60, 72), 88,
                  windows=(("body", 0.2, 1.0), ("tail", 1.5, 3.0)))
    assert set(got) == {"body", "tail"}
    assert set(got["body"]) == {48, 60, 72}
    assert all(isinstance(v, float) for v in got["body"].values())


# --- takes ---------------------------------------------------------------

def _item(notes, cc=(), seconds=10.0):
    return {"id": "t", "meta": {"seconds": seconds, "notes": [
        {"note": n, "velocity": 88, "start": s, "duration": d} for n, s, d in notes],
        "cc": [list(c) for c in cc]}}


def test_the_pedal_window_sits_before_the_pedal_lifts_not_after():
    """The mistake this helper exists to prevent, in its original form.

    Between the last note-off and the pedal-up event nothing is being driven
    and everything sounding is resonance. A window on the other side of that
    event measures the dampers landing, which is a different mechanism and
    gives an answer with the wrong sign.
    """
    from shape.takes import window_for
    item = _item([(60, 0.5 + i * 0.8, 0.12) for i in range(8)],
                 cc=((0.2, 64, 127), (7.0, 64, 0)))
    lo, hi = window_for(item, "ringing")
    assert 6.2 <= lo < hi <= 7.0
    body = window_for(item, "body")
    assert body[0] >= 0.5 and body[1] > body[0]
    # The floor is the lead-in, not the end of the file: these renders stop well
    # before their files do, so a floor read at the end is exact silence and
    # every gate built on it passes unconditionally.
    floor = window_for(item, "floor")
    assert floor[0] == 0.0 and 0.0 < floor[1] <= 0.5


def test_a_take_with_no_pedal_ends_its_ringing_window_before_the_take_does():
    from shape.takes import window_for
    item = _item([(48, 0.3, 6.0), (52, 0.3, 6.0)])
    lo, hi = window_for(item, "ringing")
    assert 6.3 <= lo < hi <= 10.0


def test_a_phrase_that_changes_pedal_repeatedly_uses_the_last_change():
    """The window must not collapse onto a moment mid-phrase.

    A ballad changes pedal on every bass note, and the pedal-up that ends the
    ringing is the first one after the last note stops -- not the first one in
    the take, which sits where notes are still being played and produces a
    hundred-millisecond window in the middle of the music.
    """
    from shape.takes import window_for
    item = _item([(60, 0.4, 0.85), (72, 2.95, 1.3), (36, 5.45, 1.0)],
                 cc=((0.45, 64, 127), (2.05, 64, 0), (2.15, 64, 127),
                     (7.20, 64, 0)))
    lo, hi = window_for(item, "ringing")
    assert abs(lo - 6.55) < 1e-9 and hi == 7.20


def test_a_take_still_sounding_at_its_end_has_no_ringing_window():
    from shape.takes import window_for
    item = _item([(60, 0.3, 9.5)])
    with pytest.raises(ValueError, match="no window"):
        window_for(item, "ringing")


def test_a_take_without_a_schedule_says_so_instead_of_guessing():
    from shape.takes import window_for
    with pytest.raises(ValueError, match="schedule"):
        window_for({"id": "old", "meta": {"seconds": 10.0}}, "ringing")


def test_band_error_removes_one_gain_and_reports_the_tilt_that_is_left():
    """A model twice as loud but identically balanced must read as flat zero,
    and one that is level overall but tilted must read as the tilt."""
    from shape.takes import band_error
    t = np.arange(int(6.0 * SR)) / SR
    rng = np.random.default_rng(9)
    ref = sum(np.sin(2 * np.pi * f * t + rng.uniform(0, 2 * np.pi))
              for f in (90.0, 300.0, 1400.0, 5000.0))
    tracks = {"m": (ref * 2.0, SR), "r": (ref, SR)}
    vals, g = band_error(tracks, "m", "r", (0.5, 2.0), (2.0, 4.0), (0.0, 0.2),
                         snr_db=-300.0)
    assert abs(g - 6.02) < 0.2
    assert max(abs(v) for v in vals if v is not None) < 0.5


def test_band_error_drops_a_band_the_reference_leaves_on_its_own_floor():
    from shape.takes import band_error
    n = int(8.0 * SR)
    t = np.arange(n) / SR
    floor = np.random.default_rng(11).standard_normal(n) * 1e-4
    # Nothing is struck until 0.3 s, so the lead-in is the floor and nothing but
    # the floor -- which is the whole reason the gate reads it from there. In
    # the measurement window 300 Hz is still sounding and 5 kHz is long gone, so
    # one band is a reading of the instrument and the other of the noise.
    struck = np.clip(t - 0.3, 0.0, None)
    ref = (np.sin(2 * np.pi * 300.0 * t) * np.clip(1.0 - struck / 5.7, 0.0, 1.0)
           * (t >= 0.3)
           + np.sin(2 * np.pi * 5000.0 * t) * np.exp(-struck / 0.2) * (t >= 0.3)
           + floor)
    tracks = {"m": (ref, SR), "r": (ref, SR)}
    vals, _ = band_error(tracks, "m", "r", (0.4, 0.9), (2.0, 4.0), (0.0, 0.28))
    bands = [(30, 60), (60, 125), (125, 250), (250, 500), (500, 1000),
             (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000)]
    assert vals[bands.index((250, 500))] is not None
    assert vals[bands.index((4000, 8000))] is None
    assert vals[bands.index((4000, 8000))] is None


def test_relative_to_cancels_a_shared_error_in_how_a_note_decays():
    """Why the repeated-note take is read this way and not by raw level.

    Both sides are given a decay that is wrong by the same factor. The raw
    levels differ; the ratio against each side's own single strike does not,
    because the error is in both terms.
    """
    from shape.takes import relative_to
    t = np.arange(int(6.0 * SR)) / SR
    one = np.sin(2 * np.pi * 300.0 * t) * np.exp(-t / 2.0)
    slow = np.sin(2 * np.pi * 300.0 * t) * np.exp(-t / 4.0)
    eight = one * np.sqrt(8.0)
    eight_slow = slow * np.sqrt(8.0)
    a = relative_to({"s": (eight, SR)}, {"s": (one, SR)}, "s", (2.0, 3.0), (2.0, 3.0))
    b = relative_to({"s": (eight_slow, SR)}, {"s": (slow, SR)}, "s",
                    (2.0, 3.0), (2.0, 3.0))
    assert abs(a - 9.03) < 0.2 and abs(b - 9.03) < 0.2


# --- search plumbing -----------------------------------------------------

def test_overrides_round_trip_and_only_carry_what_changed():
    base = {"a.k": 1.0, "a.j": 2.0}
    assert write_overrides({"a.k": 1.0, "a.j": 3.0}, base) == "a.j=3.0"
    assert read_overrides("a.j=3.0,a.k=1.5") == {"a.j": 3.0, "a.k": 1.5}


def test_an_unchanged_state_writes_nothing():
    base = {"a.k": 1.0}
    assert write_overrides(dict(base), base) == ""


def test_the_two_note_sets_are_disjoint_and_both_span_the_range():
    notes = (21, 30, 42, 54, 66, 78, 90, 108)
    fit, hold = split_notes(notes)
    assert not set(fit) & set(hold)
    assert set(fit) | set(hold) == set(notes)
    # Neither set is a register: both reach into the bottom and the top half.
    for part in (fit, hold):
        assert min(part) < 55 and max(part) > 70


@pytest.mark.parametrize("note", (21, 60, 108))
def test_note_frequencies_follow_equal_temperament(note):
    assert abs(note_hz(note) / (440.0 * 2 ** ((note - 69) / 12)) - 1.0) < 1e-12


# --- admittance ----------------------------------------------------------

def _two_stage(note, count, fast_s, slow_s, split=0.5, seconds=6.0, seed=3):
    """A note whose every partial decays at two rates at once.

    `split` is the share of each partial's starting amplitude in the fast
    component, so the early window sees a mixture and the late window sees only
    the slow one -- which is what a measured piano partial does and what the two
    windows are there to separate.
    """
    from shape.partials import note_hz, partial_hz
    t = np.arange(int(seconds * SR)) / SR
    rng = np.random.default_rng(seed)
    out = np.zeros_like(t)
    for k in range(1, count + 1):
        f = partial_hz(note_hz(note), 0.0, k)
        if f > 0.45 * SR:
            break
        env = split * np.exp(-t / fast_s) + (1.0 - split) * np.exp(-t / slow_s)
        out += (np.sin(2 * np.pi * f * t + rng.uniform(0, 2 * np.pi)) / k) * env
    return out


def test_the_two_windows_separate_the_two_rates_they_were_given():
    from shape import admittance
    from shape.partials import Track
    sig = _two_stage(48, 8, fast_s=0.30, slow_s=6.0)
    got = admittance.rates(Track(sig, 48), sig)
    assert len(got) >= 4
    for _, prompt, after in got:
        # The synthesised rates are 8.686/tau dB/s: 29 fast, 1.4 slow. Neither is
        # asserted tightly -- the early window holds a mixture of the two by
        # construction, so its fit lands between them and the property being
        # tested is that the windows do not read the same number.
        assert prompt < -8.0
        assert after > -5.0
        assert prompt < after - 5.0


def test_a_single_rate_reads_the_same_in_both_windows():
    """The null. Without it a pair of windows fitted anywhere would look like a
    double decay, because a straight line through an exponential is the same
    line wherever it is fitted -- which is exactly why that has to be shown."""
    from shape import admittance
    from shape.partials import Track
    sig = _two_stage(48, 8, fast_s=2.0, slow_s=2.0)
    got = admittance.rates(Track(sig, 48), sig)
    assert len(got) >= 4
    for _, prompt, after in got:
        assert abs(prompt - after) < 3.0


def test_the_collapse_test_cannot_agree_with_itself_on_one_note():
    """One note's ladder walks the whole frequency axis on its own, and every
    bin it fills would then be a bin whose notes agree perfectly. A bin that
    only one note reached is dropped, so a single-note corpus reports nothing
    rather than reporting a curve."""
    from shape import admittance
    from shape.partials import Track
    sig = _two_stage(48, 12, fast_s=0.3, slow_s=6.0)
    one = admittance.rates(Track(sig, 48), sig)
    assert one
    assert admittance.collapse({(48, 88): one}) == []


def test_notes_sharing_a_rate_curve_collapse_and_notes_that_do_not_spread():
    """The claim the module exists to test, both ways round.

    Two notes given the SAME per-partial decay land on one curve, and the bins
    they share report a small spread. Give one of them a decay several times
    faster and the same bins have to widen -- otherwise the spread column would
    be reporting the binning rather than the agreement.
    """
    from shape import admittance
    from shape.partials import Track

    def prof(note, fast):
        sig = _two_stage(note, 16, fast_s=fast, slow_s=6.0)
        return admittance.rates(Track(sig, note), sig)

    agree = {(48, 88): prof(48, 0.4), (55, 88): prof(55, 0.4)}
    differ = {(48, 88): prof(48, 0.4), (55, 88): prof(55, 0.08)}
    shared = admittance.collapse(agree)
    assert shared, "two notes must share at least one bin for the test to mean anything"
    wide = {round(c): iqr for c, _, iqr, _, _, _ in admittance.collapse(differ)}
    for centre, _, iqr, notes, _, _ in shared:
        assert notes == 2
        assert iqr < 12.0
        assert wide.get(round(centre), 0.0) > iqr


def test_a_partial_the_model_let_die_is_counted_rather_than_dropped():
    """The floor a rate is refused against is the REFERENCE's.

    So a None on the model side is not a measurement failure — it is the model
    having gone quiet where the reference is still sounding, which is the defect
    and not a reason to stop looking at it. Dropping it removes the partial from
    the comparison, and what is left is a shorter table whose every row still
    agrees.
    """
    from shape import admittance
    from shape.partials import Track
    ref_sig = _two_stage(48, 14, fast_s=0.4, slow_s=6.0)
    # The same note with nothing above its fourth partial: a voice that dies in
    # the top of its range.
    model_sig = _two_stage(48, 4, fast_s=0.4, slow_s=6.0)
    track = Track(ref_sig, 48)
    reference = admittance.rates(track, ref_sig)
    dropped = admittance.rates(track, model_sig)
    kept = admittance.rates(track, model_sig, keep_unfittable=True)
    assert len(dropped) < len(reference)
    assert len(kept) >= len(reference)
    assert any(p[1] is None or p[2] is None for p in kept)
    # And the reference side keeps the drop, because there a refusal is a
    # property of the recording with nothing to compare against.
    assert all(p[1] is not None and p[2] is not None for p in reference)


def test_the_report_names_a_band_the_model_could_not_answer():
    """Intersecting the two sides lets the thing being measured narrow the
    comparison: the bands a model went quiet in leave its own `collapse`, leave
    the intersection, and leave the table. The reference decides which bands
    appear."""
    from shape import admittance
    ref = {(48, 88): _two_stage(48, 16, fast_s=0.4, slow_s=6.0),
           (55, 88): _two_stage(55, 16, fast_s=0.4, slow_s=6.0)}
    model = {key: _two_stage(key[0], 5, fast_s=0.4, slow_s=6.0) for key in ref}
    text = admittance.report(ref, model)
    assert "n/a" in text
    assert "the model did not at all" in text
    assert "quiet" in text
    # A run where the model answers everything says so, and prints no absence.
    same = admittance.report(ref, dict(ref))
    assert "n/a" not in same
    assert "the model did not at all" not in same


# --------------------------------------------------------------------------- #
# What each analysis scale can and cannot separate
# --------------------------------------------------------------------------- #
def _two_modes(hz_a: float, hz_b: float, seconds: float = 2.0) -> np.ndarray:
    t = np.arange(int(48000 * seconds)) / 48000.0
    return np.sin(2 * np.pi * hz_a * t) + np.sin(2 * np.pi * hz_b * t)


def _peaks_between(signal: np.ndarray, n_fft: int, lo: float, hi: float) -> list[float]:
    mag = np.abs(np.fft.rfft(signal[:n_fft] * np.hanning(n_fft)))
    freq = np.fft.rfftfreq(n_fft, 1.0 / 48000.0)
    band = (freq > lo) & (freq < hi)
    m, f = mag[band], freq[band]
    return [float(f[k]) for k in range(1, len(m) - 1)
            if m[k] > m[k - 1] and m[k] > m[k + 1] and m[k] > 0.4 * m.max()]


def test_the_longest_scale_separates_two_modes_a_few_hertz_apart():
    """Whether two components come apart is decided by the window length alone.

    A piano's low register is a field of individual resonances rather than a
    diffuse bottom end, and two of them can sit 3.58 Hz apart. Nothing about
    transform size or peak interpolation reaches that: a finer grid places one
    peak more precisely, it does not turn one peak into two. 3.58 Hz needs at
    least 0.28 s of signal, which 8192 samples (0.17 s) does not have.
    """
    signal = _two_modes(100.0, 103.58)
    scales = {n_fft for n_fft, _, _ in DEFAULT_SCALES}
    assert 32768 in scales, "the geometry must carry a window longer than 0.28 s"

    assert len(_peaks_between(signal, 8192, 90.0, 115.0)) == 1
    long_peaks = _peaks_between(signal, 32768, 90.0, 115.0)
    assert len(long_peaks) == 2
    assert long_peaks[0] == pytest.approx(100.0, abs=1.5)
    assert long_peaks[1] == pytest.approx(103.58, abs=1.5)


def test_the_added_scale_leaves_the_existing_ones_where_they_were():
    """Consumers index scales by number, so a new one goes on the end."""
    assert DEFAULT_SCALES[0][0] == 8192
    assert DEFAULT_SCALES[1][0] == 1024


def test_a_noise_bed_measured_over_fewer_scales_is_refused_by_name(tmp_path):
    """A bed belongs to the geometry it was measured on, and says so.

    Adding a scale invalidates every cached bed. Left to fall through, that
    surfaces as a KeyError on an array called `shape2`, which does not say that
    the fix is to measure it again.
    """
    path = tmp_path / "bed.npz"
    np.savez(path, agreement=1.0, shape0=np.zeros(4), anchor0=np.zeros(4))
    with pytest.raises(ValueError, match="measure it again"):
        Bed.load(path, scales=DEFAULT_SCALES)


# --------------------------------------------------------------- phrase takes

def _burst(sr=SR, seconds=6.0, period=0.1, body=0.02, spike=0.5):
    """A transient every `period` seconds over a steady bed of amplitude `body`.

    Dense on purpose: the 95th percentile only reaches the transients when
    enough columns hold one, which is a property of the statistic and not of
    this helper -- a take with four events in twelve seconds reports its body as
    its peak. The phrase takes this measures are dense in exactly this way.
    """
    n = int(seconds * sr)
    x = body * np.sin(2 * np.pi * 220.0 * np.arange(n) / sr)
    for k in range(int(seconds / period)):
        a = int(k * period * sr)
        env = np.exp(-np.arange(min(int(0.05 * sr), n - a)) / (0.01 * sr))
        x[a:a + env.size] += spike * env
    return x


def test_a_level_offset_moves_peak_body_and_floor_together():
    """The measurement that separates "louder" from "denser".

    A version scaled by a constant is the same envelope at a different level, so
    every percentile has to move by that constant and the interval between them
    must not move at all. If `spike-body` drifted here, an output-level error
    would read as a difference in envelope shape -- which is exactly the reading
    the audition page produces, and the reason this measurement exists.
    """
    from shape.takes import drawn

    x = _burst()
    win = (0.0, 6.0)
    a = drawn(x, SR, win)
    b = drawn(x * 10 ** (-8.0 / 20.0), SR, win)
    for lo, hi in zip(a, b):
        assert lo - hi == pytest.approx(8.0, abs=0.05)
    assert (a[0] - a[1]) == pytest.approx(b[0] - b[1], abs=0.05)


def test_a_denser_body_moves_the_body_far_more_than_the_peak():
    """The other half: the same transients over a louder bed.

    The discriminator is not that the peak is unmoved -- a bed loud enough to
    matter adds to the transient it sits under, so it does move it a little --
    but that the body moves by an order of magnitude more, which closes the
    interval. That pair, body up and interval closed, is what "the wave is
    filled in" means, and it is a different reading from a level offset, where
    the two move together and the interval does not move at all.
    """
    from shape.takes import drawn

    thin = drawn(_burst(body=0.02), SR, (0.0, 6.0))
    dense = drawn(_burst(body=0.2), SR, (0.0, 6.0))
    peak_shift = dense[0] - thin[0]
    body_shift = dense[1] - thin[1]
    assert body_shift > 15.0
    assert body_shift > 5.0 * peak_shift
    assert (dense[0] - dense[1]) < (thin[0] - thin[1]) - 12.0


def test_digital_silence_is_not_a_level():
    """A column of zeros must not come back as a sentinel two rows can subtract."""
    from shape.takes import drawn

    x = np.zeros(int(2.0 * SR))
    x[:int(0.1 * SR)] = 0.5
    hi, mid, lo = drawn(x, SR, (0.0, 2.0))
    assert hi == hi                       # the burst is a real level
    assert lo != lo and mid != mid        # the silence is not


def test_a_variant_is_not_mistaken_for_the_reference():
    """`--variant` renders sit beside the references and no key tells them apart.

    Answering "whichever source is not `model`" picks the first variant, and
    every table then reads as a comparison against a reference when it is a
    comparison against the model under different constants.
    """
    from shape.takes import pick_references

    roled = {"sources": {
        "model": {"role": "model"},
        "cand_a": {"role": "model"},
        "grand-227": {"role": "reference"},
        "grand-290": {"role": "reference"},
    }}
    assert pick_references(roled) == ["grand-227", "grand-290"]

    # No roles and one other source: unambiguous, and the old pages look like this.
    assert pick_references({"sources": {"model": {}, "kit-a": {}}}) == ["kit-a"]

    # No roles and several: refused rather than guessed.
    with pytest.raises(SystemExit, match="role=reference"):
        pick_references({"sources": {"model": {}, "cand_a": {}, "grand-227": {}}})

    # Named explicitly, roles or not.
    assert pick_references({"sources": {"model": {}, "a": {}, "b": {}}},
                           "b,a") == ["b", "a"]
    with pytest.raises(SystemExit, match="no source"):
        pick_references({"sources": {"model": {}, "a": {}}}, "nope")


def test_the_reference_median_is_not_one_reference():
    """Three captured instruments, and one of them 7 dB off the other two.

    The report is against the median for this reason: reading the model against
    a single reference that happens to be first in the manifest moved its
    transient-to-body figure by more than 10 dB on a real take.
    """
    from shape.takes import envelope_report

    tracks = {
        "model": (_burst(body=0.02, spike=0.5), SR),
        "ref-a": (_burst(body=0.02, spike=0.05), SR),
        "ref-b": (_burst(body=0.2, spike=0.05), SR),   # the odd one out
        "ref-c": (_burst(body=0.02, spike=0.05), SR),
    }
    text = envelope_report(tracks, ["ref-a", "ref-b", "ref-c"], (0.0, 6.0))
    assert "reference median" in text
    assert "(reference spread)" in text
    median_row = next(r for r in text.splitlines() if "reference median" in r)
    a_row = next(r for r in text.splitlines() if r.strip().startswith("ref-a"))
    # The median follows the two that agree, not the outlier.
    assert float(median_row.split()[3]) == pytest.approx(float(a_row.split()[2]), abs=0.1)


# ---------------------------------------------------------------------------
# struck pieces: windows from the piece, and the two terms a plate needs


def _hit(t60_s, sr=48000, seconds=3.0, seed=7, hf_t60=None):
    """A decaying noise burst, optionally with its own decay rate up top."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(seconds * sr)) / sr
    lo = rng.standard_normal(len(t)) * np.exp(-6.907755 * t / t60_s)
    if hf_t60 is None:
        return lo
    # A second, brighter layer with a decay of its own, so "the top dies before
    # the body does" can be built on purpose rather than hoped for.
    hi = rng.standard_normal(len(t)) * np.exp(-6.907755 * t / hf_t60)
    hi = np.diff(np.concatenate([[0.0], hi]))  # crude high-pass
    return lo + 4.0 * hi


def test_a_pieces_windows_follow_its_own_decay_and_not_the_clock():
    short = struck.windows(_hit(0.2), 48000)
    long_ = struck.windows(_hit(3.0), 48000)
    assert short[1][1] < long_[1][1] / 3.0
    # The aftersound window has to start after the body ends, on both.
    for body, late in (short, long_):
        assert late[0] == body[1] and late[1] > late[0]


def test_a_decay_mark_is_where_the_hit_reached_that_level():
    t20, t60 = struck.decay_marks(_hit(1.0), 48000)
    assert 0.30 < t20 < 0.38          # 20 dB down at a third of the t60
    assert 0.90 < t60 < 1.10


def test_a_floor_cannot_be_read_out_of_digital_padding():
    sr = 48000
    padded = np.concatenate([_hit(0.2, seconds=0.6), np.zeros(sr * 3)])
    win = struck.floor_window(padded, sr)
    # It must land inside what was recorded, never in the zeros after it.
    assert win is not None and win[1] <= 0.6 + 1e-6


def test_a_band_that_could_not_be_read_is_reported_unusable_not_zero():
    # Nothing but padding: no floor to measure against, so no band is readable.
    values, usable = struck.mode_count(np.zeros(48000), (0.0, 0.5), 48000)
    assert not usable.any()
    assert not values.any()


def _field(n, band=(2000, 4000), seconds=3.0, sr=48000, seed=3, t60=None):
    """n partials at random frequencies inside one band, optionally decaying."""
    t = np.arange(int(seconds * sr)) / sr
    r = np.random.default_rng(seed)
    x = sum(np.sin(2 * np.pi * f * t + r.uniform(0, 6.28))
            for f in r.uniform(band[0], band[1], n))
    return x if t60 is None else x * np.exp(-6.907755 * t / t60)


def test_the_density_estimator_is_graded_against_fields_it_knows_the_size_of():
    """The count has to rise with the number of things ringing, and it has to
    keep doing so when they are decaying -- a statistic that moves with the
    decay instead is measuring the envelope and not the texture."""
    sr, band, win = 48000, (2000, 4000), (0.30, 0.60)
    counts = [modal_density(_field(n), 0, window=win, sr=sr, bands=(band,),
                            notch=False)[band][0]
              for n in (2, 8, 32, 128)]
    assert counts == sorted(counts) and counts[0] < counts[-1] / 8
    # White noise is not a resolvable field, and reads below the dense end.
    nz = modal_density(np.random.default_rng(11).standard_normal(int(3.0 * sr)), 0,
                       window=win, sr=sr, bands=(band,), notch=False)[band][0]
    assert nz < counts[-1]
    # Decay must not move it: the same fields, given a 1.5 s t60.
    decayed = [modal_density(_field(n, t60=1.5), 0, window=win, sr=sr, bands=(band,),
                             notch=False)[band][0]
               for n in (2, 8, 32, 128)]
    for a, b in zip(counts, decayed):
        assert abs(a - b) <= max(2, 0.1 * a)


def test_an_unpitched_count_cannot_be_had_by_passing_a_nominal_note():
    """The notch is uncapped, so a low note masks the whole upper spectrum and
    the count comes back zero for a signal full of resonances. That is why the
    opt-out exists rather than a convention of passing note 0."""
    sr, band, win = 48000, (2000, 4000), (0.30, 0.60)
    sig = _field(32)
    assert modal_density(sig, 21, window=win, sr=sr, bands=(band,))[band][0] == 0
    assert modal_density(sig, 21, window=win, sr=sr, bands=(band,),
                         notch=False)[band][0] > 8


def test_the_texture_window_is_a_fixed_length_inside_the_aftersound():
    short = struck.texture_window((0.1, 0.2))
    long_ = struck.texture_window((0.5, 9.0))
    assert short[0] == 0.1 and long_[0] == 0.5
    assert long_[1] - long_[0] == pytest.approx(struck.DENSITY_SPAN_S)
    assert short[1] <= 0.2 + 1e-9


def test_prompt_late_sees_a_top_that_belongs_to_the_strike_alone():
    sr = 48000
    body, late = (0.0, 0.1), (0.1, 0.8)
    t = np.arange(int(1.5 * sr)) / sr
    # A low body, not white noise: white noise is flat to Nyquist and puts most
    # of its power in the top octave, which buries the layer under test in the
    # very bands the test is about.
    lo = _field(40, band=(80, 800), seconds=1.5, sr=sr, seed=5) \
        * np.exp(-6.907755 * t / 1.0)
    top = _field(24, band=(3000, 6000), seconds=1.5, sr=sr, seed=9)
    even = lo + top * np.exp(-6.907755 * t / 1.0)
    fading = lo + top * np.exp(-6.907755 * t / 0.05)
    ev, eok = struck.prompt_late(even, body, late, sr)
    fv, fok = struck.prompt_late(fading, body, late, sr)
    ok = eok & fok
    high = np.array([b[0] >= 2000 for b in struck.STRUCK_BANDS]) & ok
    assert high.any()
    # The piece whose top belongs to the strike reads further positive up there.
    assert fv[high].mean() > ev[high].mean() + 3.0


def test_a_struck_capture_is_scored_on_different_terms_than_a_played_one():
    assert "residue" in DEFAULT_WEIGHTS and "release" in DEFAULT_WEIGHTS
    assert "residue" not in STRUCK_WEIGHTS and "release" not in STRUCK_WEIGHTS
    assert {"density", "prompt"} <= set(STRUCK_WEIGHTS)
    # The four that carry over need no partial series and must be kept.
    assert {"spectrum", "onset", "balance", "recurrence"} <= set(STRUCK_WEIGHTS)


def test_an_unscored_term_is_named_rather_than_counted_as_a_match():
    t = Terms(total=1.0, parts={"spectrum": 1.0}, per_note={}, gain_db=0.0,
              unscored=("density",))
    assert "unscored" in str(t) and "density" in str(t)


def test_a_kit_holds_out_a_dynamic_because_a_held_out_note_is_another_patch():
    fit_v, hold_v = split_velocities((64, 88, 100, 112, 127))
    assert set(fit_v) & set(hold_v) == set()
    assert fit_v and hold_v
    # Alternating rather than split in half, so neither set is one end of the
    # range: a fit shown only soft layers and held out on loud ones is asked to
    # extrapolate, which is a different question.
    assert fit_v == (64, 100, 127) and hold_v == (88, 112)


def test_the_holdout_axis_follows_whether_the_capture_is_pitched():
    class FakeLoss:
        pitched = True
        velocities = (64, 88, 100, 112)
    played = holdout(FakeLoss(), (60, 62, 64, 66))
    assert played[0] != played[1] and played[2] is None

    loss = ShapeLoss(signals=None, pitched=False, velocities=(64, 88, 100, 112))
    notes = (42, 44, 46)
    fit_notes, hold_notes, hold_loss = holdout(loss, notes)
    # Every piece is measured on both sides; what is held back is the dynamic.
    assert fit_notes == notes and hold_notes == notes
    assert hold_loss is not None
    assert set(loss.velocities) & set(hold_loss.velocities) == set()
    assert hold_loss.pitched is False


class _SaturatingLoss:
    """A loss where three moves are worth a decibel together and nothing apart.

    `a`, `b` and `c` share one saturating mechanism: the first of them buys
    almost all of the improvement and the other two buy the remainder, so
    reverting any one of the three in the final state costs a hundredth of a
    decibel while reverting all three costs 0.99. `d` does nothing at all. This
    is the arrangement leave-one-out pricing cannot see, and it is not
    hypothetical -- it is what a hi-hat fit's eighteen dropped moves were.
    """

    #: Improvement bought by 0, 1, 2 and 3 of the saturating group.
    CURVE = (0.0, 0.96, 0.98, 0.99)

    def score(self, ov, notes=()):
        moved = {p.split("=")[0] for p in ov.split(",") if p}
        total = 10.0 - self.CURVE[len(moved & {"a", "b", "c"})]
        return Terms(total=total, parts={}, per_note={}, gain_db=0.0)


def test_prune_loosens_until_the_kept_set_holds_what_the_full_set_held():
    from shape.search import prune

    base = {"a": 1.0, "b": 1.0, "c": 1.0, "d": 1.0}
    moves = {k: 2.0 for k in base}
    loss = _SaturatingLoss()
    kept, report = prune(loss, base, moves, (60,), (62,), workers=2)

    # Every individual contribution is under the strictest threshold, so the
    # first rung drops all four moves and loses 0.99 dB doing it.
    assert all(dh <= 0.02 for _, dh in report["contributions"].values())
    assert report["attempts"][0]["kept"] == 0
    assert report["attempts"][0]["hold"] > report["before"]["hold"] + 0.02

    # The rung that wins keeps the three that share the mechanism and still
    # drops the one that carries nothing, and it gives up no hold-out at all.
    assert set(kept) == {"a", "b", "c"}
    assert report["keep_db"] == 0.005
    assert report["after"]["hold"] <= report["before"]["hold"] + 0.02


def test_prune_keeps_every_move_when_no_smaller_set_holds():
    from shape.search import PRUNE_LADDER, prune

    class Redundant:
        """Two moves that do the same job, so either one alone is enough.

        Leave-one-out prices both at exactly zero -- reverting either leaves the
        other doing the work -- and every threshold in the ladder drops a move
        worth zero. Only the last rung, which keeps everything, survives the
        check, and that is what makes it a rung rather than a fallback nobody
        reaches.
        """

        def score(self, ov, notes=()):
            moved = {p.split("=")[0] for p in ov.split(",") if p}
            return Terms(total=10.0 - bool(moved & {"a", "b"}),
                         parts={}, per_note={}, gain_db=0.0)

    base = {"a": 1.0, "b": 1.0}
    moves = {k: 2.0 for k in base}
    kept, report = prune(Redundant(), base, moves, (60,), (62,), workers=2)
    assert all(dh == 0.0 for _, dh in report["contributions"].values())
    assert set(kept) == set(moves) and report["keep_db"] is None
    assert PRUNE_LADDER[-1] is None


def test_a_faded_reference_does_not_drag_its_silence_into_the_window():
    """A source that stops decaying ends the window, whatever the schedule says.

    A sampled instrument reaches the end of its sample and is faded out, and the
    file then runs on in silence. Measuring to the schedule averages one side
    over its decay and the other over a fade plus a second of nothing, and the
    two sides hold different amounts of nothing.
    """
    from shape.takes import usable_until
    n = int(8.0 * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(5)
    hiss = rng.standard_normal(n) * 3e-5
    tone = np.sin(2 * np.pi * 300.0 * t) * np.exp(-t / 3.0) * (t >= 0.3) + hiss
    faded = tone.copy()
    faded[int(5.0 * SR):] = 0.0
    assert abs(usable_until(faded, SR, (0.0, 0.28), 1.0) - 5.0) < 0.15
    # The same signal left to decay on its own is measured to where it reaches
    # its own floor, not to a fixed margin under its body: a tail forty-five
    # decibels down is an ordinary decay and cutting there would end the window
    # on whichever voice decays fastest.
    assert usable_until(tone, SR, (0.0, 0.28), 1.0) > 6.0


def test_buildup_reads_accumulation_that_a_fitted_gain_hides():
    """Two takes matched in level can still differ in what they piled up.

    `band_error` removes one gain per take, so a voice that sums eight strikes
    where the instrument does not comes out looking the same as one that gets it
    right. Measuring each source against ITS OWN plainest take is what shows it.
    """
    from shape.takes import band_buildup
    n = int(6.0 * SR)
    t = np.arange(n) / SR
    one = np.sin(2 * np.pi * 40.0 * t) * np.exp(-t / 2.0) * (t >= 0.2)
    many = one * 4.0                       # four times the tail, same shape
    base = {"m": (one, SR)}
    vals = band_buildup({"m": (many, SR)}, base, "m", (2.0, 4.0), (2.0, 4.0),
                        (0.0, 0.18), (0.0, 0.18))
    assert abs(vals[0] - 12.04) < 0.3
    same = band_buildup({"m": (one, SR)}, base, "m", (2.0, 4.0), (2.0, 4.0),
                        (0.0, 0.18), (0.0, 0.18))
    assert abs(same[0]) < 1e-6


def test_the_plainest_take_is_chosen_from_the_schedule_not_named():
    from shape.takes import plainest
    items = {"chord": _item([(48, 0.3, 6.0), (52, 0.3, 6.0), (55, 0.3, 6.0)]),
             "single": _item([(60, 0.3, 2.0)]),
             "run": _item([(60 + i, 0.3 + 0.2 * i, 1.0) for i in range(7)])}
    assert plainest(items) == "single"


def test_buildup_refuses_a_band_the_plainest_take_never_had():
    """Dividing by an absent band manufactures a surplus that is not there.

    A model with nothing in a band puts it far under its own body level, and
    every phrase measured against it then reports tens of decibels of
    accumulation -- which reads as a second, opposite defect on top of the one
    real one. The floor gate alone cannot catch it: a synthetic render's lead-in
    is exact silence, so every band clears it.
    """
    from shape.takes import band_buildup
    n = int(6.0 * SR)
    t = np.arange(n) / SR
    body = np.sin(2 * np.pi * 700.0 * t) * np.exp(-t / 1.5) * (t >= 0.2)
    # 40 Hz present in the phrase and absent from the single note. `range_db` is
    # tightened here only because a synthetic tone leaks across bands far more
    # than a recording does, which puts the floor of this fixture near -57 dB
    # rather than the -88 the real case showed.
    low = np.sin(2 * np.pi * 40.0 * t) * np.exp(-t / 2.0) * (t >= 0.2)
    base = {"m": (body + low * 1e-6, SR)}
    many = {"m": (body + low, SR)}
    vals = band_buildup(many, base, "m", (2.0, 4.0), (2.0, 4.0),
                        (0.0, 0.18), (0.0, 0.18), (0.2, 1.8), (0.2, 1.8),
                        range_db=50.0)
    assert vals[0] is None
    # With the band genuinely present on both sides it is still reported.
    both = {"m": (body + low * 0.25, SR)}
    vals = band_buildup(many, both, "m", (2.0, 4.0), (2.0, 4.0),
                        (0.0, 0.18), (0.0, 0.18), (0.2, 1.8), (0.2, 1.8),
                        range_db=50.0)
    assert vals[0] is not None and abs(vals[0] - 12.04) < 0.5


def test_a_floor_recorded_into_the_sample_is_caught_by_its_own_infrasound():
    """A sampled instrument's noise plays back WITH the note, not before it.

    So a floor read from the lead-in reads as silence and clears every band,
    while the tail being judged sits on hiss the whole time. What catches it is
    a band the instrument cannot radiate at all: measured in the same window, it
    is that source's floor at that moment, whatever the lead-in said.
    """
    from shape.takes import band_error
    n = int(6.0 * SR)
    t = np.arange(n) / SR
    rng = np.random.default_rng(3)
    playing = (t >= 0.3).astype(float)
    # Hiss that exists only while the sample plays -- flat, so it is as loud at
    # 8 Hz as at 40, which no string instrument can be.
    hiss = rng.standard_normal(n) * 1e-3 * playing
    note = np.sin(2 * np.pi * 700.0 * t) * np.exp(-(t - 0.3) / 1.2) * playing
    tracks = {"m": (note, SR), "r": (note + hiss, SR)}
    vals, _ = band_error(tracks, "m", "r", (0.4, 1.6), (2.5, 4.5), (0.0, 0.28))
    bands = [(30, 60), (60, 125), (125, 250), (250, 500), (500, 1000),
             (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000)]
    # Every band the hiss owns is refused; the one the note is in is not.
    assert vals[bands.index((30, 60))] is None
    assert vals[bands.index((125, 250))] is None
    assert vals[bands.index((500, 1000))] is not None


def test_attack_finds_the_strike_in_a_take_with_lead_in_silence():
    """The onset anchor is a fraction of the envelope PEAK, not a fixed floor.

    A captured note commonly starts a tenth of a second into the file, and an
    absolute threshold finds the room rather than the strike.
    """
    sr = 48000
    lead = int(0.1 * sr)
    t = np.arange(int(0.5 * sr)) / sr
    hit = np.sin(2 * np.pi * 400 * t) * np.exp(-t / 0.05)
    sig = np.concatenate([np.zeros(lead), hit]).astype(np.float32)
    assert abs(attack.onset_index(sig, sr) - lead) < int(0.002 * sr)


def test_attack_reads_a_band_that_builds_against_the_rest_of_its_own_strike():
    """Anchoring on the onset nulls a whole-signal delay by design.

    What is left, and what this is for, is one band arriving late relative to
    the other bands of the SAME strike -- which is what a front-loaded modal
    field looks like and what no band level can show.
    """
    sr = 48000
    t = np.arange(int(0.4 * sr)) / sr
    decay = np.exp(-t / 0.12)
    high = np.sin(2 * np.pi * 5000 * t) * decay
    low = np.sin(2 * np.pi * 350 * t) * decay
    together = high + low
    builds = high + low * (1.0 - np.exp(-t / 0.03))
    diff = attack.compare(together, builds, sr)
    band = [i for i, (lo, hi) in enumerate(attack.ATTACK_BANDS) if lo <= 350 < hi][0]
    top = [i for i, (lo, hi) in enumerate(attack.ATTACK_BANDS) if lo <= 5000 < hi][0]
    early = attack.ATTACK_WINDOWS.index(0.005)
    late = attack.ATTACK_WINDOWS.index(0.250)
    # The low band is present at once in one and not the other...
    assert diff[early][band] > 6.0
    # ...while the band that did not change reads flat, and both agree by the
    # end: it is a position in time, not a level.
    assert abs(diff[early][top]) < 3.0
    assert abs(diff[late][band]) < 3.0


def test_attack_normalises_every_window_by_one_span():
    """Two rows of a column are comparable only if one span normalised both.

    Normalising each row by its own window folds the length ratio and the
    window's own loss into the cell, which read as a reference with nothing at
    all in its first five milliseconds.
    """
    sr = 48000
    t = np.arange(int(0.4 * sr)) / sr
    # float64: at 1e-2 scaling a float32 band 110 dB down is inside its own
    # quantisation, and the test would be measuring that rather than the metric.
    sig = np.sin(2 * np.pi * 350 * t) * np.exp(-t / 0.12)
    # Scaling a signal moves no cell: the normaliser scales with it.
    a = attack.profile(sig, sr)
    b = attack.profile(sig * 0.01, sr)
    assert np.nanmax(np.abs(a - b)) < 1e-3
