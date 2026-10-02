"""Absolute, measurement-aware quality reporting for an autofit run.

The optimiser's loss is deliberately a relative number: a normalised run is
divided by its starting point and a raw run is in the units chosen by the
individual metrics.  Neither number is an oracle approximation percentage.
This module keeps the report on the fixed, declared ``TERM_UNITS`` ruler and
refuses to turn an empty measurement into a match.

The public report is intentionally based on aggregate terms.  A term is one
measurement aggregate (for example the harmonic ladder or the drum band
profile); its cells are evidence about coverage, not votes in a perceptual
similarity score.
"""

from __future__ import annotations

import math
from collections.abc import Mapping
from typing import Any

from loss_dimensions import LOSS_TERMS
from loss_weights import TERM_COUNT_KEYS, TERM_UNITS

QUALITY_BASIS = "aggregate_term_residuals"
QUALITY_METRIC = "absolute_weighted_mean_units"
DEFAULT_TERM_TARGET_UNITS = 1.0


def _as_float(value: Any) -> float | None:
    """Return a finite float, preserving unknown and non-finite values as null."""

    if value is None:
        return None
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def _weights(loss_or_weights: Any, active: tuple[str, ...] | None = None) -> dict[str, float]:
    """Read weights from a ``LossWeights`` object or a plain mapping."""

    if loss_or_weights is None:
        return {}
    raw = getattr(loss_or_weights, "weights", loss_or_weights)
    if not isinstance(raw, Mapping):
        return {}
    names = active or tuple(LOSS_TERMS)
    out: dict[str, float] = {}
    for name in names:
        value = _as_float(raw.get(name))
        if value is not None and value > 0.0:
            out[name] = value
    return out


def _active(loss_or_weights: Any, terms: Mapping[str, Any] | None = None) -> tuple[str, ...]:
    """Return active terms in the stable objective order."""

    if loss_or_weights is not None:
        method = getattr(loss_or_weights, "active", None)
        if callable(method):
            try:
                result = tuple(method())
                if result:
                    return result
            except (TypeError, ValueError):
                pass
    weights = _weights(loss_or_weights)
    if weights:
        return tuple(name for name in LOSS_TERMS if name in weights)
    return ()


def _declared_unit(name: str) -> float | None:
    """Read the fixed declared unit; never infer one from a residual."""

    value = _as_float(TERM_UNITS.get(name))
    return value if value is not None and value > 0.0 else None


def _objective_coverage(loss: Any, name: str, terms: Mapping[str, Any]) -> dict[str, Any] | None:
    """Use the objective's coverage API when the current implementation has it."""

    method = getattr(loss, "coverage", None)
    if not callable(method):
        return None
    try:
        result = method(name, terms)
    except (TypeError, KeyError, ValueError):
        return None
    # A named fraction field, so 0.1 is never read as a raw cell count.
    scalar = _as_float(result)
    if scalar is not None:
        return {"coverage": scalar}
    return None


