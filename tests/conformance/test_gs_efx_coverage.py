"""Conformance test for the GS EFX-table provenance and disagreement bookkeeping.

`tools/gs/efx-tables.json` is a generated, committed artifact; this test reads
the file itself rather than the derivation script, because it is checking what
the derivation produced, not how.

What would make this vacuous: a check that only asks whether `source` is
*present* would pass the day the derivation stamps `"measured"` on all 85
`map` entries, which is the exact collapse the split exists to catch --
`MapEntrySourceTest` also requires `measured` and `unit_overrides_assigned`
to each be used at least once, so a stamped-uniform file still fails. The
third value, `assigned`, is checked in its own test method
(`test_assigned_is_used`) rather than folded into that one: the other two
both rest on a reading, so it is the one that can collapse to zero while
they stay populated. The `disagreement` count is checked against a
list mechanically extracted from `src/midi/synth/docs/gs.md:20` (five items,
comma-separated, following "this page says to do:") rather than a value
copied by hand, so a future edit to that sentence changes what this file
asserts instead of silently drifting from it.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLES_PATH = ROOT / "tools" / "gs" / "efx-tables.json"
GS_DOC_PATH = ROOT / "src" / "midi" / "synth" / "docs" / "gs.md"

_SOURCE_VALUES = {"measured", "assigned", "unit_overrides_assigned"}

_DISAGREEMENT_ANCHOR = (
    "the two part company take the machine, because that is what this page says to do:"
)
_DISAGREEMENT_END = "of its ends."


def _load_tables() -> dict:
    return json.loads(TABLES_PATH.read_text(encoding="utf-8"))


def _iter_cells(node, path: str):
    """Yield (path, cell) for every per-setting cell reached through a list
    under `classes` -- a `breakpoints`/`entries` list, or a `sides.left`/
    `sides.right` pair. Reached only through a list is what separates a cell
    from the table- or class-level summary dict that carries the same
    `approximate` key but is never itself a list element.
    """
    if isinstance(node, dict):
        for key, value in node.items():
            yield from _iter_cells(value, f"{path}.{key}")
    elif isinstance(node, list):
        for index, item in enumerate(node):
            item_path = f"{path}[{index}]"
            if isinstance(item, dict) and "approximate" in item:
                yield item_path, item
            yield from _iter_cells(item, item_path)


def _iter_tables(classes: dict):
    """Yield (path, conversion_class, table_name, table_def) for every table."""
    for conversion_class, class_def in classes.items():
        for table_name, table_def in class_def.get("tables", {}).items():
            yield (
                f"classes.{conversion_class}.tables.{table_name}",
                conversion_class,
                table_name,
                table_def,
            )


def _prefix_match(path: str, prefix: str) -> bool:
    return path == prefix or path.startswith((prefix + ".", prefix + "["))


def _provenance_units(tables: dict):
    """Every dict this file can hang a `source` off of: a per-setting cell
    where a table enumerates one, and otherwise the table object itself -- a
    `rule`-kind table with no per-setting list (`gain.tone`, `balance.effect`)
    computes its value from a formula rather than a ladder, so the formula's
    own table dict is the only unit of provenance it has.
    """
    classes = tables.get("classes", {})
    cells = list(_iter_cells(classes, "classes"))
    units = list(cells)
    for path, _conversion_class, _table_name, table_def in _iter_tables(classes):
        if any(_prefix_match(cell_path, path) for cell_path, _ in cells):
            continue
        if isinstance(table_def, dict) and "approximate" in table_def:
            units.append((path, table_def))
    return units


def _units_for(units, conversion_class: str, table: str):
    prefix = f"classes.{conversion_class}.tables.{table}"
    return [obj for path, obj in units if _prefix_match(path, prefix)]


def _gs_md_disagreements() -> list[str]:
    """Mechanically extract the five-item list `gs.md:20` states.

    Not a hand-copied list: `test_gs_md_extraction_finds_five_items` pins that
    this still finds exactly five items against the live doc, so an edit to
    that sentence breaks the extraction loudly rather than leaving this file's
    count silently disconnected from what the doc actually says.
    """
    text = GS_DOC_PATH.read_text(encoding="utf-8")
    start = text.find(_DISAGREEMENT_ANCHOR)
    if start == -1:
        return []
    tail = text[start + len(_DISAGREEMENT_ANCHOR) :]
    end = tail.find(_DISAGREEMENT_END)
    if end == -1:
        return []
    list_text = tail[: end + len(_DISAGREEMENT_END)].strip().rstrip(".")
    return [item.strip() for item in re.split(r", (?:and )?(?=a )", list_text)]


class EfxTablesShapeTest(unittest.TestCase):
    """Guards the navigation: an absent top-level key must read as a named
    absence, not as a KeyError traceback from deep inside the other tests."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = _load_tables()

    def test_the_map_key_is_present(self) -> None:
        self.assertIn("map", self.tables, "the derivation no longer emits a top-level 'map' list")
        self.assertIsInstance(self.tables["map"], list)
        self.assertTrue(self.tables["map"], "the derivation emits an empty 'map' list")

    def test_the_classes_key_is_present(self) -> None:
        self.assertIn(
            "classes",
            self.tables,
            "the derivation no longer emits a top-level 'classes' object",
        )


