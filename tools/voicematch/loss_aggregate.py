"""Preserve difficult notes in the fitting objective without another render."""

from __future__ import annotations

import math

from loss import score_terms
from loss_dimensions import LEVEL_DELTA_CAP_DB, LOSS_TERMS, _velcurve_groups
from loss_weights import TERM_COUNT_KEYS

DEFAULT_TAIL_FRACTION = 0.25
GRID_TERMS = frozenset(("mss", "dyn", "hfdyn", "modedyn", "onsetvel", "velspan", "velcurve", "kit"))
#: Grid terms reduced per pitch, whose tail is taken over each pitch scored alone.
PITCH_GROUP_TERMS = frozenset(("dyn", "hfdyn", "velspan"))
#: The tail a gated dimension's own term takes where its gate bounds a p90: the worst tenth.
PRESENCE_TAIL_FRACTION = 0.10
#: The gate statistics a term's mean, and only its tail, can see.
CENTRAL_STATISTICS = frozenset(("median", "abs_median"))


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


def _take_tail(
    aggregate: dict,
    name: str,
    observations: list,
    oracle_rows,
    fraction: float,
    axes=("note", "velocity"),
) -> None:
    """Raise `aggregate[name]` to the mean of its worst `fraction` of observations, with metadata."""
    tail = upper_tail_mean([value for value, _ in observations], fraction)
    worst, index = max(observations)
    aggregate[f"{name}_mean"] = aggregate[name]
    aggregate[f"{name}_upper_tail"] = tail
    aggregate[f"{name}_worst"] = worst
    aggregate[f"{name}_worst_row"] = float(index)
    for axis in axes:
        value = _finite(oracle_rows[index].get(axis))
        if value is not None:
            aggregate[f"{name}_worst_{axis}"] = value
    aggregate[name] = max(aggregate[name], tail)


def _pitch_tails(aggregate, model_rows, oracle_rows, term_tails, kwargs) -> None:
    """The per-term tail of a pitch-grouped grid term, each pitch scored on its own rows."""
    names = [n for n in term_tails if n in PITCH_GROUP_TERMS and n in aggregate]
    if not names:
        return
    pitches: dict[float, list[int]] = {}
    for i, row in enumerate(model_rows):
        if row.get("note") is not None:
            pitches.setdefault(row["note"], []).append(i)
    scored = [
        (idx[0], score_terms([model_rows[i] for i in idx], [oracle_rows[i] for i in idx], **kwargs))
        for idx in pitches.values()
    ]
    for name in names:
        count_key = TERM_COUNT_KEYS[name]
        observations = [
            (value, first)
            for first, terms in scored
            if terms is not None
            and terms.get(count_key, 0.0) > 0.0
            and (value := _finite(terms.get(name))) is not None
        ]
        if observations:
            _take_tail(aggregate, name, observations, oracle_rows, term_tails[name], ("note",))
            aggregate[f"{name}_tail_fraction"] = term_tails[name]


def robust_score_terms(
    model_rows,
    oracle_rows,
    *,
    tail_fraction=DEFAULT_TAIL_FRACTION,
    term_tails: dict[str, float] | None = None,
    **kwargs,
):
    """Score the grid and retain its worst measured quarter of notes.

    Relations keep their full-grid reducer. Level uses the grid's common gain
    offset, never a separate offset that would erase each note's difference.
    Zero selects the historical mean objective; metadata comes from the same
    measured rows and requires no additional synthesis or feature extraction.
    `term_tails` gives a term its own fraction; a pitch-grouped grid term
    (`PITCH_GROUP_TERMS`) named there takes its tail over pitches.
    """
    if not math.isfinite(tail_fraction) or not 0.0 <= tail_fraction <= 1.0:
        raise ValueError("tail_fraction must be finite and in [0, 1]")
    term_tails = dict(term_tails or {})
    for name, fraction in term_tails.items():
        if not math.isfinite(fraction) or not 0.0 < fraction <= 1.0:
            raise ValueError(f"the tail fraction of {name} must be finite and in (0, 1]")
    aggregate = score_terms(model_rows, oracle_rows, **kwargs)
    if aggregate is None or not model_rows or not (tail_fraction or term_tails):
        return aggregate
    _pitch_tails(aggregate, model_rows, oracle_rows, term_tails, kwargs)
    # A row unscorable alone stays charged in the grid result and only leaves the tail statistics.
    rows = [score_terms([m], [o], **kwargs) for m, o in zip(model_rows, oracle_rows)]
    offset = aggregate.get("level_offset_db", 0.0)
    for name in LOSS_TERMS:
        fraction = term_tails.get(name, tail_fraction)
        if name in GRID_TERMS or not fraction:
            continue
        observations = []
        uncapped = []
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
                uncapped.append(None if terms is None else _finite(terms.get(f"{name}_uncapped")))
        if not observations:
            continue
        _take_tail(aggregate, name, observations, oracle_rows, fraction)
        if name in term_tails:
            aggregate[f"{name}_tail_fraction"] = fraction
        # The uncapped reading takes the same tail, and only when every row carries one.
        if f"{name}_uncapped" in aggregate and all(value is not None for value in uncapped):
            aggregate[f"{name}_uncapped"] = max(
                aggregate[f"{name}_uncapped"], upper_tail_mean(uncapped, fraction)
            )
            aggregate[f"{name}_uncapped_worst"] = max(uncapped)
    if not tail_fraction:
        return aggregate
    velocity_curves = _velcurve_groups(model_rows, oracle_rows)
    if velocity_curves:
        residuals = [group.residual_db for group in velocity_curves]
        tail = upper_tail_mean(residuals, tail_fraction)
        worst = max(velocity_curves, key=lambda group: group.residual_db)
        aggregate["velcurve_mean"] = aggregate["velcurve"]
        aggregate["velcurve_upper_tail"] = tail
        aggregate["velcurve_worst"] = worst.residual_db
        aggregate["velcurve_worst_note"] = float(worst.note)
        aggregate["velcurve"] = max(aggregate["velcurve"], tail)
    aggregate["aggregation_tail_fraction"] = tail_fraction
    return aggregate


def own_term_aggregates(terms: dict[str, dict], tail_fraction: float) -> dict[str, float]:
    """Set each own term's `aggregate`, `tail_fraction` and `covers`; return the terms' own tails.

    `terms` holds the own terms of gated dimensions, presence or already weighted. A term whose
    gate bounds a p90 takes the worst `PRESENCE_TAIL_FRACTION` of its notes (of its
    pitches for `PITCH_GROUP_TERMS`), or the run's tail where that is narrower. `covers` is the
    gated statistics the aggregate reaches: a p90 only under a tail that narrow.
    """
    own: dict[str, float] = {}
    for name, entry in terms.items():
        stats = set(entry.get("statistics") or ())
        tails = name in PITCH_GROUP_TERMS or name not in GRID_TERMS
        fraction = tail_fraction if tails and name not in GRID_TERMS else 0.0
        if "p90" in stats and tails:
            fraction = min(f for f in (PRESENCE_TAIL_FRACTION, tail_fraction) if f > 0.0)
            own[name] = fraction
        entry["aggregate"] = "tail" if fraction else "mean"
        entry["tail_fraction"] = fraction or None
        reach = CENTRAL_STATISTICS | (
            {"p90"} if 0.0 < fraction <= PRESENCE_TAIL_FRACTION else set()
        )
        entry["covers"] = sorted(stats & reach)
    return own
