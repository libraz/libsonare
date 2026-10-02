"""Preserve difficult notes in the fitting objective without another render."""

from __future__ import annotations

import math

from loss import score_terms
from loss_dimensions import LEVEL_DELTA_CAP_DB, LOSS_TERMS
from loss_weights import TERM_COUNT_KEYS

DEFAULT_TAIL_FRACTION = 0.25
GRID_TERMS = frozenset(("mss", "dyn", "hfdyn", "kit"))


def upper_tail_mean(values, fraction):
    """Mean of the worst fraction of measured, nonnegative residuals."""
    if not math.isfinite(fraction) or not 0.0 < fraction <= 1.0:
        raise ValueError("tail_fraction must be finite and in (0, 1]")
    ordered = sorted(values, reverse=True)
    if not ordered:
        return None
    if any(not math.isfinite(v) or v < 0.0 for v in ordered):
        raise ValueError("residuals must be finite and nonnegative")
    selected = ordered[: max(1, math.ceil(len(ordered) * fraction))]
    return math.fsum(selected) / len(selected)


def _finite(value):
    if value is None:
        return None
    value = float(value)
    return value if math.isfinite(value) else None


def _row_level(model, oracle, offset):
    reference = _finite(oracle.get("held_rms_dbfs"))
    if reference is None:
        return None
    observed = _finite(model.get("held_rms_dbfs"))
    if observed is None:
        return LEVEL_DELTA_CAP_DB
    return min(abs(observed - reference - offset), LEVEL_DELTA_CAP_DB)


def robust_score_terms(model_rows, oracle_rows, *, tail_fraction=DEFAULT_TAIL_FRACTION, **kwargs):
    """Score the grid and retain its worst measured quarter of notes.

    Relations keep their full-grid reducer. Level uses the grid's common gain
    offset, never a separate offset that would erase each note's difference.
    Zero selects the historical mean objective; metadata comes from the same
    measured rows and requires no additional synthesis or feature extraction.
    """
    if not math.isfinite(tail_fraction) or not 0.0 <= tail_fraction <= 1.0:
        raise ValueError("tail_fraction must be finite and in [0, 1]")
    aggregate = score_terms(model_rows, oracle_rows, **kwargs)
    if aggregate is None or not model_rows or not tail_fraction:
        return aggregate
    # A row unscorable alone stays charged in the grid result and only leaves the tail statistics.
    rows = [score_terms([m], [o], **kwargs) for m, o in zip(model_rows, oracle_rows)]
    offset = aggregate.get("level_offset_db", 0.0)
    for name in LOSS_TERMS:
        if name in GRID_TERMS:
            continue
        observations = []
        for index, (model, oracle, terms) in enumerate(zip(model_rows, oracle_rows, rows)):
            if name == "level":
                value = _row_level(model, oracle, offset)
            elif terms is None:
                continue
            else:
                count_key = TERM_COUNT_KEYS.get(name)
                if name == "lf" and "lf_notes" not in terms:
                    count_key = "lf_cells"
                if count_key is not None and terms.get(count_key, 0.0) <= 0.0:
                    continue
                if f"{name}_cells" in terms and terms[f"{name}_cells"] <= 0.0:
                    continue
                value = _finite(terms.get(name))
            if value is not None:
                observations.append((value, index))
        if not observations:
            continue
        tail = upper_tail_mean([value for value, _ in observations], tail_fraction)
        worst, index = max(observations)
        aggregate[f"{name}_mean"] = aggregate[name]
        aggregate[f"{name}_upper_tail"] = tail
        aggregate[f"{name}_worst"] = worst
        aggregate[f"{name}_worst_row"] = float(index)
        for axis in ("note", "velocity"):
            value = _finite(oracle_rows[index].get(axis))
            if value is not None:
                aggregate[f"{name}_worst_{axis}"] = value
        aggregate[name] = max(aggregate[name], tail)
    aggregate["aggregation_tail_fraction"] = tail_fraction
    return aggregate
