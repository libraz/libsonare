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

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_result_schema_surfaces as check

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
    def test_every_family_is_clean_on_every_surface(self):
        for label, family in check.FAMILIES.items():
            paths = check.schema_paths(family["source"].read_text(), label)
            self.assertIsNotNone(paths, label)
            self.assertGreater(len(paths), 0, label)
            paths, problems = check.delegate(paths, family.get("delegated", {}))
            self.assertEqual(problems, [], label)
            for side, path in family["surfaces"].items():
                missing, unreached, comparisons = check.scan_surface(
                    paths, path, family["root"]
                )
                self.assertEqual(unreached, [], f"{label} [{side}]")
                self.assertEqual(missing, [], f"{label} [{side}]")
                self.assertEqual(comparisons, len(paths), f"{label} [{side}]")

    def test_the_lists_are_distinct_populations(self):
        """A table whose entries resolved to one list would pass the case above."""
        lists = {
            label: tuple(check.schema_paths(family["source"].read_text(), label))
            for label, family in check.FAMILIES.items()
        }
        self.assertEqual(len(set(lists.values())), len(lists))

    def test_python_is_checked_for_the_explainable_mastering_results(self):
        """Each of these results is checked on Python, not only in TypeScript."""
        for label in (
            "audio_profile_schema_paths",
            "assistant_result_schema_paths",
            "streaming_preview_schema_paths",
            "match_reference_loudness_schema_paths",
            "stereo_mono_compat_schema_paths",
        ):
            self.assertIn("python", check.FAMILIES[label]["surfaces"])

    def test_both_surfaces_are_checked_for_every_family(self):
        """A family that lost a surface stays clean on the one it kept.

        Stated as a floor rather than an exact set so a family can carry an
        extra declaration of the same shape -- a shipped JSON Schema is one --
        without the addition reading as the loss this case exists to catch.
        """
        for label, family in check.FAMILIES.items():
            surfaces = family["surfaces"]
            self.assertLessEqual({"node", "wasm"}, set(surfaces), label)
            for side, path in surfaces.items():
                self.assertTrue(path.is_file(), f"{label} [{side}]: {path}")


class DelegatedSubtrees(unittest.TestCase):
    OWNER = Path(__file__)
    PATHS: ClassVar[list[str]] = [
        "profile",
        "chainConfig",
        "chainConfig.version",
        "chainConfig.params",
    ]

    def test_a_delegated_subtree_leaves_the_comparison_but_its_root_stays(self):
        kept, problems = check.delegate(self.PATHS, {"chainConfig": self.OWNER})
        self.assertEqual(problems, [])
        self.assertEqual(kept, ["profile", "chainConfig"])

    def test_a_delegation_whose_owner_check_is_absent_fails(self):
        absent = self.OWNER.with_name("no_such.py")
        _, problems = check.delegate(self.PATHS, {"chainConfig": absent})
        self.assertEqual(len(problems), 1)
        self.assertIn("no_such.py", problems[0])

    def test_a_delegation_naming_no_subtree_of_the_list_fails(self):
        _, problems = check.delegate(self.PATHS, {"profile": self.OWNER})
        self.assertEqual(len(problems), 1)
        self.assertIn("profile", problems[0])

    def test_the_assistant_chain_config_is_delegated_to_the_parameter_check(self):
        family = check.FAMILIES["assistant_result_schema_paths"]
        owner = family["delegated"]["chainConfig"]
        self.assertEqual(owner.name, "check_mastering_param_surfaces.py")


