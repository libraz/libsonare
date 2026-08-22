"""The two search strategies, over a knob vector and an evaluator.

`optimize` is coordinate descent with a golden-section line search per knob:
cheap, readable when a knob has an obvious optimum, inherently serial, and it
stalls on knobs that trade against each other. `cma_es` learns that correlation
in a covariance and steps along it, restarting with a doubled population when a
run stagnates, and scores a whole generation in one batch so the renders run
concurrently.

The `evaluator` argument is duck-typed on purpose, because both an `Evaluator`
and a stage's `SubEvaluator` view of one are passed here. What is required:
callable on a value vector, `evaluate_batch(list_of_vectors)`, and the
`trajectory` / `best_loss` / `best_values` attributes.
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
    """
    n = len(knobs)
    rng = np.random.default_rng(args.seed)
    lam = max(args.population if args.population > 0 else 4 + int(3 * math.log(n)), 4)
    x0 = np.array([_value_to_unit(k, k.start_value) for k in knobs], dtype=np.float64)

    # The start point, scored before anything else and outside the budget check.
    # It is the reference every later loss is a ratio of, and in a staged fit it
    # is also the previous stage's result — which has already been rendered, so
    # re-scoring it under this stage's weights is a cache hit. A stage that runs
    # out of budget must still leave that point as its best rather than nothing.
    evaluator([k.start_value for k in knobs])

    for attempt in range(args.restarts + 1):
        if len(evaluator.trajectory) >= args.max_evals:
            break
        if attempt > 0:
            lam *= 2
            x0 = rng.random(n)
            print(f"  restart {attempt}: population {lam}, from a fresh random point",
                  file=sys.stderr)
        _cma_run(evaluator, knobs, args, x0, lam, rng)

    return list(evaluator.best_values or [k.start_value for k in knobs])


def _cma_run(evaluator, knobs: list[Knob], args, x0, lam: int, rng) -> None:
    """One (mu/mu_w, lambda)-CMA-ES run over the knobs, in unit-cube coordinates.

    Coordinate descent moves one knob at a time, so two knobs that trade
    against each other (a level and the taper that undoes it) send it back and
    forth without either step being wrong on its own. CMA-ES learns that
    correlation in its covariance and steps along it instead — which is what
    makes a runtime-knob spec worth the evaluations it now affords.

    Samples are clipped into the cube rather than resampled or penalised: a
    knob's range is a modelling decision, and an optimum pinned to a bound is a
    result worth seeing rather than an infeasibility to hide.

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
    stall_limit = 6 + n // 4
    since_best = evaluator.best_loss

    while len(evaluator.trajectory) < args.max_evals:
        eigvals, eigvecs = np.linalg.eigh(cov)
        eigvals = np.maximum(eigvals, 1e-20)
        bd = eigvecs @ np.diag(np.sqrt(eigvals))

        take = min(lam, args.max_evals - len(evaluator.trajectory))
        if take < mu:
            break
        xs = [np.clip(xmean + sigma * (bd @ rng.standard_normal(n)), 0.0, 1.0)
              for _ in range(take)]
        losses = evaluator.evaluate_batch(
            [[_unit_to_value(k, xi) for k, xi in zip(knobs, x)] for x in xs]
        )

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
        if stall >= stall_limit and args.restarts > 0:
            print(f"  no improvement in {stall} generations — ending this run",
                  file=sys.stderr)
            return


def optimize(evaluator, knobs: list[Knob], args) -> list[float]:
    """Coordinate descent over knobs; each pass golden-sections one knob.

    Inherently serial — a golden-section step chooses its next probe from the
    previous one's result — so `--workers` does not speed this optimiser up.
    Use `--optimizer cmaes` when there are workers to spend.
    """
    current = [k.start_value for k in knobs]
    evaluator(current)  # baseline
    while len(evaluator.trajectory) < args.max_evals:
        improved = False
        for i, knob in enumerate(knobs):
            if len(evaluator.trajectory) >= args.max_evals:
                break
            budget = min(args.per_knob_evals, args.max_evals - len(evaluator.trajectory))
            if budget < 2:
                break
            a, b = _to_opt(knob, knob.lo), _to_opt(knob, knob.hi)
            tol = (b - a) * 1e-3

            def objective(t: float, _i=i, _knob=knob) -> float:
                trial = list(evaluator.best_values or current)
                trial[_i] = min(max(_from_opt(_knob, t), _knob.lo), _knob.hi)
                return evaluator(trial)

            best_t, best_f = golden_section(objective, a, b, budget, tol)
            if best_f < evaluator.best_loss + 1e-9 and evaluator.best_values is not None:
                best_val = min(max(_from_opt(knob, best_t), knob.lo), knob.hi)
                if abs(current[i] - best_val) > 0:
                    improved = True
                current[i] = best_val
        current = list(evaluator.best_values or current)
        if not improved:
            break
    return list(evaluator.best_values or current)
