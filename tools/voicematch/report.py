"""The end-of-run report, and the write-back it applies.

Everything a fit has to say once the search is over: the loss trajectory grouped
by stage, the held-out verdict, which knobs moved, a paste-ready overrides
string, and a unified diff of the source the values are about to land in.
`--dry-run` prints the diff without writing; `--out` records the whole result as
JSON.
"""

from __future__ import annotations

import difflib
import json
import math
from pathlib import Path

from _repo import REPO_ROOT
from fit_quality import dropout_terms, quality_report, significant_term_regressions
from knobs import at_bound, format_value, tunable_overrides
from loss_aggregate import DEFAULT_TAIL_FRACTION
from toneclass import fit_profile
from writeback import (
    materialize,
    patch_field_assignments,
    write_drum_fields,
    write_patch_fields,
)

#: How far a fit may move the voice's whole-grid level before the report says so
#: rather than leaving it to be noticed by ear. Generous, because a voicing
#: change legitimately moves the level a little; anything past this is the fit
#: having bought its shape with loudness.
LEVEL_DRIFT_WARN_DB = 4.0


def _finite(value):
    """Make numbers safe for strict JSON; an unscorable loss is null."""

    if value is None:
        return None
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def _json_safe(value):
    """Recursively replace non-finite numeric values before writing --out."""

    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return value


def _candidate_record(evaluator, values, fallback_loss=None, fallback_terms=None):
    """Return the exact cached terms/loss belonging to one value vector."""

    terms = None
    terms_for = getattr(evaluator, "terms_for", None)
    if callable(terms_for):
        terms = terms_for(values)
    if terms is None and fallback_terms is not None:
        terms = fallback_terms
    loss_for = getattr(evaluator, "loss_for", None)
    loss = loss_for(values) if callable(loss_for) else None
    if loss is None or loss == float("inf"):
        loss = fallback_loss
    return terms, _finite(loss)


def _quality_display(report: dict) -> None:
    """Print the absolute aggregate-term reading without calling it similarity."""

    print("\n== absolute quality (aggregate terms) ==")
    mean = report.get("absolute_weighted_mean_units")
    mean_text = "unknown" if mean is None else f"{mean:.3f} units"
    attainment = report.get("attainment_pct")
    attainment_text = "unknown" if attainment is None else f"{attainment:.1f}%"
    print(f"  basis: {report.get('basis')} / {report.get('metric')}")
    print(f"  weighted residual: {mean_text}; aggregate-term target attainment: {attainment_text}")
    unknown = report.get("unmeasured_terms") or []
    if unknown:
        print(f"  unmeasured terms: {', '.join(unknown)}")
    for name, row in report.get("terms", {}).items():
        residual = row.get("residual_units")
        text = "unknown" if residual is None else f"{residual:.3f} units"
        count = row.get("count")
        coverage = row.get("coverage")
        coverage_text = "unknown" if coverage is None else f"{coverage * 100.0:.1f}%"
        print(
            f"  {name}: raw={row.get('raw')!r} residual={text} "
            f"weight={row.get('weight', 0.0):g} count={count!r} coverage={coverage_text} "
            f"capped={row.get('capped')!r} absent={row.get('absent')!r}"
        )
        if row.get("worst_residual_units") is not None:
            condition = row["worst_condition"]
            print(
                f"    worst={row['worst_residual_units']:.3f} units "
                f"note={condition.get('note')!r} velocity={condition.get('velocity')!r} "
                f"target_met={row['target_met']}"
            )


def broad_budget_warning(max_evals: int | None, dimension: int) -> str | None:
    """Warn when a runtime fit's budget is below the 2*n+1 broad-coverage baseline."""

    minimum = 2 * dimension + 1
    if max_evals is None or not dimension or max_evals >= minimum:
        return None
    return (
        f"max_evals={max_evals} is below the 2*n+1 broad-coverage baseline "
        f"({minimum}) for {dimension} runtime dimensions; "
        "the search may leave dimensions unexplored"
    )


def search_effort(evaluator, args, dimension: int, spec_dimension: int | None = None) -> dict:
    """Describe the search budget and flag a runtime fit that cannot probe broadly."""

    evaluations = len(getattr(evaluator, "trajectory", ()) or ())
    max_evals = getattr(args, "max_evals", None)
    max_evals = int(max_evals) if max_evals is not None else None
    evals_per_knob = evaluations / dimension if dimension else None
    minimum = 2 * dimension + 1 if dimension else 0
    warning = (
        None
        if getattr(evaluator, "needs_rebuild", False)
        else broad_budget_warning(max_evals, dimension)
    )
    return {
        "optimizer": getattr(args, "optimizer", None),
        "dimension": dimension,
        "spec_dimension": spec_dimension if spec_dimension is not None else dimension,
        "max_evals": max_evals,
        "evaluations": evaluations,
        "evals_per_knob": evals_per_knob,
        "minimum_broad_evals": minimum or None,
        "budget_warning": warning,
    }


