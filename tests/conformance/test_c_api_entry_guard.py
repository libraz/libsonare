"""Stdlib self-tests for the C-ABI entry guard checker.

The checker's failure mode is a false clean, so these tests pin three things
separately: the shape of each rule on hand-written bodies, a floor on how many
entry points the scan classifies, and non-vacuity -- copying the real
translation units, removing one guard, and requiring the checker to name
exactly that entry and nothing else.  The break happens on a copy so no session
building the shared tree ever compiles the broken source.
"""

from __future__ import annotations

import importlib.util
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_c_api_entry_guard.py"
SPEC = importlib.util.spec_from_file_location("libsonare_c_api_entry_guard_checker", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)

SOURCE_DIR = ROOT / "src" / "c_api"
HEADER_DIR = ROOT / "include" / "sonare"

# Floor, not target: a definition pattern that stops matching fails here.
MIN_ENTRY_POINTS = 900


def _scan(source: str):
    """Run the checker over one synthetic translation unit."""
    with tempfile.TemporaryDirectory() as tmp:
        source_dir = Path(tmp) / "c_api"
        source_dir.mkdir()
        (source_dir / "unit.cpp").write_text(source, encoding="utf-8")
        return CHECKER.audit(source_dir, Path(tmp) / "absent", Path(tmp) / "absent")


def _problems(report) -> list[tuple[str, str]]:
    return [(f.function, f.problem) for f in report.findings]


class ClassificationTest(unittest.TestCase):
    def test_a_guarded_entry_is_clean(self) -> None:
        report = _scan(
            "SonareError sonare_f(int n, float** out) {\n"
            "  SONARE_C_API_ENTRY;\n"
            "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  SONARE_C_TRY\n"
            "  std::vector<float> v(n);\n"
            "  return SONARE_OK;\n"
            "  SONARE_C_CATCH\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])

    def test_an_allocation_before_the_try_is_reported(self) -> None:
        report = _scan(
            "SonareError sonare_f(int n) {\n"
            "  SONARE_C_API_ENTRY;\n"
            "  std::vector<float> v;\n"
            "  v.reserve(n);\n"
            "  SONARE_C_TRY\n"
            "  return SONARE_OK;\n"
            "  SONARE_C_CATCH\n"
            "}\n"
        )
        problems = [p for _, p in _problems(report)]
        self.assertIn("reserve() before SONARE_C_TRY", problems)

    def test_an_unguarded_allocating_entry_is_reported(self) -> None:
        report = _scan(
            "SonareError sonare_f(size_t n) {\n"
            "  SONARE_C_API_ENTRY;\n"
            "  std::vector<int> v;\n"
            "  v.reserve(n);\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertIn(("sonare_f", "reserve() with no guard"), _problems(report))

    def test_a_missing_clear_is_reported_on_a_sonare_error_entry(self) -> None:
        report = _scan(
            "SonareError sonare_f(int* out) {\n"
            "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
            "  *out = 1;\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertIn(
            ("sonare_f", "does not open with SONARE_C_API_ENTRY or SONARE_C_TRY"),
            _problems(report),
        )

    def test_code_after_the_catch_is_reported(self) -> None:
        report = _scan(
            "SonareError sonare_f(int n) {\n"
            "  SONARE_C_API_ENTRY;\n"
            "  SONARE_C_TRY\n"
            "  (void)n;\n"
            "  SONARE_C_CATCH\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertTrue(any("not closed" in p for _, p in _problems(report)))

    def test_each_preprocessor_branch_is_held_separately(self) -> None:
        # The guard in the feature-on branch does not cover an allocating
        # feature-off branch.
        report = _scan(
            "SonareError sonare_f(int n) {\n"
            "  SONARE_C_API_ENTRY;\n"
            "#if defined(FEATURE)\n"
            "  SONARE_C_TRY\n"
            "  return SONARE_OK;\n"
            "  SONARE_C_CATCH\n"
            "#else\n"
            "  std::vector<int> v;\n"
            "  v.reserve(n);\n"
            "  return SONARE_ERROR_NOT_SUPPORTED;\n"
            "#endif\n"
            "}\n"
        )
        self.assertIn(("sonare_f", "reserve() with no guard"), _problems(report))

    def test_a_runner_with_a_catch_all_guards_its_caller(self) -> None:
        report = _scan(
            "template <typename Fn>\n"
            "SonareError run_guarded(Fn body) {\n"
            "  SONARE_C_TRY\n"
            "  return body();\n"
            "  SONARE_C_CATCH\n"
            "}\n"
            "SonareError sonare_f(int n) {\n"
            "  SONARE_C_API_ENTRY;\n"
            "  return run_guarded([&] {\n"
            "    std::vector<int> v(n);\n"
            "    return SONARE_OK;\n"
            "  });\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])

    def test_a_rethrowing_helper_is_not_a_guard(self) -> None:
        report = _scan(
            "SonareError cleanup_then_rethrow(int n) {\n"
            "  try {\n"
            "    std::vector<int> v(n);\n"
            "  } catch (...) {\n"
            "    throw;\n"
            "  }\n"
            "  return SONARE_OK;\n"
            "}\n"
            "SonareError sonare_f(int n) {\n"
            "  SONARE_C_API_ENTRY;\n"
            "  return cleanup_then_rethrow(n);\n"
            "}\n"
        )
        self.assertIn(("sonare_f", "cleanup_then_rethrow() with no guard"), _problems(report))

    def test_the_realtime_marker_exempts_an_entry(self) -> None:
        report = _scan(
            "SonareError sonare_f(Engine* e) {\n"
            "  SONARE_C_RT_API_ENTRY;\n"
            "  e->process();\n"
            "  return SONARE_OK;\n"
            "}\n"
        )
        self.assertEqual(report.findings, [])
        self.assertEqual(report.classes.get("realtime"), 1)


class TreeTest(unittest.TestCase):
    """The scan against the shipped C-ABI sources."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.report = CHECKER.audit(SOURCE_DIR, HEADER_DIR)

    def test_the_tree_is_clean_over_a_nonzero_population(self) -> None:
        self.assertGreaterEqual(self.report.entry_points, MIN_ENTRY_POINTS)
        self.assertEqual([f.as_line() for f in self.report.findings], [])

    def _break(self, file: str, function: str, line: str) -> set[tuple[str, str]]:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "c_api"
            shutil.copytree(SOURCE_DIR, copy)
            path = copy / file
            text = path.read_text(encoding="utf-8")
            start = text.index(f" {function}(")
            at = text.index(line, start)
            path.write_text(text[:at] + text[at + len(line) :], encoding="utf-8")
            report = CHECKER.audit(copy, HEADER_DIR, ROOT / "src")
        return {(f.function, f.problem) for f in report.findings}

    def test_removing_one_entry_clear_reports_exactly_that_entry(self) -> None:
        found = self._break(
            "sonare_c_sample_bank.cpp", "sonare_sample_bank_add_zone", "  SONARE_C_API_ENTRY;\n"
        )
        self.assertEqual(
            found,
            {
                (
                    "sonare_sample_bank_add_zone",
                    "does not open with SONARE_C_API_ENTRY or SONARE_C_TRY",
                )
            },
        )

    def test_removing_one_guard_reports_exactly_that_entry(self) -> None:
        found = self._break("core_analysis.cpp", "sonare_chord_functions", "  SONARE_C_TRY\n")
        self.assertTrue(found)
        self.assertEqual({function for function, _ in found}, {"sonare_chord_functions"})


if __name__ == "__main__":
    unittest.main()