class MapEntrySourceTest(unittest.TestCase):
    """Every `map` entry carries a `source` drawn from {measured, assigned,
    unit_overrides_assigned}, and none of the three buckets is empty."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = _load_tables()
        cls.map_entries = cls.tables.get("map", [])

    def test_every_map_entry_carries_a_known_source(self) -> None:
        for index, entry in enumerate(self.map_entries):
            label = f"map[{index}] ({entry.get('type')} / {entry.get('address')})"
            with self.subTest(index=index):
                source = entry.get("source")
                self.assertIsNotNone(source, f"{label} carries no 'source'")
                self.assertIn(
                    source,
                    _SOURCE_VALUES,
                    f"{label} has source={source!r}, not one of {sorted(_SOURCE_VALUES)}",
                )

    def _source_counts(self) -> dict[str, int]:
        counts = {value: 0 for value in _SOURCE_VALUES}
        for entry in self.map_entries:
            source = entry.get("source")
            if source in counts:
                counts[source] += 1
        return counts

    def test_measured_and_unit_overrides_assigned_are_both_used(self) -> None:
        """The two buckets that rest on a reading.

        Held apart from `assigned` because the ways the split can collapse
        are not the same: these two are distinguished by whether the reading
        agreed with the law, so a change that stopped telling them apart
        leaves both populated, while `assigned` simply empties.
        """
        counts = self._source_counts()
        for value in ("measured", "unit_overrides_assigned"):
            with self.subTest(source=value):
                self.assertGreater(
                    counts[value],
                    0,
                    f"no map entry carries source={value!r} -- the provenance split "
                    f"collapsed into the other buckets (counts: {counts})",
                )

    def test_assigned_is_used(self) -> None:
        """The bucket standing for a law carried on the series alone.

        Kept a method of its own because it is the bucket most easily lost:
        `measured` and `unit_overrides_assigned` both rest on a reading, so a
        change that stopped distinguishing them would still leave two
        populated buckets, while `assigned` names the entries no reading
        placed at all and collapses to zero without anything else moving.

        `source` is the provenance of the conversion law, not of a binding,
        so this does not wait on `tools/gs/efx-bindings/*.json` -- it goes
        green with the other two, as soon as the derivation emits `source`.
        """
        counts = self._source_counts()
        self.assertGreater(
            counts["assigned"],
            0,
            f"no map entry carries source='assigned' -- expected until "
            f"tools/gs/efx-bindings/*.json exist and are read (counts: {counts})",
        )


class CellSourceTest(unittest.TestCase):
    """Provenance is a property of the cell, and a `map` entry's `source` is a
    fold over the units its (conversion_class, table) reads -- `map` alone
    cannot express "one cell of sixteen came from the unit"."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = _load_tables()
        cls.units = _provenance_units(cls.tables)
        assert cls.units, "fixture precondition: at least one provenance unit must be found"

    def test_every_unit_carries_a_source(self) -> None:
        for path, obj in self.units:
            with self.subTest(path=path):
                self.assertIn("source", obj, f"{path} carries no 'source'")

    def test_map_source_is_consistent_with_its_contributing_units(self) -> None:
        map_entries = self.tables.get("map", [])
        for index, entry in enumerate(map_entries):
            conversion_class = entry.get("conversion_class")
            table = entry.get("table")
            label = f"map[{index}] ({entry.get('type')} / {entry.get('address')})"
            with self.subTest(index=index):
                contributing = _units_for(self.units, conversion_class, table)
                self.assertTrue(
                    contributing,
                    f"{label} names ({conversion_class}, {table}), which no provenance "
                    f"unit matches",
                )
                any_unit_specific = any(obj.get("unit_specific") is True for obj in contributing)
                source = entry.get("source")
                self.assertIsNotNone(
                    source,
                    f"{label} carries no 'source'; cannot check it against its "
                    f"{len(contributing)} contributing units",
                )
                if any_unit_specific:
                    self.assertEqual(
                        source,
                        "unit_overrides_assigned",
                        f"{label} reads at least one unit-specific unit in "
                        f"({conversion_class}, {table}) but source={source!r}",
                    )
                else:
                    self.assertNotEqual(
                        source,
                        "unit_overrides_assigned",
                        f"{label} has source='unit_overrides_assigned' but none of its "
                        f"{len(contributing)} contributing units in ({conversion_class}, "
                        f"{table}) are unit-specific",
                    )


