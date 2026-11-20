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

    def test_a_present_pointer_test_is_not_a_null_refusal(self) -> None:
        r = one(
            "SonareError f(const char* error) {\n"
            '  if (error != nullptr) { set_last_error(SONARE_ERROR_INVALID_PARAMETER, error);\n'
            "    return SONARE_ERROR_INVALID_PARAMETER; }\n}\n"
        )
        self.assertEqual((r.classes, r.state), ({"value"}, "text"))

    def test_two_pointer_tests_compared_are_a_relation(self) -> None:
        r = one(
            "SonareError f(float* a, float* b) {\n"
            "  if ((a == nullptr) != (b == nullptr)) return SONARE_ERROR_INVALID_PARAMETER;\n}\n"
        )
        self.assertEqual(r.classes, {"relation"})

    def test_an_error_code_translation_is_counted_apart(self) -> None:
        source = (
            "SonareError map(sonare::ErrorCode code) {\n  switch (code) {\n"
            "    case sonare::ErrorCode::InvalidParameter:\n"
            "      return SONARE_ERROR_INVALID_PARAMETER;\n  }\n}\n"
            "SonareError g(Status s) {\n  switch (s) {\n    case Status::kBad:\n"
            "      return SONARE_ERROR_INVALID_PARAMETER;\n  }\n}\n"
        )
        result = scanner.Scan()
        scanner.scan_text("t.cpp", source, set(), result)
        self.assertEqual(result.translation_returns, 1)
        self.assertEqual([r.line for r in result.refusals], [10])

    def test_an_unguarded_return_after_its_message_is_a_texted_helper(self) -> None:
        r = one(
            "SonareError refuse_x(const char* e) {\n"
            "  set_last_error(SONARE_ERROR_INVALID_PARAMETER, e);\n"
            "  return SONARE_ERROR_INVALID_PARAMETER;\n}\n"
        )
        self.assertEqual((r.classes, r.state), ({"helper"}, "text"))

    def test_a_return_with_no_guard_is_unclassified_and_listed(self) -> None:
        r = one("SonareError f() {\n  return SONARE_ERROR_INVALID_PARAMETER;\n}\n")
        self.assertEqual((r.classes, r.guard), ({"unclassified"}, "(no guard)"))

    def test_a_return_in_a_comment_or_string_is_not_read(self) -> None:
        source = (
            "// return SONARE_ERROR_INVALID_PARAMETER;\n"
            'const char* s = "return SONARE_ERROR_INVALID_PARAMETER;";\n'
        )
        self.assertEqual(read(source), [])


def interim(source: str) -> list:
    result = scanner.Scan()
    helpers = scanner.interim_helpers({"t.cpp": source})
    scanner.scan_interim("t.cpp", source, helpers, result)
    return [(line, text) for _, line, text in result.interim]


class InterimTest(unittest.TestCase):
    def test_an_entry_literal_is_one_site_and_a_comment_is_none(self) -> None:
        source = (
            "SonareError sonare_x(void* e) {\n"
            "  // refused without reporting a cause\n"
            '  return refuse("the mixer refused without reporting a cause");\n'
            "}\n"
        )
        self.assertEqual([line for line, _ in interim(source)], [3])

    def test_a_helper_counts_once_per_call_and_not_for_its_own_body(self) -> None:
        source = (
            "SonareError refuse_edit() {\n"
            '  return refuse("x must describe sends the mixer accepts");\n'
            "}\n"
            "SonareError sonare_a() { return refuse_edit(); }\n"
            "SonareError sonare_b() { return refuse_edit(); }\n"
        )
        self.assertEqual(interim(source), [(4, "refuse_edit()"), (5, "refuse_edit()")])

    def test_an_entry_function_is_never_a_helper(self) -> None:
        source = (
            "SonareError sonare_a() {\n"
            '  return refuse("v must be a value the parameter accepts");\n'
            "}\n"
            "SonareError sonare_b() { return sonare_a(); }\n"
        )
        self.assertEqual([line for line, _ in interim(source)], [2])

    def test_a_ruled_text_is_not_interim(self) -> None:
        self.assertEqual(interim('SonareError sonare_a() { return refuse("x must be finite"); }\n'), [])


_HEADER = """
/// @brief Reads the channel.
/// @details Like every sonare_last_* accessor, it records no error.
SonareError sonare_last_thing(int* out);
/// @brief States nothing.
SonareError sonare_last_other(int* out);
"""

_ACCESSOR_SOURCE = """
SonareError render_thing(int* out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
}
SonareError sonare_last_thing(int* out) { return render_thing(out); }
SonareError sonare_last_other(int* out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
}
SonareError sonare_entry(int* out) {
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
}
"""


class AccessorTest(unittest.TestCase):
    def scan(self) -> object:
        stated = scanner.stated_accessors({"h.h": _HEADER})
        accessors = frozenset(scanner.accessor_functions({"t.cpp": _ACCESSOR_SOURCE}, stated))
        result = scanner.Scan()
        scanner.scan_text("t.cpp", _ACCESSOR_SOURCE, set(), result, accessors)
        return stated, accessors, result

    def test_only_a_declaration_stating_the_contract_is_an_accessor(self) -> None:
        stated, accessors, _ = self.scan()
        self.assertEqual(stated, {"sonare_last_thing"})
        self.assertEqual(accessors, {"sonare_last_thing", "render_thing"})

    def test_an_accessor_return_is_counted_apart_and_a_non_accessor_is_still_caught(self) -> None:
        _, _, result = self.scan()
        self.assertEqual(result.accessor_returns, 1)
        self.assertEqual([(r.line, r.state) for r in result.refusals], [(8, "none"), (12, "none")])
        self.assertEqual(scanner.counts(result)["no_text"], 2)

    def test_an_audio_thread_entry_return_is_counted_apart(self) -> None:
        source = """
SonareError sonare_rt_process(float* x) {
  SONARE_C_RT_API_ENTRY;
  if (!x) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
}
SonareError sonare_entry(float* x) {
  SONARE_C_API_ENTRY;
  if (!x) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
}
"""
        result = scanner.Scan()
        scanner.scan_text("t.cpp", source, set(), result)
        self.assertEqual(result.realtime_returns, 1)
        self.assertEqual([r.line for r in result.refusals], [9])

    def test_the_tree_has_stated_accessors(self) -> None:
        self.assertGreater(scanner.scan(scanner.ROOT).accessor_returns, 0)


class RatchetTest(unittest.TestCase):
    def test_both_ways_above_and_below_the_line_fail_and_equal_passes(self) -> None:
        line = {"no_text": 5, "null_nonconforming": 3, "unclassified": 1, "interim_no_cause": 2}
        self.assertEqual(scanner.evaluate(dict(line), line), [])
        self.assertTrue(scanner.evaluate({**line, "no_text": 6}, line))
        self.assertTrue(scanner.evaluate({**line, "no_text": 4}, line, ceiling_only=False))
        self.assertTrue(scanner.evaluate(line, {}))

    def test_ceiling_only_passes_a_count_below_the_line_and_still_fails_above(self) -> None:
        line = {"no_text": 5, "null_nonconforming": 3, "unclassified": 1, "interim_no_cause": 2}
        self.assertEqual(scanner.evaluate({**line, "no_text": 4}, line, ceiling_only=True), [])
        self.assertTrue(scanner.evaluate({**line, "no_text": 6}, line, ceiling_only=True))

    def test_the_tree_scan_reads_returns(self) -> None:
        self.assertGreater(len(scanner.scan(scanner.ROOT).refusals), 0)


if __name__ == "__main__":
    unittest.main()
