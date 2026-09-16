"""Self-tests for the result schema surface check.

The check reports a clean repository across every published list, so what has to
be established is that it could report otherwise -- and that it reached what it
counted. Three properties are easy to get wrong here and all three fail silently
towards "nothing found": anchoring the walk on the file rather than on the root
type, which lets a leaf declared by an unrelated interface satisfy a path the
root does not carry (`bpm` is declared four times in one of these files);
dropping an array segment's brackets in a way that loses the segment instead;
and reading a path list that computes some of its entries, which yields a short
list and a clean verdict over the part it managed to parse.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_result_schema_surfaces as check  # noqa: E402

ELEMENT_SURFACE = """
export interface Entry { name: string; id: number }
export interface Wrapper { entries: Entry[] }
"""


def _drop_property(text: str, root: str, prop: str) -> str:
    """Remove one property from `root` only, leaving same-named siblings alone."""
    body = check.walk.named_type_bodies(text, root)[0]
    stripped = re.sub(r"\n\s*" + prop + r"\s*\??\s*:[^;\n]*;", "\n", body, count=1)
    return text.replace(body, stripped, 1)


class TheRepository(unittest.TestCase):
    def test_every_family_is_clean_on_both_surfaces(self):
        for accessor, family in check.FAMILIES.items():
            paths = check.schema_paths(family["source"].read_text(), accessor)
            self.assertIsNotNone(paths, accessor)
            self.assertGreater(len(paths), 5, accessor)
            for side, path in family["surfaces"].items():
                missing, unreached, comparisons = check.scan(
                    paths, path.read_text(), family["root"]
                )
                self.assertEqual(unreached, [], f"{accessor} [{side}]")
                self.assertEqual(missing, [], f"{accessor} [{side}]")
                self.assertEqual(comparisons, len(paths), f"{accessor} [{side}]")

    def test_the_families_are_distinct_populations(self):
        """A table whose entries resolved to one list would pass the case above."""
        lists = {
            accessor: tuple(check.schema_paths(family["source"].read_text(), accessor))
            for accessor, family in check.FAMILIES.items()
        }
        self.assertEqual(len(set(lists.values())), len(check.FAMILIES))

    def test_both_surfaces_are_checked_for_every_family(self):
        for accessor, family in check.FAMILIES.items():
            self.assertEqual(sorted(family["surfaces"]), ["node", "wasm"], accessor)


class RootAnchoring(unittest.TestCase):
    def test_a_top_level_path_missing_from_the_root_is_reported(self):
        """The case a file-anchored walk accepts: the leaf exists elsewhere."""
        family = check.FAMILIES["analysis_result_schema_paths"]
        text = family["surfaces"]["node"].read_text()
        self.assertGreater(len(re.findall(r"^\s*bpm\s*:", text, re.M)), 1)
        missing, unreached, _ = check.scan(
            ["bpm"], _drop_property(text, family["root"], "bpm"), family["root"]
        )
        self.assertEqual(unreached, [])
        self.assertEqual(missing, ["bpm"])

    def test_a_missing_root_type_fails_rather_than_reporting_clean(self):
        missing, unreached, comparisons = check.scan(
            ["bpm"], "export interface Other { bpm: number }", "AnalysisResult"
        )
        self.assertEqual(comparisons, 0)
        self.assertEqual(missing, [])
        self.assertEqual(unreached, ["bpm"])

    def test_an_unreachable_block_fails_rather_than_passing_vacuously(self):
        surface = "export interface Root { bpm: number }"
        missing, unreached, comparisons = check.scan(
            ["melody.contour"], surface, "Root"
        )
        self.assertEqual(missing, [])
        self.assertEqual(comparisons, 0)
        self.assertEqual(unreached, ["melody.contour"])

    def test_two_missing_paths_are_both_reported(self):
        surface = "export interface Root { bpm: number }"
        missing, _, _ = check.scan(["bpm", "key", "tempo"], surface, "Root")
        self.assertEqual(missing, ["key", "tempo"])


class ArraySegments(unittest.TestCase):
    def test_a_nested_path_under_an_array_element_is_reported(self):
        """`a[].b` names a property of the element type, not a property `a[]`."""
        missing, unreached, comparisons = check.scan(
            ["entries[].name", "entries[].missing"], ELEMENT_SURFACE, "Wrapper"
        )
        self.assertEqual(unreached, [])
        self.assertEqual(comparisons, 2)
        self.assertEqual(missing, ["entries[].missing"])

    def test_a_root_array_path_resolves_against_the_element_type(self):
        """A root-array writer roots its paths at the element, so `[]` leads."""
        missing, unreached, comparisons = check.scan(
            ["[].name", "[].id", "[].missing"], ELEMENT_SURFACE, "Entry"
        )
        self.assertEqual(unreached, [])
        self.assertEqual(comparisons, 3)
        self.assertEqual(missing, ["[].missing"])

    def test_an_opaque_record_field_is_not_counted_as_compared(self):
        """The shape this check was extended to catch: a surface described a
        nested document as an open bag, so every path below it was undecidable
        rather than absent -- and silence there reads exactly like agreement."""
        surface = """
        export interface Inner { id: string }
        export interface Opaque { scene: Record<string, unknown>; tracks: Inner[] }
        """
        missing, unreached, comparisons = check.scan(
            ["scene.version", "tracks[].id"], surface, "Opaque"
        )
        self.assertEqual(missing, [])
        self.assertEqual(unreached, ["scene.version"])
        self.assertEqual(comparisons, 1)

    def test_a_path_that_is_only_brackets_is_not_counted_as_compared(self):
        missing, unreached, comparisons = check.scan(["[]"], ELEMENT_SURFACE, "Entry")
        self.assertEqual(comparisons, 0)
        self.assertEqual(missing, [])
        self.assertEqual(unreached, ["[]"])


class TheListReader(unittest.TestCase):
    def test_a_literal_list_is_read_whole(self):
        source = 'x_schema_paths() {\n  static const std::vector<std::string> paths = {\n      "a",\n      "b.c",\n  };\n  return paths;\n}'
        self.assertEqual(check.schema_paths(source, "x_schema_paths"), ["a", "b.c"])

    def test_a_computed_list_is_refused_rather_than_read_partially(self):
        """The failure this rejects reads a short list and then reports clean."""
        source = (
            "x_schema_paths() {\n"
            "  static const std::vector<std::string> paths = {\n"
            '      "a",\n'
            "      prefix + suffix,\n"
            "  };\n"
            "  return paths;\n}"
        )
        self.assertIsNone(check.schema_paths(source, "x_schema_paths"))

    def test_a_moved_or_renamed_list_reads_as_none_not_as_empty(self):
        self.assertIsNone(
            check.schema_paths("int unrelated() { return 0; }", "x_schema_paths")
        )
        for accessor, family in check.FAMILIES.items():
            source = family["source"].read_text()
            self.assertIsNone(
                check.schema_paths(source, accessor + "_no_such"), accessor
            )


if __name__ == "__main__":
    unittest.main()
