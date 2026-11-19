"""Stdlib self-tests for the facade refusal ratchet.

Each case drives the checker's own functions -- the ones its entry point calls --
so a rule is asserted through the code that ships rather than a restatement.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_facade_refusal_ratchet",
    Path(__file__).resolve().parent / "check_facade_refusal_ratchet.py",
)
assert _SPEC and _SPEC.loader
ratchet = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(ratchet)


def counts(text: str) -> dict[str, int]:
    return dict(ratchet.count_text(text))


class CountTextTest(unittest.TestCase):
    def test_counts_each_raw_error_class(self) -> None:
        text = "throw new RangeError('a');\nthrow new TypeError(`b`);\nthrow new SonareError(1, 'c');\n"
        self.assertEqual(counts(text)["raw-error"], 3)

    def test_ignores_errors_in_comments_and_strings(self) -> None:
        text = (
            "// throw new RangeError('x');\n"
            "/** @example throw new TypeError('y') */\n"
            "const s = 'new RangeError(';\n"
            "const u = 'http://example';\n"
            "throw refuse('z');\n"
        )
        self.assertEqual(counts(text)["raw-error"], 0)

    def test_a_url_in_a_string_does_not_start_a_comment(self) -> None:
        text = "const u = 'https://x'; throw new RangeError('after');\n"
        self.assertEqual(counts(text)["raw-error"], 1)

    def test_counts_name_carrying_parameters_only(self) -> None:
        text = (
            "function a(fnName: string, v: number) {}\n"
            "function b(v: number, fn: RequestFn) {}\n"
            "function c(entryName?: string) {}\n"
            "function d(argName: string, field: string) {}\n"
            "const e = { fnName: 'x' };\n"
        )
        self.assertEqual(counts(text)["fn-name-param"], 3)


class ValueEchoTest(unittest.TestCase):
    def test_counts_got_and_out_of_range_in_every_ts_literal_kind(self) -> None:
        text = (
            "throw refuse(`strip must be a track id, got '${ref}'`);\n"
            "throw refuse('index out of range');\n"
            'throw refuseType("mode must be a name, got " + typeof v);\n'
            "throw refuse(`mode must be one of a, b (got '${mode}')`);\n"
            "throw refuse('gain must be finite');\n"
            "throw refuse(`rate out of supported range [8000, 384000]`);\n"
            "throw refuse(`${key} must be a finite number`);\n"
        )
        self.assertEqual(counts(text)["value-echo"], 6)

    def test_ignores_comments_and_code(self) -> None:
        text = (
            "/** @throws RangeError when `program` is out of range */\n"
            "// a value, got here\n"
            "const gotIt = outOfRange;\n"
        )
        self.assertEqual(counts(text)["value-echo"], 0)

    def test_cpp_reads_double_quoted_literals_past_char_literals(self) -> None:
        text = (
            "char q = '\"';\n"
            'throw WasmRangeError("strip index out of range");\n'
            '// "commented, got"\n'
            'auto s = std::string(key) + " must be one of " + valid + ", got \'" + name + "\'";\n'
        )
        found = ratchet.count_text(text, ("value-echo",), cpp=True)
        self.assertEqual(dict(found), {"value-echo": 2})


class UnknownKeyTest(unittest.TestCase):
    def test_counts_each_older_spelling_and_not_the_current_one(self) -> None:
        text = (
            "throw refuseType(`unknown request key '${key}'`);\n"
            "throw refuseType(`unknown option '${key}' in 'bank'`);\n"
            "throw refuseType(\"unknown config key 'destinationId'\");\n"
            "throw refuseType(`unknown key ${path} (expected ${list})`);\n"
            "// an unknown option in a comment\n"
        )
        self.assertEqual(counts(text)["unknown-key"], 3)

    def test_cpp_counts_the_older_spelling(self) -> None:
        text = 'throw WasmTypeError("unknown option \'" + key + "\'");\n'
        found = ratchet.count_text(text, ("unknown-key",), cpp=True)
        self.assertEqual(dict(found), {"unknown-key": 1})


class ArrayScanTest(unittest.TestCase):
    def test_counts_the_retired_word_in_every_ts_literal_kind(self) -> None:
        text = (
            "throw refuse(`${key} contains a non-finite value at index ${i}`);\n"
            "throw refuse('samples contains a non-finite value');\n"
            "throw refuse(`${key} contains a non-finite sample at index ${i}`);\n"
            "/** a non-finite value in a doc comment */\n"
        )
        self.assertEqual(counts(text)["array-scan"], 2)


class EvaluateTest(unittest.TestCase):
    def found(self, raw: int, scanned: int = 1) -> dict:
        totals = {"raw-error": raw, "fn-name-param": 0}
        files = {"a.ts": dict(totals)} if raw else {}
        return {"node": {"scanned": scanned, "totals": totals, "files": files}}

    def test_equal_counts_pass(self) -> None:
        line = {"node": {"raw-error": 2, "fn-name-param": 0}}
        self.assertEqual(ratchet.evaluate(self.found(2), line), [])

    def test_a_rise_fails_naming_the_file(self) -> None:
        line = {"node": {"raw-error": 1, "fn-name-param": 0}}
        errors = ratchet.evaluate(self.found(2), line)
        self.assertEqual(len(errors), 1)
        self.assertIn("exceeds the recorded 1", errors[0])
        self.assertIn("a.ts 2", errors[0])

    def test_a_drop_fails_until_the_line_is_lowered(self) -> None:
        line = {"node": {"raw-error": 3, "fn-name-param": 0}}
        errors = ratchet.evaluate(self.found(2), line)
        self.assertEqual(len(errors), 1)
        self.assertIn("below the recorded 3", errors[0])

    def test_a_scan_that_read_nothing_fails(self) -> None:
        line = {"node": {"raw-error": 0, "fn-name-param": 0}}
        errors = ratchet.evaluate(self.found(0, scanned=0), line)
        self.assertEqual(len(errors), 1)
        self.assertIn("scanned no source", errors[0])

    def test_an_unrecorded_kind_fails(self) -> None:
        errors = ratchet.evaluate(self.found(0), {"node": {"raw-error": 0}})
        self.assertEqual(len(errors), 1)
        self.assertIn("fn-name-param: no recorded line", errors[0])


class ScanTest(unittest.TestCase):
    def test_skips_declarations_and_the_helpers_home(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for surface in ("node", "wasm"):
                src = root / ratchet.SURFACES[surface][0][0]
                (src / "internal").mkdir(parents=True)
                (src / "a.ts").write_text("throw new RangeError('a');\n")
                (src / "b.d.ts").write_text("throw new RangeError('b');\n")
                (src / "internal" / "entry.ts").write_text("throw new RangeError('c');\n")
            found = ratchet.scan(root)
            for surface in ("node", "wasm"):
                self.assertEqual(found[surface]["scanned"], 1)
                self.assertEqual(found[surface]["totals"]["raw-error"], 1)

    def test_native_reads_both_roots_and_fails_when_it_reads_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            empty = ratchet.scan(root)["native"]
            self.assertEqual(empty["scanned"], 0)
            self.assertIn("scanned no source", ratchet.evaluate({"native": empty}, {})[0])
            for rel in ratchet.SURFACES["native"][0]:
                (root / rel).mkdir(parents=True)
                (root / rel / "a.cpp").write_text('throw X("i out of range");\n')
                (root / rel / "b.h").write_text('throw X("i must be below 4");\n')
            native = ratchet.scan(root)["native"]
            self.assertEqual(native["scanned"], 4)
            self.assertEqual(native["totals"], {"value-echo": 2, "unknown-key": 0, "array-scan": 0})

    def test_core_counts_the_retired_scan_word_outside_the_native_tree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src" / "wasm").mkdir(parents=True)
            (root / "src" / "feature").mkdir()
            (root / "src" / "feature" / "a.cpp").write_text(
                'refuse("S contains a non-finite value");\n'
                'refuse("y contains a non-finite sample at index ");\n'
                "// a non-finite value in a comment\n"
            )
            (root / "src" / "wasm" / "b.cpp").write_text(
                'refuse(" contains a non-finite value");\n'
            )
            found = ratchet.scan(root)
            self.assertEqual(found["core"]["totals"], {"array-scan": 1})
            self.assertEqual(found["core"]["scanned"], 1)
            self.assertEqual(found["native"]["totals"]["array-scan"], 1)


if __name__ == "__main__":
    unittest.main()
