"""One condition cannot establish level balance after removing common gain."""

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]

from autofit_resolve import resolve_probe
from autofit_test_fixtures import _probe_args
from fit_quality import quality_report
from loss_dimensions import _level_terms
from loss_weights import LossWeights, cli_weights, unmeasurable_terms
from metrics_note import level_of
from smf import Note

SR = 48000
LEVEL_NOTE = Note(60, 80, 0.0, 1.5)


def _tone(hz: float, amp: float = 0.2, seconds: float = 1.6) -> np.ndarray:
    return amp * np.sin(2.0 * np.pi * hz * np.arange(int(seconds * SR)) / SR)


def test_mono_shapes_and_identical_channels_read_as_the_mono_signal():
    mono = _tone(220.0) + _tone(331.0, 0.05)
    want = level_of(mono, SR, LEVEL_NOTE, 1.6)
    assert want["held_rms_dbfs"] is not None
    pair = np.stack([mono, mono], axis=1)
    assert level_of(mono[:, None], SR, LEVEL_NOTE, 1.6) == want
    assert level_of(pair, SR, LEVEL_NOTE, 1.6) == want
    # Pinned against the formula the mono read has always used.
    held = mono[int(0.2 * SR) : int(1.5 * SR)]
    assert want["held_rms_dbfs"] == round(float(20 * np.log10(np.sqrt(np.mean(held**2)))), 2)
    assert want["peak_dbfs"] == round(
        float(20 * np.log10(np.max(np.abs(mono[: int(1.6 * SR)])))), 2
    )


def test_decorrelated_channels_read_their_own_power_not_the_downmix():
    stereo = np.stack([_tone(220.0), _tone(331.0)], axis=1)
    power = 0.2**2 / 2.0
    got = level_of(stereo, SR, LEVEL_NOTE, 1.6)
    assert got["held_rms_dbfs"] == pytest.approx(10.0 * np.log10(power), abs=0.05)
    assert got["peak_dbfs"] == pytest.approx(20.0 * np.log10(0.2), abs=0.05)
    assert got["held_crest_db"] == pytest.approx(got["peak_dbfs"] - got["held_rms_dbfs"], abs=0.011)
    downmix = level_of(stereo.mean(axis=1), SR, LEVEL_NOTE, 1.6)
    assert got["held_rms_dbfs"] - downmix["held_rms_dbfs"] == pytest.approx(3.0, abs=0.1)


def test_the_channels_power_is_averaged_not_the_louder_channel_taken():
    stereo = np.stack([_tone(220.0, 0.2), _tone(331.0, 0.1)], axis=1)
    power = (0.2**2 + 0.1**2) / 4.0
    got = level_of(stereo, SR, LEVEL_NOTE, 1.6)
    assert got["held_rms_dbfs"] == pytest.approx(10.0 * np.log10(power), abs=0.05)


def _decorrelated_stereo(frames: int) -> np.ndarray:
    t = np.arange(frames) / SR
    return np.stack(
        [0.2 * np.sin(2 * np.pi * 220.0 * t), 0.2 * np.sin(2 * np.pi * 331.0 * t)], axis=1
    ).astype(np.float32)


def test_model_path_reaches_level_of_without_a_downmix(monkeypatch, capsys):
    import render_rows

    def fake_render(smf_bytes, total, rate, rig=False):
        return _decorrelated_stereo(int(total * rate))

    monkeypatch.setattr(render_rows, "render_model", fake_render)
    assert (
        render_rows.render_metrics_main(
            ["--program", "0", "--pattern", "sustain", "--notes", "60", "--velocities", "80"]
        )
        == 0
    )
    row = json.loads(capsys.readouterr().out)[0]
    assert row["held_rms_dbfs"] == pytest.approx(10.0 * np.log10(0.2**2 / 2.0), abs=0.1)


def test_oracle_path_reaches_level_of_without_a_downmix(tmp_path, monkeypatch):
    import autofit
    from autofit_test_fixtures import _fit_args, _write_corpus

    corpus_path = _write_corpus(
        tmp_path / "corpus", notes=(60,), velocities=(100,), gate_ms=500, seconds=2.0
    )
    seen = []
    original = autofit.probe_rows

    def spy(mono, pattern, sr, raw=None, **kwargs):
        seen.append(raw)
        return original(mono, pattern, sr, raw=raw, **kwargs)

    def fake_oracle(corpus, pattern, sr, mono_mode="mean"):
        return _decorrelated_stereo(int(3.0 * sr))

    monkeypatch.setattr(autofit, "probe_rows", spy)
    monkeypatch.setattr(autofit, "corpus_oracle", fake_oracle)
    args = _fit_args(
        program=0,
        pattern="sustain",
        notes="60",
        velocities="100",
        corpus=str(corpus_path),
        corpus_timbre="t",
        room="none",
    )
    rows, _, _, _ = autofit.oracle_reference(args)
    assert seen and seen[0].ndim == 2 and seen[0].shape[1] == 2
    assert rows[0]["held_rms_dbfs"] == pytest.approx(10.0 * np.log10(0.2**2 / 2.0), abs=0.1)


