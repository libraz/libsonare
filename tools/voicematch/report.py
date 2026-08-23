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
from pathlib import Path

from _repo import REPO_ROOT
from knobs import at_bound, format_value, tunable_overrides
from writeback import (
    materialize,
    patch_field_assignments,
    write_drum_fields,
    write_patch_fields,
)


def report_result(knobs, pristine, best_values, evaluator, args, extra=None) -> None:
    """Print the loss trajectory, per-knob deltas, and the source diff."""
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
        print(f"  initial {evaluator.trajectory[0][1]:.4f}  ->  best {evaluator.best_loss:.4f}"
              f"  over {len(evaluator.trajectory)} evaluations")
        if evaluator.normalize:
            print("  (a ratio against the start point, which scores 1.0)")
    else:
        print("  (no evaluations)")

    extra = extra or {}
    if extra.get("validation"):
        v = extra["validation"]
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
            print("  the fitted values are worse than the defaults on notes the fit never "
                  "saw; treat the result as overfitted to the probe")

    print("\n== knob values (start -> best) ==")
    moved = 0
    for knob, best in zip(knobs, best_values):
        if format_value(best) == format_value(knob.start_value):
            continue
        moved += 1
        rel = knob.file.relative_to(REPO_ROOT) if knob.file else Path("(program table)")
        kind = "runtime" if knob.tunable else "source"
        end = at_bound(knob, best)
        note = (f"  <- at its {end}; widen the range or accept that the model cannot go "
                f"further this way" if end else "")
        print(f"  [{kind}] {rel}  {knob.label}:  "
              f"{format_value(knob.start_value)} -> {format_value(best)}{note}")
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
            fromfile=f"a/{rel}", tofile=f"b/{rel}",
        )
        chunk = "".join(diff)
        if chunk:
            any_diff = True
            print(chunk, end="")
    if not any_diff:
        print("  (no change from pristine)")

    if args.out:
        record = {
            "program": args.program,
            "drum_note": args.drum_note,
            "pattern": args.pattern,
            "notes": args.notes,
            "velocities": args.velocities,
            "loss": {"start": evaluator.trajectory[0][1] if evaluator.trajectory else None,
                     "best": evaluator.best_loss,
                     "normalized": evaluator.normalize},
            "evaluations": len(evaluator.trajectory),
            "knobs": [
                {"key": k.label, "start": k.start_value, "best": b,
                 "min": k.lo, "max": k.hi, "scale": "log" if k.log else "linear"}
                for k, b in zip(knobs, best_values)
            ],
            "overrides": overrides,
            "validation": extra.get("validation"),
            "room": extra.get("room"),
        }
        Path(args.out).write_text(json.dumps(record, indent=2) + "\n")
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
                print(f"warning: {path} changed since the fit started; the write-back "
                      f"below replaces that change with the fitted values")
            path.write_text(text)
        print("\nBest values written to source.")
