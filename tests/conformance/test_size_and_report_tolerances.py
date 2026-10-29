"""Reject non-finite comparison tolerances instead of silently disabling gates."""

from __future__ import annotations

import importlib.util
import math
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests" / "fixtures"))
import check_wasm_size

FIXTURE_PATH = ROOT / "tests" / "fixtures" / "compare_optional_fixture_reports.py"
SPEC = importlib.util.spec_from_file_location("compare_optional_fixture_reports", FIXTURE_PATH)
assert SPEC is not None and SPEC.loader is not None
REPORT_COMPARE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = REPORT_COMPARE
SPEC.loader.exec_module(REPORT_COMPARE)


class WasmSizeToleranceTest(unittest.TestCase):
    def test_non_finite_and_negative_cli_tolerances_are_refused(self) -> None:
        for value in ("nan", "inf", "-inf", "-0.1"):
            with (
                self.subTest(value=value),
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        "check_wasm_size.py",
                        "--baseline",
                        "unused",
                        "--artifact",
                        "x=unused",
                        f"--max-increase={value}",
                    ],
                ),
            ):
                with self.assertRaises(SystemExit) as raised:
                    check_wasm_size.main()
                self.assertEqual(raised.exception.code, 2)

    def test_finite_non_negative_cli_tolerance_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wasm = root / "tiny.wasm"
            wasm.write_bytes(b"\0asm\1\0\0\0")
            baseline = root / "baseline.json"
            baseline.write_text('{"format":1,"artifacts":{"tiny":{"raw":8,"gzip":28}}}')
            with mock.patch.object(
                sys,
                "argv",
                [
                    "check_wasm_size.py",
                    "--baseline",
                    str(baseline),
                    "--artifact",
                    f"tiny={wasm}",
                    "--max-increase",
                    "0",
                ],
            ):
                self.assertEqual(check_wasm_size.main(), 0)


class ReportToleranceTest(unittest.TestCase):
    @staticmethod
    def report(value: float) -> dict:
        return {
            "runs": [
                {"observations": [{"metric": "beat", "fixture": "fixture.wav", "f_measure": value}]}
            ]
        }

    def test_non_finite_or_negative_tolerance_is_refused_by_comparator(self) -> None:
        baseline = self.report(1.0)
        current = self.report(0.0)
        for tolerance in (math.nan, math.inf, -math.inf, -0.1):
            with self.subTest(tolerance=tolerance), self.assertRaises(ValueError):
                REPORT_COMPARE.compare_reports(baseline, current, tolerance)

    def test_non_finite_or_negative_cli_tolerance_is_refused(self) -> None:
        for value in ("nan", "inf", "-inf", "-0.1"):
            with (
                self.subTest(value=value),
                mock.patch.object(
                    sys,
                    "argv",
                    [
                        "compare_optional_fixture_reports.py",
                        "before.json",
                        "after.json",
                        f"--tolerance={value}",
                    ],
                ),
            ):
                with self.assertRaises(SystemExit) as raised:
                    REPORT_COMPARE.parse_args()
                self.assertEqual(raised.exception.code, 2)
