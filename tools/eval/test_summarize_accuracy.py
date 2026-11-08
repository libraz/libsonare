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

import summarize_accuracy as summarizer


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


def _chord_v2(fixture: str, metric: str, correct: float, scored: float) -> dict:
    return {
        "metric": metric,
        "fixture": fixture,
        "metric_version": "chord_overlap_v2",
        "correct_seconds": correct,
        "scored_seconds": scored,
        "annotated_seconds": scored,
        "ignored_seconds": 0.0,
        "missing_prediction_seconds": 0.0,
        "score": correct / scored if scored else None,
        "measured": scored > 0,
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
        "structure",
        "notes",
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


def test_chord_v2_reports_track_macro_and_duration_pooled_scores() -> None:
    report = _report(
        _chord_v2("public/a.wav", "chord_wcsr", 1.0, 10.0),
        _chord_v2("public/b.wav", "chord_wcsr", 90.0, 90.0),
    )
    summary = summarizer.summarize(report)
    entry = summary["dimensions"]["chord"]
    overall = entry["overall"]
    assert entry["measured"] is True
    assert entry["fixtures"] == 2
    assert overall["metric_version"] == "chord_overlap_v2"
    assert overall["wcsr_maj_min"] == overall["wcsr_maj_min_track_macro"] == 0.55
    assert overall["wcsr_maj_min_pooled"] == 0.91
    assert overall["wcsr_maj_min_correct_seconds"] == 91.0
    assert overall["wcsr_maj_min_scored_seconds"] == 100.0
    assert overall["root_scope"] == "known_chords"
    assert overall["coverage"]["attempted_fixtures"] == 2
    markdown = summarizer.to_markdown(summary)
    chord_all_row = next(
        line for line in markdown.splitlines() if line.startswith("| chord | all |")
    )
    assert "| chord | all | 2 | 91.0% |" in chord_all_row
    assert "wcsr_maj_min_track_macro 55.0%" in chord_all_row
    assert "wcsr_maj_min_correct_seconds 91.000s" in markdown
    assert "wcsr_maj_min_scored_seconds 100.000s" in markdown
    assert "wcsr_maj_min_scored_seconds 10000.0%" not in markdown


def test_chord_all_excluded_is_unmeasured_but_keeps_attempt_coverage() -> None:
    observation = _chord_v2("public/all-x.wav", "chord_wcsr", 0.0, 0.0)
    observation.update({"annotated_seconds": 12.0, "ignored_seconds": 12.0})
    summary = summarizer.summarize(_report(observation))
    chord = summary["dimensions"]["chord"]
    assert chord["measured"] is False
    assert chord["fixtures"] == 1
    assert chord["coverage"]["attempted_fixtures"] == 1
    assert "overall" not in chord
    markdown = summarizer.to_markdown(summary)
    assert "| chord | — | 1 | unmeasured | no scored chord intervals" in markdown
    assert "attempted 1, scored 0.000s, annotated 12.000s, ignored 12.000s" in markdown
    assert "no fixtures configured" not in next(
        line for line in markdown.splitlines() if line.startswith("| chord |")
    )


def test_chord_mixed_metric_versions_are_rejected() -> None:
    with_error = False
    try:
        summarizer.summarize(
            _report(
                _chord_v2("public/new.wav", "chord_wcsr", 1.0, 1.0),
                {
                    "metric": "chord_wcsr",
                    "fixture": "public/old.wav",
                    "metric_version": "legacy_midpoint_v1",
                    "wcsr": 0.4,
                },
            )
        )
    except ValueError as error:
        with_error = "mixed chord metric versions" in str(error)
    assert with_error


def test_v2_rejects_inconsistent_score_and_duration_fields() -> None:
    observation = _chord_v2("public/a.wav", "chord_wcsr", 1.0, 2.0)
    observation["score"] = 0.9
    try:
        summarizer.summarize(_report(observation))
    except ValueError as error:
        assert "score does not match its seconds" in str(error)
    else:
        raise AssertionError("inconsistent chord measurement was accepted")


def test_legacy_chord_report_is_labeled_and_keeps_old_fields() -> None:
    overall = summarizer.summarize(
        _report({"metric": "chord_wcsr", "fixture": "jaah/a.wav", "wcsr": 0.8})
    )["dimensions"]["chord"]["overall"]
    assert overall["metric_version"] == "legacy_midpoint_v1"
    assert overall["wcsr_maj_min"] == 0.8
    assert overall["wcsr_maj_min_track_macro"] == 0.8
    assert "wcsr_maj_min_pooled" not in overall


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


def _structure(fixture: str, features: str, hr05f: float, hr3f: float, pairwise_f: float) -> dict:
    return {
        "metric": "structure",
        "fixture": fixture,
        "features": features,
        "hr05f": hr05f,
        "hr3f": hr3f,
        "pairwise_f": pairwise_f,
        "min_hr3f": 0.0,
        "min_pairwise_f": 0.0,
    }


def test_structure_means_each_feature_set_separately() -> None:
    # Track a is perfect on `both` (1.0 / 1.0 / 1.0); track b finds half the
    # boundaries at 0.5 s and merges the two groups (0.5 / 1.0 / 0.5). Means: 0.75 / 1.0 / 0.75.
    summary = summarizer.summarize(
        _report(
            _structure("harmonix/a.m4a", "both", 1.0, 1.0, 1.0),
            _structure("harmonix/b.m4a", "both", 0.5, 1.0, 0.5),
            _structure("harmonix/a.m4a", "timbre", 0.0, 0.5, 0.25),
        )
    )
    entry = summary["dimensions"]["structure"]
    assert entry["measured"] is True
    assert entry["fixtures"] == 2
    overall = entry["overall"]
    assert abs(overall["mean_hr05f"] - 0.75) < 1e-9
    assert abs(overall["mean_hr3f"] - 1.0) < 1e-9
    assert abs(overall["mean_pairwise_f"] - 0.75) < 1e-9
    # A feature set must not be averaged into `both`: the timbre-only reading
    # sits beside it under its own keys.
    assert overall["timbre_mean_hr3f"] == 0.5
    assert overall["timbre_mean_pairwise_f"] == 0.25
    assert entry["by_dataset"]["harmonix"]["fixtures"] == 2


def _notes_row(
    fixture: str, f_note: float, f_note_offset: float, f_onset: float, truth: int, predicted: int
) -> dict:
    return {
        "metric": "notes",
        "fixture": fixture,
        "f_note": f_note,
        "f_note_offset": f_note_offset,
        "f_onset": f_onset,
        "truth_count": truth,
        "predicted_count": predicted,
        "min_f_note": 0.0,
        "min_f_note_offset": 0.0,
    }


def test_notes_means_and_count_ratio() -> None:
    # Track a matches every note. Track b is over-split: twice as many predicted
    # notes as annotated, so the ratio over both tracks is 30 / 20 = 1.5.
    summary = summarizer.summarize(
        _report(
            _notes_row("mir1k/a.wav", 1.0, 0.5, 1.0, 10, 10),
            _notes_row("mir1k/b.wav", 0.5, 0.25, 0.5, 10, 20),
        )
    )
    entry = summary["dimensions"]["notes"]
    assert entry["measured"] is True
    assert entry["fixtures"] == 2
    overall = entry["overall"]
    assert abs(overall["mean_f_note"] - 0.75) < 1e-9
    assert abs(overall["mean_f_note_offset"] - 0.375) < 1e-9
    assert abs(overall["mean_f_onset"] - 0.75) < 1e-9
    assert abs(overall["note_count_ratio"] - 1.5) < 1e-9
    assert entry["by_dataset"]["mir1k"]["fixtures"] == 2


def test_notes_count_ratio_is_absent_without_annotated_notes() -> None:
    summary = summarizer.summarize(_report(_notes_row("mir1k/a.wav", 0.0, 0.0, 0.0, 0, 3)))
    # Predicted notes against an empty annotation have no ratio to report.
    assert summary["dimensions"]["notes"]["overall"]["note_count_ratio"] is None


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"ok   {test.__name__}")
        except AssertionError as error:
            failed += 1
            print(f"FAIL {test.__name__}: {error}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
