#!/usr/bin/env python3
"""Regression tests for the accuracy roll-up.

What these pin is the honesty of the aggregate, not the arithmetic of a mean.
Two failure modes matter more than the averages: a dimension with no
observations must report as unmeasured rather than as a score, and a per-dataset
breakdown must not silently merge corpora, because an accuracy figure with no
corpus attached cannot be reproduced or compared.

Stdlib only; no build needed. Run directly:

    python3 tools/eval/test_summarize_accuracy.py
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import summarize_accuracy as summarizer  # noqa: E402


def _report(*observations: dict) -> dict:
    return {
        "generated_at": "2026-01-01T00:00:00+00:00",
        "runs": [{"observations": list(observations)}],
    }


def _key(fixture: str, correct: bool, category: str, score: float) -> dict:
    return {
        "metric": "key",
        "fixture": fixture,
        "correct": correct,
        "mirex_category": category,
        "mirex_score": score,
    }


def test_empty_report_reports_every_dimension_unmeasured() -> None:
    summary = summarizer.summarize(_report())
    assert summary["measured_dimensions"] == []
    assert set(summary["unmeasured_dimensions"]) == {
        "key",
        "bpm",
        "beat",
        "downbeat",
        "meter",
        "chord",
    }
    for name, entry in summary["dimensions"].items():
        assert entry["measured"] is False, name
        assert entry["fixtures"] == 0, name
        # The absence of a score is the finding. An aggregate key here would be
        # a measurement of nothing presented as a measurement.
        assert "overall" not in entry, name


def test_unmeasured_dimension_renders_as_unmeasured_not_as_zero() -> None:
    table = summarizer.to_markdown(summarizer.summarize(_report()))
    assert "unmeasured" in table
    assert "0.0%" not in table


def test_key_accuracy_and_mirex_score_aggregate() -> None:
    summary = summarizer.summarize(
        _report(
            _key("giantsteps/a.wav", True, "exact", 1.0),
            _key("giantsteps/b.wav", False, "relative", 0.3),
            _key("giantsteps/c.wav", False, "fifth", 0.5),
            _key("giantsteps/d.wav", True, "exact", 1.0),
        )
    )
    entry = summary["dimensions"]["key"]
    assert entry["measured"] is True
    assert entry["fixtures"] == 4
    assert entry["overall"]["accuracy"] == 0.5
    assert abs(entry["overall"]["mirex_score"] - 0.7) < 1e-9
    assert entry["overall"]["mirex_categories"] == {
        "exact": 2,
        "relative": 1,
        "fifth": 1,
    }


def test_datasets_are_reported_separately() -> None:
    summary = summarizer.summarize(
        _report(
            _key("giantsteps/a.wav", True, "exact", 1.0),
            _key("giantsteps/b.wav", True, "exact", 1.0),
            _key("isophonics/c.wav", False, "other", 0.0),
        )
    )
    by_dataset = summary["dimensions"]["key"]["by_dataset"]
    assert set(by_dataset) == {"giantsteps", "isophonics"}
    assert by_dataset["giantsteps"]["accuracy"] == 1.0
    assert by_dataset["giantsteps"]["fixtures"] == 2
    assert by_dataset["isophonics"]["accuracy"] == 0.0
    # The overall figure is a mean over the union, which is why the breakdown
    # has to be published alongside it.
    assert abs(summary["dimensions"]["key"]["overall"]["accuracy"] - 2.0 / 3.0) < 1e-9


def test_bpm_accuracy_uses_the_relative_tolerance() -> None:
    summary = summarizer.summarize(
        _report(
            {
                "metric": "bpm",
                "fixture": "ballroom/a.wav",
                "relative_error_percent": 1.0,
            },
            {
                "metric": "bpm",
                "fixture": "ballroom/b.wav",
                "relative_error_percent": 3.9,
            },
            {
                "metric": "bpm",
                "fixture": "ballroom/c.wav",
                "relative_error_percent": 50.0,
            },
        )
    )
    overall = summary["dimensions"]["bpm"]["overall"]
    assert overall["accuracy"] == 2.0 / 3.0
    assert overall["accuracy_tolerance"] == summarizer.BPM_ACCURACY_TOLERANCE
    assert overall["median_relative_error"] == 0.039


def test_beat_reports_mean_f_measure_and_the_share_above_threshold() -> None:
    summary = summarizer.summarize(
        _report(
            {
                "metric": "beat",
                "fixture": "smc/a.wav",
                "f_measure": 0.9,
                "threshold": 0.0,
            },
            {
                "metric": "beat",
                "fixture": "smc/b.wav",
                "f_measure": 0.3,
                "threshold": 0.0,
            },
        )
    )
    overall = summary["dimensions"]["beat"]["overall"]
    assert abs(overall["mean_f_measure"] - 0.6) < 1e-9
    assert overall["tracks_above_threshold"] == 0.5


def test_chord_keeps_the_vocabulary_gap_visible() -> None:
    # The distance between the maj/min WCSR and the exact one is what the
    # extended vocabulary is worth, so the two must not be collapsed.
    summary = summarizer.summarize(
        _report(
            {"metric": "chord_wcsr", "fixture": "jaah/a.wav", "wcsr": 0.8},
            {
                "metric": "chord_detail",
                "fixture": "jaah/a.wav",
                "root_accuracy": 0.85,
                "quality_accuracy": 0.6,
                "exact_wcsr": 0.55,
            },
            {"metric": "chord_bass_acc", "fixture": "jaah/a.wav", "accuracy": 0.7},
        )
    )
    overall = summary["dimensions"]["chord"]["overall"]
    assert overall["wcsr_maj_min"] == 0.8
    assert overall["wcsr_exact"] == 0.55
    assert overall["root_accuracy"] == 0.85
    assert overall["bass_accuracy"] == 0.7
    assert summary["dimensions"]["chord"]["fixtures"] == 1


def test_chord_bass_stays_absent_when_no_row_measured_it() -> None:
    summary = summarizer.summarize(
        _report({"metric": "chord_wcsr", "fixture": "isophonics/a.wav", "wcsr": 0.7})
    )
    overall = summary["dimensions"]["chord"]["overall"]
    assert overall["wcsr_maj_min"] == 0.7
    # No bass-labelled reference was present. Reporting 0% would read as a
    # measured failure of inversion detection rather than as its absence.
    assert overall["bass_accuracy"] is None


def test_markdown_carries_the_fixture_count_with_every_figure() -> None:
    table = summarizer.to_markdown(
        summarizer.summarize(
            _report(
                _key("giantsteps/a.wav", True, "exact", 1.0),
                _key("giantsteps/b.wav", False, "other", 0.0),
            )
        )
    )
    rows = [line for line in table.splitlines() if line.startswith("| key |")]
    assert len(rows) == 2  # overall plus one dataset
    for row in rows:
        assert "| 2 |" in row


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"ok   {test.__name__}")
        except AssertionError as error:  # noqa: PERF203
            failed += 1
            print(f"FAIL {test.__name__}: {error}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