class RootAnchoring(unittest.TestCase):
    def test_a_top_level_path_missing_from_the_root_is_reported(self):
        """The case a file-anchored walk accepts: the leaf exists elsewhere."""
        family = check.FAMILIES["analysis_result_schema_paths"]
        text = family["surfaces"]["node"].read_text()
        self.assertGreater(len(re.findall(r"^\s*bpm\s*\??\s*:", text, re.MULTILINE)), 1)
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

    def test_an_element_type_imported_from_a_sibling_module_is_reached(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "shared.ts").write_text("export interface Entry { name: string; id: number }\n")
            surface = root / "surface.ts"
            surface.write_text(
                "import type { Entry } from './shared.js';\n"
                "export interface Wrapper { entries: Entry[] }\n"
            )
            paths = ["entries[].name", "entries[].id", "entries[].missing"]
            missing, unreached, comparisons = check.scan_surface(paths, surface, "Wrapper")
            self.assertEqual(unreached, [])
            self.assertEqual(comparisons, 3)
            self.assertEqual(missing, ["entries[].missing"])

    def test_an_element_type_reached_through_a_barrel_is_reached(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "shared.ts").write_text("export interface Entry { name: string }\n")
            (root / "types.ts").write_text("export * from './shared.js';\n")
            surface = root / "surface.ts"
            surface.write_text(
                "import type { Entry } from './types.js';\n"
                "export interface Wrapper { entries: Entry[] }\n"
            )
            missing, unreached, _ = check.scan_surface(["entries[].name"], surface, "Wrapper")
            self.assertEqual((missing, unreached), ([], []))

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


class JsonSchemaSurface(unittest.TestCase):
    """The JSON Schema walk, held to the same failures the TypeScript one is.

    Each case starts from the schema actually shipped and breaks exactly one
    thing, so a clean control and a red mutant differ by that one thing and
    nothing else.
    """

    ROOT: ClassVar[str] = "MixSceneDocument"
    PATHS: ClassVar[list[str]] = ["version", "strips[].sends[].sendDb", "strips[].sends"]

    def setUp(self):
        self.schema = json.loads(
            (check.REPO_ROOT / "schemas/mixer-scene.schema.json").read_text()
        )

    def scan(self, schema):
        return check.scan_json_schema(self.PATHS, json.dumps(schema), self.ROOT)

    def test_the_shipped_schema_is_the_control(self):
        missing, unreached, comparisons = self.scan(self.schema)
        self.assertEqual((missing, unreached), ([], []))
        self.assertEqual(comparisons, len(self.PATHS))

    def test_a_removed_leaf_is_reported_as_missing(self):
        del self.schema["$defs"]["send"]["properties"]["sendDb"]
        missing, unreached, _ = self.scan(self.schema)
        self.assertEqual(missing, ["strips[].sends[].sendDb"])
        self.assertEqual(unreached, [])

    def test_a_removed_parent_takes_its_children_out_of_the_comparison(self):
        del self.schema["$defs"]["strip"]["properties"]["sends"]
        missing, unreached, comparisons = self.scan(self.schema)
        self.assertEqual(missing, ["strips[].sends"])
        self.assertEqual(unreached, ["strips[].sends[].sendDb"])
        self.assertEqual(comparisons, 2)

    def test_an_array_turned_scalar_is_caught_rather_than_flattened(self):
        """The one failure the TypeScript walk cannot see, which is why `[]`
        steps through `items` here instead of being stripped."""
        self.schema["$defs"]["strip"]["properties"]["sends"] = {"type": "string"}
        missing, unreached, _ = self.scan(self.schema)
        self.assertEqual(missing, [])
        self.assertEqual(unreached, ["strips[].sends[].sendDb"])

    def test_a_wrong_title_fails_rather_than_reporting_clean(self):
        self.schema["title"] = "SomethingElse"
        missing, unreached, comparisons = self.scan(self.schema)
        self.assertEqual(comparisons, 0)
        self.assertEqual(unreached, self.PATHS)
        self.assertEqual(missing, [])

    def test_a_cyclic_ref_terminates_instead_of_spinning(self):
        self.schema["$defs"]["send"] = {"$ref": "#/$defs/send"}
        missing, unreached, _ = self.scan(self.schema)
        self.assertEqual(unreached, ["strips[].sends[].sendDb"])
        self.assertEqual(missing, [])


PYTHON_SURFACE = """
from typing import TypedDict

class Dependency(TypedDict):
    key: str

class Entry(TypedDict):
    name: str
    dependsOn: list[Dependency]
    other: Dependency | None
"""


