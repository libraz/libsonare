#!/usr/bin/env python3
"""Stdlib self-tests for the WASM narrowing-scope checker.

The checker's own failure mode is a false clean, and it has two shapes: a
declaration pattern too narrow to contain the casts it should, and a body span
measured short.  Neither is visible from inside the scan, so the tests below
break each deliberately and require the corresponding check to fire with a
count -- that the cross-check HAS power is asserted here rather than argued in
the checker's docstring.

A floor on the real tree is pinned too, because both checks pass vacuously on an
empty population: with the cast pattern matching nothing there are no orphans
and nothing to reconcile, and the checker would certify a scan that had stopped
working.
"""

from __future__ import annotations

import importlib.util
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_wasm_narrowing_scope.py"
SPEC = importlib.util.spec_from_file_location(
    "libsonare_wasm_narrowing_checker", CHECKER_PATH
)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class ReceiverTest(unittest.TestCase):
    """The record key. A receiver reduced to its variable would merge sites."""

    def _receiver(self, source: str) -> str:
        readable, code = CHECKER.prepare(source)
        match = CHECKER._CAST.search(code)
        assert match is not None
        return CHECKER.receiver_of(readable, code, match.start())

    def test_a_field_read_keeps_its_key(self) -> None:
        self.assertEqual(
            self._receiver('int n = options["nFft"].as<int>();'), 'options["nFft"]'
        )

    def test_a_call_result_keeps_its_arguments(self) -> None:
        self.assertEqual(
            self._receiver('auto v = objectProperty(clip, "lengthSamples").as<int64_t>();'),
            'objectProperty(clip, "lengthSamples")',
        )

    def test_a_chained_subscript_keeps_both_steps(self) -> None:
        self.assertEqual(
            self._receiver('auto n = desc["anchors"]["length"].as<size_t>();'),
            'desc["anchors"]["length"]',
        )


class LexicalTest(unittest.TestCase):
    def test_a_cast_written_in_a_comment_is_not_a_narrowing(self) -> None:
        _, code = CHECKER.prepare("// reads value.as<int>() on the other path\nint f();\n")
        self.assertIsNone(CHECKER._CAST.search(code))

    def test_a_brace_inside_a_literal_does_not_move_a_body_span(self) -> None:
        source = (
            'void f(val v) {\n'
            '  log("} not a closing brace {");\n'
            '  int n = v.as<int>();\n'
            '}\n'
        )
        readable, code = CHECKER.prepare(source)
        containers = CHECKER.containers_of(Path("f.cpp"), readable, code)
        self.assertEqual(len(containers), 1)
        match = CHECKER._CAST.search(code)
        assert match is not None
        self.assertTrue(containers[0].contains(match.start()))


