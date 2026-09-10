"""The two verdicts a finished fit gets: whether to keep its winner, and where it pinned."""

from __future__ import annotations

import sys

from knobs import at_bound


def winner_or_defaults(knobs, best_values: list[float], evaluator,
                       validation: dict | None = None) -> list[float]:
    """The fit's winner, unless something measured says it is worse than the start.

    Two readings can say that, and both are refusals to write rather than
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
    """
    if getattr(evaluator, "normalize", False) and evaluator.best_loss > 1.0:
        print(f"\nthe winner scores {evaluator.best_loss:.4f} against the defaults' 1.0 — "
              f"keeping the defaults, since a fit that lost to its own start point has "
              f"nothing to write", file=sys.stderr)
        return [k.start_value for k in knobs]
    if validation and validation["best"] - validation["start"] > 0.005:
        print(f"\nthe winner scores {validation['best']:.4f} against the defaults' "
              f"{validation['start']:.4f} on the held-out {validation['axis']} — keeping "
              f"the defaults, since values that lose where the fit could not see are "
              f"fitted to the probe", file=sys.stderr)
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
