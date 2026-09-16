"""Self-tests for the analysis result surface check.

The check reports a clean repository, so what has to be established is that it
could report otherwise. Two properties are easy to get wrong here and both fail
silently towards "nothing found": anchoring the walk on the file rather than on
the root interface, which lets a leaf declared by an unrelated interface satisfy
a path the root does not carry -- `bpm` is declared four times in one of these
files -- and dropping an array segment's `[]` in a way that loses the segment
instead of the brackets.
"""

from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_analysis_result_surfaces.py"
SPEC = importlib.util.spec_from_file_location(
    "libsonare_analysis_surface_checker", CHECKER_PATH
)
assert SPEC is not None and SPEC.loader is not None
check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check)


def _root_body(text: str) -> str:
    return check.walk.named_type_bodies(text, check.ROOT_INTERFACE)[0]


def _drop_property(text: str, prop: str) -> str:
    """Remove one property from the ROOT interface only, leaving siblings alone."""
    body = _root_body(text)
    stripped = re.sub(r"\n\s*" + prop + r"\s*\??\s*:[^;\n]*;", "\n", body, count=1)
    return text.replace(body, stripped, 1)


class AnalysisResultSurfaces(unittest.TestCase):
    def test_the_repository_declares_every_schema_path_on_both_surfaces(self):
        source = check.SCHEMA_SOURCE.read_text()
        # Both published lists, so covering one and reporting clean is not a pass.
        self.assertEqual(len(check.SCHEMAS), 2)
        for accessor, root in check.SCHEMAS.items():
            paths = check.schema_paths(source, accessor)
            self.assertGreater(len(paths), 10, f"{accessor} stopped parsing")
            for _, ts_path in check.TS_SURFACES.items():
                missing, unreached, comparisons = check.scan(
                    paths, ts_path.read_text(), root
                )
                self.assertEqual(unreached, [], accessor)
                self.assertEqual(missing, [], accessor)
                self.assertEqual(comparisons, len(paths))

    def test_the_two_lists_are_distinct_populations(self):
        """A reader that returned the same list twice would pass the case above."""
        source = check.SCHEMA_SOURCE.read_text()
        analysis = check.schema_paths(source, "analysis_result_schema_paths")
        meter = check.schema_paths(source, "meter_result_schema_paths")
        self.assertNotEqual(analysis, meter)
        self.assertGreater(len(analysis), len(meter))

    def test_a_top_level_path_missing_from_the_root_is_reported(self):
        """The case a file-anchored walk accepts: the leaf exists elsewhere."""
        text = check.TS_SURFACES["node"].read_text()
        self.assertGreater(len(re.findall(r"^\s*bpm\s*:", text, re.M)), 1)
        missing, unreached, _ = check.scan(["bpm"], _drop_property(text, "bpm"))
        self.assertEqual(unreached, [])
        self.assertEqual(missing, ["bpm"])

    def test_a_nested_path_under_an_array_element_is_reported(self):
        """`a[].b` names a property of the element type, not a property `a[]`."""
        surface = """
        export interface Candidate { value: number }
        export interface AnalysisResult { bpmCandidates: Candidate[] }
        """
        missing, unreached, comparisons = check.scan(
            ["bpmCandidates[].value", "bpmCandidates[].relation"], surface
        )
        self.assertEqual(unreached, [])
        self.assertEqual(comparisons, 2)
        self.assertEqual(missing, ["bpmCandidates[].relation"])

    def test_two_missing_paths_are_both_reported(self):
        surface = "export interface AnalysisResult { bpm: number }"
        missing, _, _ = check.scan(["bpm", "key", "tempo"], surface)
        self.assertEqual(missing, ["key", "tempo"])

    def test_an_unreachable_block_fails_rather_than_passing_vacuously(self):
        surface = "export interface AnalysisResult { bpm: number }"
        missing, unreached, comparisons = check.scan(["melody.contour"], surface)
        self.assertEqual(missing, [])
        self.assertEqual(comparisons, 0)
        self.assertEqual(unreached, ["melody.contour"])

    def test_a_missing_root_interface_fails_rather_than_reporting_clean(self):
        missing, unreached, comparisons = check.scan(
            ["bpm"], "export interface Other { bpm: number }"
        )
        self.assertEqual(comparisons, 0)
        self.assertEqual(missing, [])
        self.assertEqual(len(unreached), 1)

    def test_the_path_reader_returns_nothing_when_the_list_moves(self):
        self.assertEqual(check.schema_paths("int unrelated() { return 0; }"), [])
        self.assertEqual(
            check.schema_paths(check.SCHEMA_SOURCE.read_text(), "no_such_accessor"), []
        )


if __name__ == "__main__":
    unittest.main()
