"""A search budget below what the optimizer needs is refused, and a short run is labelled.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_autofit_budget.py -q
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit
from autofit_test_fixtures import _fit_args
from knobs import Knob
from report import NOT_A_SEARCH, optimizer_minimum, search_effort


class _Reached(Exception):
    """Raised by the stubbed build step: the run got past the budget check."""


def _knobs(n: int) -> list[Knob]:
    return [
        Knob(label=f"k{i}", lo=0.0, hi=1.0, log=False, start_value=0.5, tunable=f"k{i}")
        for i in range(n)
    ]


def _run(monkeypatch, tmp_path, *, n=29, **flags):
    def reached(*a, **k):
        raise _Reached

    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", []))
    monkeypatch.setattr(autofit, "resolve_probe", lambda args: None)
    monkeypatch.setattr(autofit, "apply_spec_weights", lambda args, argv: None)
    monkeypatch.setattr(autofit, "load_spec", lambda path: [{"tunable": f"k{i}"} for i in range(n)])
    monkeypatch.setattr(autofit, "build_knobs", lambda spec, pristine, catalogue: _knobs(n))
    monkeypatch.setattr(autofit, "configure_build", reached)
    monkeypatch.setattr(autofit, "resolve_corpus", lambda args: None)
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(autofit, "knob_bounds", lambda knobs, cat: {})
    monkeypatch.setattr(autofit, "declared_ranges", lambda *a: ({}, []))
    monkeypatch.setattr(autofit, "clamp_check", lambda *a: ([], []))
    monkeypatch.setattr(
        autofit,
        "oracle_reference",
        lambda args: ([], np.zeros(4, dtype=np.float32), None, None),
    )
    base = dict(  # noqa: C408
        spec="ignored",
        program=0,
        drum_note=None,
        dump_knobs=False,
        program_only=False,
        bank=0,
        build_dir="build-budget-test",
        screen=False,
        stages=False,
        diagnose=False,
        grid=0,
        optimizer="coord",
        max_evals=30,
        metric_threads=0,
        dry_run=True,
        allow_dirty_src=True,
        drum_gate_ms=0,
    )
    base.update(flags)
    return autofit.run(_fit_args(**base))


def test_the_minimum_table():
    assert optimizer_minimum("coord", 29) == 59
    # cmaes: start point + two populations of 4 + floor(3 ln 29) = 14, above 2n+1 only for small n.
    assert optimizer_minimum("cmaes", 29) == 59
    assert optimizer_minimum("cmaes", 3) == 1 + 2 * 7
    assert optimizer_minimum("cmaes", 29, population=40) == 81
    assert optimizer_minimum("coord", 0) == 0


def test_thirty_evals_over_twenty_nine_knobs_is_refused(monkeypatch, tmp_path, capsys):
    with pytest.raises(ValueError, match=r"--max-evals 30 is below the 59.*--short-run"):
        _run(monkeypatch, tmp_path)
    assert "search budget: 29 knobs, max_evals=30" in capsys.readouterr().err


def test_a_deliberate_short_run_gets_past_the_refusal(monkeypatch, tmp_path, capsys):
    with pytest.raises(_Reached):
        _run(monkeypatch, tmp_path, short_run=True)
    assert "is not a search" in capsys.readouterr().err


def test_a_sufficient_budget_prints_the_line_and_runs(monkeypatch, tmp_path, capsys):
    with pytest.raises(_Reached):
        _run(monkeypatch, tmp_path, max_evals=59)
    assert "search budget: 29 knobs, max_evals=59, 2.0 evals per knob; coord needs at least 59" in (
        capsys.readouterr().err
    )


def test_cmaes_is_refused_on_its_own_minimum(monkeypatch, tmp_path):
    with pytest.raises(ValueError, match=r"below the 15 that --optimizer cmaes"):
        _run(monkeypatch, tmp_path, n=3, optimizer="cmaes", max_evals=14)


def test_modes_that_do_not_search_are_exempt(monkeypatch, tmp_path):
    for flag in ({"diagnose": True}, {"grid": 3}):
        # Past the budget check the run goes on to the (stubbed) build.
        with pytest.raises(_Reached):
            _run(monkeypatch, tmp_path, **flag)


def test_the_flag_is_on_the_command_line():
    assert "--short-run" in autofit.build_parser().format_help()


def _effort(evaluations: int, **args):
    evaluator = SimpleNamespace(trajectory=[(i, 1.0) for i in range(evaluations)])
    ns = argparse.Namespace(optimizer="coord", max_evals=30, minimum_evals=59, **args)
    return search_effort(evaluator, ns, 29)


def test_the_record_carries_the_minimum_and_labels_a_short_run():
    effort = _effort(30)
    assert effort["minimum_evals"] == 59
    assert effort["optimizer"] == "coord"
    assert effort["search"] == NOT_A_SEARCH
    json.dumps(effort)


def test_a_run_that_spent_the_minimum_is_a_search():
    assert _effort(59)["search"] == "search"
