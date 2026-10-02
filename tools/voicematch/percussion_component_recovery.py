"""Recover stick and noisy-body strengths from a controlled percussion oracle.

These deterministic audio fixtures test the fitting objective, not the acoustic
accuracy of the C++ engines. Every candidate uses production hit measurements,
weights, robust aggregation and the ordinary coordinate/CMA optimizer.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]

from autofit_resolve import resolve_probe
from fit_quality import quality_report
from knobs import Knob, format_value
from loss_aggregate import robust_score_terms
from loss_weights import LossWeights, cli_weights
from metrics_hit import analyze_hit
from metrics_note import level_of
from optimizers import cma_es, optimize
from smf import Note

SR = 24_000
DURATION = 1.3
TRAIN = (64, 100)
HELD = (112,)
ORACLE = (0.6, 0.7)
START = (0.03, 0.03)


def component_hit(values, velocity=100, *, family="snare", control="", seed=29):
    """Audio control with independent contact and noisy-body contributions."""
    t = np.arange(int(SR * DURATION)) / SR
    rng = np.random.default_rng(seed + velocity)
    noise = rng.standard_normal(len(t))
    spectrum = np.fft.rfft(noise)
    frequencies = np.fft.rfftfreq(len(t), 1 / SR)
    spectrum[(frequencies < 800) | (frequencies > 7500)] = 0
    hiss = np.fft.irfft(spectrum, n=len(t))
    hiss /= max(np.std(hiss), 1e-12)
    if control == "cup":
        modes = (1200, 1824, 3156)
    elif family == "metal":
        modes = tuple(np.geomspace(380, 6500, 28))
    elif family == "kick":
        modes = (55, 88, 121, 174)
    else:
        modes = (160, 250, 397, 535, 575, 740)
    decay = 0.085 if family != "metal" else 0.15
    body = sum(
        np.sin(2 * np.pi * frequency * t + 0.19 * index)
        * np.exp(-t / (decay / (1 + 0.07 * index)))
        / math.sqrt(len(modes))
        for index, frequency in enumerate(modes)
    )
    stick, noisy_body = map(float, values)
    contact = hiss * np.exp(-t / 0.0025)
    rattle = hiss * (1 - np.exp(-t / 0.001)) * np.exp(-t / 0.055)
    if control == "hand":
        stick = noisy_body = 0.0
        body *= 1 - np.exp(-t / 0.018)
    if control == "no_stick":
        stick = 0.0
    if control == "no_rattle":
        noisy_body = 0.0
    force = velocity / 127
    signal = force * body + force**0.7 * stick * contact + force**1.6 * noisy_body * rattle
    # Synthetic unequal velocity laws exercise more than global output gain.
    # They are fixture definitions, not estimates of an acoustic instrument.
    return signal + rng.standard_normal(len(t)) * 1e-6


def rows(values, velocities, *, family="snare", control="", seed=29):
    out = []
    for velocity in velocities:
        audio = component_hit(values, velocity, family=family, control=control, seed=seed)
        note = Note(38, velocity, 0.0, 0.05)
        row = analyze_hit(audio, SR, note, DURATION).to_dict()
        row.update(level_of(audio, SR, note, DURATION))
        out.append(row)
    return out


class ComponentEvaluator:
    def __init__(self, family, seed, include_evolution=True):
        self.family, self.seed = family, seed
        self.oracle = rows(ORACLE, TRAIN, family=family, seed=seed)
        args = SimpleNamespace(
            program=0,
            drum_note=38,
            diagnose=True,
            pattern="drum",
            notes="38",
            velocities="64,100",
            corpus="",
            spec="auto",
            oracle_wav="",
            validate_notes="",
            validate_velocities="",
        )
        resolve_probe(args)
        weights = cli_weights(args)
        if not include_evolution:
            weights = {
                name: weight
                for name, weight in weights.items()
                if name not in ("evolve", "diffuse")
            }
        self.loss = LossWeights(weights)
        self.cache = {}
        self.trajectory = []
        self.best_loss = math.inf
        self.best_values = None
        self.quiet = True
        self.workers = 1

    def __call__(self, values):
        key = tuple(format_value(float(value)) for value in values)
        if key not in self.cache:
            terms = robust_score_terms(
                rows(values, TRAIN, family=self.family, seed=self.seed),
                self.oracle,
                percussive=True,
            )
            if terms is None:
                raise RuntimeError("Controlled percussion fixture became unscorable")
            if self.loss.scales is None:
                self.loss.calibrate(terms)
            value = self.loss.combine(terms)
            self.cache[key] = (value, terms)
            if value < self.best_loss:
                self.best_loss, self.best_values = value, list(values)
            self.trajectory.append((self.best_loss, value, "components"))
        return self.cache[key][0]

    def evaluate_batch(self, batch):
        return [self(values) for values in batch]

    def payload(self, values, velocities, control="", *, candidate_seed=None):
        reference = rows(ORACLE, velocities, family=self.family, seed=self.seed)
        candidate = rows(
            values,
            velocities,
            family=self.family,
            control=control,
            seed=self.seed if candidate_seed is None else candidate_seed,
        )
        terms = robust_score_terms(candidate, reference, percussive=True)
        baseline = robust_score_terms(reference, reference, percussive=True)
        return {
            "loss": self.loss.combine(terms),
            "quality": quality_report(terms, self.loss.weights, baseline_terms=baseline),
            "terms": terms,
        }


def run_benchmark(
    *, family="snare", max_evals=48, optimizer="coordinate", seed=29, include_evolution=True
):
    evaluator = ComponentEvaluator(family, seed, include_evolution)
    evaluator(list(START))
    knobs = [
        Knob(label=name, lo=0.0, hi=1.2, log=False, start_value=start, tunable=name)
        for name, start in zip(("fixture.stick", "fixture.noisy_body"), START)
    ]
    args = SimpleNamespace(
        max_evals=max_evals,
        per_knob_evals=8,
        population=8 if optimizer == "cmaes" else 0,
        sigma0=0.28,
        seed=seed,
        restarts=1,
    )
    (cma_es if optimizer == "cmaes" else optimize)(evaluator, knobs, args)
    best = evaluator.best_values
    return {
        "schema": "percussion-component-recovery/v1",
        "family": family,
        "basis": "synthetic audio through production percussion objective",
        "seed": seed,
        "include_evolution": include_evolution,
        "oracle_parameters": list(ORACLE),
        "start_parameters": list(START),
        "selected_parameters": best,
        "evaluations": len(evaluator.trajectory),
        "max_evals": max_evals,
        "weights": evaluator.loss.weights,
        "training_velocities": list(TRAIN),
        "held_out_velocities": list(HELD),
        "training": {
            "start": evaluator.payload(START, TRAIN),
            "selected": evaluator.payload(best, TRAIN),
        },
        "held_out": {
            "start": evaluator.payload(START, HELD),
            "selected": evaluator.payload(best, HELD),
        },
        "controls": {
            control or "self": evaluator.payload(ORACLE, TRAIN, control)
            for control in ("", "no_stick", "no_rattle", "hand", "cup")
        },
        "independent_seed_controls": {
            str(candidate_seed): evaluator.payload(ORACLE, TRAIN, candidate_seed=candidate_seed)
            for candidate_seed in (seed + 2, seed + 18, seed + 44)
        },
        "limitations": [
            "does not establish native-model fidelity or listening acceptance",
            "single held-out velocity cannot validate level balance",
            "1 dB evolve/diffuse targets are engineering units, not statistical equivalence",
            "contact omission may overlap stochastic variation; inspect independent seed controls",
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", choices=("snare", "metal", "kick"), default="snare")
    parser.add_argument("--optimizer", choices=("coordinate", "cmaes"), default="coordinate")
    parser.add_argument("--max-evals", type=int, default=48)
    parser.add_argument("--seed", type=int, default=29)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--without-evolution", action="store_true")
    args = parser.parse_args(argv)
    result = run_benchmark(
        family=args.family,
        max_evals=args.max_evals,
        optimizer=args.optimizer,
        seed=args.seed,
        include_evolution=not args.without_evolution,
    )
    content = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.out:
        args.out.write_text(content)
    else:
        print(content, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