class PythonSurface(unittest.TestCase):
    def test_a_leaf_missing_from_the_root_class_is_reported(self):
        missing, unreached, comparisons = check.scan_python(
            ["[].name", "[].unit", "[].dependsOn[].key", "[].dependsOn[].relation"],
            PYTHON_SURFACE,
            "Entry",
        )
        self.assertEqual(unreached, [])
        self.assertEqual(comparisons, 4)
        self.assertEqual(missing, ["[].unit", "[].dependsOn[].relation"])

    def test_a_block_that_is_not_a_class_fails_rather_than_passing(self):
        missing, unreached, comparisons = check.scan_python(
            ["[].name.inner"], PYTHON_SURFACE, "Entry"
        )
        self.assertEqual((missing, comparisons), ([], 0))
        self.assertEqual(unreached, ["[].name.inner"])

    def test_a_missing_root_class_fails_rather_than_reporting_clean(self):
        missing, unreached, comparisons = check.scan_python(
            ["[].name"], PYTHON_SURFACE, "Absent"
        )
        self.assertEqual((missing, comparisons), ([], 0))
        self.assertEqual(unreached, ["[].name"])


DATACLASS_SURFACE = """
from dataclasses import dataclass

@dataclass(frozen=True)
class Dependency:
    key: str

@dataclass(frozen=True)
class Entry:
    true_peak_dbtp: float
    depends_on: tuple[Dependency, ...]
    dependsOn: str
"""


class DataclassSurface(unittest.TestCase):
    def test_a_dataclass_root_reads_each_segment_in_snake_case(self):
        missing, unreached, comparisons = check.scan_python(
            ["[].truePeakDbtp", "[].dependsOn[].key", "[].lraLu"],
            DATACLASS_SURFACE,
            "Entry",
        )
        self.assertEqual(unreached, [])
        self.assertEqual(comparisons, 3)
        self.assertEqual(missing, ["[].lraLu"])

    def test_a_camel_case_field_on_a_dataclass_does_not_answer_for_the_path(self):
        missing, _, _ = check.scan_python(
            ["[].dependsOn"],
            DATACLASS_SURFACE.replace("depends_on", "unrelated"),
            "Entry",
        )
        self.assertEqual(missing, ["[].dependsOn"])

    def test_a_typed_dict_root_still_compares_verbatim(self):
        missing, unreached, comparisons = check.scan_python(
            ["[].dependsOn[].key", "[].depends_on"], PYTHON_SURFACE, "Entry"
        )
        self.assertEqual((unreached, comparisons), ([], 2))
        self.assertEqual(missing, ["[].depends_on"])


class PythonImportedModule(unittest.TestCase):
    """A record shared between families lives in its own module and is imported
    where the root is declared; reading the root's file alone leaves every path
    beneath it unreached."""

    def surface(self, root: Path, imports: str) -> Path:
        (root / "_shared.py").write_text(
            "from dataclasses import dataclass\n\n"
            "@dataclass(frozen=True)\nclass Click:\n    per_second: float\n\n"
            "@dataclass(frozen=True)\nclass Defects:\n    click: Click | None\n"
        )
        surface = root / "_results.py"
        surface.write_text(
            "from __future__ import annotations\n"
            f"{imports}"
            "from dataclasses import dataclass\n\n"
            "@dataclass(frozen=True)\nclass Profile:\n    defects: Defects | None\n"
        )
        return surface

    PATHS: ClassVar[list[str]] = ["defects.click.perSecond", "defects.click.count"]

    def test_a_class_imported_from_a_sibling_module_is_reached(self):
        with tempfile.TemporaryDirectory() as directory:
            surface = self.surface(Path(directory), "from ._shared import Defects\n")
            missing, unreached, comparisons = check.scan_surface(self.PATHS, surface, "Profile")
        self.assertEqual((unreached, comparisons), ([], 2))
        self.assertEqual(missing, ["defects.click.count"])

    def test_without_the_import_the_paths_stay_unreached(self):
        with tempfile.TemporaryDirectory() as directory:
            surface = self.surface(Path(directory), "")
            missing, unreached, comparisons = check.scan_surface(self.PATHS, surface, "Profile")
        self.assertEqual((missing, comparisons), ([], 0))
        self.assertEqual(unreached, self.PATHS)


if __name__ == "__main__":
    unittest.main()
