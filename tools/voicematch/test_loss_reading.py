"""The loss path reads a note as its reference row was read: no stiffness for a modal one, lines masked."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from loss import corpus_reading, oracle_tnr_masks
from metrics_note import analyze_note
from smf import Note
from test_series_reading import PREROLL, SR, held, stiff_series

LINE_HZ = 10885.8


def analysed(x: np.ndarray, **kwargs):
    return analyze_note(held(x), SR, Note(60, 100, PREROLL, 1.8), 2.0, onset=PREROLL, **kwargs)


def test_a_modal_reference_leaves_the_model_no_stiffness_to_fit():
    x = stiff_series(261.63, 6e-4, partials=12, rolloff_db=3.0)
    assert analysed(x).inharmonicity_b > 1e-4
    through = analysed(x, tnr_mask={"modal": True})
    assert through.inharmonicity_b == 0.0 and through.inharmonicity_partials == 0
    own = analysed(x, reading={"modal": True})
    assert own.inharmonicity_b == 0.0 and own.tnr_mask["modal"] is True


def test_a_stationary_line_in_the_reference_masks_the_models_centroid():
    clean = stiff_series(261.63, 0.0, partials=8, rolloff_db=6.0)
    t = np.arange(clean.size) / SR
    lined = clean + 0.05 * np.sin(2 * np.pi * LINE_HZ * t)
    base = analysed(clean).centroid_hz
    assert analysed(lined).centroid_hz > 1.4 * base
    masks = {"stationary_lines_hz": [LINE_HZ]}
    assert analysed(lined, tnr_mask=masks).centroid_hz == pytest.approx(base, rel=0.02)
    # The oracle's own probe, given the same lines, records them for the model to be read through.
    oracle = analysed(lined, reading=masks)
    assert oracle.centroid_hz == analysed(lined, tnr_mask=masks).centroid_hz
    assert oracle_tnr_masks([oracle.to_dict()])[0]["stationary_lines_hz"] == [LINE_HZ]


def test_a_plain_reading_changes_nothing():
    x = stiff_series(261.63, 3e-4, partials=12, rolloff_db=3.0, noise_db=-40.0)
    assert analysed(x).to_dict() == analysed(x, reading={}).to_dict()


def test_the_corpus_reading_comes_from_the_committed_profile(tmp_path):
    corpus = SimpleNamespace(capture_id="bells", timbre="t0")
    assert corpus_reading(corpus, tmp_path) == {}
    profile = {
        "capture": {"program": 14},
        "stationary_lines": {"timbres": {"t0": {"lines": [{"hz": LINE_HZ, "takes": 9}]}}},
        "body_lines": {"timbres": {"t0": {"lines": [{"hz": 99.2, "notes": 3}]}}},
    }
    (tmp_path / "bells.json").write_text(json.dumps(profile))
    assert corpus_reading(corpus, tmp_path) == {
        "modal": True,
        "stationary_lines_hz": [LINE_HZ],
        "body_lines_hz": [99.2],
    }
    profile["capture"]["program"] = 0
    del profile["stationary_lines"], profile["body_lines"]
    (tmp_path / "bells.json").write_text(json.dumps(profile))
    assert corpus_reading(corpus, tmp_path) == {}


def test_a_body_line_in_the_reference_is_masked_from_the_models_body():
    """The body band under the note reads the model with the reference's recurring lines masked."""
    t = np.arange(int(2.0 * SR)) / SR
    x = stiff_series(370.0, 0.0, seconds=2.0, partials=6, rolloff_db=6.0)
    x = (
        x
        + 3e-3 * np.sin(2 * np.pi * 99.2 * t)
        + 3e-5 * np.random.default_rng(1).standard_normal(t.size)
    )
    note = Note(66, 100, PREROLL, 1.8)
    plain = analyze_note(held(x), SR, note, 2.0, onset=PREROLL)
    masked = analyze_note(held(x), SR, note, 2.0, onset=PREROLL, tnr_mask={"body_lines_hz": [99.2]})
    assert masked.body_below_f0_db < plain.body_below_f0_db - 10.0
    own = analyze_note(held(x), SR, note, 2.0, onset=PREROLL, reading={"body_lines_hz": [99.2]})
    assert own.body_below_f0_db == masked.body_below_f0_db