def print_level_drift(evaluator) -> None:
    """How far the winner moved the voice's overall level, which nothing charges for.

    Every loss term is either normalised by the note's own level or measured
    around the grid's median offset — deliberately, so that a fit does not spend
    its budget on an output gain. The cost is that the offset itself is free: a
    candidate that halves the voice and gets a slightly better spectrum wins,
    and the report otherwise reads as an unqualified improvement.

    Measured on a hi-hat fit that came back 31 dB down with its band profile
    bit-identical to the same values at the original gain. So this is printed
    always and not only past the threshold: a fit that held its level is worth
    saying so about, since that is the case the reader is entitled to assume and
    otherwise cannot check.
    """
    start = getattr(evaluator, "start_level_offset_db", None)
    best = getattr(evaluator, "best_level_offset_db", None)
    if start is None or best is None:
        return
    drift = best - start
    print("\n== level ==")
    print(
        f"  the winner's whole-grid level sits {drift:+.1f} dB against the start point "
        f"({start:+.1f} -> {best:+.1f} dB against the reference)"
    )
    if abs(drift) >= LEVEL_DRIFT_WARN_DB:
        print(
            f"  that is past {LEVEL_DRIFT_WARN_DB:.0f} dB and no term charged for it: "
            f"check that the shape was not bought with loudness before keeping these "
            f"values, since every metric above is level-normalised"
        )


