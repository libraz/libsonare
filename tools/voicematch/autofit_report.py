"""The two verdicts a finished fit gets: whether to keep its winner, and where it pinned."""

from __future__ import annotations

import sys

from knobs import at_bound


def winner_or_defaults(knobs, best_values: list[float], evaluator,
                       validation: dict | None = None) -> list[float]:
    """The fit's winner, unless something measured says it is worse than the start.

    Three readings can say that, and all are refusals to write rather than
    findings to report on the way past:

    Every stage's loss is a ratio against the compiled-in defaults, so those
    score exactly 1.0 and anything above it is a search that never found its own
    start point. Only meaningful while the loss is normalised — `--raw-loss` has
    no such reference point and is left alone.

    A hold-out is the same failure measured better: it scores the winner on
    notes or velocities the fit never saw, and it is the only reading in the run
    that can speak for anywhere but the probe. A winner that loses there is
    fitted to the probe, and writing it is how a fit trades a whole voice for
    three velocities. A wash is not a loss — an "unchanged off the probe" result
    improved the measured objective and is worse nowhere, so it is kept, and the
    verdict is printed above this either way.

    `tnr` is the one term in `loss.py` that is one-sided: it charges only where
    the model is NOISIER than the reference, so a candidate that walks the model
    past the reference leaves the term nothing to say and collects its whole
    unit for doing it. `loss.py` reports `tnr_notes` so a reader can tell that
    apart from a genuine match, and deliberately keeps it out of
    `TERM_COUNT_KEYS` — but that objection is about the LEVEL, since a physical
    model starting cleaner than a sampled recording is ordinary and charging it
    would penalise a voice for being clean. The DELTA is a different reading and
    the objection does not reach it: a winner comparing on fewer notes than the
    start point stopped comparing during the fit, which is the term going quiet
    rather than being satisfied.
    """
    # Which refusal fired, for the --out record. A refusal leaves the tree
    # showing a voice whose values did not change, and that reads the same as a
    # search that simply found nothing — so the reason has to outlive the
    # terminal or the finding does not survive the run.
    evaluator.write_back_refusal = None
    if getattr(evaluator, "normalize", False) and evaluator.best_loss > 1.0:
        evaluator.write_back_refusal = "lost_to_start"
        print(f"\nthe winner scores {evaluator.best_loss:.4f} against the defaults' 1.0 — "
              f"keeping the defaults, since a fit that lost to its own start point has "
              f"nothing to write", file=sys.stderr)
        return [k.start_value for k in knobs]
    if validation and validation["best"] - validation["start"] > 0.005:
        evaluator.write_back_refusal = "fitted_to_the_probe"
        print(f"\nthe winner scores {validation['best']:.4f} against the defaults' "
              f"{validation['start']:.4f} on the held-out {validation['axis']} — keeping "
              f"the defaults, since values that lose where the fit could not see are "
              f"fitted to the probe", file=sys.stderr)
        return [k.start_value for k in knobs]
    start_tnr = getattr(evaluator, "start_tnr_notes", None)
    best_tnr = getattr(evaluator, "best_tnr_notes", None)
    if start_tnr is not None and best_tnr is not None and best_tnr < start_tnr:
        evaluator.write_back_refusal = "objective_went_blind"
        print(f"\nthe winner is scored against the reference's noise on {best_tnr:g} "
              f"notes where the start point was scored on {start_tnr:g} — keeping the "
              f"defaults, since a fit that took the model past the reference collected "
              f"the whole `tnr` term for going quiet rather than for matching",
              file=sys.stderr)
        return [k.start_value for k in knobs]
    return best_values


def report_pinned(knobs, best_values: list[float]) -> list[str]:
    """Name every knob whose result sits on the end of its range.

    A search reports the best point it was allowed to visit, and a range that
    does not contain the answer produces one indistinguishable from a range that
    does: the value pins to the bound and is written back as an optimum. It is
    the most expensive failure this tool has, because nothing about the output
    looks wrong — the loss went down, the diff is small, the report is clean.
    The treble decay constant was searched over [0.5, 3.0] when it wanted 5.0,
    and 3.0 is what such a run would have reported.

    A pinned knob is not automatically wrong. A bound the engine enforces is a
    real end of the space, and an optimum genuinely sitting there is a result.
    What it is never safe to do is read it as an interior optimum, so it is
    named and the run says which end and how to widen it.

    Every knob is checked, not only the ones that moved. A start value the spec
    had to clamp into range starts pinned and stays pinned, and that is the case
    worth catching most: it never appears in a start-to-best diff, because by
    that measure nothing happened.
    """
    pinned = []
    for knob, value in zip(knobs, best_values):
        end = at_bound(knob, value)
        if end is not None:
            limit = knob.lo if end == "minimum" else knob.hi
            pinned.append(f"{knob.label} = {value:g} at its {end} ({limit:g})")
    return pinned