def test_one_condition_level_is_a_tautology_and_is_not_a_fit_axis():
    reference = [{"held_rms_dbfs": -30.0}]
    for level in (-90.0, -30.0, -3.0):
        assert _level_terms([{"held_rms_dbfs": level}], reference)[0] == 0.0
    args = _probe_args(program=40, pattern="sustain", notes="67")
    resolve_probe(args)
    assert "level" not in cli_weights(args)
    assert "level" in dict(unmeasurable_terms(args))


def test_explicit_level_requires_a_contrast_after_removing_gain():
    with pytest.raises(ValueError, match="level.*contrast"):
        resolve_probe(_probe_args(program=40, pattern="sustain", notes="67", w_level=1.0))


def test_level_weight_loaded_after_probe_resolution_still_requires_contrast():
    args = _probe_args(program=40, pattern="sustain", notes="67")
    resolve_probe(args)
    args.w_level = 1.0  # Spec weights are applied after resolving the probe.
    with pytest.raises(ValueError, match="level.*contrast"):
        cli_weights(args)


@pytest.mark.parametrize("pattern,notes", [("sustain", "55,67"), ("velocity", "67")])
def test_register_or_velocity_contrast_keeps_level_balance(pattern, notes):
    args = _probe_args(program=40, pattern=pattern, notes=notes)
    resolve_probe(args)
    assert cli_weights(args)["level"] > 0.0


@pytest.mark.parametrize("missing", [None, float("nan")])
def test_one_usable_reference_level_does_not_establish_a_measured_contrast(missing):
    oracle = [
        {"note": 60, "velocity": 80, "held_rms_dbfs": -30.0},
        {"note": 72, "velocity": 80, "held_rms_dbfs": missing},
    ]
    counts = {}
    level, _, _ = _level_terms(oracle, oracle, counts=counts)
    terms = {"level": level, "comparable": 1.0, **counts}
    objective = LossWeights({"level": 1.0})
    objective.anchor(terms)
    report = quality_report(terms, objective)
    assert level == 0.0
    assert report["terms"]["level"]["residual_units"] is None
    assert report["met"] is False


def test_repeated_same_condition_is_not_a_level_contrast():
    oracle = [{"note": 60, "velocity": 80, "held_rms_dbfs": -30.0}] * 2
    counts = {}
    level, _, _ = _level_terms(oracle, oracle, counts=counts)
    terms = {"level": level, "comparable": 1.0, **counts}
    objective = LossWeights({"level": 1.0})
    objective.anchor(terms)
    assert quality_report(terms, objective)["met"] is False


def test_even_grid_uses_symmetric_common_gain():
    oracle = [{"held_rms_dbfs": -30.0}, {"held_rms_dbfs": -30.0}]
    model = [{"held_rms_dbfs": -30.0}, {"held_rms_dbfs": -24.0}]
    balance, _, gain = _level_terms(model, oracle)
    assert gain == 3.0
    assert balance == 3.0
    assert _level_terms(model[::-1], oracle) == (balance, 0.0, gain)
    shifted = [{"held_rms_dbfs": row["held_rms_dbfs"] + 9.0} for row in model]
    assert _level_terms(shifted, oracle)[0] == balance


def test_capped_string_stretch_cannot_be_a_structural_diagnosis():
    from diagnose import diagnose
    from loss import score_terms
    from metrics import MIN_PARTIALS_FOR_B

    reference = {
        "harmonics_db": [0.0, -6.0, -12.0] + [-20.0] * 9,
        "f0_cents_err": 0.0,
        "tnr_db": 30.0,
        "sustain_slope_db_s": -3.0,
        "release_ms": 80.0,
        "attack_ms": 5.0,
        "inharmonicity_b": 0.0,
        "inharmonicity_partials": MIN_PARTIALS_FOR_B,
    }
    oracle = [reference]
    model = [reference | {"inharmonicity_b": 0.01}]
    terms = score_terms(model, oracle)
    assert terms["stiff"] == 50.0
    assert terms["stiff_capped"] == 1.0
    objective = LossWeights({"stiff": 1.0})
    objective.anchor(terms)
    assert not quality_report(terms, objective)["terms"]["stiff"]["coverage_complete"]
    diag = diagnose(
        terms,
        [("decay", "lo", terms, "spec"), ("decay", "hi", terms, "spec")],
        objective.weights,
        objective=objective,
    )
    assert next(t for t in diag.terms if t.term == "stiff").verdict == "measurement-limited"
    assert not diag.structural()
