"""The two search strategies, over a knob vector and an evaluator.

`optimize` is multi-start coordinate descent with a golden-section line search
per knob: cheap, readable when a knob has an obvious optimum, inherently
serial, and it stalls on knobs that trade against each other. `cma_es` learns
that correlation in a covariance and steps along it, restarting with a doubled
population when a run stagnates, and scores a whole generation in one batch so
the renders run concurrently.

Both loops end on the budget and on nothing else. A converged descent, a
collapsed CMA-ES distribution and an exhausted restart ladder each leave a
start finished rather than the search finished, so what follows is another
start from a fresh random point sharing the same best-so-far.

The `evaluator` argument is duck-typed on purpose, because both an `Evaluator`
and a stage's `SubEvaluator` view of one are passed here. What is required:
callable on a value vector, `evaluate_batch(list_of_vectors)`, and the
`trajectory` / `best_loss` / `best_values` attributes.

`trajectory` carries one entry per distinct candidate the run has SCORED, which
is what `--max-evals` is a budget of and what both loops terminate on. It is not
a count of renders: a candidate an earlier run measured is scored without one.
Budgeting renders instead would let a warm run replay the same search for free
and never stop, and ending a round on "nothing rendered" would stop it after one
pass with a worse answer than the same search made cold. A search is defined by
where it went; what that cost is the evaluator's business.
"""

from __future__ import annotations

import math
import sys

import numpy as np
from knobs import Knob


def _to_opt(knob: Knob, value: float) -> float:
    return math.log(value) if knob.log else value


def _from_opt(knob: Knob, t: float) -> float:
    return math.exp(t) if knob.log else t


#: How far a coordinate pass narrows each knob's bracket for the next pass.
#: A pass searches a window centred on the knob's current value; without the
#: narrowing every pass re-probes the same points, which a cached evaluator
#: answers for free, so the search ends on its second pass however much budget
#: is left. Measured on a synthetic Rosenbrock: budgets of 30, 400 and 4000 all
#: stopped after 13 evaluations at the same loss.
PASS_SHRINK = 0.5

#: Consecutive starts that may consume no budget at all before a loop gives up.
#: Both loops terminate on the budget, so a start that scores nothing new is the
#: only way either can fail to terminate.
BARREN_STARTS = 3


def golden_section(objective, a: float, b: float, max_evals: int, tol: float):
    """Minimise a unimodal 1-D objective on [a, b] within an eval budget.

    Returns (best_t, best_loss). `objective` is assumed cached, so re-probing a
    point is cheap.
    """
    inv_phi = (math.sqrt(5.0) - 1.0) / 2.0
    c = b - inv_phi * (b - a)
    d = a + inv_phi * (b - a)
    fc = objective(c)
    fd = objective(d)
    evals = 2
    best_t, best_f = (c, fc) if fc <= fd else (d, fd)
    while evals < max_evals and (b - a) > tol:
        if fc < fd:
            b, d, fd = d, c, fc
            c = b - inv_phi * (b - a)
            fc = objective(c)
            evals += 1
            if fc < best_f:
                best_t, best_f = c, fc
        else:
            a, c, fc = c, d, fd
            d = a + inv_phi * (b - a)
            fd = objective(d)
            evals += 1
            if fd < best_f:
                best_t, best_f = d, fd
    return best_t, best_f


def _unit_to_value(knob: Knob, u: float) -> float:
    """Map a [0, 1] coordinate to the knob's value, honouring its scale."""
    u = min(max(u, 0.0), 1.0)
    if knob.log:
        return math.exp(math.log(knob.lo) + u * (math.log(knob.hi) - math.log(knob.lo)))
    return knob.lo + u * (knob.hi - knob.lo)


def _value_to_unit(knob: Knob, v: float) -> float:
    if knob.log:
        return (math.log(v) - math.log(knob.lo)) / (math.log(knob.hi) - math.log(knob.lo))
    return (v - knob.lo) / (knob.hi - knob.lo)


