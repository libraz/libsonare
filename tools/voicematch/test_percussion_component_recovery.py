"""Component amplitude recovery through the production percussion objective."""

import sys
from pathlib import Path

import pytest

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]

from percussion_component_recovery import ORACLE, run_benchmark


def test_snare_component_fit_recovers_contact_and_body_on_new_velocity():
    report = run_benchmark(max_evals=64)
    assert report["evaluations"] <= report["max_evals"]
    assert report["selected_parameters"] == pytest.approx(ORACLE, abs=0.15)
    for partition in ("training", "held_out"):
        before, after = report[partition]["start"], report[partition]["selected"]
        assert after["loss"] < before["loss"]
        for term in ("evolve", "diffuse"):
            assert after["terms"][term] < before["terms"][term]
    assert "level" in report["held_out"]["selected"]["quality"]["unmeasured_terms"]
    assert report["controls"]["self"]["loss"] == 0.0
    for control in ("no_stick", "no_rattle", "hand", "cup"):
        assert report["controls"][control]["loss"] > 0.0
        assert report["controls"][control]["terms"]["evolve"] > 0.0
    for term in ("evolve", "diffuse"):
        stochastic_max = max(
            item["terms"][term] for item in report["independent_seed_controls"].values()
        )
        for control in ("no_rattle", "cup"):
            assert report["controls"][control]["terms"][term] > stochastic_max