class UnitSpecificEntryTest(unittest.TestCase):
    """Every `unit_specific: true` `map` entry carries
    `source == 'unit_overrides_assigned'`, and there are exactly six.

    Six is measured against the committed file, not assumed: the file has ten
    `unit_specific: true` occurrences in total, but four of them are not `map`
    entries -- two are table-level summary flags (`classes.accel.tables.rotor`,
    `classes.freq.tables.eq` themselves) and two are per-setting cells reached
    through `entries` rather than through `map`. Only the six left are `map`
    entries, which is what this criterion is about.
    """

    EXPECTED_UNIT_SPECIFIC_MAP_ENTRIES = 6

    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = _load_tables()
        cls.map_entries = cls.tables.get("map", [])

    def test_unit_specific_map_entries_have_the_overriding_source(self) -> None:
        flagged = [e for e in self.map_entries if e.get("unit_specific") is True]
        self.assertEqual(
            len(flagged),
            self.EXPECTED_UNIT_SPECIFIC_MAP_ENTRIES,
            f"expected {self.EXPECTED_UNIT_SPECIFIC_MAP_ENTRIES} map entries flagged "
            f"unit_specific, found {len(flagged)}: "
            f"{[(e.get('type'), e.get('address')) for e in flagged]}",
        )
        for entry in flagged:
            label = f"{entry.get('type')} / {entry.get('address')}"
            with self.subTest(entry=label):
                self.assertEqual(
                    entry.get("source"),
                    "unit_overrides_assigned",
                    f"{label} is unit_specific but source={entry.get('source')!r}",
                )


class DisagreementListTest(unittest.TestCase):
    """`disagreement` names all five places `gs.md:20` records the measured
    law overriding the printed one."""

    # Each marker is a substring quoted verbatim from the item at the same
    # index in the mechanical extraction below --
    # `test_the_markers_are_literally_drawn_from_the_extracted_items` proves
    # it, so nothing here is a second, independently maintained list.
    MARKERS = ("279", "63/64", "gain window", "level curve", "centre")

    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = _load_tables()
        cls.gs_md_items = _gs_md_disagreements()

    def test_gs_md_extraction_finds_five_items(self) -> None:
        """Pins the extraction against the live doc, not a copied count."""
        self.assertEqual(
            len(self.gs_md_items),
            5,
            f"expected 5 items extracted from gs.md:20, found "
            f"{len(self.gs_md_items)}: {self.gs_md_items}",
        )

    def test_the_markers_are_literally_drawn_from_the_extracted_items(self) -> None:
        self.assertEqual(len(self.MARKERS), len(self.gs_md_items))
        for marker, item in zip(self.MARKERS, self.gs_md_items, strict=True):
            with self.subTest(marker=marker):
                self.assertIn(marker, item, f"{marker!r} is not in the extracted item {item!r}")

    def test_the_disagreement_list_is_present(self) -> None:
        self.assertIn(
            "disagreement",
            self.tables,
            "the derivation no longer emits a top-level 'disagreement' list",
        )

    def test_the_disagreement_list_names_all_five_gs_md_points(self) -> None:
        disagreements = self.tables.get("disagreement")
        self.assertIsInstance(
            disagreements,
            list,
            f"'disagreement' is {type(disagreements).__name__}, not a list",
        )
        self.assertEqual(
            len(disagreements),
            len(self.gs_md_items),
            f"gs.md:20 names {len(self.gs_md_items)} disagreements "
            f"({self.gs_md_items}), efx-tables.json names {len(disagreements)}",
        )
        haystack = " ".join(str(item) for item in disagreements)
        for marker, item in zip(self.MARKERS, self.gs_md_items, strict=True):
            with self.subTest(disagreement=item):
                self.assertIn(
                    marker,
                    haystack,
                    f"no entry in 'disagreement' mentions {marker!r} (gs.md: {item!r})",
                )


if __name__ == "__main__":
    unittest.main()
