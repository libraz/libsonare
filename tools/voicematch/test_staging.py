"""What the screening report has to say about the bar that cut the knobs.

The count alone cannot be read: 27 of 34 kept is a good screening when the bar
sits in a gap and a coin flip when it sits inside a continuum, and the two print
the same line. These cases pin the readings that tell them apart, and the last
one pins the dilution invariance the `share` column exists for.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import pytest
from knobs import Knob
from loss_weights import LossWeights
from staging import _point_effect, report_effect_distribution, screen_knobs


def _report(effects, threshold, capsys) -> str:
    report_effect_distribution(effects, threshold)
    return capsys.readouterr().err


def _continuum(n: int = 34):
    """Effects spread evenly over four decades — no gap anywhere."""
    return [(f"k{i}", 10 ** (-5 + 4 * i / (n - 1))) for i in range(n)]


def _cliff():
    """Seven knobs that move nothing and ten that move a lot."""
    return [(f"dead{i}", 1e-7) for i in range(7)] + [
        (f"live{i}", 0.05 + 0.01 * i) for i in range(10)
    ]


def test_a_bar_inside_a_continuum_is_reported_as_one(capsys):
    out = _report(_continuum(), 0.002, capsys)
    assert "<- the bar is in this bucket" in out
    straddle = next(line for line in out.splitlines() if "straddling the bar" in line)
    assert "1.3x apart" in straddle, straddle


def test_a_bar_in_a_gap_is_reported_as_one(capsys):
    out = _report(_cliff(), 0.002, capsys)
    straddle = next(line for line in out.splitlines() if "straddling the bar" in line)
    assert "500000.0x apart" in straddle, straddle
    # Every threshold on the ladder keeps the same ten knobs, which is the
    # reading that says the exact value does not matter here.
    ladder = next(line for line in out.splitlines() if "another bar would keep" in line)
    assert ladder.count("->10") == 7, ladder


def test_the_share_column_is_invariant_to_dilution_and_the_count_is_not(capsys):
    """A saturated term scales every effect together; the absolute bar does not.

    This is the whole reason the share reading is printed. The control is the
    count on the same data: it MUST move, or the invariance below is invariance
    over a change that did not happen.
    """
    effects = _continuum()
    clean = _report(effects, 0.002, capsys)
    diluted = _report([(label, e * 0.572) for label, e in effects], 0.002, capsys)

    def line(out, needle):
        return next(ln for ln in out.splitlines() if needle in ln)

    clean_share = line(clean, "knobs kept by share:").split("knobs kept by share:")[1]
    diluted_share = line(diluted, "knobs kept by share:").split("knobs kept by share:")[1]
    assert clean_share == diluted_share

    clean_ladder = line(clean, "another bar would keep")
    diluted_ladder = line(diluted, "another bar would keep")
    assert clean_ladder != diluted_ladder, (
        "the control did not move, so the invariance above is over nothing"
    )
    assert "0.002->15" in clean_ladder, clean_ladder
    assert "0.002->13" in diluted_ladder, diluted_ladder


def test_an_empty_probe_prints_nothing_rather_than_dividing_by_a_largest(capsys):
    assert _report([], 0.002, capsys) == ""


def test_a_knob_that_moved_nothing_does_not_divide_the_straddle(capsys):
    out = _report([("dead", 0.0), ("live", 0.5)], 0.002, capsys)
    assert "moved nothing at all" in out


def _screen_args(threshold=0.001, max_evals=1000):
    return argparse.Namespace(screen_threshold=threshold, max_evals=max_evals)


class _TermEvaluator:
    """A cache-bearing evaluator whose scalar loss deliberately cancels terms."""

    def __init__(self, terms):
        self._terms = terms
        self.cache = {}
        self.quiet = False

    def key(self, values):
        return tuple(values)

    def __call__(self, values):
        terms = self._terms(values)
        self.cache[self.key(values)] = terms
        return 0.0

    def evaluate_batch(self, points):
        return [self(values) for values in points]


def _knob(label, start=0.5):
    return Knob(label, 0.0, 1.0, False, start, tunable=f"fake.{label}")


def test_screening_uses_term_deltas_when_the_scalar_loss_cancels(capsys):
    knobs = [_knob("a")]

    def terms(values):
        return {"tone": values[0], "ring": -values[0]}

    kept = screen_knobs(_TermEvaluator(terms), knobs, _screen_args())
    assert kept == [0]
    assert "term" in capsys.readouterr().err


def test_screening_reads_a_calibrated_objective_through_its_term_contributions():
    loss = LossWeights({"cents": 1.0})
    baseline = {"cents": 2.0, "harm": 0.0}
    loss.calibrate(baseline)
    evaluator = _TermEvaluator(lambda values: {})
    evaluator.loss = loss
    # `harm` carries no weight, so only the weighted cents movement counts, in start-relative units.
    candidate = {"cents": 3.0, "harm": 50.0}
    assert _point_effect(evaluator, 1.0, 1.0, baseline, candidate) == pytest.approx(0.5)


def test_screening_probes_jointly_inert_switches_under_activation(capsys):
    knobs = [_knob("switch-a"), _knob("switch-b")]

    def response(values):
        # Neither switch moves by itself. Together at the high end they enable
        # the mechanism, and taking either one back out disables it.
        return 0.0 if values == [1.0, 1.0] else 1.0

    class _ScalarEvaluator:
        quiet = False

        def __call__(self, values):
            return response(values)

        def evaluate_batch(self, points):
            return [self(values) for values in points]

    kept = screen_knobs(_ScalarEvaluator(), knobs, _screen_args(max_evals=20))
    assert kept == [0, 1]
    assert "conditional" in capsys.readouterr().err


class _TrajectoryEvaluator:
    """Small evaluator that exposes the budgeted candidate trajectory."""

    def __init__(self, response, spent=0):
        self.response = response
        self.trajectory = [(0.0, 0.0, "fit")] * spent
        self.calls = []
        self.quiet = False

    def __call__(self, values):
        self.calls.append(list(values))
        self.trajectory.append((0.0, self.response(values), "fit"))
        return self.response(values)

    def evaluate_batch(self, points):
        return [self(values) for values in points]


def test_screening_keeps_all_when_the_initial_probe_does_not_fit_remaining_budget(capsys):
    knobs = [_knob(f"k{i}") for i in range(3)]
    evaluator = _TrajectoryEvaluator(lambda values: sum(values), spent=2)

    # Three knobs need baseline + six endpoints.  Only five of the seven
    # allowed trajectory slots remain, so a partial screen must not discard an
    # unprobed knob or exceed the caller's budget.
    kept = screen_knobs(evaluator, knobs, _screen_args(max_evals=7))

    assert kept == [0, 1, 2]
    assert len(evaluator.trajectory) == 2
    assert evaluator.calls == []
    assert "retaining all 3 knobs without probes" in capsys.readouterr().err


def test_conditional_screening_uses_the_endpoint_that_actually_activates_the_feature(capsys):
    knobs = [_knob("activator", start=0.2), _knob("conditional")]

    def response(values):
        # The useful endpoint is the one near the start (lo=0).  The farther
        # endpoint (hi=1) leaves the feature disabled.  The second knob only
        # moves the loss inside that low-end activation context.
        return values[1] if values[0] == 0.0 else 1.0

    kept = screen_knobs(_TrajectoryEvaluator(response), knobs, _screen_args(max_evals=20))

    assert kept == [0, 1]
    assert "conditional" in capsys.readouterr().err


def test_screening_retains_a_knob_with_a_nonfinite_endpoint(capsys):
    knobs = [_knob("unstable"), _knob("inert")]

    def response(values):
        # The low endpoint is invalid, even though the interior/default is
        # finite. That is insufficient evidence to classify this knob as dead.
        return math.inf if values == [0.0, 0.5] else 1.0

    kept = screen_knobs(_TrajectoryEvaluator(response), knobs, _screen_args(max_evals=20))

    assert kept == [0]
    assert "nonfinite" in capsys.readouterr().err


def test_screening_retains_all_knobs_when_the_baseline_is_nonfinite(capsys):
    knobs = [_knob("a"), _knob("b")]
    evaluator = _TrajectoryEvaluator(lambda values: math.inf)

    kept = screen_knobs(evaluator, knobs, _screen_args(max_evals=20))

    assert kept == [0, 1]
    assert len(evaluator.calls) == 1
    assert "baseline loss is nonfinite" in capsys.readouterr().err