def _fold_into_cube(x):
    """Fold a sample back into the unit cube by reflection rather than clipping.

    Clipping stacks every out-of-range sample onto the bound itself, so a wide
    sigma near an edge manufactures pinned knobs: the report reads "at its
    maximum", which is the one annotation that says a search range was too
    narrow. Reflection keeps the bound reachable without ever concentrating mass
    there — a knob whose optimum really is the bound still gets there, because
    the mean converges onto it from the inside.
    """
    y = np.mod(np.abs(x), 2.0)
    return np.where(y > 1.0, 2.0 - y, y)


def cma_es(evaluator, knobs: list[Knob], args) -> list[float]:
    """CMA-ES over the knobs, restarted with a doubled population on stagnation.

    One run converges to whichever basin its start point sits in. Restarting
    from a fresh random point with twice the population (IPOP) is the standard
    way to spend a larger budget on a multi-modal landscape, and a physical
    voice's is multi-modal — an excitation spectrum and a loop decay can trade
    against each other into several distinct configurations that all sound
    approximately right and only one of which is the reference's.

    The restarts share one best-so-far, so the answer is the best point any of
    them found. They also share the budget: `--max-evals` is the total across
    every restart, not per restart.

    **`--restarts` caps the population doublings, not the restarts.** A run ends
    on convergence or stagnation long before the budget does, so a fixed number
    of them discards whatever is left — which is how `--restarts 3` over 4000
    evaluations stopped at 679. Restarting continues from a fresh random point
    at the base population once the ladder of doublings is spent, and the loop
    ends only when the budget is gone or a start scores nothing new.
    """
    n = len(knobs)
    rng = np.random.default_rng(args.seed)
    base_lam = max(args.population if args.population > 0 else 4 + int(3 * math.log(n)), 4)
    lam = base_lam
    x0 = np.array([_value_to_unit(k, k.start_value) for k in knobs], dtype=np.float64)

    # The start point, scored before anything else and outside the budget check.
    # It is the reference every later loss is a ratio of, and in a staged fit it
    # is also the previous stage's result — which has already been rendered, so
    # re-scoring it under this stage's weights is a cache hit. A stage that runs
    # out of budget must still leave that point as its best rather than nothing.
    evaluator([k.start_value for k in knobs])

    attempt = 0
    barren = 0
    while True:
        next_lam = lam * 2 if 0 < attempt <= args.restarts else lam
        if args.max_evals - len(evaluator.trajectory) < max(2, next_lam // 2):
            break
        if attempt > 0:
            lam = next_lam
            x0 = rng.random(n)
            print(f"  restart {attempt}: population {lam}, from a fresh random point",
                  file=sys.stderr)
        before = len(evaluator.trajectory)
        _cma_run(evaluator, knobs, args, x0, lam, rng)
        attempt += 1
        barren = barren + 1 if len(evaluator.trajectory) == before else 0
        if barren >= BARREN_STARTS:
            break

    return list(evaluator.best_values or [k.start_value for k in knobs])


def _cma_run(evaluator, knobs: list[Knob], args, x0, lam: int, rng) -> None:
    """One (mu/mu_w, lambda)-CMA-ES run over the knobs, in unit-cube coordinates.

    Coordinate descent moves one knob at a time, so two knobs that trade
    against each other (a level and the taper that undoes it) send it back and
    forth without either step being wrong on its own. CMA-ES learns that
    correlation in its covariance and steps along it instead — which is what
    makes a runtime-knob spec worth the evaluations it now affords.

    Samples are folded into the cube by reflection rather than clipped,
    resampled or penalised: a knob's range is a modelling decision, and an
    optimum pinned to a bound is a result worth seeing — but only when the
    search put it there, which clipping cannot distinguish from a wide sigma
    near an edge.

    The whole population of a generation is scored in one batch, so the renders
    run concurrently: they are independent subprocesses and nothing in a
    generation depends on another sample of the same generation.
    """
    n = len(knobs)
    xmean = np.array(x0, dtype=np.float64)
    sigma = args.sigma0

    mu = lam // 2
    weights = np.log(mu + 0.5) - np.log(np.arange(1, mu + 1))
    weights /= weights.sum()
    mueff = 1.0 / np.sum(weights**2)

    cc = (4 + mueff / n) / (n + 4 + 2 * mueff / n)
    cs = (mueff + 2) / (n + mueff + 5)
    c1 = 2 / ((n + 1.3) ** 2 + mueff)
    cmu = min(1 - c1, 2 * (mueff - 2 + 1 / mueff) / ((n + 2) ** 2 + mueff))
    damps = 1 + 2 * max(0.0, math.sqrt((mueff - 1) / (n + 1)) - 1) + cs
    chi_n = math.sqrt(n) * (1 - 1 / (4 * n) + 1 / (21 * n * n))

    pc = np.zeros(n)
    ps = np.zeros(n)
    cov = np.eye(n)
    generation = 0
    stall = 0
    # CMA-ES's own stagnation criterion. The previous `6 + n // 4` was short
    # enough to abort a run still adapting its covariance, which only stayed
    # harmless because it was disabled at `--restarts 0`; now that a restart
    # always follows, cutting a converging run short costs the whole answer.
    stall_limit = 10 + int(30 * n / lam)
    since_best = evaluator.best_loss

    while len(evaluator.trajectory) < args.max_evals:
        eigvals, eigvecs = np.linalg.eigh(cov)
        eigvals = np.maximum(eigvals, 1e-20)
        bd = eigvecs @ np.diag(np.sqrt(eigvals))

        take = min(lam, args.max_evals - len(evaluator.trajectory))
        if take < mu:
            break
        xs = [_fold_into_cube(xmean + sigma * (bd @ rng.standard_normal(n)))
              for _ in range(take)]
        before = len(evaluator.trajectory)
        losses = evaluator.evaluate_batch(
            [[_unit_to_value(k, xi) for k, xi in zip(knobs, x)] for x in xs]
        )
        if len(evaluator.trajectory) == before:
            # Every candidate had already been scored, so the budget did not
            # move and the loop has nothing to terminate on. It happens where the
            # distribution has collapsed onto points already visited: the samples
            # round to visited keys, the mean is the weighted mean of those same
            # points and does not move, and sigma shrinks from here rather than
            # recovering. That is convergence, so the run ends and hands what is
            # left to a restart.
            print(f"  gen {generation + 1}: every candidate already evaluated — "
                  f"the search has converged", file=sys.stderr)
            return

        # A generation where nothing scored is a generation with no ranking, and
        # feeding an arbitrary order into the covariance update would teach the
        # search a direction the renders never supported. This happens when the
        # distribution has wandered somewhere every candidate silences the voice
        # or fails to sound the probe notes; the answer is to step back towards
        # the mean, which is the last point known to score.
        if not any(math.isfinite(v) for v in losses):
            sigma = max(sigma * 0.5, 1e-4)
            generation += 1
            print(f"  gen {generation}: no candidate scored — sigma halved to {sigma:.4f}",
                  file=sys.stderr)
            continue

        order = np.argsort(losses)[:mu]
        x_old = xmean.copy()
        xmean = np.sum([weights[i] * xs[k] for i, k in enumerate(order)], axis=0)

        inv_sqrt = eigvecs @ np.diag(1.0 / np.sqrt(eigvals)) @ eigvecs.T
        ps = (1 - cs) * ps + math.sqrt(cs * (2 - cs) * mueff) * (inv_sqrt @ (xmean - x_old)) / sigma
        generation += 1
        hsig = (np.linalg.norm(ps) / math.sqrt(1 - (1 - cs) ** (2 * generation)) / chi_n
                < 1.4 + 2 / (n + 1))
        pc = (1 - cc) * pc + (hsig and 1.0 or 0.0) * math.sqrt(cc * (2 - cc) * mueff) * \
            (xmean - x_old) / sigma

        artmp = np.array([(xs[k] - x_old) / sigma for k in order])
        cov = ((1 - c1 - cmu) * cov
               + c1 * (np.outer(pc, pc) + (0.0 if hsig else cc * (2 - cc)) * cov)
               + cmu * (artmp.T @ np.diag(weights) @ artmp))
        cov = np.triu(cov) + np.triu(cov, 1).T  # keep it symmetric against drift
        sigma *= math.exp((cs / damps) * (np.linalg.norm(ps) / chi_n - 1))
        sigma = float(np.clip(sigma, 1e-4, 1.0))

        # Stagnation: generations that improve on nothing. Ending the run hands
        # the remaining budget to a restart, which is a better use of it than
        # more samples from a distribution that has stopped finding anything.
        if evaluator.best_loss < since_best - 1e-9:
            since_best = evaluator.best_loss
            stall = 0
        else:
            stall += 1
        print(f"  gen {generation}: sigma={sigma:.4f} best={evaluator.best_loss:.4f}"
              f"{f' (stalled {stall})' if stall else ''}", file=sys.stderr)
        if stall >= stall_limit:
            print(f"  no improvement in {stall} generations — ending this run",
                  file=sys.stderr)
            return


def _descend(evaluator, knobs: list[Knob], args, current: list[float]) -> None:
    """Coordinate descent from one start point, until the budget or the point runs out.

    Each pass line-searches one knob at a time over a window centred on that
    knob's current value, and narrows the window by `PASS_SHRINK` for the next
    pass. The first pass sees the whole range, so a knob with an obvious optimum
    is found as directly as it ever was; later passes refine around what the
    earlier ones chose instead of re-probing the same golden-section points, and
    the descent ends once the narrowing has taken a pass down to points already
    scored.

    `current` is updated in place and is the only point the line search moves
    around. It is deliberately not the run's best-so-far: reading that would
    drag every later start onto the first one's basin, which is the whole of
    what a multi-start is for.
    """
    n = len(knobs)
    opt_bounds = [(_to_opt(k, k.lo), _to_opt(k, k.hi)) for k in knobs]
    widths = [1.0] * n
    f_current = evaluator(current)
    while len(evaluator.trajectory) < args.max_evals:
        before = len(evaluator.trajectory)
        for i, knob in enumerate(knobs):
            if len(evaluator.trajectory) >= args.max_evals:
                break
            budget = min(args.per_knob_evals, args.max_evals - len(evaluator.trajectory))
            if budget < 2:
                break
            lo_t, hi_t = opt_bounds[i]
            tol = (hi_t - lo_t) * 1e-3
            half = 0.5 * (hi_t - lo_t) * widths[i]
            centre = _to_opt(knob, current[i])
            a, b = max(lo_t, centre - half), min(hi_t, centre + half)
            if b - a <= tol:
                continue

            def objective(t: float, _i=i, _knob=knob) -> float:
                trial = list(current)
                trial[_i] = min(max(_from_opt(_knob, t), _knob.lo), _knob.hi)
                return evaluator(trial)

            best_t, best_f = golden_section(objective, a, b, budget, tol)
            if best_f <= f_current:
                current[i] = min(max(_from_opt(knob, best_t), knob.lo), knob.hi)
                f_current = best_f
            widths[i] *= PASS_SHRINK
        # A pass that scored nothing new has converged at this width, and the
        # descent ends there rather than re-widening its windows for another
        # sweep of the whole range. Re-widening is the stronger search and that
        # is exactly why it is not done: on the violin at 4000 evaluations it
        # took the training loss from 0.6299 to 0.5864 and the held-out loss
        # from 0.7177 to 0.7788, spending the extra power on the probe. What
        # follows instead is a fresh random start, which cannot.
        if len(evaluator.trajectory) == before:
            break


def optimize(evaluator, knobs: list[Knob], args) -> list[float]:
    """Multi-start coordinate descent; each pass golden-sections one knob.

    One descent converges into whichever basin its start sits in and then has
    nothing left to spend, so the remaining budget goes to a descent from a
    fresh random point — the same argument IPOP makes for CMA-ES, and the same
    shared best-so-far. `--max-evals` is the total across every start.

    Inherently serial — a golden-section step chooses its next probe from the
    previous one's result — so `--workers` does not speed this optimiser up.
    Use `--optimizer cmaes` when there are workers to spend.
    """
    rng = np.random.default_rng(args.seed)
    n = len(knobs)
    current = [k.start_value for k in knobs]
    evaluator(current)  # baseline

    start = 0
    barren = 0
    while len(evaluator.trajectory) < args.max_evals:
        if start > 0:
            current = [_unit_to_value(k, u) for k, u in zip(knobs, rng.random(n))]
            print(f"  start {start}: descending from a fresh random point", file=sys.stderr)
        before = len(evaluator.trajectory)
        _descend(evaluator, knobs, args, current)
        barren = 0 if len(evaluator.trajectory) > before else barren + 1
        start += 1
        if barren >= BARREN_STARTS:
            break
    return list(evaluator.best_values or current)
