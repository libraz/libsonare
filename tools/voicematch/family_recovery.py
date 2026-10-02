"""Known-audio recovery checks for each physical-source family.

This is a small benchmark for the measurement and search layers that needs no
listener.  It renders one deterministic piece containing a training grid and a
disjoint holdout, runs both through the production ``probe_rows`` path, and
then scores the rows with ``robust_score_terms`` and the class defaults.  The signal
renderer is deliberately local to this file: it is an oracle with two known
parameters, not a claim that a two-parameter model represents a real physical
instrument.

The two parameters are a spectral/decay control and a velocity response
exponent.  They are kept intentionally coupled in the renderer because a real
physical knob often moves more than one measured axis.  The report therefore
publishes normalized parameter distance, absolute fixed-unit residuals and
coverage instead of asserting exact parameter recovery.

Run from the repository root, for example::

    rye run --pyproject bindings/python/pyproject.toml python \
        tools/voicematch/family_recovery.py --family all --optimizer cmaes \
        --max-evals 24 --out family-recovery.json
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import numpy as np

_TOOLS_DIR = Path(__file__).resolve().parent
_TOOLS_ROOT = _TOOLS_DIR.parent
for _path in (_TOOLS_ROOT, _TOOLS_DIR):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

from fit_quality import aggregate_term_residuals
from knobs import Knob, format_value
from loss import probe_rows
from loss_aggregate import robust_score_terms
from loss_weights import LossWeights
from metrics_signal import normalize_rms
from optimizers import cma_es, optimize
from patterns import Pattern, pattern_length
from smf import Note
from toneclass import default_weights, tone_class

SCHEMA = "family-fitting-recovery/v1"
SAMPLE_RATE = 12_000
SEED = 17
PARAMETER_NAMES = ("synthetic.spectral_decay", "synthetic.velocity_gain_exponent")
PARAMETER_RANGES = ((0.55, 1.45), (0.55, 1.45))
ORACLE_VALUES = (1.08, 0.92)
START_VALUES = (0.68, 1.34)


@dataclass(frozen=True)
class FamilySpec:
    name: str
    program: int
    percussive: bool
    train_pairs: tuple[tuple[int, int], ...]
    holdout_pairs: tuple[tuple[int, int], ...]
    note_duration: float
    gap: float
    tail: float


FAMILY_SPECS: dict[str, FamilySpec] = {
    "bowed": FamilySpec(
        "bowed", 40, False, ((48, 48), (48, 96), (60, 48), (60, 96)), ((72, 112),), 2.2, 0.55, 1.0
    ),
    "plucked": FamilySpec(
        "plucked",
        24,
        False,
        ((48, 48), (48, 96), (60, 48), (60, 96)),
        ((72, 112),),
        2.2,
        0.55,
        1.0,
    ),
    "hammered": FamilySpec(
        "hammered",
        0,
        False,
        ((48, 48), (48, 96), (60, 48), (60, 96)),
        ((72, 112),),
        6.3,
        0.65,
        1.0,
    ),
    "percussion": FamilySpec(
        "percussion", 0, True, ((49, 48), (49, 96)), ((49, 112),), 1.8, 2.1, 0.4
    ),
}


def _effective_weights(spec: FamilySpec) -> tuple[dict[str, float], list[str]]:
    """Resolve class defaults and name structural omissions explicitly."""

    weights = default_weights(spec.program, percussive=spec.percussive)
    unavailable: list[str] = []
    # A one-piece percussion check has no kit relation, as in the production CLI.
    if spec.percussive and "kit" in weights:
        weights.pop("kit")
        unavailable.append("kit")
    return weights, unavailable


def _make_patterns(spec: FamilySpec) -> tuple[Pattern, Pattern, Pattern]:
    """Return one piece plus views of its disjoint train and holdout rows."""

    pairs = spec.train_pairs + spec.holdout_pairs
    notes: list[Note] = []
    train: list[Note] = []
    holdout: list[Note] = []
    at = 0.1
    train_count = len(spec.train_pairs)
    for index, (note, velocity) in enumerate(pairs):
        item = Note(note, velocity, at, spec.note_duration)
        notes.append(item)
        (train if index < train_count else holdout).append(item)
        at += spec.note_duration + spec.gap

    def view(name: str, selected: list[Note]) -> Pattern:
        return Pattern(
            name,
            notes,
            analysis_notes=selected,
            tail=spec.tail,
            channel=9 if spec.percussive else 0,
            percussive=spec.percussive,
        )

    return (
        view(f"{spec.name}:piece", notes),
        view(f"{spec.name}:train", train),
        view(f"{spec.name}:holdout", holdout),
    )


def _render_audio(
    spec: FamilySpec,
    pattern: Pattern,
    values: tuple[float, float] | list[float],
    *,
    seed: int = SEED,
    excitation: str = "oracle",
    envelope: str = "oracle",
    dynamics: str = "oracle",
    gain_offset: float = 1.0,
) -> np.ndarray:
    """Render deterministic audio with controlled family-specific behaviour."""

    spectral, velocity_exponent = (float(v) for v in values)
    if envelope == "wrong":
        envelope_scale = 2.35
    else:
        envelope_scale = 1.0
    dynamic_exponent = velocity_exponent + (0.48 if dynamics == "wrong" else 0.0)
    wrong_excitation = excitation == "wrong"

    length = int((pattern_length(pattern) + 0.2) * SAMPLE_RATE)
    output = np.zeros(length, dtype=np.float64)
    for index, note in enumerate(pattern.notes):
        start = int(note.start * SAMPLE_RATE)
        stop = min(length, int((note.start + note.dur) * SAMPLE_RATE))
        if stop <= start:
            continue
        t = np.arange(stop - start, dtype=np.float64) / SAMPLE_RATE
        f0 = 440.0 * 2.0 ** ((note.note - 69) / 12.0)
        force = max(float(note.velocity) / 96.0, 0.05)
        gain = force**velocity_exponent
        brightness = force**dynamic_exponent

        if spec.name == "bowed":
            harmonics = np.arange(1.0, 13.0)
            amplitudes = harmonics ** (-0.50 - 0.64 * spectral)
            amplitudes *= 1.0 + 0.15 * brightness * harmonics / harmonics[-1]
            vibrato_cents = 8.0 * np.sin(2.0 * np.pi * 5.0 * t)
            phase = 2.0 * np.pi * np.cumsum(f0 * 2.0 ** (vibrato_cents / 1200.0)) / SAMPLE_RATE
            tone = sum(
                amplitude * np.sin(harmonic * phase)
                for harmonic, amplitude in zip(harmonics, amplitudes)
            )
            if wrong_excitation:
                # Pluck-like impulse in place of the bow, same partials: only onset/decay moves.
                envelope_value = (1.0 - np.exp(-t / 0.003)) * np.exp(
                    -t / (0.48 * spectral * envelope_scale)
                )
            else:
                envelope_value = (1.0 - np.exp(-t / 0.10)) * np.exp(
                    -t / (7.0 * spectral * envelope_scale)
                )
            envelope_value *= 1.0 + 0.035 * np.sin(2.0 * np.pi * 5.0 * t + 0.4)
        elif spec.name == "plucked":
            harmonics = np.arange(1.0, 13.0)
            amplitudes = harmonics ** (-0.25 + 0.70 * spectral)
            amplitudes *= 1.0 + 0.16 * brightness * harmonics / harmonics[-1]
            tone = sum(
                amplitude * np.sin(2.0 * np.pi * f0 * harmonic * t)
                for harmonic, amplitude in zip(harmonics, amplitudes)
            )
            if wrong_excitation:
                # Bow-like slow onset and held sustain in place of the pluck's free fall.
                envelope_value = (1.0 - np.exp(-t / 0.10)) * np.exp(
                    -t / (7.0 * spectral * envelope_scale)
                )
            else:
                envelope_value = (1.0 - np.exp(-t / 0.004)) * np.exp(
                    -t / ((0.45 + 0.75 * spectral) * envelope_scale)
                )
        elif spec.name == "hammered":
            harmonics = np.arange(1.0, 13.0)
            amplitudes = harmonics ** (-0.15 + 0.60 * spectral)
            amplitudes *= 1.0 + 0.20 * brightness * harmonics / harmonics[-1]
            frequencies = f0 * harmonics * np.sqrt(1.0 + 0.00008 * harmonics**2)
            tone = sum(
                amplitude * np.sin(2.0 * np.pi * frequency * t)
                for frequency, amplitude in zip(frequencies, amplitudes)
            )
            if wrong_excitation:
                # Bow-like slow build in place of the hammer, pitch series unchanged.
                envelope_value = (1.0 - np.exp(-t / 0.10)) * np.exp(
                    -t / (7.0 * spectral * envelope_scale)
                )
            else:
                envelope_value = (1.0 - np.exp(-t / 0.005)) * np.exp(
                    -t / ((0.70 + 1.10 * spectral) * envelope_scale)
                )
        else:
            if wrong_excitation:
                # Sparse high-Q resonances with no strike noise, the cup/glass false match.
                ratios = np.asarray((1.0, 1.52, 2.63), dtype=np.float64)
                amplitudes = np.asarray((1.0, 0.52, 0.26), dtype=np.float64)
            else:
                ratios = np.asarray((1.0, 1.52, 2.30, 3.40, 4.80), dtype=np.float64)
                amplitudes = np.asarray((1.0, 0.65, 0.42, 0.30, 0.20), dtype=np.float64)
            amplitudes *= 1.0 + 0.15 * brightness * np.arange(len(ratios)) / max(len(ratios) - 1, 1)
            tone = sum(
                amplitude * np.sin(2.0 * np.pi * f0 * ratio * t)
                for ratio, amplitude in zip(ratios, amplitudes)
            )
            if wrong_excitation:
                envelope_value = (1.0 - np.exp(-t / 0.025)) * np.exp(
                    -t / (0.85 * spectral * envelope_scale)
                )
            else:
                envelope_value = (1.0 - np.exp(-t / 0.002)) * np.exp(
                    -t / ((0.12 + 0.28 * spectral) * envelope_scale)
                )
                rng = np.random.default_rng(seed + 101 * index + 17 * note.velocity)
                tone += (
                    0.05
                    * max(0.05, 1.0 - brightness)
                    * rng.standard_normal(t.size)
                    * np.exp(-t / 0.12)
                )

        output[start:stop] += 0.05 * gain * tone * envelope_value
    return output * float(gain_offset)


def _measure(
    spec: FamilySpec,
    patterns: tuple[Pattern, Pattern, Pattern],
    values: tuple[float, float] | list[float],
    *,
    seed: int = SEED,
    excitation: str = "oracle",
    envelope: str = "oracle",
    dynamics: str = "oracle",
    gain_offset: float = 1.0,
) -> tuple[list[dict], list[dict]]:
    """Render once, then measure train and holdout views of the same piece."""

    piece, train_pattern, holdout_pattern = patterns
    raw = _render_audio(
        spec,
        piece,
        values,
        seed=seed,
        excitation=excitation,
        envelope=envelope,
        dynamics=dynamics,
        gain_offset=gain_offset,
    )
    mono = normalize_rms(raw)
    train_rows = probe_rows(mono, train_pattern, SAMPLE_RATE, raw=raw)
    holdout_rows = probe_rows(mono, holdout_pattern, SAMPLE_RATE, raw=raw)
    return train_rows, holdout_rows


def _safe(value: Any) -> float | None:
    if value is None:
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def _feature_snapshot(spec: FamilySpec, rows: list[dict]) -> dict[str, float | None]:
    """Summarize measured feature families for a control explanation."""

    def median(field: str) -> float | None:
        values = [_safe(row.get(field)) for row in rows]
        values = [value for value in values if value is not None]
        return None if not values else round(float(np.median(values)), 6)

    if spec.percussive:
        mode_counts = [len(row.get("modal_hz") or []) for row in rows]
        return {
            "attack_ms": median("attack_ms"),
            "decay_ms": median("decay_ms"),
            "flatness_db": median("flatness_db"),
            "modal_count": None if not mode_counts else round(float(np.median(mode_counts)), 6),
        }
    harmonic = []
    for row in rows:
        values = row.get("harmonics_db")
        if isinstance(values, list) and len(values) > 1 and _safe(values[1]) is not None:
            harmonic.append(float(values[1]))
    return {
        "attack_fine_ms": median("attack_fine_ms"),
        "sustain_slope_db_s": median("sustain_slope_db_s"),
        "release_ms": median("release_ms"),
        "harmonic_h2_db": None if not harmonic else round(float(np.median(harmonic)), 6),
    }


def _feature_delta(
    baseline: dict[str, float | None], altered: dict[str, float | None]
) -> dict[str, float | None]:
    return {
        name: None
        if baseline.get(name) is None or altered.get(name) is None
        else round(abs(float(altered[name]) - float(baseline[name])), 6)
        for name in baseline
    }


def _excitation_definition(spec: FamilySpec) -> str:
    if spec.name == "bowed":
        return "replace the bowed slow attack and long sustain with a 3 ms pluck-like impulse and 0.48 s free decay"
    if spec.name in {"plucked", "hammered"}:
        return "replace the pluck/hammer impulse with a 100 ms bowed-style onset and 7 s held decay"
    return "replace the dense broadband transient with three sparse high-Q resonances and no strike noise"


def _residual_payload(
    terms: dict[str, float] | None,
    oracle_terms: dict[str, float] | None,
    rows: list[dict],
    requested: int,
    weights: dict[str, float],
) -> dict[str, Any]:
    """Publish absolute fixed-unit residuals and measurement coverage."""

    if terms is None:
        return {
            "absolute_residual": None,
            "coverage": {
                "rows_requested": requested,
                "rows_measured": len(rows),
                "comparable": False,
            },
        }
    oracle_terms = oracle_terms or {}
    quality_loss = LossWeights(dict(weights))
    quality_loss.anchor(oracle_terms)
    quality_rows = aggregate_term_residuals(
        terms,
        quality_loss,
        active=tuple(weights),
        baseline_terms=oracle_terms,
    )
    term_rows: dict[str, Any] = {}
    unavailable: list[str] = []
    for term, quality in quality_rows.items():
        residual = _safe(quality.get("residual_units"))
        known = residual is not None and not bool(quality.get("unmeasured"))
        if not known:
            unavailable.append(term)
        observed_raw = _safe(quality.get("raw"))
        term_payload = {
            # `raw_observed` keeps the reducer's value; `raw` is null when nothing was compared.
            "raw": observed_raw if known else None,
            "raw_observed": observed_raw,
            "perceptual_units": residual,
            "weight": _safe(quality.get("weight")),
            "weighted_perceptual_units": (_safe(quality.get("weighted_units")) if known else None),
            "expected_count": _safe(quality.get("expected")),
            "measured_count": _safe(quality.get("count")),
            "coverage": _safe(quality.get("coverage")),
            "capped": _safe(quality.get("capped")),
            "absent": _safe(quality.get("absent")),
            "skipped": _safe(quality.get("skipped")),
            "compared": _safe(quality.get("compared")),
            "coverage_complete": bool(quality.get("coverage_complete")),
            "status": "measured" if known else "unmeasured",
        }
        for source, target in (
            ("mean_raw", "mean"),
            ("upper_tail_raw", "upper_tail"),
            ("worst_raw", "worst"),
            ("worst_residual_units", "worst_units"),
        ):
            value = _safe(quality.get(source))
            if value is not None:
                term_payload[target] = value if target.endswith("units") else round(value, 9)
        condition = quality.get("worst_condition")
        if isinstance(condition, dict):
            for axis in ("row", "note", "velocity"):
                value = _safe(condition.get(axis))
                if value is not None:
                    term_payload[f"worst_{axis}"] = value
        term_rows[term] = term_payload

    weighted = sum(
        row["weighted_perceptual_units"]
        for row in term_rows.values()
        if row["weighted_perceptual_units"] is not None
    )
    cells = {term: row["measured_count"] for term, row in term_rows.items()}
    return {
        "absolute_residual": {
            "terms": {term: row["raw"] for term, row in term_rows.items()},
            "perceptual_units": {term: row["perceptual_units"] for term, row in term_rows.items()},
            "weighted_perceptual_units": round(float(weighted), 9),
            "by_term": term_rows,
        },
        "coverage": {
            "rows_requested": int(requested),
            "rows_measured": len(rows),
            "term_cells": cells,
            "unavailable_terms": sorted(set(unavailable)),
            "comparable": bool(terms.get("comparable", 0.0) > 0.0),
        },
    }


class FamilyEvaluator:
    """Optimizer protocol backed by measured deterministic audio."""

    def __init__(
        self,
        spec: FamilySpec,
        patterns: tuple[Pattern, Pattern, Pattern],
        oracle_train: list[dict],
        oracle_terms: dict[str, float],
        weights: dict[str, float],
        *,
        seed: int,
    ) -> None:
        self.spec = spec
        self.patterns = patterns
        self.oracle_train = oracle_train
        self.oracle_terms = oracle_terms
        self.weights = weights
        self.seed = seed
        self.loss = LossWeights(dict(weights))
        self.cache: dict[
            tuple[str, ...], tuple[float, dict[str, float] | None, list[dict], list[dict]]
        ] = {}
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
        self, values: list[float] | tuple[float, ...], **kwargs
    ) -> tuple[list[dict], list[dict]]:
        return _measure(self.spec, self.patterns, values, seed=self.seed, **kwargs)

    def terms(
        self, values: list[float] | tuple[float, ...]
    ) -> tuple[dict[str, float] | None, list[dict], list[dict]]:
        train_rows, holdout_rows = self.measure(values)
        terms = robust_score_terms(train_rows, self.oracle_train, percussive=self.spec.percussive)
        return terms, train_rows, holdout_rows

    def __call__(self, values: list[float]) -> float:
        key = self.key(values)
        if key not in self.cache:
            terms, train_rows, holdout_rows = self.terms(values)
            if terms is None:
                loss = math.inf
            else:
                if not self._calibrated:
                    self.loss.calibrate(terms)
                    self._calibrated = True
                loss = self.loss.combine(terms)
            self.cache[key] = (loss, terms, train_rows, holdout_rows)
        loss, _, _, _ = self.cache[key]
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
        self, values: list[float] | tuple[float, ...]
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        terms, train_rows, holdout_rows = self.terms(values)
        train = _residual_payload(
            terms, self.oracle_terms, train_rows, len(self.spec.train_pairs), self.weights
        )
        oracle_holdout = _measure(self.spec, self.patterns, ORACLE_VALUES, seed=self.seed)[1]
        holdout_terms = robust_score_terms(
            holdout_rows, oracle_holdout, percussive=self.spec.percussive
        )
        holdout = _residual_payload(
            holdout_terms,
            robust_score_terms(oracle_holdout, oracle_holdout, percussive=self.spec.percussive),
            holdout_rows,
            len(self.spec.holdout_pairs),
            self.weights,
        )
        return train, holdout

    def control_payload(self, values: list[float] | tuple[float, ...], **kwargs) -> dict[str, Any]:
        train_rows, _ = self.measure(values, **kwargs)
        terms = robust_score_terms(train_rows, self.oracle_train, percussive=self.spec.percussive)
        payload = _residual_payload(
            terms, self.oracle_terms, train_rows, len(self.spec.train_pairs), self.weights
        )
        payload["objective_loss"] = None if terms is None else round(self.loss.combine(terms), 9)
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


def _distance(values: list[float] | tuple[float, ...]) -> float:
    normalized = [
        (float(value) - oracle) / (hi - lo)
        for value, oracle, (lo, hi) in zip(values, ORACLE_VALUES, PARAMETER_RANGES)
    ]
    return float(np.sqrt(np.mean(np.square(normalized))))


def _control_self_zero(evaluator: FamilyEvaluator) -> dict[str, Any]:
    rows, _ = evaluator.measure(ORACLE_VALUES)
    terms = robust_score_terms(rows, rows, percussive=evaluator.spec.percussive)
    return _residual_payload(terms, terms, rows, len(evaluator.spec.train_pairs), evaluator.weights)


def run_family(
    family: str,
    *,
    optimizer: str = "cmaes",
    max_evals: int = 24,
    seed: int = SEED,
) -> dict[str, Any]:
    """Run one family and return a JSON-compatible recovery report."""

    if family not in FAMILY_SPECS:
        raise ValueError(f"unknown family {family!r}; choose from {sorted(FAMILY_SPECS)}")
    if optimizer not in {"coordinate", "cmaes"}:
        raise ValueError("optimizer must be 'coordinate' or 'cmaes'")
    if max_evals < 12:
        raise ValueError("max_evals must be at least 12 for a meaningful recovery run")

    spec = FAMILY_SPECS[family]
    started = time.perf_counter()
    patterns = _make_patterns(spec)
    weights, unavailable = _effective_weights(spec)
    oracle_train, _ = _measure(spec, patterns, ORACLE_VALUES, seed=seed)
    oracle_terms = robust_score_terms(oracle_train, oracle_train, percussive=spec.percussive)
    if oracle_terms is None:
        raise RuntimeError(f"oracle audio for {family} was not comparable")
    evaluator = FamilyEvaluator(spec, patterns, oracle_train, oracle_terms, weights, seed=seed)
    initial = evaluator(list(START_VALUES))
    args = _optimizer_args(max_evals, seed, optimizer)
    search = optimize if optimizer == "coordinate" else cma_es
    best = tuple(float(value) for value in search(evaluator, _knobs(), args))
    if evaluator.best_values is not None:
        best = tuple(float(value) for value in evaluator.best_values)

    initial_train, initial_holdout = evaluator.payload(START_VALUES)
    final_train, final_holdout = evaluator.payload(best)
    controls = {
        "self_zero": _control_self_zero(evaluator),
        "wrong_excitation": evaluator.control_payload(ORACLE_VALUES, excitation="wrong"),
        "wrong_envelope": evaluator.control_payload(ORACLE_VALUES, envelope="wrong"),
        "wrong_dynamics": evaluator.control_payload(ORACLE_VALUES, dynamics="wrong"),
        "gain_offset_invariance": {
            "scale": 3.7,
            "base": evaluator.control_payload(ORACLE_VALUES),
            "scaled": evaluator.control_payload(ORACLE_VALUES, gain_offset=3.7),
        },
    }
    baseline_features = _feature_snapshot(spec, evaluator.measure(ORACLE_VALUES)[0])
    wrong_features = _feature_snapshot(
        spec, evaluator.measure(ORACLE_VALUES, excitation="wrong")[0]
    )
    controls["wrong_excitation"]["definition"] = _excitation_definition(spec)
    controls["wrong_excitation"]["features"] = {
        "baseline": baseline_features,
        "altered": wrong_features,
        "absolute_delta": _feature_delta(baseline_features, wrong_features),
    }
    base_gain = controls["gain_offset_invariance"]["base"]["absolute_residual"]
    scaled_gain = controls["gain_offset_invariance"]["scaled"]["absolute_residual"]
    base_value = None if base_gain is None else base_gain["weighted_perceptual_units"]
    scaled_value = None if scaled_gain is None else scaled_gain["weighted_perceptual_units"]
    controls["gain_offset_invariance"]["absolute_delta"] = (
        None
        if base_value is None or scaled_value is None
        else round(abs(base_value - scaled_value), 9)
    )
    unavailable = sorted(
        set(unavailable) | set(initial_train["coverage"].get("unavailable_terms", []))
    )
    return {
        "schema": SCHEMA,
        "family": family,
        "tone_class": tone_class(spec.program, drum_note=49 if spec.percussive else None).value,
        "seed": int(seed),
        "sample_rate": SAMPLE_RATE,
        "piece": {
            "training_pairs": [list(pair) for pair in spec.train_pairs],
            "withheld_pairs": [list(pair) for pair in spec.holdout_pairs],
            "note_duration_s": spec.note_duration,
            "gap_s": spec.gap,
        },
        "objective_weights": weights,
        "unavailable_default_terms": unavailable,
        "limits": {
            "oracle": "deterministic synthetic audio; this does not model the C++ physical engine",
            "parameter_recovery": "normalized distance is diagnostic; exact parameter equality is not required",
            "holdout": "one disjoint note/velocity case, or one withheld velocity for percussion",
            "unavailable_default_terms": unavailable,
        },
        "signal": {
            "parameter_names": list(PARAMETER_NAMES),
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
            "cache_entries": len(evaluator.cache),
            "elapsed_s": round(time.perf_counter() - started, 6),
            "start_parameters": dict(zip(PARAMETER_NAMES, START_VALUES)),
            "best_parameters": dict(zip(PARAMETER_NAMES, best)),
            "start_normalized_parameter_distance": round(_distance(START_VALUES), 9),
            "best_normalized_parameter_distance": round(_distance(best), 9),
            "initial_loss": round(float(initial), 9),
            "final_loss": round(float(evaluator.best_loss), 9),
            "training": {"initial": initial_train, "final": final_train},
            "withheld": {"initial": initial_holdout, "final": final_holdout},
        },
        "controls": controls,
    }


def run_benchmark(
    *,
    family: str = "all",
    optimizer: str = "cmaes",
    max_evals: int = 24,
    seed: int = SEED,
) -> dict[str, Any]:
    """Run one family or the complete family matrix."""

    if family == "all":
        reports = {
            name: run_family(name, optimizer=optimizer, max_evals=max_evals, seed=seed)
            for name in FAMILY_SPECS
        }
        return {"schema": SCHEMA, "family": "all", "seed": int(seed), "reports": reports}
    return run_family(family, optimizer=optimizer, max_evals=max_evals, seed=seed)


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--family", choices=("bowed", "plucked", "hammered", "percussion", "all"), default="all"
    )
    parser.add_argument("--optimizer", choices=("coordinate", "cmaes"), default="cmaes")
    parser.add_argument("--max-evals", type=int, default=24)
    parser.add_argument("--seed", type=int, default=SEED)
    parser.add_argument("--out", type=Path, help="also write the JSON report to this path")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    report = run_benchmark(
        family=args.family, optimizer=args.optimizer, max_evals=args.max_evals, seed=args.seed
    )
    encoded = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded + "\n")
    print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
