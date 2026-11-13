"""Tone-to-noise and the valleys on the note's fundamental's clock, and the capture states that follow.

Signals are built with a known pitch motion, noise or detune, and the reading has to
land where the same note held still does; none is checked against its own output.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import metrics_warp
from metrics_modulation import Vibrato
from metrics_note import analyze_note
from metrics_partials import measure_tnr_mask, tnr_mask_parts, tone_to_noise_db
from metrics_signal import _spectrum, midi_to_hz
from profile_measure import measure_note, tnr_unreliable
from smf import Note
from test_series_reading import PREROLL, SR, held, stiff_series


def tnr_of(x: np.ndarray, f0: float) -> float:
    """The raw reading: the tone-to-noise ratio of `x` through its own mask, no warp."""
    freqs, mag = _spectrum(x, SR)
    return tone_to_noise_db(freqs, mag, f0, 0.0, *tnr_mask_parts(measure_tnr_mask(x, SR, f0)))


def test_forcing_the_warp_on_a_steady_noisy_tone_does_not_move_its_ratio():
    """The control: unwarped along a track that is not there, the reading is the raw one."""
    x = stiff_series(261.63, 0.0, partials=8, rolloff_db=6.0, noise_db=-34.0)
    freqs, mag = _spectrum(x, SR)
    raw = tone_to_noise_db(freqs, mag, 261.63)
    y = metrics_warp.unwarp(x, SR, 261.63, Vibrato(20.0, 5.0, 6.0))
    assert y is not None
    wf, wm = _spectrum(y, SR)
    assert tone_to_noise_db(wf, wm, 261.63) == pytest.approx(raw, abs=1.0)


def test_a_deep_vibrato_with_noise_reads_the_tone_to_noise_of_the_same_note_held_still():
    still = stiff_series(261.63, 0.0, partials=8, rolloff_db=6.0, noise_db=-30.0)
    moving = stiff_series(261.63, 0.0, cents=50.0, partials=8, rolloff_db=6.0, noise_db=-30.0)
    steady = measure_note(held(still), SR, 60, preroll_s=PREROLL, gate_s=1.8)
    vib = measure_note(held(moving), SR, 60, preroll_s=PREROLL, gate_s=1.8)
    assert vib["unwarp"]["kept"]
    assert vib["tnr_db"] == pytest.approx(steady["tnr_db"], abs=3.0)


def test_a_clean_tone_under_a_deep_vibrato_does_not_read_as_noise():
    """A clean organ through a rotating speaker: 20 dB under what it reads on the raw signal."""
    x = stiff_series(261.63, 0.0, cents=50.0, partials=8, rolloff_db=6.0)
    row = measure_note(held(x), SR, 60, preroll_s=PREROLL, gate_s=1.8)
    assert row["unwarp"]["kept"]
    assert row["tnr_db"] > tnr_of(x, 261.63) + 20.0


def test_a_model_note_is_unwarped_where_its_reference_moves():
    """The model is read as its reference was: the vibrato its own signal carries is warped out."""
    x = held(stiff_series(261.63, 0.0, cents=50.0, partials=8, rolloff_db=6.0, noise_db=-30.0))
    ref = measure_note(x, SR, 60, preroll_s=PREROLL, gate_s=1.8)
    note = Note(60, 100, PREROLL, 1.8)
    through = analyze_note(x, SR, note, 2.0, onset=PREROLL, tnr_mask=ref["tnr_mask"])
    assert through.tnr_db == pytest.approx(ref["tnr_db"], abs=3.0)


def test_a_model_note_is_read_raw_where_its_reference_holds_still():
    x = held(stiff_series(261.63, 0.0, cents=50.0, partials=8, rolloff_db=6.0, noise_db=-30.0))
    note = Note(60, 100, PREROLL, 1.8)
    still_mask = {}
    through = analyze_note(x, SR, note, 2.0, onset=PREROLL, tnr_mask=still_mask)
    own = analyze_note(x, SR, note, 2.0, onset=PREROLL)
    assert through.tnr_db < own.tnr_db - 10.0


def test_a_modal_note_is_never_unwarped():
    """Bell partials sit outside a harmonic mask and no pitch track moves them as one series."""
    f0 = midi_to_hz(60)
    t = np.arange(int(1.5 * SR)) / SR
    ratios = (1.0, 2.76, 5.4, 8.93, 13.34)
    x = sum(np.exp(-1.2 * i * t) * np.sin(2 * np.pi * r * f0 * t) for i, r in enumerate(ratios))
    row = measure_note(held(x), SR, 60, preroll_s=PREROLL, gate_s=1.8, modal=True)
    assert "unwarp" not in row and "broad_lines" not in row


def test_a_unison_carries_broad_lines():
    x = stiff_series(261.63, 3e-4, detune_cents=6.0, partials=14, rolloff_db=2.0)
    row = measure_note(held(x), SR, 60, preroll_s=PREROLL, gate_s=1.8)
    assert row["broad_lines"]["broad_ratio"] >= metrics_warp.BROAD_RATIO


def test_tnr_unreliable_is_stated_per_timbre_where_most_of_its_rows_have_broad_lines():
    broad = {"timbre": "dark", "broad_lines": {}}
    clean = {"timbre": "bright"}
    state = tnr_unreliable([broad, broad, {**broad, "timbre": "dark"}, clean, clean, clean])
    assert state is not None
    assert state["timbres"] == {"dark": {"rows": 3, "of": 3}}
    still = {"timbre": "dark"}
    assert tnr_unreliable([broad, still, still, clean]) is None


def independent_voices(f0: float = 261.63, n_voices: int = 3, seconds: float = 1.5) -> np.ndarray:
    """Voices each under its own 4-6 Hz vibrato: the sum has no one pitch track."""
    t = np.arange(int(seconds * SR)) / SR
    out = np.zeros_like(t)
    for i in range(n_voices):
        rate = 4.0 + i
        clock = t + 40.0 / 1200.0 * np.log(2.0) / (2.0 * np.pi * rate) * np.sin(
            2 * np.pi * rate * t + i
        )
        out += sum(np.sin(2 * np.pi * k * f0 * (1.0 + 0.003 * i) * clock) / k for k in range(1, 9))
    return out


def test_independent_vibratos_are_an_ensemble_and_read_raw():
    row = measure_note(held(independent_voices()), SR, 60, preroll_s=PREROLL, gate_s=1.8)
    assert row["unwarp"]["state"] == "ensemble" and not row["unwarp"]["kept"]
    assert "unresolved" not in row and "tnr_db" in row


def test_an_undecided_row_takes_no_stiffness_tone_to_noise_or_valley_reading(monkeypatch):
    """Between the measured ranges neither reading is chosen: the fields are absent, not zero."""
    undecided = (
        metrics_warp.CoherenceState.UNDECIDED,
        0.55,
        "coherence 0.550 between the single-source and ensemble ranges",
    )
    monkeypatch.setattr(metrics_warp, "coherence_state", lambda source: undecided)
    x = stiff_series(261.63, 3e-4, cents=50.0, partials=14, rolloff_db=2.0, noise_db=-40.0)
    row = measure_note(held(x), SR, 60, preroll_s=PREROLL, gate_s=1.8)
    assert row["unresolved"]["reason"] == undecided[2]
    assert set(row["unresolved"]["withheld"]) == {"inharmonicity_b", "tnr_db", "valley_db"}
    assert not {"inharmonicity_b", "tnr_db", "valley_db"} & set(row)
    assert row["partials_fit"] == 0 and row["inharmonicity_reliable"] is False
    assert row["unwarp"]["state"] == "undecided" and not row["unwarp"]["kept"]
    # The loss path reads the model note the same way: no stiffness, no tone-to-noise.
    through = analyze_note(
        held(x), SR, Note(60, 100, PREROLL, 1.8), 2.0, onset=PREROLL, tnr_mask=row["tnr_mask"]
    )
    assert through.inharmonicity_b == 0.0 and through.inharmonicity_partials == 0
    assert np.isnan(through.tnr_db)