class SyntheticTreeTest(unittest.TestCase):
    """Drives both scans over a tree holding each shape that has hidden before."""

    SOURCE = """
#include <emscripten/val.h>
using emscripten::val;

// A reader: takes a val, returns a number, sits outside the shared directory.
int localIntOption(val object, const char* key, int fallback) {
  return object[key].as<int>();
}

// A qualified parameter spelling, which a pattern without the qualified-name
// alternative cannot see.
inline uint32_t qualifiedReader(emscripten::val object, const char* key) {
  return object[key].as<uint32_t>();
}

val entryPoint(val options) {
  const int n = options["nFft"].as<int>();
  // A lambda whose conversion is in its body rather than its return type. A
  // scan shaped for named signatures assigns this to the enclosing function or
  // to nothing at all.
  auto each = [](val row) {
    return row["id"].as<int>();
  };
  // Far enough past the opening brace that a short fixed window stops here.
  PADDING
  const int tail = options["hopLength"].as<int>();
  return val(n + tail + each(options));
}
"""

    def _tree(self, root: Path, *, padding: int = 0) -> Path:
        wasm = root / "src" / "wasm" / "bindings"
        _write(wasm / "common" / "common.cpp", "int sharedReader(val v) { return 1; }\n")
        _write(
            wasm / "domain" / "unit.cpp",
            self.SOURCE.replace("PADDING", "// pad\n" * padding),
        )
        return root / "src" / "wasm"

    def _rooted(self, root: Path):
        return mock.patch.multiple(
            CHECKER,
            ROOT=root,
            WASM_TREE=root / "src" / "wasm",
            SHARED_READER_DIR=root / "src" / "wasm" / "bindings" / "common",
        )

    def test_every_shape_is_found_and_contained(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = self._tree(root)
            with self._rooted(root):
                scan = CHECKER.Scan(tree)
            self.assertEqual(scan.orphans, [], "every cast must have a container")
            self.assertEqual(scan.reconcile(), [])
            self.assertEqual(len(scan.sites), 5)
            owners = sorted(
                site.container.name for site in scan.sites if site.container
            )
            self.assertEqual(
                owners,
                ["<lambda>", "entryPoint", "entryPoint", "localIntOption", "qualifiedReader"],
                "the lambda's own conversion belongs to the lambda, not to its caller",
            )
            readers = sorted(c.name for c in scan.readers)
            self.assertEqual(readers, ["localIntOption", "qualifiedReader", "sharedReader"])

    def test_dropping_the_qualified_name_alternative_orphans_its_casts(self) -> None:
        """The first revert. A narrower declaration pattern, measured."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = self._tree(root)
            narrower = re.compile(r"\bval\b(?<!::val)")
            with self._rooted(root), mock.patch.object(CHECKER, "_VAL_PARAM", narrower):
                scan = CHECKER.Scan(tree)
            self.assertEqual(
                [site.receiver for site in scan.orphans], ['object[key]']
            )

    def test_a_fixed_body_window_makes_the_two_counts_disagree(self) -> None:
        """The second revert. A body measured short, caught by reconciliation."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = self._tree(root, padding=40)
            with self._rooted(root):
                balanced = CHECKER.Scan(tree)
                windowed = CHECKER.Scan(tree, body_window=200)
            self.assertEqual(balanced.reconcile(), [])
            disagreements = windowed.reconcile()
            self.assertEqual(len(disagreements), 1)
            self.assertIn("entryPoint", disagreements[0])

    def test_a_cast_pattern_matching_nothing_fails_the_self_check(self) -> None:
        """The mandatory one. Two empty sets agree perfectly."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = self._tree(root)
            dead = re.compile(r"\.as<\s*never_a_type\s*>\(\)")
            with self._rooted(root), mock.patch.object(CHECKER, "_CAST", dead):
                scan = CHECKER.Scan(tree)
            # Both checks go green on the empty population, which is exactly why
            # neither can stand in for the floor.
            self.assertEqual(scan.orphans, [])
            self.assertEqual(scan.reconcile(), [])
            floor = {"files": 1, "containers": 1, "narrowings": 1, "readers": 1}
            failures = CHECKER._self_check(scan, floor)
            self.assertEqual(len(failures), 2)
            self.assertTrue(any("narrowings" in line for line in failures))
            self.assertTrue(any("files" in line for line in failures))


class FailureClassTest(unittest.TestCase):
    """Each reported class, reverted one site at a time.

    A class asserted only through a clean run is asserted vacuously: a check
    that never fires and a check that cannot fire look identical from the
    outside.  The two populations are separate defects with separate remedies,
    so they are reverted separately -- a single revert that reddens both would
    leave either class free to be the one that did nothing.
    """

    SHARED = 'int sharedReader(val v) { return 1; }\n'
    LOCAL = """
#include <emscripten/val.h>
using emscripten::val;

// The file-local reader: a val in, a number out, outside the shared directory.
int localIntOption(val object, const char* key) {
  return object[key].as<int>();
}

val entryPoint(val options) {
  const int n = options["nFft"].as<int>();
  return val(n);
}
"""

    FLOOR = {"files": 1, "containers": 1, "narrowings": 1, "readers": 1}

    def _records(self, *, keep_reader: bool = True, keep_narrowing: bool = True) -> dict:
        data: dict = {"shapes": [], "narrowings": [], "readers": []}
        if keep_reader:
            data["readers"].append(
                {
                    "file": "bindings/domain/unit.cpp",
                    "symbol": "localIntOption",
                    "reason": "test fixture",
                }
            )
        if keep_narrowing:
            data["narrowings"].append(
                {
                    "file": "bindings/domain/unit.cpp",
                    "receiver": 'options["nFft"]',
                    "type": "int",
                    "reason": "test fixture",
                }
            )
        # The reader's own narrowing is recorded unconditionally, so reverting
        # the reader record cannot redden the narrowing class as a side effect.
        data["narrowings"].append(
            {
                "file": "bindings/domain/unit.cpp",
                "receiver": "object[key]",
                "type": "int",
                "reason": "test fixture",
            }
        )
        return data

    def _run(self, root: Path, records: dict) -> list[tuple[str, list[str]]]:
        wasm = root / "src" / "wasm" / "bindings"
        _write(wasm / "common" / "common.cpp", self.SHARED)
        _write(wasm / "domain" / "unit.cpp", self.LOCAL)
        with mock.patch.multiple(
            CHECKER,
            ROOT=root,
            WASM_TREE=root / "src" / "wasm",
            SHARED_READER_DIR=root / "src" / "wasm" / "bindings" / "common",
        ):
            scan = CHECKER.Scan(root / "src" / "wasm")
            return CHECKER.evaluate(scan, CHECKER.Records(records), self.FLOOR)

    def test_the_fixture_is_clean_with_both_records_in_place(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self._run(Path(tmp), self._records()), [])

    def test_reverting_the_reader_record_reports_exactly_that_reader(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            failures = self._run(Path(tmp), self._records(keep_reader=False))
            self.assertEqual(len(failures), 1, failures)
            heading, lines = failures[0]
            self.assertIn("file-local readers", heading)
            self.assertEqual(len(lines), 1)
            self.assertIn("localIntOption", lines[0])

    def test_reverting_the_narrowing_record_reports_exactly_that_narrowing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            failures = self._run(Path(tmp), self._records(keep_narrowing=False))
            self.assertEqual(len(failures), 1, failures)
            heading, lines = failures[0]
            self.assertIn("neither performed by the shared reader", heading)
            self.assertEqual(len(lines), 1)
            self.assertIn('options["nFft"]', lines[0])

    def test_marking_a_record_pending_reports_it(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            records = self._records()
            records["readers"][0]["triage"] = "pending"
            failures = self._run(Path(tmp), records)
            self.assertEqual(len(failures), 1, failures)
            heading, lines = failures[0]
            self.assertIn("without grading what it does", heading)
            self.assertEqual(len(lines), 1)
            self.assertIn("localIntOption", lines[0])


class RecordsTest(unittest.TestCase):
    def test_every_record_carries_a_reason(self) -> None:
        data = CHECKER.load_records()
        entries = data["shapes"] + data["narrowings"] + data["readers"]
        self.assertTrue(entries)
        for entry in entries:
            self.assertTrue(entry.get("reason", "").strip(), entry)

    def test_no_record_is_left_ungraded(self) -> None:
        records = CHECKER.Records(CHECKER.load_records())
        self.assertEqual(
            records.pending_count(),
            0,
            "a pending record states a site was enumerated and not graded, and "
            "the checker fails on one; a record added without a verdict belongs "
            "in the triage, not in the file",
        )


class RealTreeTest(unittest.TestCase):
    """The floor, pinned against the tree the checker actually guards."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.scan = CHECKER.Scan()
        cls.data = CHECKER.load_records()

    def test_the_population_is_the_size_it_is_pinned_for(self) -> None:
        self.assertEqual(CHECKER._self_check(self.scan, self.data["floor"]), [])

    def test_both_integer_and_size_narrowings_are_in_the_population(self) -> None:
        # A type list that lost an entry would shrink the population without
        # emptying it, which the floor alone could survive.
        types = {site.type for site in self.scan.sites}
        for name in ("int", "size_t", "uint32_t", "unsigned"):
            self.assertIn(name, types)

    def test_the_type_list_still_names_the_widths_the_tree_has_no_instance_of(self) -> None:
        # The tree currently narrows to none of these, so the population check
        # above cannot see them dropped from the list -- and the day one lands,
        # a list missing its spelling reports nothing at all.
        for name in ("int8_t", "uint8_t", "int16_t", "uint16_t", "int64_t", "uint64_t"):
            self.assertIn(name, CHECKER.INTEGER_TYPES)

    def test_val_taking_lambdas_are_in_the_container_population(self) -> None:
        # The tree writes val-taking lambdas, and a scan shaped only for named
        # signatures finds none of them -- which would make any narrowing that
        # later lands inside one an orphan the record file cannot key on.
        # Attribution itself is measured on the synthetic tree; no lambda in
        # this tree currently narrows, so only the container half is pinned
        # here rather than asserting a subject that is not there.
        self.assertTrue([c for c in self.scan.containers if c.is_lambda])

    def test_the_tree_is_clean(self) -> None:
        self.assertEqual(self.scan.orphans, [])
        self.assertEqual(self.scan.reconcile(), [])


if __name__ == "__main__":
    unittest.main()
