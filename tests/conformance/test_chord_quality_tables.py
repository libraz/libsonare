#!/usr/bin/env python3
"""Stdlib self-tests for the chord-quality table check.

Every case drives :func:`check_chord_quality_tables.evaluate` -- the function
the shipping entry point calls -- over a copy of the real files with one edit,
so a perturbation is demonstrated rather than asserted about. No source file in
the tree is touched.

The case that matters is the swap. Two entries exchanged inside one table leave
its name set identical and its mapping wrong, which is what a check comparing
sorted names would pass; that set is asserted to be unchanged in the same
fixture, so the swap case cannot be satisfied by a set comparison.
"""

from __future__ import annotations

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_chord_quality_tables",
    Path(__file__).resolve().parent / "check_chord_quality_tables.py",
)
assert _SPEC and _SPEC.loader
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)

TYPES = check.CORE_ENUM.path
C_HEADER = check.ORDINAL_TABLES[0].path
EMBIND = check.ORDINAL_TABLES[1].path
WASM_CONST = check.ORDINAL_TABLES[2].path
WASM_DTS = check.ORDINAL_TABLES[3].path
PY_DETECTION = check.ORDINAL_TABLES[4].path
PY_MUSIC = check.ORDINAL_TABLES[5].path
NODE_ADDON = check.ORDINAL_TABLES[6].path
NODE_TYPES = check.NAME_TABLES[0].path
PY_SUFFIXES = check.NAME_TABLES[2].path

FILES = (
    TYPES,
    C_HEADER,
    EMBIND,
    WASM_CONST,
    WASM_DTS,
    PY_DETECTION,
    PY_MUSIC,
    NODE_ADDON,
    NODE_TYPES,
    PY_SUFFIXES,
)

# A quality appended to the core, in the three spellings the surfaces use.
APPENDED_CORE = ("  Dominant7s9,    ///< 7#9\n};", "  Dominant7s9,    ///< 7#9\n  Dominant7s11,\n};")


class _CopiedTree(unittest.TestCase):
    """A throwaway tree holding the real files, optionally with one edit each."""

    def tree(
        self,
        edits: dict[str, tuple[str, str]] | None = None,
        omit: tuple[str, ...] = (),
    ) -> Path:
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root)
        for relative in FILES:
            if relative in omit:
                continue
            text = (check.ROOT / relative).read_text(encoding="utf-8")
            if edits and relative in edits:
                old, new = edits[relative]
                self.assertIn(old, text, f"{relative}: the fixture anchor is gone")
                self.assertEqual(text.count(old), 1, f"{relative}: the fixture anchor is not unique")
                text = text.replace(old, new, 1)
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(text, encoding="utf-8")
        return root

    def classes(self, failures: list[tuple[str, list[str]]]) -> list[str]:
        return [heading for heading, _ in failures]

    def lines(self, failures: list[tuple[str, list[str]]], fragment: str) -> list[str]:
        matched = [lines for heading, lines in failures if fragment in heading]
        self.assertEqual(len(matched), 1, f"expected one `{fragment}` class, got {self.classes(failures)}")
        return matched[0]


class ShippingTreeTest(unittest.TestCase):
    def test_the_tree_as_it_stands_passes(self) -> None:
        self.assertEqual(check.evaluate(), [])

    def test_every_table_is_located_and_holds_the_whole_enum(self) -> None:
        scan = check.Scan()
        self.assertEqual(len(scan.core), check.FLOOR["qualities"])
        self.assertEqual(scan.core_count, len(scan.core))
        self.assertEqual(scan.c_count, len(scan.core))
        for table, located in scan.ordinal.items():
            with self.subTest(table=table.display):
                self.assertIsNotNone(located, f"{table.display} was not located")
                self.assertEqual(sorted(located.by_ordinal), sorted(scan.core))
        for table, located in scan.named.items():
            with self.subTest(table=table.display):
                self.assertIsNotNone(located, f"{table.display} was not located")
                self.assertEqual(len(located.keys), len(scan.core))

    def test_the_tables_are_compared_against_spelled_core_names(self) -> None:
        scan = check.Scan()
        node = scan.ordinal[check.ORDINAL_TABLES[6]]
        self.assertEqual(node.by_ordinal[13], "halfDim7")
        self.assertEqual(check.expected_name(scan.core[13], "camel"), "halfDim7")
        self.assertEqual(check.expected_name(scan.core[13], "c_macro"), "SONARE_CHORD_HALF_DIM7")


