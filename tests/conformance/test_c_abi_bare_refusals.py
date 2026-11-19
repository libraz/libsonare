"""Stdlib self-tests for the bare C-ABI refusal scanner.

Each case drives the scanner's own functions on a small C++ text, so a rule is asserted
through the code that ships rather than a restatement.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_c_abi_bare_refusals",
    Path(__file__).resolve().parent / "check_c_abi_bare_refusals.py",
)
assert _SPEC and _SPEC.loader
scanner = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = scanner
_SPEC.loader.exec_module(scanner)


def read(source: str, refusing: set[str] | None = None) -> list:
    result = scanner.Scan()
    helpers = scanner.refusing_helpers({"t.cpp": source}) if refusing is None else refusing
    scanner.scan_text("t.cpp", source, helpers, result)
    return result.refusals


def one(source: str):
    found = read(source)
    assert len(found) == 1, found
    return found[0]


class ClassifyTest(unittest.TestCase):
    def test_a_braceless_null_guard_without_text_is_null_and_untexted(self) -> None:
        r = one(
            "SonareError f(float* out) {\n  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n}\n"
        )
        self.assertEqual((r.classes, r.state), ({"null"}, "none"))

    def test_the_ruled_null_text_conforms_and_a_prefixed_one_does_not(self) -> None:
        body = (
            "SonareError f(float* out) {{\n  if (out == nullptr) {{\n"
            '    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "{msg}");\n'
            "    return SONARE_ERROR_INVALID_PARAMETER;\n  }}\n}}\n"
        )
        self.assertEqual(one(body.format(msg="out must not be NULL")).state, "conforming")
        self.assertEqual(one(body.format(msg="f: out must not be NULL")).state, "text")

    def test_a_null_allowed_with_a_zero_count_is_null_class(self) -> None:
        r = one(
            "SonareError f(const int* xs, size_t count) {\n"
            "  if (count > 0 && !xs) return SONARE_ERROR_INVALID_PARAMETER;\n}\n"
        )
        self.assertEqual(r.classes, {"null"})

    def test_disjuncts_are_filed_separately_and_a_mix_is_mixed(self) -> None:
        r = one(
            "SonareError f(float* out, int n, float g, int a, int b) {\n"
            "  if (!out || n <= 0 || !std::isfinite(g) || a > b)\n"
            "    return SONARE_ERROR_INVALID_PARAMETER;\n}\n"
        )
        self.assertEqual(r.classes, {"null", "count", "value", "relation"})
        self.assertEqual(r.label, "mixed")

    def test_an_arrow_member_is_not_read_as_a_comparison(self) -> None:
        r = one(
            "SonareError f(const Cfg* config) {\n"
            "  if (config->struct_version != SONARE_CFG_VERSION) return SONARE_ERROR_INVALID_PARAMETER;\n}\n"
        )
        self.assertEqual(r.classes, {"value"})

    def test_a_helper_that_sets_the_message_makes_the_return_helper_texted(self) -> None:
        source = (
            "int factor_or_refuse(int f) {\n"
            '  if (f < 1) { set_last_error(SONARE_ERROR_INVALID_PARAMETER, "f must be 1"); return 0; }\n'
            "  return f;\n}\n"
            "SonareError g(int f) {\n"
            "  if (!factor_or_refuse(f)) return SONARE_ERROR_INVALID_PARAMETER;\n}\n"
        )
        found = read(source)
        self.assertEqual([(r.classes, r.state) for r in found], [({"helper"}, "helper")])

    def test_a_return_with_no_guard_is_unclassified_and_listed(self) -> None:
        r = one("SonareError f() {\n  return SONARE_ERROR_INVALID_PARAMETER;\n}\n")
        self.assertEqual((r.classes, r.guard), ({"unclassified"}, "(no guard)"))

    def test_a_return_in_a_comment_or_string_is_not_read(self) -> None:
        source = (
            "// return SONARE_ERROR_INVALID_PARAMETER;\n"
            'const char* s = "return SONARE_ERROR_INVALID_PARAMETER;";\n'
        )
        self.assertEqual(read(source), [])


class RatchetTest(unittest.TestCase):
    def test_both_ways_above_and_below_the_line_fail_and_equal_passes(self) -> None:
        line = {"no_text": 5, "null_nonconforming": 3, "unclassified": 1}
        self.assertEqual(scanner.evaluate(dict(line), line), [])
        self.assertTrue(scanner.evaluate({**line, "no_text": 6}, line))
        self.assertTrue(scanner.evaluate({**line, "no_text": 4}, line, ceiling_only=False))
        self.assertTrue(scanner.evaluate(line, {}))

    def test_ceiling_only_passes_a_count_below_the_line_and_still_fails_above(self) -> None:
        line = {"no_text": 5, "null_nonconforming": 3, "unclassified": 1}
        self.assertEqual(scanner.evaluate({**line, "no_text": 4}, line, ceiling_only=True), [])
        self.assertTrue(scanner.evaluate({**line, "no_text": 6}, line, ceiling_only=True))

    def test_the_tree_scan_reads_returns(self) -> None:
        self.assertGreater(len(scanner.scan(scanner.ROOT).refusals), 0)


if __name__ == "__main__":
    unittest.main()
