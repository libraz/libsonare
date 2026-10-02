"""Synthetic recovery benchmark for the voicematch objective, scored against a known oracle.

The benchmark renders a small, deterministic modal drum and measures it with
the production percussion path.  It is deliberately a calibration harness,
not a model of libsonare's physical engine: the oracle and candidate renderer
are both two-parameter synthetic signals.  A known oracle makes it possible to
separate search failure from an objective that cannot see a defect.

Run from the repository root, for example::

    rye run --pyproject bindings/python/pyproject.toml python \
        tools/voicematch/fitting_recovery.py --optimizer cmaes --out recovery.json

The JSON report is ``fitting-recovery/v1``.  It contains raw metric residuals,
the number of measured cells, a velocity withheld from fitting, and negative
controls for pitch, sparse cup-like resonance, and texture.  The signal model
is intentionally small so the command needs no C++ build, plugin, capture or
listener.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import numpy as np

# The voicematch modules are script-oriented and import each other by their
# short names.  Keep this module runnable both as a file and from its test.
_TOOLS_DIR = Path(__file__).resolve().parent
_TOOLS_ROOT = _TOOLS_DIR.parent
if str(_TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(_TOOLS_ROOT))
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

from fit_quality import aggregate_term_residuals
from knobs import Knob, format_value
from loss_aggregate import robust_score_terms
from loss_weights import TERM_COUNT_KEYS, LossWeights
from metrics_hit import analyze_hit
from metrics_note import level_of
from optimizers import cma_es, optimize
from smf import Note
from toneclass import default_weights

SCHEMA = "fitting-recovery/v1"
SAMPLE_RATE = 24_000
# Note 49 is in the production long-decay set, so analyze_hit keeps the full
# 1.3-second signal for the density floor and prompt/late windows.  Shorter
# generic drum windows end before a defensible recording floor exists, which
# would make the new texture terms look like a zero match.
DURATION_S = 1.3
DRUM_NOTE = 49
TRAINING_VELOCITIES = (48, 96)
WITHHELD_VELOCITIES = (112,)
ORACLE_VALUES = (1.46, 0.28)
START_VALUES = (1.30, 0.08)
PARAMETER_NAMES = ("synthetic.modal_ratio", "synthetic.texture")
PARAMETER_RANGES = ((1.20, 1.75), (0.02, 0.55))

# These are the drum terms actually used by the benchmark.  Keeping the list
# explicit makes the report's absolute residual stable if another shared loss
# term is added to the production vocabulary later.
REPORT_TERMS = (
    "level",
    "band",
    "bdecay",
    "env",
    "crest",
    "tilt",
    "bright",
    "tonal",
    "rise",
    "strike",
    "ring",
    "modes",
    "density",
    "prompt",
    "evolve",
    "diffuse",
    "lf",
)
# Exercise the same default selected by autofit for a percussion probe.  This
# is intentionally not a benchmark-only weighting: if density/prompt are
# wired incorrectly, the recovery report will show it in both loss and cells.
OBJECTIVE_WEIGHTS = default_weights(0, percussive=True)


def _clip(value: float, lo: float, hi: float) -> float:
    return float(np.clip(value, lo, hi))


def render_synthetic_hit(
    velocity: int,
    modal_ratio: float,
    texture: float,
    *,
    seed: int = 17,
    cup: bool = False,
) -> np.ndarray:
    """Render one deterministic short drum hit.

    ``modal_ratio`` moves the second dense-drum mode and ``texture`` mixes a
    decaying broadband strike into the modal body.  ``cup`` is a control only:
    it replaces the dense mode set with three sparse, long resonances, a useful
    proxy for a cup tap falsely matching a drum.  The benchmark
    never uses a hand-written feature distance; callers pass this audio through
    :func:`metrics_hit.analyze_hit` and :func:`loss_aggregate.robust_score_terms`.
    """

    n_samples = int(DURATION_S * SAMPLE_RATE)
    t = np.arange(n_samples, dtype=np.float64) / SAMPLE_RATE
    rng = np.random.default_rng(seed + velocity * 101)
    velocity_scale = 0.72 + 0.28 * velocity / 127.0
    base_hz = 175.0 * (1.0 + 0.02 * (velocity - 96) / 96.0)

    if cup:
        # Sparse high-Q modes: a different topology the modal and flatness terms must see.
        ratios = np.asarray((1.0, 1.52, 2.63), dtype=np.float64)
        amplitudes = np.asarray((1.0, 0.52, 0.26), dtype=np.float64)
        decays = np.asarray((0.20, 0.18, 0.16), dtype=np.float64)
    else:
        ratios = np.asarray((1.0, modal_ratio, 1.95, 2.62, 3.54, 4.71), dtype=np.float64)
        amplitudes = np.asarray((1.0, 0.62, 0.45, 0.33, 0.23, 0.16), dtype=np.float64)
        decays = np.asarray((0.18, 0.16, 0.14, 0.12, 0.10, 0.09), dtype=np.float64)

    body = np.zeros(n_samples, dtype=np.float64)
    for ratio, amplitude, decay in zip(ratios, amplitudes, decays):
        body += amplitude * np.sin(2.0 * np.pi * base_hz * ratio * t) * np.exp(-t / decay)

    # One noise sequence per velocity, so the landscape carries no Monte Carlo noise.
    effective_texture = _clip(texture * velocity_scale, 0.0, 1.0)
    noise = rng.standard_normal(n_samples) * np.exp(-t / (0.06 + 0.05 * effective_texture))
    signal = (1.0 - effective_texture) * body + 0.85 * effective_texture * noise
    attack = 1.0 - np.exp(-t / 0.0015)
    return signal * attack


def measure_rows(
    values: tuple[float, float] | list[float],
    velocities: tuple[int, ...] | list[int],
    *,
    seed: int = 17,
    cup: bool = False,
) -> list[dict[str, Any]]:
    """Render and measure a velocity grid with the production hit analyzer."""

    modal_ratio, texture = (float(v) for v in values)
    rows: list[dict[str, Any]] = []
    for velocity in velocities:
        note = Note(DRUM_NOTE, int(velocity), 0.0, DURATION_S)
        audio = render_synthetic_hit(int(velocity), modal_ratio, texture, seed=seed, cup=cup)
        row = analyze_hit(audio, SAMPLE_RATE, note, DURATION_S).to_dict()
        row.update(level_of(audio, SAMPLE_RATE, note, DURATION_S))
        rows.append(row)
    return rows


def _coverage(terms: dict[str, float] | None, rows: list[dict[str, Any]], requested: int) -> dict:
    """Report measured rows and production term cell counts, including zeros."""

    terms = terms or {}
    cells = {}
    for term in REPORT_TERMS:
        count_key = TERM_COUNT_KEYS.get(term)
        if count_key is not None:
            cells[term] = round(terms.get(count_key, 0.0))
        elif term in {"env", "crest"}:
            # One fixed comparison per row, so the reducer has no count key.
            cells[term] = len(rows)
        else:
            cells[term] = None
    return {
        "rows_requested": int(requested),
        "rows_measured": len(rows),
        "rows_with_modes": sum(bool(row.get("modal_hz")) for row in rows),
        "rows_with_texture": sum(row.get("flatness_db") is not None for row in rows),
        "rows_with_density": sum(any(row.get("modal_density_valid", ())) for row in rows),
        "rows_with_prompt": sum(any(row.get("prompt_late_valid", ())) for row in rows),
        "term_cells": cells,
        "comparable": bool(terms and terms.get("comparable", 0.0) > 0.0),
    }


def _residual_payload(
    terms: dict[str, float] | None,
    rows: list[dict[str, Any]],
    requested: int,
    *,
    oracle_terms: dict[str, float],
    weights: dict[str, float] | None = None,
) -> dict:
    """Turn raw production terms into an absolute residual report.

    A term with no comparison against the oracle's anchored counts is reported
    as null and listed in ``unavailable_terms`` rather than scored as a match.
    """

    weights = weights or OBJECTIVE_WEIGHTS
    quality_loss = LossWeights(dict(weights))
    quality_loss.anchor(oracle_terms)
    quality = aggregate_term_residuals(
        terms, quality_loss, active=REPORT_TERMS, baseline_terms=oracle_terms
    )
    safe_terms: dict[str, float | None] = {}
    units: dict[str, float | None] = {}
    unavailable: list[str] = []
    weighted_units = 0.0
    for term in REPORT_TERMS:
        row = quality[term]
        residual = row.get("residual_units")
        known = terms is not None and residual is not None and not row.get("unmeasured")
        if not known:
            unavailable.append(term)
        safe_terms[term] = round(float(row["raw"]), 9) if known else None
        units[term] = round(float(residual), 9) if known else None
        if known and row.get("weighted_units") is not None:
            weighted_units += float(row["weighted_units"])
    coverage = _coverage(terms, rows, requested)
    coverage["unavailable_terms"] = unavailable
    return {
        "absolute_residual": {
            "terms": safe_terms,
            "perceptual_units": units,
            "weighted_perceptual_units": round(float(weighted_units), 9),
        },
        "coverage": coverage,
    }


def _complete_weighted(payload: dict) -> float | None:
    """Weighted residual, or None when any reported term went unmeasured."""

    if payload["coverage"]["unavailable_terms"]:
        return None
    return float(payload["absolute_residual"]["weighted_perceptual_units"])


def self_zero_passed(payload: dict) -> bool:
    """Whether a self-match scored zero with every reported term measured."""

    return _complete_weighted(payload) == 0.0


def cup_worse(cup: dict, dense: dict) -> bool | None:
    """Whether the cup control scores worse; None when either side is incomplete."""

    cup_units, dense_units = _complete_weighted(cup), _complete_weighted(dense)
    if cup_units is None or dense_units is None:
        return None
    return cup_units > dense_units


class SyntheticEvaluator:
    """Runtime-knob evaluator implementing the existing optimizer protocol.

    The values are local runtime knobs for this synthetic renderer; no source
    files or C++ builds are involved.  The class deliberately mirrors the
    optimizer-facing part of ``autofit.Evaluator``: cached candidate scoring,
    ``trajectory``, ``best_loss``/``best_values``, and batch evaluation.
    """

    def __init__(
        self,
        oracle_rows: list[dict[str, Any]],
        velocities: tuple[int, ...],
        *,
        seed: int,
    ) -> None:
        self.oracle_rows = oracle_rows
        self.oracle_terms = robust_score_terms(oracle_rows, oracle_rows, percussive=True)
        self.velocities = velocities
        self.seed = seed
        self.loss = LossWeights(dict(OBJECTIVE_WEIGHTS))
        self.cache: dict[tuple[str, ...], tuple[float, dict[str, float], list[dict[str, Any]]]] = {}
        self.seen: set[tuple[str, ...]] = set()
        self.trajectory: list[tuple[float, float, str]] = []
        self.best_loss = math.inf
        self.best_values: list[float] | None = None
        self.workers = 1
        self.quiet = True
        self._calibrated = False

    @staticmethod
    def key(values: list[float] | tuple[float, ...]) -> tuple[str, ...]:
        return tuple(format_value(float(value)) for value in values)

    def measure(
        self, values: tuple[float, float] | list[float], *, cup: bool = False
    ) -> tuple[dict[str, float] | None, list[dict[str, Any]]]:
        rows = measure_rows(values, self.velocities, seed=self.seed, cup=cup)
        terms = robust_score_terms(rows, self.oracle_rows, percussive=True)
        return terms, rows

    def __call__(self, values: list[float]) -> float:
        key = self.key(values)
        if key not in self.cache:
            terms, rows = self.measure(values)
            if terms is None:
                loss = math.inf
            else:
                if not self._calibrated:
                    self.loss.calibrate(terms)
                    self._calibrated = True
                loss = self.loss.combine(terms)
            self.cache[key] = (loss, terms or {}, rows)
        loss, terms, _ = self.cache[key]
        if key not in self.seen:
            self.seen.add(key)
            if loss < self.best_loss:
                self.best_loss = loss
                self.best_values = list(values)
            self.trajectory.append((self.best_loss, loss, "fit"))
        return loss

    def evaluate_batch(self, batch: list[list[float]]) -> list[float]:
        return [self(values) for values in batch]

    def payload(
        self, values: tuple[float, float] | list[float], velocities: tuple[int, ...]
    ) -> dict:
        rows = measure_rows(values, velocities, seed=self.seed)
        oracle = (
            self.oracle_rows
            if velocities == self.velocities
            else measure_rows(ORACLE_VALUES, velocities, seed=self.seed)
        )
        terms = robust_score_terms(rows, oracle, percussive=True)
        oracle_terms = robust_score_terms(oracle, oracle, percussive=True)
        return _residual_payload(
            terms, rows, len(velocities), oracle_terms=oracle_terms, weights=OBJECTIVE_WEIGHTS
        )

    def control_payload(
        self,
        values: tuple[float, float],
        *,
        cup: bool = False,
    ) -> dict:
        rows = measure_rows(values, self.velocities, seed=self.seed, cup=cup)
        terms = robust_score_terms(rows, self.oracle_rows, percussive=True)
        payload = _residual_payload(
            terms,
            rows,
            len(self.velocities),
            oracle_terms=self.oracle_terms,
            weights=OBJECTIVE_WEIGHTS,
        )
        payload["loss"] = round(float(self.loss.combine(terms)), 9) if terms else None
        return payload


def _knobs() -> list[Knob]:
    return [
        Knob(
            label=name,
            lo=lo,
            hi=hi,
            log=False,
            start_value=start,
            tunable=name,
        )
        for name, (lo, hi), start in zip(PARAMETER_NAMES, PARAMETER_RANGES, START_VALUES)
    ]


def _optimizer_args(max_evals: int, seed: int, optimizer: str) -> SimpleNamespace:
    return SimpleNamespace(
        max_evals=max_evals,
        per_knob_evals=8,
        population=8 if optimizer == "cmaes" else 0,
        sigma0=0.28,
        seed=seed,
        restarts=1,
    )


def run_benchmark(
    *,
    optimizer: str = "cmaes",
    max_evals: int = 72,
    seed: int = 17,
) -> dict:
    """Run a deterministic training/withheld recovery case and controls."""

    if optimizer not in {"coordinate", "cmaes"}:
        raise ValueError("optimizer must be 'coordinate' or 'cmaes'")
    if max_evals < 12:
        raise ValueError("max_evals must be at least 12 for a meaningful recovery run")

    oracle_train = measure_rows(ORACLE_VALUES, TRAINING_VELOCITIES, seed=seed)
    evaluator = SyntheticEvaluator(oracle_train, TRAINING_VELOCITIES, seed=seed)
    knobs = _knobs()
    initial = evaluator(list(START_VALUES))
    args = _optimizer_args(max_evals, seed, optimizer)
    search = optimize if optimizer == "coordinate" else cma_es
    best = tuple(float(value) for value in search(evaluator, knobs, args))
    # An all-cache final generation can return a non-best vector; prefer the tracked best.
    if evaluator.best_values is not None:
        best = tuple(float(value) for value in evaluator.best_values)

    initial_train = evaluator.payload(START_VALUES, TRAINING_VELOCITIES)
    final_train = evaluator.payload(best, TRAINING_VELOCITIES)
    initial_withheld = evaluator.payload(START_VALUES, WITHHELD_VELOCITIES)
    final_withheld = evaluator.payload(best, WITHHELD_VELOCITIES)

    dense_control = evaluator.control_payload(ORACLE_VALUES)
    cup_control = evaluator.control_payload(ORACLE_VALUES, cup=True)
    controls = {
        "self_zero": dense_control,
        "pitch_shift": evaluator.control_payload((1.30, ORACLE_VALUES[1])),
        "texture_shift": evaluator.control_payload((ORACLE_VALUES[0], 0.03)),
        "cup_sparse_resonator": cup_control,
        "cup_vs_dense": {
            "dense": dense_control,
            "cup": cup_control,
            "cup_worse": cup_worse(cup_control, dense_control),
        },
    }
    controls["self_zero"]["passed"] = self_zero_passed(dense_control)
    return {
        "schema": SCHEMA,
        "seed": int(seed),
        "sample_rate": SAMPLE_RATE,
        "duration_s": DURATION_S,
        "signal": {
            "note": DRUM_NOTE,
            "oracle_parameters": dict(zip(PARAMETER_NAMES, ORACLE_VALUES)),
            "parameter_ranges": {
                name: {"min": lo, "max": hi}
                for name, (lo, hi) in zip(PARAMETER_NAMES, PARAMETER_RANGES)
            },
        },
        "fit": {
            "optimizer": optimizer,
            "max_evals": int(max_evals),
            "evaluations": len(evaluator.trajectory),
            "training_velocities": list(TRAINING_VELOCITIES),
            "withheld_velocities": list(WITHHELD_VELOCITIES),
            "start_parameters": dict(zip(PARAMETER_NAMES, START_VALUES)),
            "best_parameters": dict(zip(PARAMETER_NAMES, best)),
            "initial_loss": round(float(initial), 9),
            "final_loss": round(float(evaluator.best_loss), 9),
            "initial": initial_train,
            "final": final_train,
            "withheld": {"initial": initial_withheld, "final": final_withheld},
        },
        "controls": controls,
        "objective_weights": OBJECTIVE_WEIGHTS,
    }


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--optimizer", choices=("coordinate", "cmaes"), default="cmaes")
    parser.add_argument("--max-evals", type=int, default=72)
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument("--out", type=Path, help="also write the JSON report to this path")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    report = run_benchmark(optimizer=args.optimizer, max_evals=args.max_evals, seed=args.seed)
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded + "\n")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
