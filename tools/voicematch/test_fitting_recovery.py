"""Synthetic oracle recovery checks for the fitting harness."""

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

from fitting_recovery import (
    OBJECTIVE_WEIGHTS,
    ORACLE_VALUES,
    REPORT_TERMS,
    SCHEMA,
    TRAINING_VELOCITIES,
    _residual_payload,
    cup_worse,
    main,
    measure_rows,
    run_benchmark,
    self_zero_passed,
)
from loss_aggregate import robust_score_terms


def _weighted(report: dict, section: str, *, withheld: bool = False) -> float:
    if withheld:
        payload = report["fit"]["withheld"][section]
    else:
        payload = report["fit"][section]
    return float(payload["absolute_residual"]["weighted_perceptual_units"])


def test_recovery_uses_percussion_metrics_and_withheld_velocity() -> None:
    report = run_benchmark(optimizer="cmaes", max_evals=36, seed=17)

    assert report["schema"] == SCHEMA
    assert report["fit"]["evaluations"] <= report["fit"]["max_evals"]
    assert report["fit"]["final_loss"] < report["fit"]["initial_loss"]
    assert _weighted(report, "final") < _weighted(report, "initial")
    assert _weighted(report, "final", withheld=True) < _weighted(report, "initial", withheld=True)

    coverage = report["fit"]["final"]["coverage"]
    assert coverage["rows_requested"] == 2
    assert coverage["rows_measured"] == 2
    assert coverage["rows_with_modes"] == 2
    assert coverage["rows_with_texture"] == 2
    assert coverage["rows_with_density"] == 2
    assert coverage["rows_with_prompt"] == 2
    assert coverage["comparable"] is True
    for term in REPORT_TERMS:
        assert coverage["term_cells"][term] > 0, term

    # An exact synthetic self-match is the zero control.  The two deliberately
    # altered controls and the sparse cup topology must be visible to the
    # production percussion objective.
    controls = report["controls"]
    assert controls["self_zero"]["absolute_residual"]["weighted_perceptual_units"] == 0.0
    assert controls["self_zero"]["passed"] is True
    for name in ("pitch_shift", "texture_shift", "cup_sparse_resonator"):
        assert controls[name]["absolute_residual"]["weighted_perceptual_units"] > 0.0, name
    assert (
        controls["cup_sparse_resonator"]["absolute_residual"]["weighted_perceptual_units"]
        > controls["self_zero"]["absolute_residual"]["weighted_perceptual_units"]
    )
    assert controls["cup_vs_dense"]["cup_worse"] is True
    assert report["objective_weights"] == OBJECTIVE_WEIGHTS


@pytest.mark.parametrize("optimizer", ["coordinate", "cmaes"])
def test_both_existing_optimizers_recover_without_parameter_equality(optimizer: str) -> None:
    """Search wiring improves residuals without assuming identifiable knobs."""

    report = run_benchmark(optimizer=optimizer, max_evals=30, seed=17)
    assert report["fit"]["final_loss"] < report["fit"]["initial_loss"]
    assert _weighted(report, "final") < _weighted(report, "initial")
    assert _weighted(report, "final", withheld=True) < _weighted(report, "initial", withheld=True)


def test_cli_prints_and_writes_versioned_json(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    out = tmp_path / "recovery.json"
    assert (
        main(["--optimizer", "coordinate", "--max-evals", "24", "--seed", "17", "--out", str(out)])
        == 0
    )

    printed = json.loads(capsys.readouterr().out)
    written = json.loads(out.read_text())
    assert printed == written
    assert printed["schema"] == SCHEMA
    assert printed["fit"]["withheld_velocities"] == [112]


def test_an_unmeasured_term_cannot_pass_a_control() -> None:
    rows = measure_rows(ORACLE_VALUES, TRAINING_VELOCITIES)
    oracle_terms = robust_score_terms(rows, rows, percussive=True)
    blind = dict(oracle_terms, density=0.0, density_bins=0.0)
    blind_payload = _residual_payload(blind, rows, 2, oracle_terms=oracle_terms)
    full_payload = _residual_payload(oracle_terms, rows, 2, oracle_terms=oracle_terms)
    worse = dict(oracle_terms, band=6.0)
    worse_payload = _residual_payload(worse, rows, 2, oracle_terms=oracle_terms)

    assert blind_payload["absolute_residual"]["perceptual_units"]["density"] is None
    assert "density" in blind_payload["coverage"]["unavailable_terms"]
    assert self_zero_passed(full_payload) is True
    assert self_zero_passed(blind_payload) is False
    assert cup_worse(worse_payload, full_payload) is True
    assert cup_worse(worse_payload, blind_payload) is None