class SpellingTest(unittest.TestCase):
    """The exemptions are a list of two enumerators, not a relaxed comparison."""

    def test_each_spelling_is_a_total_rule_over_the_enum(self) -> None:
        self.assertEqual(check.expected_name("Sus2Add4", "c_macro"), "SONARE_CHORD_SUS2_ADD4")
        self.assertEqual(check.expected_name("MinorMajor7", "camel"), "minorMajor7")
        self.assertEqual(check.expected_name("Major", "pascal"), "Major")

    def test_the_altered_ninths_are_the_only_exempt_enumerators(self) -> None:
        self.assertEqual(set(check.SPELLING_EXEMPTIONS), {"Dominant7b9", "Dominant7s9"})
        for exemption in check.SPELLING_EXEMPTIONS.values():
            self.assertTrue(exemption.reason.strip())
        self.assertEqual(check.expected_name("Dominant7b9", "camel"), "dominant7Flat9")
        self.assertEqual(check.expected_name("Dominant7s9", "pascal"), "Dominant7Sharp9")

    def test_a_case_difference_outside_the_list_is_not_absorbed(self) -> None:
        """Nothing normalises case generally, so a rename still reads as a rename."""
        self.assertNotEqual(check.expected_name("Halfdim7", "camel"), "halfDim7")


class ArrangementEnumTest(unittest.TestCase):
    """The coarser arrangement enum is a separate vocabulary, not drift."""

    ARRANGEMENT = "src/arrangement/harmonic_timeline.h"

    def test_the_arrangement_enum_is_never_opened(self) -> None:
        scanned = {t.path for t in (check.CORE_ENUM, *check.ORDINAL_TABLES, *check.NAME_TABLES)}
        self.assertNotIn(self.ARRANGEMENT, scanned)

    def test_no_located_table_carries_an_arrangement_enumerator(self) -> None:
        arrangement = check.core_enum(
            (check.ROOT / self.ARRANGEMENT).read_text(encoding="utf-8"),
            check.Table(self.ARRANGEMENT, "ChordQuality", "core_enum", "pascal", "core", ""),
        )
        self.assertIsNotNone(arrangement)
        names = set(arrangement[1].values())
        self.assertEqual(len(names), 8)
        scan = check.Scan()
        self.assertEqual(names & set(scan.core.values()), set())
        for located in scan.ordinal.values():
            self.assertEqual(names & set(located.by_ordinal.values()), set())


