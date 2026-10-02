"""Regression tests for the normal percussion texture measurements."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from loss import percussion_terms
from loss_weights import TERM_UNITS
from metrics_hit import analyze_hit
from metrics_texture import TEXTURE_BINS, analyze_texture
from smf import Note
from toneclass import default_weights

SR = 16_000
HIT_SR = 48_000


def _hit_with_texture(
    density: list[float],
    prompt: list[float],
    *,
    valid: list[bool] | None = None,
) -> dict:
    """A coarse-profile-identical hit with only texture changed."""
    valid = [True] * TEXTURE_BINS if valid is None else valid
    return {
        "bands_db": [0.0, -4.0, -8.0, -12.0],
        "band_decay_db_s": [-20.0, -30.0],
        "attack_ms": 2.0,
        "decay_ms": 240.0,
        "crest_db": 12.0,
        "modal_density": density,
        "modal_density_valid": valid,
        "prompt_late_db": prompt,
        "prompt_late_valid": valid,
    }


def _texture_signal(*, dense: bool, seconds: float = 8.0) -> np.ndarray:
    """Matched-envelope controls: a cup has few lines, a drum has a field."""
    n = int(seconds * SR)
    t = np.arange(n, dtype=np.float64) / SR
    rng = np.random.default_rng(5)
    envelope = np.exp(-t / 0.45)
    if dense:
        # A diffuse field in the same broad 250 Hz--4 kHz region.  The tiny
        # tail keeps a real floor available without changing the hit envelope.
        frequencies = np.linspace(260.0, 3900.0, 96)
        field = sum(
            np.sin(2.0 * np.pi * f * t + rng.uniform(0.0, 2.0 * np.pi)) for f in frequencies
        ) / np.sqrt(len(frequencies))
        body = envelope * field
    else:
        body = envelope * sum(np.sin(2.0 * np.pi * f * t) for f in (320.0, 760.0, 1420.0))
    return body + rng.normal(0.0, 1.0e-4, n)


def _fast_hit_signal(*, dense: bool) -> np.ndarray:
    """A production-sized 1.8 s hit whose floor remains measurable after t60."""
    n = int(1.8 * HIT_SR)
    t = np.arange(n, dtype=np.float64) / HIT_SR
    envelope = np.exp(-t / 0.1)
    rng = np.random.default_rng(12 if dense else 11)
    if dense:
        frequencies = np.linspace(260.0, 3900.0, 96)
        body = sum(
            np.sin(2.0 * np.pi * f * t + rng.uniform(0.0, 2.0 * np.pi)) for f in frequencies
        ) / np.sqrt(len(frequencies))
    else:
        body = sum(np.sin(2.0 * np.pi * f * t) for f in (320.0, 760.0, 1420.0))
    return envelope * body + rng.normal(0.0, 1.0e-5, n)


def _hit_row(signal: np.ndarray) -> dict:
    """Run the actual normal hit path, including its 1.8 s analysis ceiling."""
    return analyze_hit(
        signal,
        HIT_SR,
        Note(38, 100, 0.0, 0.05),
        len(signal) / HIT_SR,
    ).to_dict()


def test_sparse_cup_and_dense_drum_have_the_same_coarse_loss_inputs_but_differ_in_texture():
    sparse = [1.0, 2.0, 3.0, 3.0, 2.0, 1.0, 0.0]
    dense = [30.0, 30.0, 30.0, 30.0, 30.0, 30.0, 30.0]
    prompt = [2.0] * TEXTURE_BINS
    terms = percussion_terms(
        [_hit_with_texture(sparse, prompt)], [_hit_with_texture(dense, prompt)]
    )
    assert terms is not None
    assert terms["band"] == 0.0
    assert terms["bdecay"] == 0.0
    assert terms["density"] > 0.0
    assert terms["density_bins"] == TEXTURE_BINS


def test_a_tonal_oracle_is_allowed_to_stay_tonal():
    row = _hit_with_texture([2.0, 3.0, 1.0, 0.0, 0.0, 0.0, 0.0], [4.0] * TEXTURE_BINS)
    terms = percussion_terms([row], [row])
    assert terms is not None
    assert terms["density"] == 0.0
    assert terms["prompt"] == 0.0
    assert terms["density_bins"] == TEXTURE_BINS
    assert terms["prompt_bins"] == TEXTURE_BINS


def test_reference_valid_cells_charge_missing_model_texture_instead_of_withdrawing_the_question():
    oracle = _hit_with_texture([2.0] * TEXTURE_BINS, [3.0] * TEXTURE_BINS)
    model = _hit_with_texture(
        [0.0] * TEXTURE_BINS,
        [0.0] * TEXTURE_BINS,
        valid=[False] * TEXTURE_BINS,
    )
    terms = percussion_terms([model], [oracle])
    assert terms is not None
    assert terms["density"] > 0.0 and terms["prompt"] > 0.0
    assert terms["density_bins"] == TEXTURE_BINS
    assert terms["prompt_bins"] == TEXTURE_BINS
    assert terms["density_absent"] == TEXTURE_BINS
    assert terms["prompt_absent"] == TEXTURE_BINS


def test_old_profiles_without_texture_fields_are_unmeasured_not_zero_matches():
    old = {
        "bands_db": [0.0, -4.0, -8.0, -12.0],
        "band_decay_db_s": [-20.0, -30.0],
        "attack_ms": 2.0,
        "decay_ms": 240.0,
        "crest_db": 12.0,
    }
    terms = percussion_terms([old], [old])
    assert terms is not None
    assert terms["density"] == 0.0 and terms["prompt"] == 0.0
    assert terms["density_bins"] == 0.0 and terms["prompt_bins"] == 0.0
    assert terms["density_cells"] == 0.0 and terms["prompt_cells"] == 0.0


def test_texture_extraction_separates_sparse_and_dense_controls_with_same_envelope():
    sparse = analyze_texture(_texture_signal(dense=False), SR)
    dense = analyze_texture(_texture_signal(dense=True), SR)
    sparse_count = sum(
        v for v, ok in zip(sparse["modal_density"], sparse["modal_density_valid"]) if ok
    )
    dense_count = sum(
        v for v, ok in zip(dense["modal_density"], dense["modal_density_valid"]) if ok
    )
    assert sum(sparse["modal_density_valid"]) >= 1
    assert sum(dense["modal_density_valid"]) >= 1
    assert dense_count > sparse_count


def test_normal_analyze_hit_path_separates_a_cup_from_a_dense_drum_after_coarse_fields_match():
    """The regression must exercise the 1.8 s production hit path, not only a long helper."""
    sparse = _hit_row(_fast_hit_signal(dense=False))
    dense = _hit_row(_fast_hit_signal(dense=True))
    assert all(sparse["modal_density_valid"])
    assert all(dense["modal_density_valid"])
    assert any(sparse["prompt_late_valid"])
    assert any(dense["prompt_late_valid"])

    # Hold the existing coarse evidence exactly equal.  The residual below is
    # therefore caused by the newly measured resonance field and strike/late
    # colour, rather than by the already-covered band profile or envelope.
    for key in ("bands_db", "band_decay_db_s", "attack_ms", "decay_ms", "crest_db"):
        sparse[key] = dense[key]
    terms = percussion_terms([sparse], [dense])
    assert terms is not None
    assert terms["density_bins"] == TEXTURE_BINS
    assert terms["prompt_bins"] > 0.0
    assert terms["density"] > 0.0
    assert terms["prompt"] > 0.0

    weights = default_weights(0, drum_note=38, percussive=True)
    assert weights["density"] > 0.0 and weights["prompt"] > 0.0
    texture_units = (
        weights["density"] * terms["density"] / TERM_UNITS["density"]
        + weights["prompt"] * terms["prompt"] / TERM_UNITS["prompt"]
    )
    assert texture_units > 1.0


def test_prompt_late_from_actual_hits_separates_an_early_highband_click_from_sustained_highband_colour():
    """A high band present only in the strike must not match one that persists."""
    n = int(1.8 * HIT_SR)
    t = np.arange(n, dtype=np.float64) / HIT_SR
    low = np.sin(2.0 * np.pi * 300.0 * t)
    high = np.sin(2.0 * np.pi * 3000.0 * t)
    rng = np.random.default_rng(13)

    early = _hit_row(
        0.8 * np.exp(-t / 0.1) * low + 0.8 * np.exp(-t / 0.015) * high + rng.normal(0.0, 1.0e-5, n)
    )
    sustained = _hit_row(
        0.8 * np.exp(-t / 0.1) * low + 0.8 * np.exp(-t / 0.1) * high + rng.normal(0.0, 1.0e-5, n)
    )
    high_band = 5  # 2--4 kHz in the shared texture octave grid.
    assert early["prompt_late_valid"][high_band]
    assert sustained["prompt_late_valid"][high_band]
    assert early["prompt_late_db"][high_band] > sustained["prompt_late_db"][high_band] + 10.0


def test_short_recording_and_missing_floor_are_unmeasured():
    short = analyze_texture(_texture_signal(dense=True, seconds=0.2), SR)
    assert not any(short["modal_density_valid"])
    assert not any(short["prompt_late_valid"])

    # A complete hit with no post-release floor is still a valid signal, but
    # modal density has no defensible noise-floor gate. Prompt colour remains
    # available because it only asks about the two recorded windows.
    no_floor = analyze_texture(_texture_signal(dense=True, seconds=2.0), SR)
    assert not any(no_floor["modal_density_valid"])
    assert any(no_floor["prompt_late_valid"])


def test_band_ceiling_marks_unrecorded_texture_cells_invalid():
    full = analyze_texture(_texture_signal(dense=True), SR)
    cut = analyze_texture(_texture_signal(dense=True), SR, max_band_hz=1000.0)
    assert sum(cut["modal_density_valid"]) < sum(full["modal_density_valid"])
    assert all(not cut["modal_density_valid"][i] for i in range(4, TEXTURE_BINS))
    assert all(not cut["prompt_late_valid"][i] for i in range(4, TEXTURE_BINS))
