"""Report consumers reject truthy non-boolean correctness values."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "eval"))
sys.path.insert(0, str(ROOT / "tests" / "fixtures"))

import compare_optional_fixture_reports as comparator
import evaluate_optional_fixture_targets as evaluator
import summarize_accuracy

MISSING = object()


def invalid_correct_values() -> list[tuple[str, object]]:
    return [
        ("missing", MISSING),
        ("null", None),
        ("zero", 0),
        ("one", 1),
        ("true string", "true"),
        ("false string", "false"),
        ("list", []),
        ("object", {}),
    ]


def with_correct(value: object) -> dict:
    item = {"metric": "key", "expected_root": 0, "expected_mode": 0}
    if value is not MISSING:
        item["correct"] = value
    return item


def report(*items: dict) -> dict:
    return {"runs": [{"observations": list(items)}]}


class CorrectnessValidationTest(unittest.TestCase):
    def test_accuracy_rollup_rejects_malformed_key_and_meter_values(self) -> None:
        for metric in ("key", "meter"):
            for label, value in invalid_correct_values():
                with self.subTest(metric=metric, value=label):
                    item = with_correct(value)
                    item["metric"] = metric
                    with self.assertRaisesRegex(ValueError, "correct.*boolean"):
                        summarize_accuracy.summarize(report(item))

    def test_target_evaluator_rejects_values_only_in_consumed_branches(self) -> None:
        for metric in ("key", "meter", "meter_4_4"):
            for label, value in invalid_correct_values():
                with self.subTest(branch="metric_value", metric=metric, value=label):
                    item = with_correct(value)
                    item["metric"] = metric
                    with self.assertRaisesRegex(ValueError, "correct.*boolean"):
                        evaluator.metric_value(item)

        for metric in ("key", "meter", "meter_4_4"):
            for label, value in invalid_correct_values():
                with self.subTest(branch="metric_pass", metric=metric, value=label):
                    item = with_correct(value)
                    item["metric"] = metric
                    with self.assertRaisesRegex(ValueError, "correct.*boolean"):
                        evaluator.metric_pass(item)

        # A key observation without a reference has no threshold pass/fail.
        # That branch must not start validating a field it does not consume.
        self.assertIsNone(evaluator.metric_pass(with_correct("false") | {"expected_root": None}))

    def test_comparator_validates_only_its_key_score_fallback(self) -> None:
        for label, value in invalid_correct_values():
            with (
                self.subTest(value=label),
                self.assertRaisesRegex(ValueError, "correct.*boolean"),
            ):
                comparator.key_score(with_correct(value))

        # A measured MIREX score takes precedence, and an unknown reference
        # keeps the measured-root tuple comparison without reading correct.
        self.assertEqual(comparator.key_score(with_correct("false") | {"mirex_score": 0.75}), 0.75)
        self.assertEqual(
            comparator.key_score(
                {"metric": "key", "correct": "false", "measured_root": 4, "measured_mode": 1}
            ),
            (4, 1),
        )

    def test_boolean_values_keep_existing_aggregate_results(self) -> None:
        observations = [
            {
                "metric": "key",
                "fixture": "set/a",
                "correct": True,
                "expected_root": 0,
                "mirex_score": 1.0,
            },
            {
                "metric": "key",
                "fixture": "set/b",
                "correct": False,
                "expected_root": 1,
                "mirex_score": 0.0,
            },
            {"metric": "meter", "fixture": "set/a", "correct": True},
            {"metric": "meter", "fixture": "set/b", "correct": False},
        ]
        summary = summarize_accuracy.summarize(report(*observations))
        self.assertEqual(summary["dimensions"]["key"]["overall"]["accuracy"], 0.5)
        self.assertEqual(summary["dimensions"]["meter"]["overall"]["accuracy"], 0.5)

        evaluated = evaluator.evaluate(
            report(*observations),
            {"key": {"pass_rate_min": 0.5}, "meter": {"pass_rate_min": 0.5}},
            require_metrics=True,
        )
        by_metric = {item["metric"]: item for item in evaluated["summaries"]}
        self.assertEqual(by_metric["key"]["mean"], 0.5)
        self.assertEqual(by_metric["key"]["threshold_pass_rate"], 0.5)
        self.assertEqual(by_metric["meter"]["mean"], 0.5)
        self.assertEqual(by_metric["meter"]["threshold_pass_rate"], 0.5)

        self.assertEqual(comparator.key_score(observations[0]), 1.0)
        self.assertEqual(comparator.key_score(observations[1]), 0.0)

    def test_other_metrics_do_not_start_consuming_correct(self) -> None:
        item = {
            "metric": "bpm",
            "correct": "false",
            "relative_error_percent": 1.0,
            "tolerance_percent": 4.0,
        }
        self.assertEqual(evaluator.metric_value(item), 1.0)
        self.assertTrue(evaluator.metric_pass(item))
        self.assertEqual(comparator.score(item), 1.0)
        self.assertEqual(
            summarize_accuracy.summarize(report(item | {"fixture": "set/a"}))["dimensions"]["bpm"][
                "overall"
            ]["accuracy"],
            1.0,
        )


class ReportCommandValidationTest(unittest.TestCase):
    @staticmethod
    def run_script(script: str, *arguments: str, cwd: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(ROOT / script), *arguments],
            cwd=cwd,
            capture_output=True,
            check=False,
            text=True,
        )

    def test_accuracy_cli_bootstraps_shared_validator_from_another_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cwd = Path(temporary)
            source = cwd / "report.json"
            source.write_text(json.dumps(report(with_correct("false") | {"fixture": "set/a"})))
            result = self.run_script("tools/eval/summarize_accuracy.py", str(source), cwd=cwd)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn("correct' must be a boolean", result.stderr)

    def test_fixture_clis_fail_before_writing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cwd = Path(temporary)
            source = cwd / "report.json"
            source.write_text(json.dumps(report(with_correct("false") | {"fixture": "set/a"})))
            output = cwd / "sentinel.json"
            output.write_text("keep this output")

            evaluated = self.run_script(
                "tests/fixtures/evaluate_optional_fixture_targets.py",
                str(source),
                "--metrics",
                "key",
                "--output",
                str(output),
                cwd=cwd,
            )
            self.assertNotEqual(evaluated.returncode, 0)
            self.assertEqual(evaluated.stdout, "")
            self.assertIn("correct' must be a boolean", evaluated.stderr)
            self.assertEqual(output.read_text(), "keep this output")

            compared = self.run_script(
                "tests/fixtures/compare_optional_fixture_reports.py",
                str(source),
                str(source),
                "--output",
                str(output),
                cwd=cwd,
            )
            self.assertNotEqual(compared.returncode, 0)
            self.assertEqual(compared.stdout, "")
            self.assertIn("correct' must be a boolean", compared.stderr)
            self.assertEqual(output.read_text(), "keep this output")

    def test_polyphony_negative_requirement_is_refused_before_opening_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cwd = Path(temporary)
            result = self.run_script(
                "tools/eval/summarize_polyphony.py",
                str(cwd / "does-not-exist.jsonl"),
                "--require",
                "-1",
                cwd=cwd,
            )
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")
            self.assertIn("non-negative", result.stderr)

    def test_polyphony_zero_requirement_still_accepts_observations(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cwd = Path(temporary)
            source = cwd / "observations.jsonl"
            source.write_text(
                'POLYF0_EVAL {"item":"one","voices":1,"frames":1,'
                '"precision":1.0,"recall":1.0,"f_measure":1.0,'
                '"polyphony_error":0.0,"tp":1,"fp":0,"fn":0}\n'
            )
            result = self.run_script(
                "tools/eval/summarize_polyphony.py",
                str(source),
                "--require",
                "0",
                cwd=cwd,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("pooled over 1 items", result.stdout)


if __name__ == "__main__":
    unittest.main()