class SwappedEntriesTest(_CopiedTree):
    """Two entries exchanged: the set holds, the mapping does not."""

    WASM_SWAP = {WASM_CONST: ("  Major7: 5,\n  Minor7: 6,", "  Major7: 6,\n  Minor7: 5,")}
    PY_SWAP = {PY_DETECTION: ('    5: "major7",\n    6: "minor7",', '    5: "minor7",\n    6: "major7",')}

    def test_a_swap_in_the_wasm_constant_is_reported_at_both_ordinals(self) -> None:
        failures = check.evaluate(self.tree(self.WASM_SWAP))
        lines = self.lines(failures, "disagree with the core enum at an ordinal")
        self.assertEqual(len(lines), 2, lines)
        self.assertIn("ordinal 5 is `Minor7`", lines[0])
        self.assertIn("core has `Major7`", lines[0])
        self.assertIn("ordinal 6 is `Major7`", lines[1])
        self.assertIn("core has `Minor7`", lines[1])
        self.assertEqual(len(failures), 1, self.classes(failures))

    def test_the_swapped_table_has_an_identical_name_set(self) -> None:
        """A check comparing sorted names would see nothing here."""
        root = self.tree(self.WASM_SWAP)
        table = check.ORDINAL_TABLES[2]
        before = check.Scan().ordinal[table]
        after = check.Scan(root).ordinal[table]
        self.assertEqual(sorted(before.by_ordinal.values()), sorted(after.by_ordinal.values()))
        self.assertNotEqual(before.by_ordinal, after.by_ordinal)

    def test_a_swap_in_the_python_table_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(self.PY_SWAP)), "disagree with the core enum at an ordinal"
        )
        self.assertEqual(len(lines), 2, lines)
        self.assertIn("ordinal 5 is `minor7`", lines[0])
        self.assertIn("ordinal 6 is `major7`", lines[1])

    def test_a_swap_in_the_node_addon_switch_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(
                self.tree(
                    {
                        NODE_ADDON: (
                            '    case SONARE_CHORD_MAJOR7:\n      return "major7";',
                            '    case SONARE_CHORD_MAJOR7:\n      return "minor7";',
                        )
                    }
                )
            ),
            "disagree with the core enum at an ordinal",
        )
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("ordinal 5 is `minor7`", lines[0])


class RenamedEntryTest(_CopiedTree):
    def test_a_renamed_python_entry_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(
                self.tree({PY_DETECTION: ('    13: "halfDim7",', '    13: "halfDiminished7",')})
            ),
            "disagree with the core enum at an ordinal",
        )
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("ordinal 13 is `halfDiminished7`", lines[0])
        self.assertIn("spelled `halfDim7` here", lines[0])

    def test_a_renamed_embind_export_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(
                self.tree({EMBIND: ('.value("Sus2Add4"', '.value("Sus2PlusAdd4"')})
            ),
            "disagree with the core enum at an ordinal",
        )
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("ordinal 16 is `Sus2PlusAdd4`", lines[0])

    def test_a_renamed_key_in_a_name_table_is_reported_both_ways(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree({PY_SUFFIXES: ('"minorMajor7": "mM7"', '"minMaj7": "mM7"')})),
            "key set no longer matches the core enum",
        )
        self.assertEqual(len(lines), 2, lines)
        self.assertIn("`minorMajor7` is absent", lines[0])
        self.assertIn("`minMaj7` is not a core quality", lines[1])

    def test_a_renamed_node_union_member_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree({NODE_TYPES: ("    | 'halfDim7'", "    | 'halfDiminished7'")})),
            "key set no longer matches the core enum",
        )
        self.assertEqual(len(lines), 2, lines)


class MissingTableTest(_CopiedTree):
    """A table the scan cannot read is a table nothing compares."""

    def test_a_deleted_table_file_is_reported_and_drops_the_count(self) -> None:
        failures = check.evaluate(self.tree(omit=(PY_MUSIC,)))
        missing = self.lines(failures, "could not be located")
        self.assertEqual(len(missing), 1, missing)
        self.assertIn(PY_MUSIC, missing[0])
        floor = self.lines(failures, "no longer locates the population")
        self.assertTrue(any("ordinal_tables: found 6, floor is 7" in line for line in floor), floor)

    def test_a_renamed_table_symbol_is_reported(self) -> None:
        missing = self.lines(
            check.evaluate(self.tree({PY_MUSIC: ("    quality_names = {", "    names = {")})),
            "could not be located",
        )
        self.assertEqual(len(missing), 1, missing)
        self.assertIn("quality_names", missing[0])

    def test_a_second_table_of_the_same_name_is_ambiguous_rather_than_guessed(self) -> None:
        missing = self.lines(
            check.evaluate(
                self.tree({PY_MUSIC: ("    quality_names = {", "    quality_names = {}\n    quality_names = {")})
            ),
            "could not be located",
        )
        self.assertEqual(len(missing), 1, missing)

    def test_every_table_reports_on_its_own(self) -> None:
        for path in (WASM_CONST, WASM_DTS, NODE_TYPES, PY_SUFFIXES, EMBIND, NODE_ADDON, C_HEADER):
            with self.subTest(path=path):
                missing = self.lines(check.evaluate(self.tree(omit=(path,))), "could not be located")
                self.assertTrue(any(path in line for line in missing), missing)


