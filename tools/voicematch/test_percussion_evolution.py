"""Production-path regressions for onset/body/late percussion evidence."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fit_quality import quality_report
from loss import percussion_terms
from loss_aggregate import robust_score_terms
from loss_weights import TERM_COUNT_KEYS, LossWeights
from metrics_hit import analyze_hit
from metrics_texture import (
    EVOLUTION_BINS,
    EVOLUTION_WINDOWS_COUNT,
    analyze_evolution,
)
from percussion_component_recovery import ORACLE, SR, component_hit, rows
from smf import Note
from toneclass import default_weights


def _row(values=ORACLE, *, velocity=100, family="snare", control="", duration=1.3):
    audio = component_hit(values, velocity, family=family, control=control)
    audio = audio[: int(duration * SR)]
    return analyze_hit(audio, SR, Note(38, velocity, 0.0, 0.05), duration).to_dict()


def _terms(control="", *, family="snare"):
    reference = rows(ORACLE, (64, 100), family=family)
    candidate = rows(ORACLE, (64, 100), family=family, control=control)
    return robust_score_terms(candidate, reference, percussive=True)


def test_evolution_shape_self_match_and_gain_are_invariant():
    row = _row()
    assert len(row["evolution_db"]) == 3
    assert all(len(values) == EVOLUTION_BINS for values in row["evolution_db"])
    assert len(row["window_flatness_db"]) == EVOLUTION_WINDOWS_COUNT
    terms = percussion_terms([row], [row])
    assert terms["evolve"] == 0.0 and terms["diffuse"] == 0.0

    gained_audio = component_hit(ORACLE, 100) * 4.0
    gained = analyze_hit(gained_audio, SR, Note(38, 100, 0.0, 0.05), 1.3).to_dict()
    for left, right in zip(row["evolution_db"], gained["evolution_db"]):
        assert left == pytest.approx(right, abs=0.01)
    assert row["window_flatness_db"] == pytest.approx(gained["window_flatness_db"], abs=0.01)


def test_common_power_ignores_content_above_the_reference_ceiling():
    audio = component_hit(ORACLE, 100)
    t = np.arange(len(audio), dtype=np.float64) / SR
    high = audio * 10.0 + 3.0 * np.sin(2.0 * np.pi * 11000.0 * t) * np.exp(-t / 0.15)
    plain = analyze_evolution(audio, SR, max_band_hz=8000.0)
    extended = analyze_evolution(high, SR, max_band_hz=8000.0)
    for window, valid in enumerate(plain["evolution_valid"]):
        for band, ok in enumerate(valid):
            if ok and extended["evolution_valid"][window][band]:
                assert extended["evolution_db"][window][band] == pytest.approx(
                    plain["evolution_db"][window][band], abs=0.05
                )
    assert extended["window_flatness_db"] == pytest.approx(plain["window_flatness_db"], abs=0.05)


def test_common_ruler_preserves_sample_zero_and_rejects_tiny_tonal_leakage():
    t = np.arange(int(1.3 * SR), dtype=np.float64) / SR
    pure = np.sin(2.0 * np.pi * 90.0 * t) * np.exp(-t / 0.06)
    quiet_noise = pure + 1.0e-8 * np.sin(2.0 * np.pi * 2300.0 * t)
    pure_metrics = analyze_evolution(pure, SR)
    quiet_metrics = analyze_evolution(quiet_noise, SR)
    assert quiet_metrics["evolution_valid"] == pure_metrics["evolution_valid"]
    assert quiet_metrics["window_flatness_valid"] == pure_metrics["window_flatness_valid"]

    impulse = np.zeros(int(0.3 * SR), dtype=np.float64)
    impulse[0] = 1.0
    impulse_metrics = analyze_evolution(impulse, SR)
    assert any(impulse_metrics["evolution_valid"][0])
    assert impulse_metrics["evolution_valid"][0][0] is False


def test_floor_crossing_keeps_a_finite_candidate_continuous():
    t = np.arange(int(1.3 * SR), dtype=np.float64) / SR
    low = np.sin(2.0 * np.pi * 90.0 * t) * np.exp(-t / 0.06)
    rng = np.random.default_rng(73)
    high = rng.standard_normal(len(t))
    spectrum = np.fft.rfft(high)
    frequencies = np.fft.rfftfreq(len(t), 1.0 / SR)
    spectrum[(frequencies < 4400.0) | (frequencies > 7600.0)] = 0.0
    high = np.fft.irfft(spectrum, n=len(t))
    high = high / np.std(high) * np.exp(-t / 0.06)
    amplitude = 0.0026140244863273833
    # The amplitude puts the band at the floor; +/-1.2 % is about 0.1 dB either side.
    reference = analyze_hit(
        low + amplitude * 1.012 * high, SR, Note(38, 100, 0.0, 0.05), 1.3
    ).to_dict()
    candidate = analyze_hit(
        low + amplitude * 0.988 * high, SR, Note(38, 100, 0.0, 0.05), 1.3
    ).to_dict()
    assert reference["evolution_valid"][2][7] is True
    assert candidate["evolution_valid"][2][7] is False
    assert candidate["evolution_measured"][2][7] is True
    terms = percussion_terms([candidate], [reference])
    assert terms["evolve_absent"] == 0.0
    assert terms["evolve"] < 0.5


def test_missing_contact_and_rattle_move_real_evolution_terms():
    no_stick = _terms("no_stick")
    no_rattle = _terms("no_rattle")
    hand = _terms("hand")
    cup = _terms("cup")
    assert no_stick["evolve"] > 0.1
    assert no_rattle["evolve"] > no_stick["evolve"]
    assert hand["evolve"] >= no_rattle["evolve"]
    assert hand["diffuse"] >= no_rattle["diffuse"]
    assert cup["evolve"] > 1.0 and cup["diffuse"] > 1.0
    # A focal defect lifts the scored value above the plain cell mean.
    assert no_stick["evolve"] > no_stick["evolution_cell_mean"]
    assert hand["diffuse"] > hand["window_flatness_cell_mean"]


def test_temporal_arrangement_is_seen_even_when_coarse_band_profile_is_close():
    reference = rows(ORACLE, (100,), family="snare")
    candidate = rows(ORACLE, (100,), family="snare", control="no_stick")
    terms = robust_score_terms(candidate, reference, percussive=True)
    # The missing contact contributes almost nothing to the integrated profile,
    # but it removes a real onset cell from the common-power trajectory.
    assert terms["band"] < 0.1
    assert terms["evolve"] > terms["band"]
    assert terms["evolve_cells"] > 0.0


def test_missing_candidate_cells_are_capped_without_shifting_later_windows():
    oracle = rows(ORACLE, (64, 100), family="snare")
    model = [dict(row) for row in oracle]
    model[0]["evolution_db"] = [
        [],
        list(model[0]["evolution_db"][1]),
        list(model[0]["evolution_db"][2]),
    ]
    model[0]["evolution_valid"] = [
        [],
        list(model[0]["evolution_valid"][1]),
        list(model[0]["evolution_valid"][2]),
    ]
    terms = percussion_terms(model, oracle)
    expected = sum(oracle[0]["evolution_valid"][0])
    assert terms["evolve_absent"] == expected
    assert terms["evolution_cells"] > expected
    assert terms["evolve"] > 0.0


def test_short_invalid_and_silent_audio_is_unknown_and_reference_cells_are_charged():
    full = _row()
    short = _row(duration=0.1)
    assert not any(short["evolution_valid"][2])
    assert short["window_flatness_valid"][2] is False
    terms = percussion_terms([short], [full])
    assert terms["evolution_cells"] > 0.0
    assert terms["evolve_absent"] > 0.0
    assert terms["window_flatness_cells"] > 0.0
    assert terms["diffuse_absent"] > 0.0

    silent = analyze_evolution(np.zeros(int(0.3 * SR)), SR)
    invalid = analyze_evolution(np.full(int(0.3 * SR), np.nan), SR)
    edge = analyze_evolution(component_hit(ORACLE), SR, max_band_hz=40.0)
    low_sr = analyze_evolution(component_hit(ORACLE)[: int(0.3 * SR)], 8000)
    assert not any(any(row) for row in silent["evolution_valid"])
    assert not any(any(row) for row in invalid["evolution_valid"])
    assert not any(any(row) for row in edge["evolution_valid"])
    assert not any(edge["window_flatness_valid"])
    assert all(not row[-1] for row in low_sr["evolution_valid"])


def test_reference_unavailable_is_provenance_and_quality_never_calls_it_a_match():
    valid = _row()
    valid_terms = percussion_terms([valid], [valid])
    objective = LossWeights({"evolve": 1.0, "diffuse": 1.0})
    objective.anchor(valid_terms)
    self_report = quality_report(valid_terms, objective, baseline_terms=valid_terms)
    assert self_report["terms"]["evolve"]["target_met"] is True
    assert self_report["terms"]["diffuse"]["target_met"] is True
    assert self_report["terms"]["evolve"]["unmeasured"] is False
    assert self_report["terms"]["evolve"]["reference_unavailable"] > 0.0

    invalid = dict(valid)
    invalid["evolution_db"] = [[0.0] * EVOLUTION_BINS for _ in valid["evolution_db"]]
    invalid["evolution_valid"] = [[False] * EVOLUTION_BINS for _ in valid["evolution_valid"]]
    invalid["evolution_measured"] = [[False] * EVOLUTION_BINS for _ in valid["evolution_measured"]]
    invalid["window_flatness_db"] = [0.0] * EVOLUTION_WINDOWS_COUNT
    invalid["window_flatness_valid"] = [False] * EVOLUTION_WINDOWS_COUNT
    invalid["window_flatness_measured"] = [False] * EVOLUTION_WINDOWS_COUNT
    invalid_terms = percussion_terms([invalid], [invalid])
    assert invalid_terms["evolution_cells"] == 0.0
    assert invalid_terms["window_flatness_cells"] == 0.0
    assert invalid_terms["evolve_skipped"] == 0.0
    assert invalid_terms["diffuse_skipped"] == 0.0
    assert invalid_terms["evolution_reference_unavailable"] > 0.0
    assert invalid_terms["window_flatness_reference_unavailable"] > 0.0
    invalid_objective = LossWeights({"evolve": 1.0, "diffuse": 1.0})
    invalid_objective.anchor(invalid_terms)
    invalid_report = quality_report(invalid_terms, invalid_objective, baseline_terms=invalid_terms)
    assert invalid_report["terms"]["evolve"]["reference_unavailable"] > 0.0
    assert invalid_report["terms"]["diffuse"]["reference_unavailable"] > 0.0
    assert invalid_report["terms"]["evolve"]["unmeasured"] is True
    assert invalid_report["terms"]["diffuse"]["unmeasured"] is True
    assert invalid_report["terms"]["evolve"]["target_met"] is None
    assert invalid_report["terms"]["diffuse"]["target_met"] is None
    assert invalid_report["met"] is False


def test_defaults_include_new_percussion_terms_but_not_pitched_terms():
    percussion = default_weights(0, drum_note=38, percussive=True)
    pitched = default_weights(0)
    assert percussion["evolve"] == 1.0 and percussion["diffuse"] == 1.0
    assert "evolve" not in pitched and "diffuse" not in pitched
    assert TERM_COUNT_KEYS["evolve"] == "evolution_cells"
    assert TERM_COUNT_KEYS["diffuse"] == "window_flatness_cells"
