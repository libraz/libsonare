#!/usr/bin/env python3
"""Stdlib self-tests for the C-ABI pointer-contract checker.

Two failure modes are pinned separately, because they hide from each other.

A *classification* failure changes which declarations the checker looks at, and
the tests below fix each rule on a hand-written header: a release entry point is
not an allocator, a section banner is not a doc block, a ``const`` input is not
an out-parameter.

A *silent* failure is the one that certifies nothing: if the declaration pattern
stops matching, the population empties and every contract assertion passes over
it.  So the tree tests pin a floor on the classified population, and demonstrate
non-vacuity by copying the real headers, stripping one declaration's lifetime
sentence, and requiring the checker to name exactly that declaration.
"""

from __future__ import annotations

import importlib.util
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_c_api_pointer_contracts.py"
SPEC = importlib.util.spec_from_file_location(
    "libsonare_c_api_pointer_contracts_checker", CHECKER_PATH
)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
# Registered before execution because the checker defines dataclasses, which
# resolve their annotations through sys.modules at class-creation time.
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)

HEADER_DIR = ROOT / "include" / "sonare"

# The declaration the non-vacuity break strips: a library-owned thread-local
# string whose doc states both what invalidates it and that NULL is possible.
BREAK_HEADER = "sonare_c_features.h"
BREAK_DECLARATION = "sonare_hz_to_note"
BREAK_FIRST_LINE = "/// @details Thread-local storage"
BREAK_LAST_MARKER = "never free it."

# Floors, not targets.  Raise only alongside a real widening of the scan.
MIN_DECLARATIONS = 600
MIN_POINTER_DECLARATIONS = 250


def _audit(header_source: str, name: str = "api.h"):
    with tempfile.TemporaryDirectory() as tmp:
        header_dir = Path(tmp) / "sonare"
        header_dir.mkdir(parents=True)
        (header_dir / name).write_text(header_source, encoding="utf-8")
        return CHECKER.audit(header_dir)


