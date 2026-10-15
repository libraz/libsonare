"""The two verdicts a finished fit gets: whether to keep its winner, and where it pinned."""

from __future__ import annotations

import sys

from fit_quality import dropout_terms, significant_term_regressions
from knobs import at_bound
from profile_gate import FLOOR_GUESSES


def winner_or_defaults(
    knobs, best_values: list[float], evaluator, validation: dict | None = None
) -> list[float]:
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
    unit for doing it. How many notes it still charges cannot separate that from
    a match — both read zero — so the guard reads the two things directly: the
    median distance the model sits past the reference (`tnr_past_db`), refused
    when it grew by more than the gate's own `tnr` floor, and the notes the
    model fell silent on (`tnr_absent`), refused when there are more of them.
    The level of either at the start is not a finding: a physical model cleaner
    than a sampled recording is ordinary, and only the fit's own growth is.
    """
    # Which refusal fired, for the --out record. A refusal leaves the tree
    # showing a voice whose values did not change, and that reads the same as a
    # search that simply found nothing — so the reason has to outlive the
    # terminal or the finding does not survive the run.
    evaluator.write_back_refusal = None
    evaluator.selected_dropout = []
    evaluator.selected_regressions = []
    evaluator.search_dropout = []
    evaluator.search_regressions = []

    # Cache-only lookups (the tree is restored); the winner's loss never pairs with the defaults.
    start_values = [k.start_value for k in knobs]
    search_terms = _terms_for(evaluator, best_values)
    start_terms = _terms_for(evaluator, start_values)
    if search_terms is not None and start_terms is not None:
        evaluator.search_regressions = significant_term_regressions(
            start_terms, search_terms, getattr(evaluator, "loss", None)
        )
        dropped = dropout_terms(start_terms, search_terms, getattr(evaluator, "loss", None))
        evaluator.search_dropout = dropped
        if dropped:
            evaluator.selected_dropout = dropped
            evaluator.write_back_refusal = "measurement_dropout"
            print(
                "\nthe winner lost all measurements for active term(s) "
                f"{', '.join(dropped)} — keeping the defaults, since the selected "
                "candidate would be judged on less evidence",
                file=sys.stderr,
            )
            _select(evaluator, start_values)
            return start_values
    if getattr(evaluator, "normalize", False) and evaluator.best_loss > 1.0:
        evaluator.write_back_refusal = "lost_to_start"
        print(
            f"\nthe winner scores {evaluator.best_loss:.4f} against the defaults' 1.0 — "
            f"keeping the defaults, since a fit that lost to its own start point has "
            f"nothing to write",
            file=sys.stderr,
        )
        _select(evaluator, start_values)
        return start_values
    if (
        validation
        and "best" in validation
        and "start" in validation
        and validation["best"] - validation["start"] > 0.005
    ):
        evaluator.write_back_refusal = "fitted_to_the_probe"
        print(
            f"\nthe winner scores {validation['best']:.4f} against the defaults' "
            f"{validation['start']:.4f} on the held-out {validation['axis']} — keeping "
            f"the defaults, since values that lose where the fit could not see are "
            f"fitted to the probe",
            file=sys.stderr,
        )
        _select(evaluator, start_values)
        return start_values
    reading = _went_blind(
        evaluator,
        getattr(evaluator, "best_tnr_absent", None),
        getattr(evaluator, "best_tnr_past_db", None),
    )
    if reading is not None:
        evaluator.write_back_refusal = "objective_went_blind"
        print(
            f"\nthe winner {reading} — keeping the defaults, since the one-sided `tnr` "
            f"term charges nothing for either and the fit collected it for leaving the "
            f"reference rather than for matching it",
            file=sys.stderr,
        )
        _select(evaluator, start_values)
        return start_values
    _select(evaluator, best_values)
    return best_values


def _went_blind(evaluator, absent: float | None, past: float | None) -> str | None:
    """How a candidate's `tnr` readings outgrew the start point's, or None if they did not."""

    start_absent = getattr(evaluator, "start_tnr_absent", None)
    start_past = getattr(evaluator, "start_tnr_past_db", None)
    if start_absent is not None and absent is not None and absent > start_absent:
        return f"falls silent on {absent:g} notes where the start point fell silent on {start_absent:g}"
    if start_past is not None and past is not None and past - start_past > FLOOR_GUESSES["tnr"]:
        return (
            f"sits a median {past:.2f} dB cleaner than the reference where the start point "
            f"sat {start_past:.2f}"
        )
    return None


def short_of_blind(best_values: list[float], evaluator) -> list[float]:
    """The furthest best-so-far point of the search the `tnr` guard admits.

    The guard in `winner_or_defaults` refuses a winner that bought its loss by
    walking past the reference, which used to discard the whole fit — and with
    it every improvement the search made before it started walking. The path
    holds each best-so-far point and the cache holds its terms, so the latest
    point that stayed on the reference's side is found without a render. It
    becomes the search winner, so the hold-out and the report score what is
    written; the unconstrained winner is kept in `walked_back`.
    """
    evaluator.walked_back = None
    path = getattr(evaluator, "path", None) or []
    reading = _went_blind(
        evaluator,
        getattr(evaluator, "best_tnr_absent", None),
        getattr(evaluator, "best_tnr_past_db", None),
    )
    if reading is None or not path:
        return best_values
    for eval_no, loss, _stage, values in reversed(path):
        if loss >= 1.0:
            break
        terms = _terms_for(evaluator, values)
        if terms is None or _went_blind(
            evaluator, terms.get("tnr_absent"), terms.get("tnr_past_db")
        ):
            continue
        evaluator.walked_back = {
            "reading": reading,
            "from_eval": path[-1][0],
            "from_loss": path[-1][1],
            "to_eval": eval_no,
            "to_loss": loss,
        }
        print(
            f"\nthe winner {reading} — walking back to eval #{eval_no} (loss {loss:.4f}), "
            f"the furthest point of the search that stayed on the reference's side",
            file=sys.stderr,
        )
        evaluator.best_values = list(values)
        evaluator.best_loss = loss
        evaluator.best_terms = dict(terms)
        evaluator.best_level_offset_db = terms.get("level_offset_db")
        evaluator.best_tnr_notes = terms.get("tnr_notes")
        evaluator.best_tnr_absent = terms.get("tnr_absent")
        evaluator.best_tnr_past_db = terms.get("tnr_past_db")
        return list(values)
    return best_values


def _terms_for(evaluator, values: list[float]) -> dict | None:
    """Read a candidate's cached raw terms without rendering a new candidate."""

    method = getattr(evaluator, "terms_for", None)
    return method(values) if callable(method) else None


def _select(evaluator, values: list[float]) -> None:
    """Cache the selected vector, raw terms and loss as one consistent record."""

    evaluator.selected_values = list(values)
    evaluator.selected_terms = _terms_for(evaluator, values)
    loss_for = getattr(evaluator, "loss_for", None)
    evaluator.selected_loss = loss_for(values) if callable(loss_for) else None
    baseline_terms = getattr(evaluator, "baseline_terms", None)
    if baseline_terms is not None and evaluator.selected_terms is not None:
        evaluator.selected_regressions = significant_term_regressions(
            baseline_terms,
            evaluator.selected_terms,
            getattr(evaluator, "loss", None),
        )
        evaluator.selected_dropout = dropout_terms(
            baseline_terms,
            evaluator.selected_terms,
            getattr(evaluator, "loss", None),
        )


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
