"""What `diagnose` concludes from a set of probe renders, and what it refuses to.

The verdicts are the point: every one of them sends the reader somewhere
different, and the two that matter most — a term nothing reaches, and a term
everything reaches but nothing improves — look identical if only the improvement
is measured. So each verdict has a case, and so does the confusion between them.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from diagnose import (  # noqa: E402
    CONNECTED_UNITS,
    TERM_MEANS,
    TERM_UNITS,
    Diagnosis,
    diagnose,
    print_report,
    probe_axes,
)
from loss import LOSS_TERMS, TERM_FLOORS, measured_terms  # noqa: E402

PITCHED_WEIGHTS = {"harm": 1.0, "cents": 1.0, "slope": 1.0, "env": 1.0}


def terms(**over: float) -> dict[str, float]:
    """A full term dict, so a test states only the term it is about."""
    return {name: 0.0 for name in LOSS_TERMS} | over


def probes(*rows: tuple[str, str, dict | None, str]) -> list:
    return list(rows)


def verdict_of(diag: Diagnosis, term: str) -> str:
    return next(t.verdict for t in diag.terms if t.term == term)


def row(diag: Diagnosis, term: str):
    return next(t for t in diag.terms if t.term == term)


# --------------------------------------------------------------------------
# the verdicts


def test_a_term_no_knob_moves_is_unreachable():
    base = terms(harm=8.0)
    diag = diagnose(base, probes(
        ("a.x", "lo", terms(harm=8.0), "clamp"),
        ("a.x", "hi", terms(harm=8.0), "clamp"),
        ("a.y", "lo", terms(harm=7.999), "clamp"),
        ("a.y", "hi", terms(harm=8.001), "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "harm") == "unreachable"
    assert [t.term for t in diag.structural()] == ["harm"]


def test_unreachable_over_a_clamp_bound_does_not_ask_for_a_wider_range():
    """A null over the engine's own interval is the strongest claim available.

    Telling the reader to widen a range that is already everything the engine
    accepts sends them to look for headroom that cannot exist, which is the one
    way this verdict wastes someone's afternoon.
    """
    base = terms(harm=8.0)
    diag = diagnose(base, probes(
        ("a.x", "lo", terms(harm=8.0), "clamp"),
        ("a.x", "hi", terms(harm=8.0), "clamp"),
    ), PITCHED_WEIGHTS)
    note = row(diag, "harm").note
    assert "widen" not in note
    assert "interval the engine accepts" in note


def test_unreachable_over_a_heuristic_range_asks_for_a_wider_one_first():
    base = terms(harm=8.0)
    diag = diagnose(base, probes(
        ("a.x", "lo", terms(harm=8.0), "auto"),
        ("a.x", "hi", terms(harm=8.0), "auto"),
    ), PITCHED_WEIGHTS)
    assert "widen" in row(diag, "harm").note


def test_a_converged_term_is_spent_not_unreachable():
    """The trap this tool exists to avoid.

    Once a fit has written back, every knob sits at its own optimum, so no
    single move improves anything. Read on improvement alone, a perfectly
    well-modelled voice reports every measurement as structurally missing.
    """
    base = terms(harm=8.0)
    diag = diagnose(base, probes(
        ("a.x", "lo", terms(harm=12.0), "clamp"),
        ("a.x", "hi", terms(harm=11.0), "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "harm") == "spent"
    assert not diag.structural()


def test_a_knob_that_closes_most_of_the_gap_is_reachable():
    base = terms(harm=8.0)
    diag = diagnose(base, probes(
        ("a.x", "lo", terms(harm=1.5), "clamp"),
        ("a.x", "hi", terms(harm=9.0), "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "harm") == "reachable"
    best = row(diag, "harm").best
    assert best.knob == "a.x" and best.at == "lo"


def test_a_knob_that_closes_a_little_of_the_gap_is_partial():
    base = terms(harm=8.0)
    diag = diagnose(base, probes(
        ("a.x", "lo", terms(harm=6.5), "clamp"),
        ("a.x", "hi", terms(harm=8.4), "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "harm") == "partial"


def test_a_term_inside_its_own_floor_is_matched():
    diag = diagnose(terms(cents=0.4), probes(
        ("a.x", "lo", terms(cents=0.4), "clamp"),
        ("a.x", "hi", terms(cents=0.4), "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "cents") == "matched"


def test_an_unweighted_term_is_reported_as_never_looked_at():
    """A fit is blind to a term it does not weight, so its residual is news.

    Calling it structural would be wrong twice over: nothing has tried to close
    it, and the knobs that would are right there.
    """
    base = terms(tnr=9.0)
    diag = diagnose(base, probes(
        ("a.x", "lo", terms(tnr=2.0), "clamp"),
        ("a.x", "hi", terms(tnr=9.0), "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "tnr") == "unscored"
    assert "no weight" in row(diag, "tnr").note


# --------------------------------------------------------------------------
# what belongs to which metric set


def test_a_percussion_probe_does_not_report_the_harmonic_terms():
    """Both reducers fill every LOSS_TERM, so an absent term reads as a zero.

    Reporting that zero as `matched` would tell a drum fit its harmonic ladder
    is perfect, which is not a thing a drum has.
    """
    diag = diagnose(terms(band=6.0), probes(
        ("a.x", "lo", terms(band=6.0), "clamp"),
        ("a.x", "hi", terms(band=6.0), "clamp"),
    ), {"band": 1.0, "bdecay": 1.0}, percussive=True)
    reported = {t.term for t in diag.terms}
    assert "harm" not in reported and "cents" not in reported
    assert reported == set(measured_terms(percussive=True))


def test_the_multiscale_term_is_absent_rather_than_matched_when_unweighted():
    """It is the one term not computed unless weighted, so its zero is a gap."""
    diag = diagnose(terms(), probes(
        ("a.x", "lo", terms(), "clamp"),
        ("a.x", "hi", terms(), "clamp"),
    ), {"harm": 1.0})
    assert verdict_of(diag, "mss") == "not computed"


# --------------------------------------------------------------------------
# connectivity


def test_connectivity_counts_movement_in_either_direction():
    """A knob that only makes a term worse still proves the mechanism is wired."""
    floor = TERM_FLOORS["harm"]
    base = terms(harm=8.0)
    diag = diagnose(base, probes(
        ("worse.only", "lo", terms(harm=8.0 + 5 * floor), "clamp"),
        ("worse.only", "hi", terms(harm=8.0 + 6 * floor), "clamp"),
    ), PITCHED_WEIGHTS)
    assert row(diag, "harm").movers == 1
    assert verdict_of(diag, "harm") == "spent"


def test_movement_under_the_connectivity_threshold_does_not_count():
    floor = TERM_FLOORS["harm"]
    base = terms(harm=8.0)
    diag = diagnose(base, probes(
        ("a.x", "lo", terms(harm=8.0 - 0.5 * CONNECTED_UNITS * floor), "clamp"),
        ("a.x", "hi", terms(harm=8.0 + 0.5 * CONNECTED_UNITS * floor), "clamp"),
    ), PITCHED_WEIGHTS)
    assert row(diag, "harm").movers == 0
    assert verdict_of(diag, "harm") == "unreachable"


def test_a_knob_moving_no_measurement_is_named():
    diag = diagnose(terms(harm=8.0), probes(
        ("live.knob", "lo", terms(harm=2.0), "clamp"),
        ("live.knob", "hi", terms(harm=8.0), "clamp"),
        ("dead.knob", "lo", terms(harm=8.0), "clamp"),
        ("dead.knob", "hi", terms(harm=8.0), "clamp"),
    ), PITCHED_WEIGHTS)
    assert diag.inert_knobs == ["dead.knob"]


# --------------------------------------------------------------------------
# robustness


def test_a_probe_that_did_not_render_is_skipped_rather_than_scored_as_zero():
    """An unscorable render comes back as None; counting it as 0.0 would read
    as a knob that closes the whole gap."""
    diag = diagnose(terms(harm=8.0), probes(
        ("a.x", "lo", None, "clamp"),
        ("a.x", "hi", terms(harm=8.0), "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "harm") == "unreachable"
    assert row(diag, "harm").best.gain == 0.0


def test_a_silenced_render_is_not_scored_as_a_perfect_match():
    """The failure this hits on a real run, every run.

    The probe deliberately visits both extremes of every range, and one extreme
    of any gain is silence. `score_terms` reports a render with no analysable
    note as every term at zero — which is the best score there is — so read
    literally the knob that silences the voice is the knob that fixed
    everything, and it wins on every term at once.
    """
    silent = terms() | {"comparable": 0.0}
    diag = diagnose(terms(harm=8.0) | {"comparable": 1.0}, probes(
        ("a.gain", "lo", silent, "clamp"),
        ("a.gain", "hi", terms(harm=8.2) | {"comparable": 1.0}, "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "harm") != "reachable"
    assert row(diag, "harm").best.gain == 0.0
    assert diag.unscorable == ["a.gain:lo"]


def test_an_unflagged_term_dict_is_taken_at_face_value():
    """Terms measured before the flag existed, and every hand-built case here."""
    diag = diagnose(terms(harm=8.0), probes(
        ("a.x", "lo", terms(harm=1.0), "clamp"),
        ("a.x", "hi", terms(harm=8.0), "clamp"),
    ), PITCHED_WEIGHTS)
    assert verdict_of(diag, "harm") == "reachable"
    assert diag.unscorable == []


def test_the_probe_axes_are_counted_off_the_rows_that_were_scored():
    """Not off the flags: the note list is usually left to the pattern to pick,
    so the flags are empty on exactly the runs where a fixed axis misleads."""
    sustain = [{"note": n, "velocity": 100} for n in (48, 60, 72)]
    assert probe_axes(sustain, "sustain") == "the sustain pattern over 3 notes and one velocity"
    velocity = [{"note": 60, "velocity": v} for v in (40, 70, 100, 127)]
    assert probe_axes(velocity, "velocity") == "the velocity pattern over one note and 4 velocities"


def test_a_fixed_axis_is_named_next_to_the_inert_knobs(capsys):
    """A dynamics control cannot move a single-velocity probe, and reads exactly
    like a knob the voice never touches."""
    diag = diagnose(terms(harm=8.0), probes(
        ("a.velocity_range_db", "lo", terms(harm=8.0), "clamp"),
        ("a.velocity_range_db", "hi", terms(harm=8.0), "clamp"),
    ), PITCHED_WEIGHTS, axes="the sustain pattern over 3 notes and one velocity")
    print_report(diag)
    out = capsys.readouterr().out
    assert "one velocity" in out
    assert "inert" in out.lower()


def test_no_probes_at_all_still_produces_a_report():
    diag = diagnose(terms(harm=8.0), [], PITCHED_WEIGHTS)
    assert verdict_of(diag, "harm") == "unreachable"
    assert row(diag, "harm").probed == 0
    assert diag.inert_knobs == []


def test_a_non_finite_residual_is_dropped_rather_than_ranked():
    diag = diagnose(terms(harm=float("inf")), probes(
        ("a.x", "lo", terms(harm=1.0), "clamp"),
        ("a.x", "hi", terms(harm=1.0), "clamp"),
    ), PITCHED_WEIGHTS)
    assert "harm" not in {t.term for t in diag.terms}


def test_the_json_record_round_trips():
    diag = diagnose(terms(harm=8.0), probes(
        ("a.x", "lo", terms(harm=2.0), "clamp"),
        ("a.x", "hi", terms(harm=8.0), "clamp"),
    ), PITCHED_WEIGHTS)
    record = diag.to_dict()
    harm = next(t for t in record["terms"] if t["term"] == "harm")
    assert harm["best"]["knob"] == "a.x"
    assert harm["strongest"]["source"] == "clamp"
    assert record["inert_knobs"] == []


@pytest.mark.parametrize("percussive", [False, True])
def test_the_report_prints_for_either_metric_set(capsys, percussive):
    diag = diagnose(terms(harm=8.0, band=6.0), probes(
        ("a.x", "lo", terms(harm=8.0, band=6.0), "clamp"),
        ("a.x", "hi", terms(harm=8.0, band=6.0), "clamp"),
    ), PITCHED_WEIGHTS | {"band": 1.0}, percussive=percussive)
    print_report(diag)
    out = capsys.readouterr().out
    assert "what the residual is made of" in out
    assert "hypothesis to test" in out


def test_the_report_says_so_when_nothing_is_structural(capsys):
    diag = diagnose(terms(harm=8.0), probes(
        ("a.x", "lo", terms(harm=1.0), "clamp"),
        ("a.x", "hi", terms(harm=8.0), "clamp"),
    ), PITCHED_WEIGHTS)
    print_report(diag)
    out = capsys.readouterr().out
    assert "Every measurement is reachable" in out


def test_the_verdict_can_be_written_to_a_file(tmp_path, capsys):
    diag = diagnose(terms(harm=8.0), probes(
        ("a.x", "lo", terms(harm=8.0), "clamp"),
        ("a.x", "hi", terms(harm=8.0), "clamp"),
    ), PITCHED_WEIGHTS)
    out = tmp_path / "diag.json"
    print_report(diag, out_path=str(out))
    capsys.readouterr()
    assert out.exists() and '"unreachable"' in out.read_text()


def test_every_loss_term_has_a_unit_and_a_meaning_the_report_can_print():
    """Two hand-kept tables against one list, which is a mirror and drifts.

    A term missing from either prints an empty unit or no explanation at all,
    and a diagnosis whose whole job is to tell a reader where to go next says
    nothing about that one. Nothing else notices: the report renders, the run
    exits zero, and the gap is a blank column.
    """
    missing_units = [t for t in LOSS_TERMS if t not in TERM_UNITS]
    missing_means = [t for t in LOSS_TERMS if t not in TERM_MEANS]
    assert not missing_units, f"no unit for {missing_units}"
    assert not missing_means, f"no explanation for {missing_means}"