def _coverage(
    loss: Any,
    name: str,
    terms: Mapping[str, Any],
    baseline_terms: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Normalise coverage from LossWeights and legacy term count fields."""

    supplied = _objective_coverage(loss, name, terms) or {}
    count_key = TERM_COUNT_KEYS.get(name)
    resolve_count = getattr(loss, "_count_key", None)
    if callable(resolve_count):
        count_key = resolve_count(name, terms)
    cells_key = f"{name}_cells"
    capped_key = f"{name}_capped"
    absent_key = f"{name}_absent"
    skipped_key = f"{name}_skipped"

    # The anchored baseline is the denominator; candidate counts are provenance only.
    count = _as_float(supplied.get("count"))
    has_count = count_key is not None and count_key in terms
    if count is None and has_count:
        count = _as_float(terms.get(count_key))
    cells = _as_float(supplied.get("cells"))
    if cells is None:
        cells = _as_float(supplied.get("expected"))
    if cells is None:
        cells = _as_float(terms.get(cells_key))
    capped = _as_float(supplied.get("capped"))
    if capped is None:
        capped = _as_float(terms.get(capped_key))
    absent = _as_float(supplied.get("absent"))
    if absent is None:
        absent = _as_float(terms.get(absent_key))
    skipped = _as_float(supplied.get("skipped"))
    if skipped is None:
        skipped = _as_float(terms.get(skipped_key))

    # Capped cells are evidence but not unqualified measurements; a zero count is unmeasured.
    baseline_counts = getattr(loss, "baseline_counts", None)
    baseline_count = None
    if isinstance(baseline_counts, Mapping) and count_key is not None:
        baseline_count = _as_float(baseline_counts.get(name))
    baseline_cells = None
    if baseline_terms is not None:
        baseline_cells = _as_float(baseline_terms.get(cells_key))
    if baseline_cells is None:
        anchored_terms = getattr(loss, "baseline_terms", None)
        if isinstance(anchored_terms, Mapping):
            baseline_cells = _as_float(anchored_terms.get(cells_key))
    # Pitched `lf` is a fixed oracle-band set (`lf_cells`, no `lf_notes`), never an absence.
    fixed_lf_cells = (
        name == "lf"
        and count_key is not None
        and count is None
        and (baseline_count is None or baseline_count <= 0.0)
        and cells is not None
        and cells > 0.0
        and (baseline_cells is None or baseline_cells > 0.0)
    )
    count_tracked = count_key is not None and not fixed_lf_cells
    # Coverage is against the anchored count: dropping from 100 to 10 cells is 10%, not 10/10.
    expected = baseline_cells if fixed_lf_cells else baseline_count
    if expected is None and "expected" in supplied:
        expected = _as_float(supplied.get("expected"))
    if expected is None and cells is not None and baseline_count is not None:
        expected = cells
    compared = None
    if cells is not None and capped is not None:
        compared = max(0.0, cells - capped)
    elif count is not None:
        compared = count
    # Count-tracked terms need a current and a positive anchored count; fixed sets are known.
    known = (
        (count is not None and baseline_count is not None and baseline_count > 0.0)
        if count_tracked
        else True
    )
    ratio = (
        max(0.0, min(1.0, cells / expected))
        if fixed_lf_cells and cells is not None and expected is not None and expected > 0.0
        else None
    )
    if count_tracked:
        measurement_count = count
        unmeasured = (
            not known
            or measurement_count is None
            or measurement_count <= 0.0
            or baseline_count is None
            or baseline_count <= 0.0
        )
    elif cells is not None:
        # Zero cells is unmeasured even when the raw residual reads as a perfect zero.
        unmeasured = cells <= 0.0
    else:
        unmeasured = False
    supplied_ratio = _as_float(supplied.get("coverage"))
    if fixed_lf_cells:
        # Keep the oracle-cell ratio; the objective API reports 1.0 when its count anchor is zero.
        pass
    elif supplied_ratio is not None:
        ratio = max(0.0, min(1.0, supplied_ratio))
    elif expected is not None and expected > 0.0 and count is not None and known:
        ratio = max(0.0, min(1.0, count / expected))
    # Complete needs every planned comparison: caps, absences and skips all leave it incomplete.
    positive_missing = any(value is not None and value > 0.0 for value in (capped, absent, skipped))
    coverage_complete = known and not positive_missing and ratio is not None and ratio >= 1.0
    return {
        "count": count,
        "expected": expected,
        "coverage": ratio,
        "capped": capped,
        "absent": absent,
        "skipped": skipped,
        "compared": compared,
        "known": known,
        "unmeasured": unmeasured,
        "coverage_complete": coverage_complete,
    }


def _term_contributions(loss: Any, terms: Mapping[str, Any]) -> dict[str, float]:
    """Use LossWeights' fixed-unit contribution API where available."""

    method = getattr(loss, "term_contributions", None)
    if callable(method):
        try:
            result = method(terms, fixed_units=True)
        except (TypeError, KeyError, ValueError):
            result = None
        if isinstance(result, Mapping):
            out: dict[str, float] = {}
            for name, value in result.items():
                converted = _as_float(value)
                if converted is not None:
                    out[str(name)] = converted
            return out

    weights = _weights(loss)
    out = {}
    for name, weight in weights.items():
        raw = _as_float(terms.get(name))
        unit = _declared_unit(name)
        if raw is not None and unit is not None:
            out[name] = weight * abs(raw) / unit
    return out


def aggregate_term_residuals(
    terms: Mapping[str, Any] | None,
    loss_or_weights: Any,
    *,
    active: tuple[str, ...] | None = None,
    baseline_terms: Mapping[str, Any] | None = None,
) -> dict[str, dict[str, Any]]:
    """Return absolute residuals for active aggregate terms.

    The return value is a mapping rather than a cell list.  Each entry is one
    aggregate term and carries its raw metric, its fixed-unit residual, its
    weight and measurement provenance.  This is the canonical input to
    :func:`quality_report`.
    """

    terms = terms or {}
    names = active or _active(loss_or_weights, terms)
    weights = _weights(loss_or_weights, names)
    # Uniform weights only for a bare mapping, never for a real LossWeights.
    if not weights and isinstance(loss_or_weights, Mapping):
        weights = {name: 1.0 for name in names}
    contributions = _term_contributions(loss_or_weights, terms)
    rows: dict[str, dict[str, Any]] = {}
    for name in names:
        raw = _as_float(terms.get(name))
        unit = _declared_unit(name)
        coverage = _coverage(loss_or_weights, name, terms, baseline_terms)
        # A zero count or non-comparable render is unknown, kept null rather than a zero match.
        comparable = _as_float(terms.get("comparable"))
        unmeasured = coverage["unmeasured"] or comparable == 0.0 or raw is None
        residual_units = None if unmeasured or unit is None else abs(raw) / unit
        worst = _as_float(terms.get(f"{name}_worst"))
        worst_units = (
            abs(worst) / unit if worst is not None and residual_units is not None else None
        )
        weight = weights.get(name)
        if weight is None:
            weight = 0.0
        weighted_units = None
        if residual_units is not None and weight > 0.0:
            # Prefer the objective's own contribution so units and weighting live in one place.
            weighted_units = contributions.get(name)
            if weighted_units is None:
                weighted_units = weight * residual_units
        target_met = (
            residual_units is not None
            and coverage["coverage_complete"]
            and residual_units <= DEFAULT_TERM_TARGET_UNITS
            and (worst_units is None or worst_units <= DEFAULT_TERM_TARGET_UNITS)
        )
        rows[name] = {
            "raw": raw,
            "unit": unit,
            "unit_source": "loss_weights.TERM_UNITS" if unit is not None else None,
            "residual_units": residual_units,
            "mean_raw": _as_float(terms.get(f"{name}_mean")),
            "upper_tail_raw": _as_float(terms.get(f"{name}_upper_tail")),
            "worst_raw": worst,
            "worst_residual_units": worst_units,
            "worst_condition": (
                {
                    axis: _as_float(terms.get(f"{name}_worst_{axis}"))
                    for axis in ("row", "note", "velocity")
                }
                if worst is not None
                else None
            ),
            "weight": weight,
            "weighted_units": weighted_units,
            "target_units": DEFAULT_TERM_TARGET_UNITS,
            "target_met": target_met if residual_units is not None else None,
            "count": coverage["count"],
            "expected": coverage["expected"],
            "coverage": coverage["coverage"],
            "capped": coverage["capped"],
            "absent": coverage["absent"],
            "skipped": coverage["skipped"],
            "compared": coverage["compared"],
            "known": coverage["known"],
            "unmeasured": unmeasured,
            "coverage_complete": coverage["coverage_complete"],
        }
    return rows


def absolute_weighted_mean_units(
    residuals: Mapping[str, Mapping[str, Any]],
) -> float | None:
    """Return the weighted mean of aggregate residuals in fixed units.

    Unknown terms are excluded by default and are reported separately.  The
    caller must inspect ``unmeasured_terms``/``full_known_coverage`` before
    treating the number as a complete quality result.
    """

    numerator = 0.0
    denominator = 0.0
    for row in residuals.values():
        value = _as_float(row.get("weighted_units"))
        weight = _as_float(row.get("weight"))
        if value is None or weight is None or weight <= 0.0:
            continue
        numerator += value
        denominator += weight
    return numerator / denominator if denominator > 0.0 else None


def quality_report(
    terms: Mapping[str, Any] | None,
    loss_or_weights: Any,
    *,
    loss: float | None = None,
    baseline_terms: Mapping[str, Any] | None = None,
    validation: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Build a JSON-ready absolute quality report.

    ``attainment_pct`` is the percentage of *active aggregate terms* whose
    residual is at or below one declared term unit. It does not count cells and
    does not claim perceptual similarity. A report with missing coverage is
    explicitly unvalidated even when every measured term attains its target.
    """

    terms = terms or {}
    active = _active(loss_or_weights, terms)
    rows = aggregate_term_residuals(
        terms, loss_or_weights, active=active, baseline_terms=baseline_terms
    )
    measured = [row for row in rows.values() if row["residual_units"] is not None]
    met = [row for row in measured if row["target_met"]]
    unknown = [name for name, row in rows.items() if row["unmeasured"]]
    weighted_mean = absolute_weighted_mean_units(rows)
    weighted_total = sum(
        float(row["weighted_units"]) for row in rows.values() if row["weighted_units"] is not None
    )
    total_weight = sum(float(row["weight"]) for row in rows.values() if row["weight"] > 0.0)
    attainment = 100.0 * len(met) / len(active) if active else None
    measured_attainment = 100.0 * len(met) / len(measured) if measured else None
    full_known_coverage = bool(active) and all(
        row["coverage_complete"] and not row["unmeasured"] for row in rows.values()
    )
    baseline = None
    if baseline_terms is not None:
        # The baseline shows the improvement and the remaining distance together.
        baseline_rows = aggregate_term_residuals(
            baseline_terms,
            loss_or_weights,
            active=active,
            baseline_terms=baseline_terms,
        )
        baseline = {
            "absolute_weighted_mean_units": absolute_weighted_mean_units(baseline_rows),
            "terms": baseline_rows,
        }
    validation_record = dict(validation) if validation is not None else None
    if validation_record is None:
        validation_record = {"status": "independent_missing"}
    elif not validation_record.get("independent", True):
        validation_record.setdefault("status", "independent_missing")

    return {
        "basis": QUALITY_BASIS,
        "metric": QUALITY_METRIC,
        "unit_source": "loss_weights.TERM_UNITS",
        "term_target_units": DEFAULT_TERM_TARGET_UNITS,
        "target_basis": "one declared term unit for the aggregate and each reported worst note",
        "aggregation_tail_fraction": _as_float(terms.get("aggregation_tail_fraction")),
        "loss": _as_float(loss),
        "active_terms": list(active),
        "terms": rows,
        "absolute_weighted_mean_units": weighted_mean,
        "absolute_weighted_sum_units": weighted_total,
        "active_weight": total_weight,
        "attainment_pct": attainment,
        "measured_attainment_pct": measured_attainment,
        "attained_terms": [name for name, row in rows.items() if row["target_met"]],
        "unmeasured_terms": unknown,
        "full_known_coverage": full_known_coverage,
        "met": full_known_coverage and len(met) == len(active),
        "baseline": baseline,
        "validation": validation_record,
    }


def significant_term_regressions(
    start_terms: Mapping[str, Any] | None,
    selected_terms: Mapping[str, Any] | None,
    loss_or_weights: Any,
    *,
    min_delta_units: float = 1.0,
) -> list[dict[str, Any]]:
    """Report material per-term regressions without making a blanket refusal.

    The one-unit default is the same declared aggregate target used by the
    quality report. Callers can lower or raise it for a review, but this helper
    is diagnostic: a regression is evidence to inspect, not an automatic
    rejection of every otherwise useful candidate.
    """

    if not start_terms or not selected_terms:
        return []
    active = _active(loss_or_weights, start_terms)
    start = aggregate_term_residuals(
        start_terms, loss_or_weights, active=active, baseline_terms=start_terms
    )
    selected = aggregate_term_residuals(
        selected_terms, loss_or_weights, active=active, baseline_terms=start_terms
    )
    out = []
    for name in active:
        before = _as_float(start.get(name, {}).get("residual_units"))
        after = _as_float(selected.get(name, {}).get("residual_units"))
        if before is None or after is None or after - before < min_delta_units:
            continue
        out.append(
            {
                "term": name,
                "start_units": before,
                "selected_units": after,
                "delta_units": after - before,
                "start_coverage": start[name].get("coverage"),
                "selected_coverage": selected[name].get("coverage"),
            }
        )
    return sorted(out, key=lambda row: row["delta_units"], reverse=True)


def dropout_terms(
    start_terms: Mapping[str, Any] | None,
    selected_terms: Mapping[str, Any] | None,
    loss_or_weights: Any,
) -> list[str]:
    """Return active terms measured at start but entirely absent when selected."""

    if not start_terms or not selected_terms:
        return []
    active = _active(loss_or_weights, start_terms)
    start = aggregate_term_residuals(
        start_terms, loss_or_weights, active=active, baseline_terms=start_terms
    )
    selected = aggregate_term_residuals(
        selected_terms, loss_or_weights, active=active, baseline_terms=start_terms
    )
    return [
        name for name in active if not start[name]["unmeasured"] and selected[name]["unmeasured"]
    ]


__all__ = [
    "DEFAULT_TERM_TARGET_UNITS",
    "QUALITY_BASIS",
    "QUALITY_METRIC",
    "absolute_weighted_mean_units",
    "aggregate_term_residuals",
    "dropout_terms",
    "quality_report",
    "significant_term_regressions",
]
