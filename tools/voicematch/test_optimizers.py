"""What the two search loops are required to do with a budget.

Every case here is about reach rather than about quality: a search that stops
with budget left, or that concentrates samples on a bound, produces a result
that reads exactly like a converged one. The objectives are synthetic so the
answer is known, and each test states the number of evaluations it observed
rather than only a comparison, so a case that stops reaching says so.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import optimizers
from knobs import Knob, format_value
from optimizers import (
    BARREN_STARTS,
    _fold_into_cube,
    cma_es,
    golden_section,
    optimize,
)


def _args(**kwargs) -> argparse.Namespace:
    base = {"max_evals": 400, "per_knob_evals": 6, "population": 0, "sigma0": 0.25,
            "seed": 0, "restarts": 0}
    base.update(kwargs)
    return argparse.Namespace(**base)


def _knobs(n: int, lo: float = 0.0, hi: float = 1.0, start: float | None = None):
    mid = 0.5 * (lo + hi) if start is None else start
    return [Knob(label=f"k{i}.v", lo=lo, hi=hi, log=False, start_value=mid, tunable=f"k{i}.v")
            for i in range(n)]


class Recorder:
    """The evaluator contract both loops are duck-typed on, over a plain function.

    `trajectory` grows once per DISTINCT candidate, keyed through `format_value`
    exactly as `Evaluator` keys its store, so a re-probe is free here for the
    same reason it is free there.
    """

    def __init__(self, objective):
        self.objective = objective
        self.trajectory: list[tuple[float, float, str]] = []
        self.seen: set[tuple[str, ...]] = set()
        self.calls = 0
        self.best_loss = math.inf
        self.best_values: list[float] | None = None
        self.workers = 1
        self.quiet = True

    def __call__(self, values) -> float:
        self.calls += 1
        loss = float(self.objective(values))
        key = tuple(format_value(v) for v in values)
        if key not in self.seen:
            self.seen.add(key)
            if loss < self.best_loss:
                self.best_loss, self.best_values = loss, list(values)
            self.trajectory.append((self.best_loss, loss, "fit"))
        return loss

    def evaluate_batch(self, batch) -> list[float]:
        return [self(values) for values in batch]


def rosenbrock(values) -> float:
    """The standard 2-D Rosenbrock on [0, 1], minimum 0 at (0.6, 0.6)."""
    x, y = (2.0 * v - 0.2 for v in values)
    return (1.0 - x) ** 2 + 100.0 * (y - x * x) ** 2


def ellipse(values) -> float:
    """A rotated ill-conditioned quadratic: coordinate-hostile, convex."""
    v = np.asarray(values, dtype=np.float64) - 0.37
    n = len(v)
    rot = np.eye(n)
    for i in range(n - 1):
        c, s = math.cos(0.4), math.sin(0.4)
        block = np.eye(n)
        block[i, i] = block[i + 1, i + 1] = c
        block[i, i + 1], block[i + 1, i] = -s, s
        rot = block @ rot
    w = rot @ v
    return float(sum((10.0 ** (3.0 * i / max(1, n - 1))) * wi * wi for i, wi in enumerate(w)))


# --------------------------------------------------------------------------- #
# A budget is spent, not abandoned
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize("budget", [30, 400, 4000])
def test_coordinate_descent_spends_the_budget_it_is_given(budget):
    """A larger budget has to buy a larger search, or `--max-evals` means nothing.

    Every pass used to line-search the knob's whole range from scratch, so pass
    two re-probed pass one's points, scored nothing new and broke the loop:
    budgets of 30, 400 and 4000 all stopped after 13 evaluations at the same
    loss. The bound below is deliberately loose — what is asserted is that the
    search kept going, not how well it did.
    """
    rec = Recorder(rosenbrock)
    optimize(rec, _knobs(2), _args(max_evals=budget))
    assert len(rec.trajectory) >= 0.8 * budget, (
        f"budget {budget} bought only {len(rec.trajectory)} evaluations"
    )


def test_coordinate_descent_improves_with_the_budget():
    """The reason to spend more: on the same problem, more budget wins."""
    losses = {}
    for budget in (30, 400, 4000):
        rec = Recorder(rosenbrock)
        optimize(rec, _knobs(2), _args(max_evals=budget))
        losses[budget] = rec.best_loss
    assert losses[400] < losses[30]
    assert losses[4000] < losses[400]


def test_cma_es_restarts_until_the_budget_is_gone_not_until_the_ladder_is():
    """`--restarts N` caps the population doublings; it may not cap the search.

    `range(restarts + 1)` returned with the budget untouched once the ladder was
    spent, so raising `--restarts` made the answer worse: restarts=3 over 4000
    stopped at 679 evaluations where restarts=0 ran 558 and reached a far better
    point. Both must now spend what they were given.
    """
    spent = {}
    for restarts in (0, 3):
        rec = Recorder(ellipse)
        cma_es(rec, _knobs(8), _args(max_evals=1200, restarts=restarts))
        spent[restarts] = len(rec.trajectory)
    assert spent[0] >= 1100, f"restarts=0 spent {spent[0]} of 1200"
    assert spent[3] >= 1100, f"restarts=3 spent {spent[3]} of 1200"


@pytest.mark.parametrize("optimizer", [optimize, cma_es])
def test_a_larger_budget_is_never_a_shorter_search(optimizer):
    rec_small = Recorder(ellipse)
    optimizer(rec_small, _knobs(6), _args(max_evals=200))
    rec_big = Recorder(ellipse)
    optimizer(rec_big, _knobs(6), _args(max_evals=2000))
    assert len(rec_big.trajectory) > len(rec_small.trajectory)


# --------------------------------------------------------------------------- #
# Multi-start
# --------------------------------------------------------------------------- #
def test_coordinate_descent_leaves_a_local_basin():
    """Two basins, the deeper one unreachable from the start by descent alone.

    The start sits inside the shallow basin and every coordinate step away from
    it climbs, so a single descent can only return the shallow optimum. A budget
    large enough for a second start must find the deeper one.
    """

    def two_basins(values):
        x = values[0]
        shallow = 0.20 + 30.0 * (x - 0.20) ** 2
        deep = 60.0 * (x - 0.85) ** 2
        return min(shallow, deep)

    knobs = _knobs(1, start=0.20)
    one = Recorder(two_basins)
    optimize(one, knobs, _args(max_evals=14, per_knob_evals=6))
    assert one.best_loss > 0.19, "the single-start control already escaped, so it proves nothing"

    many = Recorder(two_basins)
    optimize(many, knobs, _args(max_evals=400, per_knob_evals=6))
    assert many.best_loss < 1e-3, f"no start reached the deep basin (best {many.best_loss})"


def test_a_later_start_is_not_dragged_onto_the_running_best():
    """A start that line-searches around the best-so-far is not a second start.

    The objective is flat everywhere but in one narrow well, so the only thing
    a descent can report is where it walked. Two starts must visit two
    neighbourhoods.
    """
    visited: list[float] = []

    def flat(values):
        visited.append(values[0])
        return 1.0 if abs(values[0] - 0.05) > 0.02 else 0.0

    optimize(Recorder(flat), _knobs(1, start=0.5), _args(max_evals=60, per_knob_evals=4))
    assert max(visited) - min(visited) > 0.5


# --------------------------------------------------------------------------- #
# A bound is reported because the search chose it
# --------------------------------------------------------------------------- #
def test_samples_are_folded_into_the_cube_not_stacked_on_its_faces():
    rng = np.random.default_rng(7)
    raw = rng.normal(0.5, 0.8, size=20000)
    folded = _fold_into_cube(raw)
    assert folded.min() >= 0.0
    assert folded.max() <= 1.0
    clipped = np.clip(raw, 0.0, 1.0)
    on_face = int(np.sum((clipped <= 0.0) | (clipped >= 1.0)))
    assert on_face > 2000, "the control never left the cube, so it proves nothing"
    assert int(np.sum((folded <= 0.0) | (folded >= 1.0))) == 0


def test_folding_is_an_involution_on_the_cube_and_periodic_outside_it():
    inside = np.array([0.0, 0.25, 0.5, 1.0])
    assert np.allclose(_fold_into_cube(inside), inside)
    assert np.allclose(_fold_into_cube(np.array([1.3, -0.2, 2.5, 3.5])),
                       np.array([0.7, 0.2, 0.5, 0.5]))


def _pinned(knobs, values) -> list[str]:
    return [k.label for k, v in zip(knobs, values)
            if v <= k.lo + 1e-3 or v >= k.hi - 1e-3]


def test_cma_es_does_not_manufacture_a_pin_near_an_interior_optimum(monkeypatch):
    """An optimum a percent off a bound must not be reported ON the bound.

    A pin is the annotation that says a search range was too narrow, and it is
    the most expensive thing this tool can get wrong, so one the sampler
    produced rather than the landscape is a false finding about the spec.

    The control is the clipping the fold replaced, installed over
    `_fold_into_cube` for one run: it has to pin, or the fold's clean result
    says nothing. Both runs see the same seeds, the same budget and the same
    objective, whose optimum is interior by construction.

    What is asserted is the separation between the two, not that the fold pins
    nothing. At this budget the search's own residual is the same order as the
    pin threshold, so a coordinate reaches it without anything having been
    stacked on the face -- over 20 seeds the fold pins about 1% of coordinates
    and clipping over half, and which coordinates those are moves with the host's
    LAPACK. A budget large enough to separate them outright converges the clipped
    run off the face too, which would leave the control proving nothing.
    """
    def near_edge(values):
        return float(sum((v - 0.01) ** 2 for v in values))

    seeds, dimensions = 20, 12
    clipped, folded = [], []
    for seed in range(seeds):
        knobs = _knobs(dimensions)
        monkeypatch.setattr(optimizers, "_fold_into_cube",
                            lambda x: np.clip(x, 0.0, 1.0))
        clipped += _pinned(knobs, cma_es(Recorder(near_edge), knobs,
                                         _args(max_evals=300, sigma0=0.3, seed=seed)))
        knobs = _knobs(dimensions)
        monkeypatch.undo()
        folded += _pinned(knobs, cma_es(Recorder(near_edge), knobs,
                                        _args(max_evals=300, sigma0=0.3, seed=seed)))

    total = seeds * dimensions
    assert len(clipped) >= total // 4, (
        f"the control pinned {len(clipped)} of {total}, so it never reproduced the defect"
    )
    assert len(folded) * 5 <= len(clipped), (
        f"pinned {len(folded)} of {total} against the control's {len(clipped)}, "
        f"where the optimum is interior: {folded}"
    )


# --------------------------------------------------------------------------- #
# Termination, which the budget alone no longer guarantees
# --------------------------------------------------------------------------- #
class _NothingNew:
    """Answers everything from a store and never grows its trajectory.

    The one state in which neither loop can terminate on the budget, so each has
    to terminate on the starts instead.
    """

    def __init__(self, start):
        self.trajectory: list[tuple[float, float, str]] = []
        self.best_loss = 1.0
        self.best_values = list(start)
        self.workers = 1
        self.quiet = True
        self.calls = 0

    def __call__(self, values) -> float:
        self.calls += 1
        assert self.calls < 20000, "the search did not terminate"
        return 1.0

    def evaluate_batch(self, batch) -> list[float]:
        return [self(v) for v in batch]


@pytest.mark.parametrize("optimizer", [optimize, cma_es])
def test_a_search_that_can_score_nothing_new_still_terminates(optimizer):
    knobs = _knobs(3)
    rec = _NothingNew([k.start_value for k in knobs])
    assert optimizer(rec, knobs, _args(max_evals=4000)) == [k.start_value for k in knobs]
    assert rec.calls > 0


def test_the_barren_guard_is_the_thing_that_stops_it():
    """Stated so the constant cannot be lowered to 0 without a test going red."""
    assert BARREN_STARTS >= 1


# --------------------------------------------------------------------------- #
# Determinism: the zero control every later comparison rests on
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize("optimizer", [optimize, cma_es])
def test_two_runs_of_one_seed_are_bit_identical(optimizer):
    def run():
        rec = Recorder(ellipse)
        best = optimizer(rec, _knobs(5), _args(max_evals=300, seed=11))
        return best, len(rec.trajectory), rec.best_loss

    first, second = run(), run()
    assert first == second


@pytest.mark.parametrize("optimizer", [optimize, cma_es])
def test_two_seeds_do_not_produce_one_search(optimizer, capsys):
    """The control for the control: if the seed reached nothing, identity is vacuous.

    Compared on where each run went rather than on what it returned — a convex
    objective has one answer, so two seeds agreeing about it says the search
    worked, while two seeds visiting the same points would say the seed is dead.

    The budget has to be large enough for the coordinate descent to finish its
    first start, because that one begins at the knobs' defaults and is the same
    under every seed. At 300 evaluations it is not, and the test read as a dead
    seed when what it had measured was a descent still descending — so the run
    asserts it saw a second start before comparing anything.
    """
    def run(seed):
        rec = Recorder(ellipse)
        optimizer(rec, _knobs(5), _args(max_evals=4000, seed=seed))
        return sorted(rec.seen), capsys.readouterr().err

    first, first_log = run(11)
    second, _ = run(12)
    if optimizer is optimize:
        assert "descending from a fresh random point" in first_log, (
            "the descent never restarted, so the seed was never consulted"
        )
    assert first != second


# --------------------------------------------------------------------------- #
# golden_section itself
# --------------------------------------------------------------------------- #
def test_golden_section_narrows_a_unimodal_objective():
    calls = []

    def f(t):
        calls.append(t)
        return (t - 0.3) ** 2

    best_t, best_f = golden_section(f, 0.0, 1.0, 20, 1e-4)
    assert abs(best_t - 0.3) < 1e-2
    assert best_f < 1e-4
    assert len(calls) <= 20
