"""Reject non-finite comparison tolerances instead of silently disabling gates."""

from __future__ import annotations

import importlib.util
import json
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


class WasmIostreamGuardTest(unittest.TestCase):
    def run_check(self, payload: bytes) -> int:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wasm = root / "tiny.wasm"
            # The payload rides in a custom section, so the module stays well formed.
            custom = b"\x01m" + payload
            wasm.write_bytes(b"\0asm\1\0\0\0\x00" + bytes([len(custom)]) + custom)
            baseline = root / "baseline.json"
            baseline.write_text('{"format":1,"artifacts":{"tiny":{"raw":4096,"gzip":4096}}}')
            argv = ["check_wasm_size.py", "--baseline", str(baseline), "--artifact", f"tiny={wasm}"]
            with mock.patch.object(sys, "argv", argv):
                return check_wasm_size.main()

    def test_a_module_without_iostream_passes(self) -> None:
        self.assertEqual(self.run_check(b""), 0)

    def test_an_iostream_type_name_fails_under_the_size_limit(self) -> None:
        for marker in check_wasm_size.IOSTREAM_MARKERS:
            with self.subTest(marker=marker):
                self.assertEqual(self.run_check(b"\x00" + marker + b"IcEE\x00"), 1)


class WasmForbiddenBindingGuardTest(unittest.TestCase):
    def run_check(self, name: str, payload: bytes) -> int:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wasm = root / "tiny.wasm"
            custom = b"\x01m" + payload
            wasm.write_bytes(b"\0asm\1\0\0\0\x00" + bytes([len(custom)]) + custom)
            baseline = root / "baseline.json"
            artifact = {name: {"raw": 4096, "gzip": 4096}}
            baseline.write_text(json.dumps({"format": 1, "artifacts": artifact}))
            argv = [
                "check_wasm_size.py",
                "--baseline",
                str(baseline),
                "--artifact",
                f"{name}={wasm}",
            ]
            with mock.patch.object(sys, "argv", argv):
                return check_wasm_size.main()

    def test_the_analysis_module_fails_on_a_forbidden_binding(self) -> None:
        for binding in check_wasm_size.FORBIDDEN_BINDINGS["sonare-analysis.wasm"]:
            with self.subTest(binding=binding):
                self.assertEqual(self.run_check("sonare-analysis.wasm", b"\0" + binding + b"\0"), 1)

    def test_the_full_module_may_carry_the_same_binding(self) -> None:
        self.assertEqual(self.run_check("sonare.wasm", b"\0systemInfo\0"), 0)

    def test_a_longer_name_containing_the_binding_passes(self) -> None:
        self.assertEqual(self.run_check("sonare-analysis.wasm", b"\0systemInfoJson\0"), 0)


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