class AppendedQualityTest(_CopiedTree):
    """A quality added to the core with no table updated."""

    def test_an_append_without_the_count_fails_on_the_anchor(self) -> None:
        failures = check.evaluate(self.tree({TYPES: APPENDED_CORE}))
        anchor = self.lines(failures, "anchor the tables are held to")
        self.assertEqual(len(anchor), 2, anchor)
        self.assertTrue(all("has 26 enumerators" in line for line in anchor), anchor)

    def test_an_append_with_both_counts_leaves_every_table_short(self) -> None:
        root = self.tree({TYPES: APPENDED_CORE})
        for relative, edit in (
            (TYPES, ("kChordQualityCount = 25", "kChordQualityCount = 26")),
            (C_HEADER, ("SONARE_CHORD_QUALITY_COUNT 25", "SONARE_CHORD_QUALITY_COUNT 26")),
        ):
            path = root / relative
            text = path.read_text(encoding="utf-8")
            self.assertIn(edit[0], text)
            path.write_text(text.replace(edit[0], edit[1], 1), encoding="utf-8")

        failures = check.evaluate(root)
        drift = self.lines(failures, "disagree with the core enum at an ordinal")
        self.assertEqual(len(drift), len(check.ORDINAL_TABLES), drift)
        for line in drift:
            self.assertIn("ordinal 25 is absent", line)
            self.assertIn("Dominant7s11", line)
        names = self.lines(failures, "key set no longer matches the core enum")
        self.assertEqual(len(names), 3, names)
        self.assertTrue(all("`dominant7s11` is absent" in line for line in names), names)


class StaleExemptionTest(_CopiedTree):
    """An exemption expires with the enumerator it spells."""

    def test_an_exemption_whose_enumerator_is_gone_is_reported(self) -> None:
        failures = check.evaluate(self.tree({TYPES: ("  Dominant7b9,", "  Dominant7Flat9,")}))
        stale = self.lines(failures, "name an enumerator the core no longer has")
        self.assertEqual(len(stale), 1, stale)
        self.assertIn("Dominant7b9 -> Dominant7Flat9", stale[0])

    def test_the_exemptions_are_load_bearing(self) -> None:
        """Drop them and the shipping tree fails, so they suppress something real."""
        saved = dict(check.SPELLING_EXEMPTIONS)
        check.SPELLING_EXEMPTIONS.clear()
        try:
            drift = self.lines(check.evaluate(), "disagree with the core enum at an ordinal")
            self.assertTrue(any("Dominant7b9" in line for line in drift), drift)
            self.assertTrue(any("Dominant7s9" in line for line in drift), drift)
        finally:
            check.SPELLING_EXEMPTIONS.update(saved)
        self.assertEqual(check.evaluate(), [])


class FloorTest(_CopiedTree):
    def test_an_unparsable_core_enum_fails_rather_than_matching_everything(self) -> None:
        failures = check.evaluate(self.tree({TYPES: ("enum class ChordQuality {", "enum class ChordQualityV2 {")}))
        self.assertTrue(any("no `enum class ChordQuality`" in line for line in self.lines(failures, "anchor the tables")))
        floor = self.lines(failures, "no longer locates the population")
        self.assertTrue(any("qualities: found 0, floor is 25" in line for line in floor), floor)

    def test_the_floor_is_independent_of_the_declared_tables(self) -> None:
        self.assertGreaterEqual(len(check.ORDINAL_TABLES), check.FLOOR["ordinal_tables"])
        self.assertGreaterEqual(len(check.NAME_TABLES), check.FLOOR["name_tables"])
        self.assertEqual(check.FLOOR["qualities"], 25)


if __name__ == "__main__":
    unittest.main()