def report_result(knobs, pristine, best_values, evaluator, args, extra=None) -> None:
    """Print the loss trajectory, per-knob deltas, and the source diff."""
    extra = extra or {}
    start_values = [k.start_value for k in knobs]
    search_values = list(getattr(evaluator, "best_values", None) or best_values)
    search_terms, search_loss = _candidate_record(
        evaluator,
        search_values,
        fallback_loss=getattr(evaluator, "best_loss", None),
        fallback_terms=getattr(evaluator, "best_terms", None),
    )
    selected_terms, selected_loss = _candidate_record(
        evaluator,
        best_values,
        fallback_terms=(
            getattr(evaluator, "best_terms", None) if best_values == search_values else None
        ),
    )
    if selected_loss is None and best_values == start_values and evaluator.trajectory:
        selected_loss = evaluator.trajectory[0][1]
    evaluator.selected_values = list(best_values)
    evaluator.selected_terms = selected_terms
    evaluator.selected_loss = selected_loss
    loss_object = getattr(evaluator, "loss", None)
    validation_record = extra.get("validation")
    selected_validation = validation_record
    if validation_record is not None:
        selected_validation = dict(validation_record)
        selected_validation["evaluated_values"] = list(best_values)
        if best_values == search_values:
            selected_validation["candidate"] = "selected_search_winner"
            if "best" in selected_validation:
                selected_validation["selected"] = selected_validation["best"]
        else:
            # A fallback selects the measured baseline, never the refused winner's score.
            selected_validation["candidate"] = "selected_defaults"
            selected_validation.pop("best", None)
            if best_values == start_values and "start" in selected_validation:
                selected_validation["selected"] = selected_validation["start"]
            else:
                selected_validation.update(
                    status="independent_unavailable", independent=False, selected=None
                )
    search_quality = quality_report(
        search_terms,
        loss_object,
        loss=search_loss,
        baseline_terms=getattr(evaluator, "baseline_terms", None),
        validation=validation_record,
    )
    selected_quality = quality_report(
        selected_terms,
        loss_object,
        loss=selected_loss,
        baseline_terms=getattr(evaluator, "baseline_terms", None),
        validation=selected_validation,
    )
    baseline_terms = getattr(evaluator, "baseline_terms", None)
    if baseline_terms is not None and selected_terms is not None:
        # Regressions of the values selected after every guard, not of the search candidate.
        regressions = significant_term_regressions(baseline_terms, selected_terms, loss_object)
    else:
        regressions = getattr(evaluator, "selected_regressions", None) or []
    if baseline_terms is not None and selected_terms is not None:
        selected_dropout = dropout_terms(baseline_terms, selected_terms, loss_object)
    else:
        selected_dropout = getattr(evaluator, "selected_dropout", None) or []
    search_regressions = getattr(evaluator, "search_regressions", None) or []
    if not search_regressions and baseline_terms is not None and search_terms is not None:
        search_regressions = significant_term_regressions(baseline_terms, search_terms, loss_object)
    search_dropout = getattr(evaluator, "search_dropout", None) or []

    print("\n== loss trajectory (improvements only) ==")
    if evaluator.trajectory:
        # Only the steps that moved it, grouped by stage: a four-hundred
        # evaluation run prints four hundred copies of the same number
        # otherwise, and losses from two stages are not comparable — each stage
        # scores under its own weights.
        stages: list[tuple[str, list[str]]] = []
        previous = None
        for i, (best, _, stage) in enumerate(evaluator.trajectory, start=1):
            if not stages or stages[-1][0] != stage:
                stages.append((stage, []))
                previous = None
            if previous is None or best < previous - 1e-9:
                stages[-1][1].append(f"#{i} {best:.4f}")
                previous = best
        for stage, steps in stages:
            print(f"  [{stage}] " + "  ->  ".join(steps))
        # What the run measured is worth saying when it is not what it looked
        # at: a fit resumed against a store did the same search as the one that
        # filled it, and a report that only counted evaluations would read as if
        # it had rendered them again.
        paid = (
            ""
            if evaluator.n_renders == len(evaluator.trajectory)
            else f", {evaluator.n_renders} rendered"
        )
        print(
            f"  initial {evaluator.trajectory[0][1]:.4f}  ->  best {evaluator.best_loss:.4f}"
            f"  over {len(evaluator.trajectory)} evaluations{paid}"
        )
        if evaluator.normalize:
            print("  (a ratio against the start point, which scores 1.0)")
    else:
        print("  (no evaluations)")

    if extra.get("validation"):
        v = extra["validation"]
        if "start" not in v or "best" not in v:
            print(
                f"\n== held-out {v.get('axis', 'axis')} ==\n"
                "  independent validation: missing — final quality is unvalidated"
            )
        else:
            margin = v["start"] - v["best"]
            if margin > 0.005:
                verdict = "generalises"
            elif margin < -0.005:
                verdict = "does NOT generalise — worse than the defaults off the probe"
            else:
                verdict = "unchanged off the probe"
            print(f"\n== held-out {v['axis']} {v['held_out']} ==")
            print(f"  start {v['start']:.4f}  ->  best {v['best']:.4f}   {verdict}")
            if margin < -0.005:
                print(
                    "  the fitted values are worse than the defaults on notes the fit never "
                    "saw; treat the result as overfitted to the probe"
                )

    _quality_display(selected_quality)
    if regressions:
        print("  significant per-term regressions (review; no blanket rejection):")
        for row in regressions:
            print(
                f"    {row['term']}: {row['start_units']:.3f} -> "
                f"{row['selected_units']:.3f} units (+{row['delta_units']:.3f})"
            )

    effort = search_effort(
        evaluator,
        args,
        int(extra.get("search_dimension", len(knobs))),
        extra.get("spec_dimension", len(knobs)),
    )
    print("\n== search effort ==")
    print(
        f"  optimizer={effort['optimizer'] or 'unknown'} dimension={effort['dimension']} "
        f"evaluations={effort['evaluations']} max_evals={effort['max_evals']!r} "
        f"evals_per_knob={effort['evals_per_knob']!r}"
    )
    if effort["budget_warning"]:
        print(f"  warning: {effort['budget_warning']}")
    full_spec_warning = extra.get("full_spec_budget_warning")
    if full_spec_warning and full_spec_warning != effort["budget_warning"]:
        print(f"  warning: {full_spec_warning}")
        effort["full_spec_budget_warning"] = full_spec_warning

    print_level_drift(evaluator)

    print("\n== knob values (start -> best) ==")
    moved = 0
    for knob, best in zip(knobs, best_values):
        if format_value(best) == format_value(knob.start_value):
            continue
        moved += 1
        rel = knob.file.relative_to(REPO_ROOT) if knob.file else Path("(program table)")
        kind = "runtime" if knob.tunable else "source"
        end = at_bound(knob, best)
        note = (
            f"  <- at its {end}; widen the range or accept that the model cannot go "
            f"further this way"
            if end
            else ""
        )
        print(
            f"  [{kind}] {rel}  {knob.label}:  "
            f"{format_value(knob.start_value)} -> {format_value(best)}{note}"
        )
    print(f"  ({moved} of {len(knobs)} knobs moved; the rest stayed at their defaults)")

    print("\n== overrides (paste-ready, for an ad-hoc render) ==")
    overrides = tunable_overrides(knobs, best_values, changed_only=True)
    print(f"  SONARE_TUNING_OVERRIDES='{overrides}'" if overrides else "  (nothing moved)")

    per_patch, per_drum, unnamed = patch_field_assignments(knobs, best_values)
    if unnamed:
        print("\n== family patch fields (no per-patch assignment site) ==")
        print("  These are built by a loop over a table, so there is no line to update;")
        print("  place them where that table is built, or keep them as overrides above.")
        for key in sorted(unnamed):
            print(f"    {key}")

    edited = materialize(knobs, best_values, pristine, source_only=False)
    baseline = dict(pristine)
    written: dict[Path, str] = {}
    # Each write-back starts from what the previous one produced, so two kinds
    # of edit landing in one file compose instead of overwriting each other.
    if per_patch:
        written.update(write_patch_fields(per_patch, edited))
    if per_drum:
        written.update(write_drum_fields(per_drum, {**edited, **written}))
    for path, text in written.items():
        baseline.setdefault(path, path.read_text())
        edited[path] = text

    print("\n== source diff (pristine -> best) ==")
    any_diff = False
    for path, new_text in edited.items():
        rel = str(path.relative_to(REPO_ROOT))
        diff = difflib.unified_diff(
            baseline[path].splitlines(keepends=True),
            new_text.splitlines(keepends=True),
            fromfile=f"a/{rel}",
            tofile=f"b/{rel}",
        )
        chunk = "".join(diff)
        if chunk:
            any_diff = True
            print(chunk, end="")
    if not any_diff:
        print("  (no change from pristine)")

    if args.out:
        search_effort_record = effort
        profile = fit_profile(
            args.program,
            drum_note=getattr(args, "drum_note", None),
            percussive=getattr(args, "percussive", False),
        )
        record = {
            "program": args.program,
            "drum_note": args.drum_note,
            "pattern": args.pattern,
            "notes": args.notes,
            "velocities": args.velocities,
            "loss": {
                "start": _finite(evaluator.trajectory[0][1]) if evaluator.trajectory else None,
                # `best` stays the search winner; the explicit objects below govern a fallback.
                "best": search_loss,
                "normalized": evaluator.normalize,
                "selected": selected_loss,
            },
            "evaluations": len(evaluator.trajectory),
            "renders": evaluator.n_renders,
            "knobs": [
                {
                    "key": k.label,
                    "start": _finite(k.start_value),
                    "best": _finite(b),
                    "min": _finite(k.lo),
                    "max": _finite(k.hi),
                    "scale": "log" if k.log else "linear",
                }
                for k, b in zip(knobs, best_values)
            ],
            "overrides": overrides,
            "validation": extra.get("validation"),
            "validation_partition": extra.get("validation_partition"),
            "room": extra.get("room"),
            "pinned": extra.get("pinned", []),
            "search": search_effort_record,
            "fit_profile": {
                "tone_class": profile.tone_class.value,
                "excitation": profile.excitation.value,
                "keyboard": profile.keyboard,
                "weights": getattr(loss_object, "weights", {}),
                "loss_tail_fraction": getattr(args, "loss_tail_fraction", DEFAULT_TAIL_FRACTION),
            },
            "search_winner": {
                "values": [_finite(v) for v in search_values],
                "loss": search_loss,
                "quality": search_quality,
                "dropout_terms": search_dropout,
                "regressions": search_regressions,
            },
            "selected": {
                "values": [_finite(v) for v in best_values],
                "loss": selected_loss,
                "quality": selected_quality,
                "write_back_refusal": getattr(evaluator, "write_back_refusal", None),
                "dropout_terms": selected_dropout,
                "regressions": regressions,
                "refused_candidate": (
                    {
                        "values": [_finite(v) for v in search_values],
                        "loss": search_loss,
                        "dropout_terms": search_dropout,
                        "regressions": search_regressions,
                    }
                    if getattr(evaluator, "write_back_refusal", None)
                    else None
                ),
            },
        }
        Path(args.out).write_text(json.dumps(_json_safe(record), indent=2, allow_nan=False) + "\n")
        print(f"\nResult written to {args.out}")

    if args.dry_run:
        print("\n--dry-run: source left pristine, best values NOT written.")
    else:
        for path, text in edited.items():
            # The write-back is computed from the text snapshotted when the fit
            # started, so a file edited meanwhile loses that edit. Say so rather
            # than let it happen silently: the values are in the diff and the
            # overrides string above either way.
            if path in baseline and path.exists() and path.read_text() != baseline[path]:
                print(
                    f"warning: {path} changed since the fit started; the write-back "
                    f"below replaces that change with the fitted values"
                )
            path.write_text(text)
        print("\nBest values written to source.")
