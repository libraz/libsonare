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


class PositionalFailureClassTest(unittest.TestCase):
    """Each positional class, reverted one at a time on a scratch tree.

    Separately, and in both directions: a parameter that appears must be
    reported, a record that stops matching must be reported, and a parameter the
    record does cover must not be.  One mutation that reddens everything would
    leave any of the three free to be the check that never fires.
    """

    SOURCE = """
#include <emscripten/val.h>
using emscripten::val;

val setStripGain(unsigned int strip_index, float gain_db) {
  return val(strip_index);
}

val analyze(val samples, int sample_rate) {
  return val(sample_rate);
}

EMSCRIPTEN_BINDINGS(unit) {
  function("setStripGain", &setStripGain);
  function("analyze", &analyze);
}
"""

    FLOOR = {"registrations": 1, "declarations": 1, "parameters": 1, "functions": 1, "files": 1}

    def _records(self, **overrides) -> dict:
        shapes = [
            {
                "name": "slot_index",
                "parameter_pattern": "(?:^|_)index$",
                "count": 1,
                "status": "open",
                "defect": "wraps",
                "selects": "a real strip",
                "reason": "test fixture",
            },
            {
                "name": "extent_or_size",
                "parameter_pattern": ".",
                "count": 1,
                "status": "open",
                "defect": "wraps",
                "selects": "a different analysis",
                "reason": "test fixture",
            },
        ]
        section = {"floor": self.FLOOR, "shapes": shapes, "parameters": [], "constructors": []}
        section.update(overrides)
        return {"positional": section}

    def _run(self, root: Path, records: dict, source: str | None = None):
        tree = root / "src" / "wasm"
        _write(tree / "bindings" / "domain" / "unit.cpp", source or self.SOURCE)
        with mock.patch.multiple(CHECKER, ROOT=root, WASM_TREE=tree):
            scan = CHECKER.PositionalScan(tree)
            return scan, CHECKER.evaluate_positional(scan, CHECKER.PositionalRecords(records))

    def test_the_fixture_is_clean_with_both_shapes_in_place(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            scan, failures = self._run(Path(tmp), self._records())
            self.assertEqual(len(scan.route_r), 2)
            self.assertEqual(failures, [])

    def test_a_new_positional_parameter_is_reported(self) -> None:
        """Direction one. A registration grows a narrow parameter."""
        grown = self.SOURCE.replace(
            "val analyze(val samples, int sample_rate) {",
            "val analyze(val samples, int sample_rate, int n_fft) {",
        )
        with tempfile.TemporaryDirectory() as tmp:
            _, failures = self._run(Path(tmp), self._records(), grown)
            headings = [heading for heading, _ in failures]
            self.assertEqual(len(failures), 1, failures)
            self.assertIn("no longer cover the population they record", headings[0])
            self.assertEqual(failures[0][1], ["  extent_or_size: recorded 1, found 2"])

    def test_a_parameter_no_shape_matches_is_reported(self) -> None:
        """Direction one again, where the residue shape is not there to absorb it."""
        with tempfile.TemporaryDirectory() as tmp:
            records = self._records()
            records["positional"]["shapes"] = [records["positional"]["shapes"][0]]
            _, failures = self._run(Path(tmp), records)
            self.assertEqual(len(failures), 1, failures)
            heading, lines = failures[0]
            self.assertIn("not recorded", heading)
            self.assertEqual(len(lines), 1)
            self.assertIn("analyze(sample_rate: int)", lines[0])

    def test_a_record_matching_nothing_is_reported(self) -> None:
        """Direction two. A shape whose population is gone."""
        with tempfile.TemporaryDirectory() as tmp:
            records = self._records()
            records["positional"]["shapes"].insert(
                0,
                {
                    "name": "entity_handle",
                    "parameter_pattern": "(?:^|_)id$",
                    "count": 3,
                    "status": "open",
                    "defect": "wraps",
                    "selects": "an existing track",
                    "reason": "test fixture for a record that suppresses nothing",
                },
            )
            _, failures = self._run(Path(tmp), records)
            headings = [heading for heading, _ in failures]
            self.assertEqual(len(failures), 2, failures)
            self.assertIn("no longer cover the population they record", headings[0])
            self.assertEqual(failures[0][1], ["  entity_handle: recorded 3, found 0"])
            self.assertIn("matched nothing", headings[1])
            self.assertEqual(failures[1][1], ["  shape:entity_handle"])

    def test_a_recorded_parameter_is_not_reported(self) -> None:
        """Direction three. The one that makes the other two mean something."""
        with tempfile.TemporaryDirectory() as tmp:
            scan, failures = self._run(Path(tmp), self._records())
            self.assertEqual(
                sorted(p.name for p in scan.route_r), ["sample_rate", "strip_index"]
            )
            self.assertEqual(failures, [])

    def test_a_record_without_a_verdict_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            records = self._records()
            records["positional"]["shapes"][0]["status"] = "pending"
            _, failures = self._run(Path(tmp), records)
            self.assertEqual(len(failures), 1, failures)
            heading, lines = failures[0]
            self.assertIn("carry no verdict", heading)
            self.assertEqual(lines, ["  slot_index: status 'pending'"])

    def test_a_float_named_like_a_count_is_reported(self) -> None:
        """The float boundary's trip-wire, which nothing in the tree trips today."""
        counted = self.SOURCE.replace("float gain_db", "float n_frames")
        with tempfile.TemporaryDirectory() as tmp:
            _, failures = self._run(Path(tmp), self._records(), counted)
            self.assertEqual(len(failures), 1, failures)
            heading, lines = failures[0]
            self.assertIn("named like a count or an index", heading)
            self.assertEqual(len(lines), 1)
            self.assertIn("setStripGain(n_frames: float)", lines[0])

    def test_an_unresolvable_registration_is_reported(self) -> None:
        orphaned = self.SOURCE.replace("&analyze)", "&analyzeElsewhere)")
        with tempfile.TemporaryDirectory() as tmp:
            records = self._records()
            records["positional"]["shapes"][1]["count"] = 0
            _, failures = self._run(Path(tmp), records, orphaned)
            headings = [heading for heading, _ in failures]
            self.assertIn("bind a symbol no declaration in the tree matches", headings[0])
            self.assertEqual(len(failures[0][1]), 1)
            self.assertIn("analyzeElsewhere", failures[0][1][0])

    def test_an_empty_population_fails_the_floor(self) -> None:
        """The mandatory one. Two empty sets agree perfectly."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = root / "src" / "wasm"
            _write(tree / "bindings" / "domain" / "unit.cpp", "// nothing here\n")
            with mock.patch.multiple(CHECKER, ROOT=root, WASM_TREE=tree):
                scan = CHECKER.PositionalScan(tree)
            # Both cross-checks go green on the empty population, which is
            # exactly why neither can stand in for the floor.
            self.assertEqual(scan.unresolved, [])
            self.assertEqual(scan.reconcile(), [])
            failures = CHECKER._positional_self_check(scan, self.FLOOR)
            self.assertEqual(len(failures), 5)


class PositionalVerdictCostTest(unittest.TestCase):
    """That `benign` costs more than `open`, measured one rule at a time.

    The two verdicts mean opposite things, so they must not be one word apart.
    Each rule below is reverted on its own and required to fire alone: a rule
    asserted only through a clean run is asserted vacuously, and a mutation that
    reddens several classes leaves any of them free to be the one that never
    fires.
    """

    SOURCE = """
#include <emscripten/val.h>
using emscripten::val;

void seek(int64_t timeline_sample) {}
void setStripGain(unsigned int strip_index, float gain_db) {}

EMSCRIPTEN_BINDINGS(unit) {
  function("seek", &seek);
  function("setStripGain", &setStripGain);
}
"""

    FLOOR = {"registrations": 1, "declarations": 1, "parameters": 1, "functions": 1, "files": 1}
    QUOTE = "the mechanism, readable here"

    def _records(self) -> dict:
        return {
            "positional": {
                "floor": self.FLOOR,
                "shapes": [
                    {
                        "name": "sample_position",
                        "types": ["int64_t"],
                        "parameter_pattern": ".",
                        "count": 1,
                        "status": "benign",
                        "defect": "refuses",
                        "mechanism": [{"file": "glue.js", "contains": self.QUOTE}],
                        "reason": "test fixture",
                    },
                    {
                        "name": "slot_index",
                        "parameter_pattern": ".",
                        "count": 1,
                        "status": "open",
                        "defect": "wraps",
                        "selects": "a real strip",
                        "reason": "test fixture",
                    },
                ],
                "parameters": [],
                "constructors": [],
            }
        }

    def _run(self, root: Path, records: dict):
        tree = root / "src" / "wasm"
        _write(tree / "bindings" / "domain" / "unit.cpp", self.SOURCE)
        _write(root / "glue.js", f"// {self.QUOTE}\n")
        with mock.patch.multiple(CHECKER, ROOT=root, WASM_TREE=tree):
            scan = CHECKER.PositionalScan(tree)
            return CHECKER.evaluate_positional(
                scan, CHECKER.PositionalRecords(records, root=root)
            )

    def _only(self, mutate) -> tuple[str, list[str]]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            records = self._records()
            mutate(records["positional"])
            failures = self._run(root, records)
            self.assertEqual(len(failures), 1, failures)
            return failures[0]

    def test_the_fixture_is_clean(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self._run(Path(tmp), self._records()), [])

    def test_a_record_without_a_reason_is_reported(self) -> None:
        heading, lines = self._only(lambda p: p["shapes"][0].pop("reason"))
        self.assertIn("carry no reason", heading)
        self.assertEqual(lines, ["  sample_position"])

    def test_a_record_whose_reason_is_blank_is_reported(self) -> None:
        heading, lines = self._only(lambda p: p["shapes"][0].update(reason="   "))
        self.assertIn("carry no reason", heading)
        self.assertEqual(lines, ["  sample_position"])

    def test_an_open_record_without_selects_is_reported(self) -> None:
        heading, lines = self._only(lambda p: p["shapes"][1].pop("selects"))
        self.assertIn("without saying what a wrapped value selects", heading)
        self.assertEqual(lines, ["  slot_index"])

    def test_a_benign_record_without_a_mechanism_is_reported(self) -> None:
        heading, lines = self._only(lambda p: p["shapes"][0].pop("mechanism"))
        self.assertIn("mechanism that cannot be read", heading)
        self.assertEqual(lines, ["  sample_position: benign with no mechanism citation"])

    def test_a_mechanism_quote_that_is_not_there_is_reported(self) -> None:
        heading, lines = self._only(
            lambda p: p["shapes"][0]["mechanism"][0].update(contains="not in that file")
        )
        self.assertIn("mechanism that cannot be read", heading)
        self.assertIn("does not contain", lines[0])

    def test_a_mechanism_citing_a_missing_tracked_file_is_reported(self) -> None:
        heading, lines = self._only(
            lambda p: p["shapes"][0]["mechanism"][0].update(file="nowhere.js")
        )
        self.assertIn("mechanism that cannot be read", heading)
        self.assertIn("no such file, and it is not marked built", lines[0])

    def test_a_missing_file_marked_built_is_not_reported(self) -> None:
        # The embind glue is the build's output and no committed file carries
        # it, so `built` has to excuse an absent artifact -- and only that. The
        # test above pins that it does not excuse a wrong quote.
        with tempfile.TemporaryDirectory() as tmp:
            records = self._records()
            records["positional"]["shapes"][0]["mechanism"][0] = {
                "file": "dist/never-built.js",
                "built": True,
                "contains": "anything at all",
            }
            self.assertEqual(self._run(Path(tmp), records), [])

    def test_flipping_an_open_record_to_benign_is_reported(self) -> None:
        """The one that matters. A citation is pasteable; a declared type is not."""

        def flip(positional: dict) -> None:
            shape = positional["shapes"][1]
            shape["status"] = "benign"
            shape["defect"] = "refuses"
            shape["selects"] = ""
            # The realistic attack: a citation that does resolve and does match.
            shape["mechanism"] = [{"file": "glue.js", "contains": self.QUOTE}]

        heading, lines = self._only(flip)
        self.assertIn("state a conversion rule their parameters' types do not have", heading)
        # Both halves of the derivation object, and neither is reachable from
        # this file: the type says what the rule is, and the rule says which
        # verdict is available.
        self.assertEqual(
            lines,
            [
                "  slot_index: benign is only reachable where the conversion "
                "refuses, and ['unsigned int'] wraps",
                "  slot_index: states defect 'refuses', but ['unsigned int'] is "
                "converted by the 'wraps' rule",
            ],
        )

    def test_relabelling_the_defect_is_reported(self) -> None:
        heading, lines = self._only(lambda p: p["shapes"][1].update(defect="refuses"))
        self.assertIn("state a conversion rule their parameters' types do not have", heading)
        self.assertIn("is converted by the 'wraps' rule", lines[0])

    def test_a_shape_covering_two_conversion_rules_is_reported(self) -> None:
        def merge(positional: dict) -> None:
            positional["shapes"] = [
                {
                    "name": "everything",
                    "parameter_pattern": ".",
                    "count": 2,
                    "status": "open",
                    "defect": "wraps",
                    "selects": "something",
                    "reason": "test fixture",
                }
            ]

        heading, lines = self._only(merge)
        self.assertIn("state a conversion rule", heading)
        self.assertIn("more than one rule", lines[0])

    def test_the_derivation_is_the_one_embind_uses(self) -> None:
        # Asserted directly as well, because every rule above reads through it.
        self.assertEqual(CHECKER.derive_defect({"uint32_t", "int", "size_t"}), "wraps")
        self.assertEqual(CHECKER.derive_defect({"int64_t", "uint64_t"}), "refuses")
        self.assertEqual(CHECKER.derive_defect({"float"}), "saturates")
        self.assertIsNone(CHECKER.derive_defect({"int", "int64_t"}))


class PositionalRouteTest(unittest.TestCase):
    """That the two routes can disagree, measured rather than argued."""

    SOURCE = """
#include <emscripten/val.h>
using emscripten::val;

namespace other {
void reset(int generation) {}
}  // namespace other

struct Engine {
  void reset(val options);
};

void Engine::reset(val options) {}

EMSCRIPTEN_BINDINGS(unit) {
  function("reset", &Engine::reset);
}
"""

    def _scan(self, root: Path, **kwargs):
        tree = root / "src" / "wasm"
        _write(tree / "bindings" / "domain" / "unit.cpp", self.SOURCE)
        with mock.patch.multiple(CHECKER, ROOT=root, WASM_TREE=tree):
            return CHECKER.PositionalScan(tree, **kwargs)

    def test_qualified_resolution_finds_the_right_overload(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            scan = self._scan(Path(tmp))
            self.assertEqual(scan.conflicting, [])
            self.assertEqual(scan.route_r, [])
            self.assertEqual(scan.reconcile(), [])

    def test_resolving_on_the_short_name_alone_is_caught(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            scan = self._scan(Path(tmp), short_name_only=True)
            self.assertEqual(len(scan.conflicting), 1)
            registration, candidates = scan.conflicting[0]
            self.assertEqual(registration.js_name, "reset")
            # The method's header declaration and its definition are both here,
            # and they agree; the free function is the one that does not.
            self.assertEqual(
                sorted({d.signature for d in candidates}),
                ["Engine::reset(val)", "reset(int)"],
            )


class RecordsTest(unittest.TestCase):
    def test_every_record_carries_a_reason(self) -> None:
        data = CHECKER.load_records()
        entries = data["shapes"] + data["narrowings"] + data["readers"]
        entries += data["positional"]["shapes"] + data["positional"]["constructors"]
        entries += data["positional"]["parameters"]
        self.assertTrue(entries)
        for entry in entries:
            self.assertTrue(entry.get("reason", "").strip(), entry)

    def test_every_open_positional_record_says_what_a_wrapped_value_selects(self) -> None:
        # `open` is a claim about consequence, not a shrug. A record that made it
        # without naming the thing a wrapped value picks would be `pending` under
        # another name, which the checker refuses.
        data = CHECKER.load_records()["positional"]
        for entry in data["shapes"] + data["constructors"] + data["parameters"]:
            if entry.get("status") == "open":
                self.assertTrue(entry.get("selects", "").strip(), entry)

    def test_no_positional_record_is_left_ungraded(self) -> None:
        records = CHECKER.PositionalRecords(CHECKER.load_records())
        self.assertEqual(records.ungraded(), [])
        self.assertEqual(records.missing_reasons(), [])
        self.assertEqual(records.unsupported_open(), [])
        self.assertEqual(records.unsupported_benign(), [])

    def test_the_only_benign_positional_record_is_the_one_that_refuses(self) -> None:
        # The asymmetry, asserted on the shipping records rather than only on a
        # fixture: every other verdict in the file is open, and the derivation
        # is what keeps it that way.
        data = CHECKER.load_records()["positional"]
        benign = [
            entry
            for entry in data["shapes"] + data["constructors"] + data["parameters"]
            if entry.get("status") == "benign"
        ]
        self.assertEqual([entry["name"] for entry in benign], ["sixty_four_bit"])
        self.assertEqual(benign[0]["defect"], "refuses")
        self.assertTrue(benign[0]["mechanism"])

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


class RealTreePositionalTest(unittest.TestCase):
    """The positional population, against the tree the checker actually guards."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.scan = CHECKER.PositionalScan()
        cls.records = CHECKER.PositionalRecords(CHECKER.load_records())

    def test_the_population_is_the_size_it_is_pinned_for(self) -> None:
        self.assertEqual(CHECKER._positional_self_check(self.scan, self.records.floor), [])

    def test_every_registration_resolves_to_one_signature(self) -> None:
        self.assertEqual(self.scan.unresolved, [])
        self.assertEqual(self.scan.conflicting, [])

    def test_the_two_routes_agree(self) -> None:
        self.assertEqual(self.scan.reconcile(), [])
        self.assertEqual(len(self.scan.route_r), len(self.scan.route_d))

    def test_the_widths_the_tree_has_no_instance_of_are_still_named(self) -> None:
        # The tree passes none of these positionally today, so the population
        # check cannot see one dropped -- and the day one lands, a list missing
        # its spelling reports nothing at all.
        for name in ("int8_t", "uint8_t", "int16_t", "uint16_t", "uint64_t"):
            self.assertIn(name, CHECKER.POSITIONAL_TYPES)

    def test_no_float_parameter_is_named_like_a_count(self) -> None:
        # The float exclusion rests on this, and only this half of it can stop
        # being true without anyone touching the checker.
        named = [p.name for p in self.scan.float_parameters if CHECKER._COUNT_SHAPED.search(p.name)]
        self.assertEqual(named, [])
        self.assertTrue(self.scan.float_parameters, "the float population is not empty")


if __name__ == "__main__":
    unittest.main()
