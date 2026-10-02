"""Known-audio recovery checks for all physical-source families."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_TOOLS_DIR = Path(__file__).resolve().parent
_TOOLS_ROOT = _TOOLS_DIR.parent
for _path in (_TOOLS_ROOT, _TOOLS_DIR):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

from family_recovery import FAMILY_SPECS, SCHEMA, main, run_benchmark

FAMILIES = ("bowed", "plucked", "hammered", "percussion")


def _residual(report: dict, split: str, stage: str) -> float:
    return float(report["fit"][split][stage]["absolute_residual"]["weighted_perceptual_units"])


def test_the_family_matrix_is_the_four_physical_sources() -> None:
    assert set(FAMILY_SPECS) == set(FAMILIES)


@pytest.mark.parametrize("family", FAMILIES)
def test_each_family_improves_disjoint_holdout_and_reports_measurement_holes(family: str) -> None:
    result = run_benchmark(family=family, optimizer="coordinate", max_evals=12, seed=17)

    assert result["schema"] == SCHEMA
    fit = result["fit"]
    assert fit["evaluations"] <= fit["max_evals"]
    assert fit["final_loss"] < fit["initial_loss"]
    assert _residual(result, "training", "final") < _residual(result, "training", "initial")
    assert _residual(result, "withheld", "final") < _residual(result, "withheld", "initial")
    train = {tuple(pair) for pair in result["piece"]["training_pairs"]}
    holdout = {tuple(pair) for pair in result["piece"]["withheld_pairs"]}
    assert train.isdisjoint(holdout), family
    assert (
        result["fit"]["best_normalized_parameter_distance"]
        < result["fit"]["start_normalized_parameter_distance"]
    )
    assert result["controls"]["self_zero"]["absolute_residual"]["weighted_perceptual_units"] == 0.0
    for name in ("wrong_excitation", "wrong_envelope", "wrong_dynamics"):
        assert result["controls"][name]["absolute_residual"]["weighted_perceptual_units"] > 0.0, (
            family,
            name,
        )
    excitation = result["controls"]["wrong_excitation"]
    assert excitation["definition"]
    feature_deltas = [
        value
        for value in excitation["features"]["absolute_delta"].values()
        if value is not None and value > 0.1
    ]
    assert len(feature_deltas) >= 2, family
    if family == "percussion":
        assert "high-Q" in excitation["definition"]
        assert "no strike noise" in excitation["definition"]
        deltas = excitation["features"]["absolute_delta"]
        assert deltas["modal_count"] >= 1.0
        assert deltas["flatness_db"] > 10.0
        assert excitation["absolute_residual"]["terms"]["prompt"] > 0.0
        assert "density" in excitation["coverage"]["unavailable_terms"]
        assert excitation["absolute_residual"]["perceptual_units"]["density"] is None
    else:
        deltas = excitation["features"]["absolute_delta"]
        assert deltas["attack_fine_ms"] > 50.0
        assert deltas["sustain_slope_db_s"] > 1.0
        assert deltas["harmonic_h2_db"] == 0.0
    assert result["controls"]["gain_offset_invariance"]["absolute_delta"] <= 0.05
    unavailable = result["unavailable_default_terms"]
    assert unavailable == sorted(unavailable)
    measured_holes = set(result["fit"]["training"]["initial"]["coverage"]["unavailable_terms"])
    assert measured_holes <= set(unavailable)
    if family == "percussion":
        assert "kit" in unavailable


@pytest.mark.parametrize("optimizer", ["coordinate", "cmaes"])
def test_single_family_cli_keeps_absolute_and_upper_tail_metadata(
    optimizer: str, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    out = tmp_path / "family.json"
    assert (
        main(
            [
                "--family",
                "percussion",
                "--optimizer",
                optimizer,
                "--max-evals",
                "12",
                "--out",
                str(out),
            ]
        )
        == 0
    )
    printed = json.loads(capsys.readouterr().out)
    written = json.loads(out.read_text())
    assert printed == written
    assert printed["schema"] == SCHEMA
    assert printed["family"] == "percussion"
    terms = printed["fit"]["training"]["final"]["absolute_residual"]["by_term"]
    assert terms["band"]["status"] == "measured"
    assert "upper_tail" in terms["band"]
    assert "worst_note" in terms["band"]
    assert printed["controls"]["gain_offset_invariance"]["absolute_delta"] <= 0.05
