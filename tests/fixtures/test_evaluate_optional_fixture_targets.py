"""Tests for per-dataset targets, the derived beat offset and baseline drops.

Stdlib only; no build needed. Run directly:

    python3 tests/fixtures/test_evaluate_optional_fixture_targets.py
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import evaluate_optional_fixture_targets as evaluator


def _beat(dataset: str, name: str, f: float, offset: float | None = None) -> dict[str, Any]:
    item: dict[str, Any] = {
        "metric": "beat",
        "fixture": f"{dataset}/{dataset}/{name}.m4a",
        "f_measure": f,
        "threshold": 0.0,
    }
    if offset is not None:
        item["median_offset_ms"] = offset
    return item


def _structure(dataset: str, name: str, hr3f: float, features: str = "both") -> dict[str, Any]:
    return {
        "metric": "structure",
        "fixture": f"{dataset}/{dataset}/{name}.m4a",
        "features": features,
        "hr05f": 0.0,
        "hr3f": hr3f,
        "pairwise_f": hr3f,
        "min_hr3f": 0.0,
        "min_pairwise_f": 0.0,
    }


def _report(*observations: dict[str, Any]) -> dict[str, Any]:
    return {"runs": [{"suite": "music", "observations": list(observations)}]}


def _summary(result: dict[str, Any], key: str) -> dict[str, Any]:
    return next(item for item in result["summaries"] if item["metric"] == key)


def test_dataset_floor_pass_and_fail() -> None:
    report = _report(_beat("a", "1", 0.9), _beat("a", "2", 0.8), _beat("b", "1", 0.5))
    targets = {"beat@a": {"mean_min": 0.8, "min_count": 2}, "beat@b": {"mean_min": 0.6}}
    result = evaluator.evaluate(report, targets, False)
    assert _summary(result, "beat@a")["ok"] and _summary(result, "beat@a")["count"] == 2
    assert not _summary(result, "beat@b")["ok"] and _summary(result, "beat@b")["count"] == 1
    assert not result["ok"]


def test_beat_offset_is_median_over_tracks_above_half_f() -> None:
    report = _report(
        _beat("a", "1", 0.9, 10.0),
        _beat("a", "2", 0.9, 14.0),
        _beat("a", "3", 0.9, -30.0),
        _beat("a", "4", 0.4, 500.0),
    )
    summary = _summary(evaluator.evaluate(report, {"beat_offset@a": {"abs_max": 20.0}}, False), "beat_offset@a")
    assert summary["count"] == 3 and summary["median"] == 10.0 and summary["ok"], summary
    summary = _summary(evaluator.evaluate(report, {"beat_offset@a": {"abs_max": 5.0}}, False), "beat_offset@a")
    assert not summary["ok"], summary


def test_missing_beat_offset_fails_under_require_metrics() -> None:
    report = _report(_beat("a", "1", 0.9), _beat("a", "2", 0.9))
    targets = {"beat_offset@a": {"abs_max": 20.0}}
    assert evaluator.evaluate(report, targets, False)["summaries"] == []
    assert evaluator.evaluate(report, targets, False)["ok"]
    result = evaluator.evaluate(report, targets, True)
    assert not result["ok"] and _summary(result, "beat_offset@a")["count"] == 0


def test_max_drop_against_baseline() -> None:
    baseline = _report(_structure("a", "1", 0.50), _structure("a", "2", 0.60), _structure("b", "1", 0.9))
    targets = {"structure_hr3f@a": {"max_drop": 0.01}}
    within = _report(_structure("a", "1", 0.545), _structure("a", "2", 0.555))
    result = evaluator.evaluate(within, targets, False, baseline)
    assert _summary(result, "structure_hr3f@a")["baseline_mean"] == 0.55
    assert result["ok"]
    dropped = _report(_structure("a", "1", 0.53), _structure("a", "2", 0.53))
    assert not evaluator.evaluate(dropped, targets, False, baseline)["ok"]
    assert not evaluator.evaluate(within, targets, False)["ok"]


def test_structure_uses_only_the_both_feature_set() -> None:
    report = _report(_structure("a", "1", 0.4), _structure("a", "1", 0.0, "timbre"))
    result = evaluator.evaluate(report, {"structure_hr3f@a": {"mean_min": 0.4}}, False)
    assert _summary(result, "structure_hr3f@a")["count"] == 1 and result["ok"]


def test_bare_metric_keys_still_group_all_datasets() -> None:
    report = _report(_beat("a", "1", 0.9), _beat("b", "1", 0.7))
    result = evaluator.evaluate(report, {"beat": {"mean_min": 0.8}}, False)
    assert _summary(result, "beat")["count"] == 2 and result["ok"]


def _notes(
    f_note: float, f_note_offset: float, min_f_note: float, min_f_note_offset: float
) -> dict[str, Any]:
    return {
        "metric": "notes",
        "fixture": "mir1k/mir1k/Ada_01.wav",
        "f_note": f_note,
        "f_note_offset": f_note_offset,
        "f_onset": 0.9,
        "truth_count": 30,
        "predicted_count": 30,
        "min_f_note": min_f_note,
        "min_f_note_offset": min_f_note_offset,
    }


def test_notes_value_is_note_f_measure() -> None:
    assert evaluator.metric_value(_notes(0.812, 0.5, 0.7, 0.4)) == 0.812


def test_notes_pass_requires_both_floors() -> None:
    assert evaluator.metric_pass(_notes(0.8, 0.5, 0.7, 0.4)) is True
    assert evaluator.metric_pass(_notes(0.6, 0.5, 0.7, 0.4)) is False
    # A note F above its floor does not excuse an offset F below its floor.
    assert evaluator.metric_pass(_notes(0.8, 0.3, 0.7, 0.4)) is False


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"ok   {test.__name__}")
        except (AssertionError, KeyError, StopIteration) as error:
            failed += 1
            print(f"FAIL {test.__name__}: {error!r}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