class ClassificationTest(unittest.TestCase):
    def test_a_pointer_return_needs_invalidation_and_nullability(self) -> None:
        report = _audit("/// @brief Nearest note name.\nconst char* sonare_note(float hz);\n")
        self.assertEqual(len(report.findings), 1)
        finding = report.findings[0]
        self.assertEqual(finding.klass, CHECKER.CLASS_OWNED_RETURN)
        self.assertIn("what invalidates", finding.missing)
        self.assertIn("whether NULL", finding.missing)

    def test_a_complete_owned_return_doc_is_clean(self) -> None:
        report = _audit(
            "/// @brief Nearest note name, or NULL on failure.\n"
            "/// @details Thread-local storage, overwritten by the next call.\n"
            "const char* sonare_note(float hz);\n"
        )
        self.assertEqual(report.findings, [])

    def test_a_pointer_to_pointer_out_param_needs_its_release_call(self) -> None:
        report = _audit(
            "/// @brief Computes a thing.\n"
            "SonareError sonare_thing(const float* in, float** out, size_t* n);\n"
        )
        self.assertEqual(len(report.findings), 1)
        self.assertEqual(report.findings[0].klass, CHECKER.CLASS_ALLOCATED_OUT)
        self.assertEqual(report.findings[0].subkinds, [CHECKER.SUBKIND_POINTER_TO_POINTER])

    def test_naming_the_release_call_satisfies_it(self) -> None:
        report = _audit(
            "/// @brief Computes a thing. Free @p out with sonare_free_floats.\n"
            "SonareError sonare_thing(const float* in, float** out, size_t* n);\n"
        )
        self.assertEqual(report.findings, [])

    def test_a_release_entry_point_is_not_an_allocator(self) -> None:
        report = _audit(
            "typedef struct {\n  float* data;\n} SonareThingResult;\n"
            "void sonare_free_thing_result(SonareThingResult* result);\n"
        )
        self.assertEqual(report.findings, [])
        self.assertEqual(report.classified[CHECKER.CLASS_ALLOCATED_OUT], 0)

    def test_a_result_struct_contract_may_live_on_the_struct(self) -> None:
        report = _audit(
            "/* Holds the arrays. Free both with sonare_free_thing_result. */\n"
            "typedef struct {\n  float* data;\n} SonareThingResult;\n"
            "/// @brief Computes a thing.\n"
            "SonareError sonare_thing(const float* in, SonareThingResult* out);\n"
        )
        self.assertEqual(report.findings, [])
        self.assertEqual(report.classified[CHECKER.CLASS_ALLOCATED_OUT], 1)

    def test_a_result_struct_with_no_contract_anywhere_is_reported(self) -> None:
        report = _audit(
            "typedef struct {\n  float* data;\n} SonareThingResult;\n"
            "/// @brief Computes a thing.\n"
            "SonareError sonare_thing(const float* in, SonareThingResult* out);\n"
        )
        self.assertEqual([f.subkinds for f in report.findings], [[CHECKER.SUBKIND_RESULT_STRUCT]])

    def test_a_const_input_pointer_is_not_a_lifecycle_declaration(self) -> None:
        report = _audit("SonareError sonare_thing(const float* in, size_t n);\n")
        self.assertEqual(report.findings, [])
        self.assertEqual(sum(report.classified.values()), 0)

    def test_a_callback_and_user_pointer_need_their_retention_stated(self) -> None:
        report = _audit(
            "/// @brief Runs with progress.\n"
            "SonareError sonare_run(SonareProgressCallback cb, void* user_data);\n"
        )
        self.assertEqual([f.klass for f in report.findings], [CHECKER.CLASS_RETAINED_INPUT])

    def test_stating_the_retention_satisfies_it(self) -> None:
        report = _audit(
            "/// @brief Runs with progress.\n"
            "/// @details @p user_data must remain valid for the duration of the call.\n"
            "SonareError sonare_run(SonareProgressCallback cb, void* user_data);\n"
        )
        self.assertEqual(report.findings, [])

    def test_a_section_banner_is_not_a_declarations_doc_block(self) -> None:
        # One banner must not certify every declaration under it.
        report = _audit(
            "// ==========================================================\n"
            "// Spectral -- each frees with sonare_free_floats\n"
            "// ==========================================================\n"
            "SonareError sonare_thing(const float* in, float** out, size_t* n);\n"
        )
        self.assertEqual(len(report.findings), 1)

    def test_a_declaration_following_another_inherits_nothing(self) -> None:
        report = _audit(
            "/// @brief First. Free @p out with sonare_free_floats.\n"
            "SonareError sonare_first(const float* in, float** out, size_t* n);\n"
            "SonareError sonare_second(const float* in, float** out, size_t* n);\n"
        )
        self.assertEqual([f.name for f in report.findings], ["sonare_second"])

    def test_a_multi_line_declaration_is_parsed_whole(self) -> None:
        report = _audit(
            "/// @brief Computes a thing.\n"
            "SonareError sonare_thing(const float* in,\n"
            "                         size_t n,\n"
            "                         float** out,\n"
            "                         size_t* out_count);\n"
        )
        self.assertEqual([f.subkinds for f in report.findings], [[CHECKER.SUBKIND_POINTER_TO_POINTER]])


class TreeTest(unittest.TestCase):
    """The scan against the shipped public headers."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.report = CHECKER.audit(HEADER_DIR)

    def test_the_scan_classifies_a_nonzero_population(self) -> None:
        self.assertGreaterEqual(self.report.declarations, MIN_DECLARATIONS)
        self.assertGreaterEqual(self.report.pointer_declarations, MIN_POINTER_DECLARATIONS)
        for klass, count in self.report.classified.items():
            self.assertGreater(count, 0, f"{klass} matched nothing")

    def test_the_calibration_declaration_is_clean_as_written(self) -> None:
        named = [f for f in self.report.findings if f.name == BREAK_DECLARATION]
        self.assertEqual(named, [], "the calibration declaration must start clean")

    def test_stripping_one_lifetime_sentence_names_exactly_that_declaration(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "sonare"
            shutil.copytree(HEADER_DIR, copy)
            path = copy / BREAK_HEADER
            lines = path.read_text(encoding="utf-8").split("\n")
            start = next(i for i, line in enumerate(lines) if line.startswith(BREAK_FIRST_LINE))
            end = next(i for i in range(start, len(lines)) if BREAK_LAST_MARKER in lines[i])
            path.write_text("\n".join(lines[:start] + lines[end + 1 :]), encoding="utf-8")

            broken = CHECKER.audit(copy)

        baseline = {(f.klass, f.name, f.missing) for f in self.report.findings}
        new = sorted({(f.klass, f.name, f.missing) for f in broken.findings} - baseline)
        self.assertEqual(
            new,
            [
                (
                    CHECKER.CLASS_OWNED_RETURN,
                    BREAK_DECLARATION,
                    "what invalidates the returned pointer",
                )
            ],
        )


if __name__ == "__main__":
    unittest.main()
