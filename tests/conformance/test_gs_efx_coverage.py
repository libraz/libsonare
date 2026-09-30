"""Conformance tests for the GS EFX tables and the binding-row tooling.

The first half checks the provenance and disagreement bookkeeping in the
generated tables; the second drives ``tools/gs/coverage.py`` and the two
header generators over tiny row sets written to a temporary directory.

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

import contextlib
import copy
import importlib.util
import json
import re
import tempfile
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


GS_TOOLS = ROOT / "tools" / "gs"


def _tool(name: str):
    spec = importlib.util.spec_from_file_location(f"gs_{name}", GS_TOOLS / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


coverage = _tool("coverage")
bindings_header = _tool("bindings_header")
join_header = _tool("join_header")

# A two-type printed set standing in for the archive: one continuous byte, one
# three-state list, one two-state list, a rate column and a signed range.
PRINTED = {
    "01 42": {3: "*6", 4: "00–7F", 5: "00/01/02", 6: "00/01", 7: "0F–71"},
    "04 00": {3: "00/01", 11: "00/01", 12: "00–7F"},
}
NAMES = {
    "01 42": {3: "Rate", 4: "Depth", 5: "Pre Filter", 6: "Out", 7: "Feedback"},
    "04 00": {3: "Cmp Sw", 11: "CF Sel", 12: "OD Level"},
}
INVENTED = {"basis": "invented", "replaced_when": {"model_binding": ["01 42", "40 03 07"]}}


def _row(slot: int, **fields) -> dict:
    return {"type": "01 42", "slot": slot, **fields}


def _designed(slot: int, law: str, printed: str, **fields) -> dict:
    fields.setdefault("stage", "stereo.autoPan")
    return _row(
        slot, designed=dict(INVENTED, law=law), printed_values=printed, key="depth", **fields
    )


@contextlib.contextmanager
def _row_dir(rows: list[dict]):
    """Rows written to a temporary binding directory, one file per MSB, as the tools read them."""
    with tempfile.TemporaryDirectory() as name:
        by_msb: dict[str, list[dict]] = {}
        for row in rows:
            by_msb.setdefault(row["type"].split()[0], []).append(row)
        for msb, group in by_msb.items():
            (Path(name) / f"{msb}.json").write_text(json.dumps(group), encoding="utf-8")
        yield Path(name)


def _tally(rows: list[dict], names=NAMES, laws=None, claims=None) -> dict:
    """Tally rows that carry the name ``names`` prints for their slot, as transcribed rows do.

    A row that already sets ``printed_name`` keeps it; ``printed_name=None``
    leaves the field out.
    """
    named = []
    for row in rows:
        row = dict(row)
        if "printed_name" not in row:
            row["printed_name"] = names[row["type"]][row["slot"]]
        if row["printed_name"] is None:
            del row["printed_name"]
        named.append(row)
    with _row_dir(named) as row_dir:
        loaded = coverage.load_bindings(row_dir)
    laws = laws if laws is not None else coverage.load_laws(coverage.DEFAULT_LAWS)
    measured = coverage.measured_laws(coverage.DEFAULT_TABLES)
    return coverage.tally(loaded, PRINTED, laws, measured, claims)


class CoverageVocabularyTest(unittest.TestCase):
    """The designed / enables vocabulary is parsed, counted and checked, and the retired forms refused."""

    def assertRefused(self, rows: list[dict], fragment: str, **kwargs) -> None:
        with self.assertRaises(SystemExit) as caught:
            _tally(rows, **kwargs)
        self.assertIn(fragment, str(caught.exception.code))

    def test_each_retired_form_is_refused(self) -> None:
        for key in ("state", "unmapped", "unreadable", "builder"):
            with self.subTest(form=key):
                self.assertRefused([_row(4, **{key: "a reason"})], key)

    def test_line_one_counts_the_three_forms(self) -> None:
        result = _tally(
            [
                _row(3, **{"class": "rate", "table": "wide"}, stage="s", key="rateHz"),
                _designed(4, "d.unit", "00–7F"),
                {
                    "type": "04 00",
                    "slot": 3,
                    "enables": {
                        "stages": [{"stage": "dynamics.compressor"}],
                        "on_states": [1],
                        **INVENTED,
                    },
                },
            ]
        )
        lines = coverage.summary_lines(result, coverage.per_msb_of(PRINTED))
        self.assertEqual(lines[0], "GS EFX coverage: printed=8 translated=1 designed=1 enables=1")
        self.assertEqual(lines[1], "GS EFX basis: carried=0 invented=2")
        self.assertIn("forms[01]: translated=1 designed=1 enables=0", lines)
        self.assertIn("forms[04]: translated=0 designed=0 enables=1", lines)

    def test_missing_replaced_when_is_refused(self) -> None:
        row = _designed(4, "d.unit", "00–7F")
        del row["designed"]["replaced_when"]
        self.assertRefused([row], "replaced_when")

    def test_unknown_replaced_when_key_is_refused(self) -> None:
        row = _designed(4, "d.unit", "00–7F")
        row["designed"]["replaced_when"] = {"someday": True}
        self.assertRefused([row], "replaced_when")

    def test_enables_without_replaced_when_is_refused(self) -> None:
        row = {
            "type": "04 00",
            "slot": 3,
            "enables": {"stages": [{"stage": "x"}], "on_states": [1], "basis": "invented"},
        }
        self.assertRefused([row], "replaced_when")

    def test_unknown_designed_law_is_refused(self) -> None:
        self.assertRefused([_designed(4, "d.nothing_like_it", "00–7F")], "d.nothing_like_it")

    def test_carried_needs_a_measured_law_and_its_source(self) -> None:
        carried = {
            "basis": "carried",
            "law": "level.output",
            "replaced_when": {"stage_passed": "04 00"},
        }
        row = {
            "type": "04 00",
            "slot": 12,
            "designed": dict(carried),
            "printed_values": "00–7F",
            "stage": "utility.gain",
            "key": "levelDb",
        }
        self.assertRefused([row], "from")
        row["designed"] = dict(carried, law="level.nowhere", **{"from": "a-claim"})
        self.assertRefused([row], "level.nowhere")
        row["designed"] = dict(carried, **{"from": "a-claim"})
        self.assertEqual(_tally([row])["basis"]["carried"], 1)

    def test_designed_row_holds_its_printed_values_to_the_archive(self) -> None:
        self.assertRefused([_designed(4, "d.unit", "00–7E")], "printed_values")
        row = _designed(4, "d.unit", "00–7F")
        del row["printed_values"]
        self.assertRefused([row], "printed_values")

    def test_enum_state_count_must_match_the_printed_list(self) -> None:
        self.assertRefused([_designed(5, "d.enum2", "00/01/02")], "state")
        self.assertEqual(_tally([_designed(5, "d.enum3", "00/01/02")])["counts"]["designed"], 1)

    def test_state_list_refuses_a_continuous_law(self) -> None:
        self.assertRefused([_designed(5, "d.unit", "00/01/02")], "state list")

    def test_continuous_range_refuses_an_enum_law(self) -> None:
        self.assertRefused([_designed(4, "d.enum128", "00–7F")], "d.enum128")
        self.assertRefused([_designed(4, "d.enum2", "00–7F")], "continuous")

    def test_ordinal_and_printed_mark_are_checked(self) -> None:
        ok = _designed(4, "d.unit", "00–7F", ordinal=1, printed_mark="+")
        self.assertEqual(_tally([ok])["counts"]["designed"], 1)
        self.assertRefused([_designed(4, "d.unit", "00–7F", ordinal=-1)], "ordinal")
        self.assertRefused([_designed(4, "d.unit", "00–7F", printed_mark="*")], "printed_mark")

    def test_name_rules_run_on_the_rows_printed_name(self) -> None:
        rows = [_designed(4, "d.unit", "00–7F"), _designed(5, "d.enum3", "00/01/02")]
        self.assertEqual(_tally(rows)["names_checked"], 2)

    def test_a_new_vocabulary_row_without_printed_name_is_refused(self) -> None:
        self.assertRefused([_designed(4, "d.unit", "00–7F", printed_name=None)], "printed_name")

    def test_a_law_other_than_the_rule_is_refused(self) -> None:
        self.assertRefused([_designed(4, "d.feedback", "00–7F")], "d.unit")

    def test_no_matching_rule_is_refused(self) -> None:
        names = copy.deepcopy(NAMES)
        names["01 42"][4] = "Nothing Printed Like It"
        self.assertRefused([_designed(4, "d.unit", "00–7F")], "no name rule", names=names)

    def test_two_matching_rules_are_refused(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        laws["name_rules"].append({"name": "Depth", "printed": "00–7F", "law": "d.feedback"})
        self.assertRefused([_designed(4, "d.unit", "00–7F")], "2 name rules", laws=laws)

    def test_enables_rows_must_look_up_as_enables(self) -> None:
        enable = {
            "type": "04 00",
            "slot": 3,
            "enables": {"stages": [{"stage": "dynamics.compressor"}], "on_states": [1], **INVENTED},
        }
        self.assertEqual(_tally([enable])["counts"]["enables"], 1)
        names = copy.deepcopy(NAMES)
        names["04 00"][3] = "Out"
        self.assertRefused([enable], "enables", names=names)

    def test_select_needs_one_stage_per_printed_state(self) -> None:
        select = {
            "type": "04 00",
            "slot": 11,
            "enables": {"select": [{"stage": "a"}, {"stage": "b"}, {"stage": "c"}], **INVENTED},
        }
        self.assertRefused([select], "select")

    def test_rule_file_resolves_every_law_it_names(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        laws["name_rules"].append({"name": "X", "law": "d.missing"})
        with self.assertRaises(SystemExit):
            coverage.check_rules(laws, coverage.measured_laws(coverage.DEFAULT_TABLES))


class BindingRowHeaderTest(unittest.TestCase):
    """The generators emit the new row and enable arrays beside the old ones."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.order = bindings_header.class_order(GS_TOOLS / "derive_efx_tables.py")
        tables = json.loads((GS_TOOLS / "efx-tables.json").read_text(encoding="utf-8"))
        cls.classes = tables["classes"]
        cls.laws = coverage.load_laws(coverage.DEFAULT_LAWS)

    def _render(self, rows: list[dict]) -> str:
        with _row_dir(rows) as row_dir:
            loaded = bindings_header.load_rows(row_dir)
        row_entries, enables = bindings_header.collect_rows(
            loaded, self.classes, self.order, self.laws
        )
        return bindings_header.render(row_entries, enables)

    def test_translated_designed_and_enables_rows_are_emitted(self) -> None:
        carried = {
            "basis": "carried",
            "law": "drive.gain",
            "from": "a-claim",
            "replaced_when": {"claim_names": ["01 42", "40 03 0A"]},
        }
        text = self._render(
            [
                _row(3, **{"class": "rate", "table": "wide"}, stage="fx.a", key="rateHz"),
                _row(
                    7,
                    **{"class": "ratio", "table": "percent"},
                    printed_values="0F–71",
                    range=[-98, 98],
                    stage="fx.a",
                    key="feedback",
                ),
                _designed(4, "d.unit", "00–7F", ordinal=1, printed_mark="#"),
                _designed(5, "d.enum3", "00/01/02"),
                _row(6, designed=carried, printed_values="00/01", stage="fx.b", key="inputDb"),
                {
                    "type": "04 00",
                    "slot": 3,
                    "enables": {
                        "stages": [{"stage": "fx.a"}, {"stage": "fx.b", "ordinal": 1}],
                        "on_states": [1, 127],
                        **INVENTED,
                    },
                },
                {
                    "type": "04 00",
                    "slot": 11,
                    "enables": {"select": [{"stage": "fx.a"}, {"stage": "fx.b"}], **INVENTED},
                },
            ]
        )
        # The older table and its struct are gone; the rows are the only rendering.
        self.assertNotIn("struct GsEfxBinding {", text)
        self.assertNotIn("kGsEfxBindings ", text)
        self.assertIn('#include "midi/synth/gs_efx_convert.h"', text)
        self.assertIn("std::array<GsEfxBindingRow, 5> kGsEfxBindingRows", text)
        self.assertIn("std::array<GsEfxEnable, 2> kGsEfxEnables", text)
        # The ratio row carries its endpoints; the invented row its law, domain,
        # ordinal and mark; the enum its state count; the carried row the drive class.
        self.assertIn("GsEfxDesignedLaw kGsEfxLawUnit = {kGsEfxFormLinear, 0.0f, 1.0f, 0};", text)
        self.assertIn("GsEfxDesignedLaw kGsEfxLawEnum3 = {kGsEfxFormEnum, 0.0f, 2.0f, 3};", text)
        self.assertNotIn("kGsEfxDesignedLaws", text)
        self.assertRegex(text, r"kGsEfxRowTranslated, 14, 0, kGsEfxLawNone, 0x0F, 0x71, -98, 98")
        self.assertRegex(
            text,
            r"kGsEfxRowDesigned, 0, 0, kGsEfxLawUnit, 0x00, 0x7F, 0, 0, "
            r"GsEfxOut::kValue, \d+, 1, \d+, '#'",
        )
        self.assertRegex(text, r"kGsEfxLawEnum3, 0x00, 0x02")
        self.assertRegex(text, r"kGsEfxRowDesigned, 15, 0, kGsEfxLawNone")
        self.assertRegex(
            text,
            r"kGsEfxEnableStages, \{\d+, \d+, 0, 0\}, \{0, 1, 0, 0\}, 2, "
            r"\{0x2u, 0x0u, 0x0u, 0x80000000u\}",
        )
        self.assertRegex(text, r"kGsEfxEnableSelect, \{\d+, \d+, 0, 0\}, \{0, 0, 0, 0\}, 2, ")

    def test_the_out_selector_follows_the_key(self) -> None:
        self.assertEqual(bindings_header.out_of("drumUndershootHz"), "GsEfxOut::kUndershootHz")
        self.assertEqual(bindings_header.out_of("undershootHz"), "GsEfxOut::kUndershootHz")
        self.assertEqual(bindings_header.out_of("decelTauS"), "GsEfxOut::kDecelTau")
        self.assertEqual(bindings_header.out_of("accelTauS"), "GsEfxOut::kAccelTau")
        self.assertEqual(bindings_header.out_of("rateHz"), "GsEfxOut::kValue")

    def test_the_join_header_carries_the_new_forms(self) -> None:
        with _row_dir(
            [
                _designed(4, "d.unit", "00–7F", printed_mark="+"),
                {
                    "type": "04 00",
                    "slot": 3,
                    "enables": {"stages": [{"stage": "x"}], "on_states": [1], **INVENTED},
                },
            ]
        ) as row_dir:
            entries = join_header.collect(join_header.load_rows(row_dir))
        text = join_header.render(entries)
        self.assertIn("kGsEfxJoinDesigned", text)
        self.assertIn("kGsEfxJoinEnables", text)
        self.assertIn("inline constexpr int kGsEfxJoinDesignedRows = 1;", text)
        self.assertIn("inline constexpr int kGsEfxJoinEnablesRows = 1;", text)
        self.assertIn("kGsEfxJoinInvented", text)


class SelectedTargetsTest(unittest.TestCase):
    """A byte driving one stage a select chooses between drives every one of them."""

    @staticmethod
    def _select() -> dict:
        return {
            "type": "04 00",
            "slot": 11,
            "enables": {
                "select": [
                    {"stage": "effects.modulation.chorus"},
                    {"stage": "effects.modulation.flanger"},
                ],
                **INVENTED,
            },
        }

    @staticmethod
    def _mix(**fields) -> dict:
        return {
            "type": "04 00",
            "slot": 12,
            "class": "level",
            "table": "output",
            "stage": "effects.modulation.chorus",
            "key": "dryWet",
            **fields,
        }

    def test_a_row_reaching_one_selectable_stage_is_refused(self) -> None:
        with self.assertRaises(SystemExit) as caught:
            _tally([self._select(), self._mix()])
        self.assertIn("effects.modulation.flanger", str(caught.exception.code))

    def test_alternatives_covering_the_select_are_accepted(self) -> None:
        alternatives = [{"stage": "effects.modulation.flanger", "key": "dryWet"}]
        result = _tally([self._select(), self._mix(alternatives=alternatives)])
        self.assertEqual(result["counts"]["translated"], 1)

    def test_alternatives_no_select_names_are_refused(self) -> None:
        alternatives = [{"stage": "effects.modulation.phaser", "key": "dryWet"}]
        with self.assertRaises(SystemExit) as caught:
            _tally([self._select(), self._mix(alternatives=alternatives)])
        self.assertIn("alternatives", str(caught.exception.code))

    def test_the_committed_rows_pass(self) -> None:
        coverage.check_selected_targets(coverage.load_bindings(GS_TOOLS / "efx-bindings"))


class CoverageReportTest(unittest.TestCase):
    """The report says how many rows the name rules were checked on."""

    def test_the_name_check_count_is_printed(self) -> None:
        result = _tally([_designed(4, "d.unit", "00–7F")])
        lines = coverage.summary_lines(result, coverage.per_msb_of(PRINTED))
        self.assertIn("name_rules: checked=1", lines)


class AmpSwitchRuleTest(unittest.TestCase):
    """An amp switch turns the cabinet on and off; it is not a stage switch."""

    def test_amp_sw_is_a_two_state_enum(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        for name in ("Amp Sw", "OD Amp Sw", "DS Amp Sw", "OD1 Amp Sw"):
            with self.subTest(name=name):
                self.assertEqual(coverage.rule_matches(laws, name, "00/01"), ["d.enum2"])
        self.assertEqual(coverage.rule_matches(laws, "OD Sw", "00/01"), ["enables"])
        self.assertEqual(coverage.rule_matches(laws, "CF Sel", "00/01"), ["enables"])


class StageQualifiedRuleTest(unittest.TestCase):
    """A rule narrowed to a receiving stage wins over the unqualified one it overlaps."""

    CHORUS = "effects.modulation.chorus"
    FLANGER = "effects.modulation.flanger"

    def test_depth_follows_the_receiving_stage(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        match = coverage.rule_matches
        self.assertEqual(match(laws, "Cho Depth", "00–7F", self.CHORUS), ["d.chorus_depth_ms"])
        self.assertEqual(match(laws, "FL Depth", "00–7F", self.FLANGER), ["d.flanger_depth_ms"])
        for stage in ("effects.modulation.ensemble", "effects.delay.stereo"):
            self.assertEqual(match(laws, "Mod Depth", "00–7F", stage), ["d.chorus_depth_ms"])
        self.assertEqual(match(laws, "Depth", "00–7F", "stereo.autoPan"), ["d.unit"])
        self.assertEqual(match(laws, "Depth", "00–7F"), ["d.unit"])

    def test_a_row_is_checked_against_its_own_stage(self) -> None:
        on_chorus = _designed(4, "d.unit", "00–7F", stage=self.CHORUS)
        with self.assertRaises(SystemExit) as caught:
            _tally([on_chorus])
        self.assertIn("d.chorus_depth_ms", str(caught.exception.code))
        ok = _designed(4, "d.chorus_depth_ms", "00–7F", stage=self.CHORUS)
        self.assertEqual(_tally([ok])["counts"]["designed"], 1)

    def test_a_qualified_rule_names_a_stage(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        laws["name_rules"].append({"name": "X", "stage": 3, "law": "d.unit"})
        with self.assertRaises(SystemExit):
            coverage.check_rules(laws, coverage.measured_laws(coverage.DEFAULT_TABLES))


class OrdinalListTest(unittest.TestCase):
    """A row reaching both of two same-named stages lists both ordinals."""

    def test_the_slot_is_counted_once(self) -> None:
        result = _tally([_designed(4, "d.unit", "00–7F", ordinal=[0, 1])])
        self.assertEqual(result["counts"]["designed"], 1)

    def test_a_bad_ordinal_list_is_refused(self) -> None:
        for bad in ([], [0, 0], [0, -1], [0, "1"]):
            with self.subTest(ordinal=bad), self.assertRaises(SystemExit) as caught:
                _tally([_designed(4, "d.unit", "00–7F", ordinal=bad)])
            self.assertIn("ordinal", str(caught.exception.code))

    def test_one_generated_row_per_ordinal_and_one_join_row(self) -> None:
        row = _designed(4, "d.unit", "00–7F", ordinal=[0, 1])
        order = bindings_header.class_order(GS_TOOLS / "derive_efx_tables.py")
        classes = json.loads((GS_TOOLS / "efx-tables.json").read_text(encoding="utf-8"))
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        with _row_dir([row]) as row_dir:
            loaded = bindings_header.load_rows(row_dir)
            entries, _ = bindings_header.collect_rows(loaded, classes["classes"], order, laws)
            joined = join_header.collect(join_header.load_rows(row_dir))
        self.assertEqual(sorted(e["ordinal"] for e in entries), [0, 1])
        self.assertEqual(len(joined), 1)
        self.assertIn("kGsEfxJoinInvented,0x03,", join_header.render(joined).replace(" ", ""))


class SteppedLawTest(unittest.TestCase):
    """A linear or log law with n_states reads a state list's byte as a state index."""

    STAGE = "stereo.autoPan"

    def _laws(self, n_states: int = 3) -> dict:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        laws["laws"]["d.test_step"] = {"form": "linear", "lo": 1, "hi": 3, "n_states": n_states}
        laws["name_rules"].append(
            {"name": "Pre Filter|Depth", "stage": self.STAGE, "law": "d.test_step"}
        )
        return laws

    def assertRefused(self, rows: list[dict], fragment: str, laws: dict) -> None:
        with self.assertRaises(SystemExit) as caught:
            _tally(rows, laws=laws)
        self.assertIn(fragment, str(caught.exception.code))

    def test_a_stepped_law_is_accepted_on_a_state_list(self) -> None:
        result = _tally([_designed(5, "d.test_step", "00/01/02")], laws=self._laws())
        self.assertEqual(result["counts"]["designed"], 1)

    def test_the_state_count_must_match_the_printed_list(self) -> None:
        self.assertRefused([_designed(5, "d.test_step", "00/01/02")], "states", self._laws(4))

    def test_a_stepped_law_is_refused_on_a_continuous_range(self) -> None:
        self.assertRefused([_designed(4, "d.test_step", "00–7F")], "continuous", self._laws())

    def test_a_continuous_law_is_still_refused_on_a_state_list(self) -> None:
        self.assertRefused([_designed(5, "d.unit", "00/01/02")], "state list", self._laws())

    def test_only_linear_and_log_laws_step(self) -> None:
        for law in (
            {"form": "db", "lo": -6, "hi": 0, "n_states": 3},
            {"form": "linear", "lo": 0, "hi": 1, "n_states": 1},
            {"form": "linear", "lo": 0, "hi": 1, "n_states": 17},
        ):
            with self.subTest(law=law), tempfile.TemporaryDirectory() as name:
                laws = json.loads(coverage.DEFAULT_LAWS.read_text(encoding="utf-8"))
                laws["laws"]["d.test_step"] = law
                path = Path(name) / "laws.json"
                path.write_text(json.dumps(laws), encoding="utf-8")
                with self.assertRaises(SystemExit) as caught:
                    coverage.load_laws(path)
                self.assertIn("n_states", str(caught.exception.code))

    def test_the_generated_row_carries_the_state_count(self) -> None:
        law = coverage.designed_law(self._laws(), "d.test_step")
        self.assertEqual(law, {"form": "linear", "lo": 1, "hi": 3, "n_states": 3})
        self.assertEqual(coverage.designed_law(self._laws(), "d.unit")["n_states"], 0)


class StateListLawRuleTest(unittest.TestCase):
    """A state list landing on a physical key follows a stepped law, not a bare index."""

    def test_state_lists_on_physical_keys_follow_stepped_laws(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        cases = (
            ("Hum Type", "00/01", "saturation.bitcrusher", "d.hum_hz", 2),
            ("Ratio", "00/01/02/03", "dynamics.limiter", "d.limiter_ratio", 4),
            ("Pre Filter", "00–05", "saturation.bitcrusher", "d.lofi_filter_hz", 6),
            ("Post Filter", "00–05", "saturation.bitcrusher", "d.lofi_filter_hz", 6),
            ("Type", "00/01/02/03/04/05", "effects.reverb.dattorro", "d.type_index6", 6),
            ("Lo-Fi Type", "00–08", "saturation.bitcrusher", "d.type_index9", 9),
            ("Lo-Fi Type", "00–05", "saturation.bitcrusher", "d.type_index6", 6),
        )
        for name, printed, stage, law, n_states in cases:
            with self.subTest(name=name, printed=printed):
                self.assertEqual(coverage.rule_matches(laws, name, printed, stage), [law])
                self.assertEqual(coverage.designed_law(laws, law)["n_states"], n_states)
        gate = coverage.rule_matches(laws, "Type", "00/01/02/03", "effects.reverb.dattorro")
        self.assertEqual(gate, ["d.enum4"])

    def test_the_vowel_drive_is_a_fraction(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        vowel = coverage.rule_matches(laws, "Drive", "00–7F", "effects.filter.vowel")
        self.assertEqual(vowel, ["d.unit"])
        amp = coverage.rule_matches(laws, "OD Drive", "00–7F", "saturation.ampSim")
        self.assertEqual(amp, ["drive.gain"])

    def test_a_drive_switch_is_a_two_state_enum(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        self.assertEqual(coverage.rule_matches(laws, "Drive Sw", "00/01"), ["d.enum2"])

    def test_accel_is_a_glide_except_on_the_rotary(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        match = coverage.rule_matches
        self.assertEqual(match(laws, "Accel", "*14", "effects.delay.stereo"), ["d.glide_ms"])
        self.assertEqual(match(laws, "Accel", "*14", "effects.filter.vowel"), ["d.glide_ms"])
        rotary = "effects.modulation.rotary"
        self.assertEqual(match(laws, "RT Hi Accl", "*14", rotary), ["accel.rotor"])

    def test_tremolo_phase_is_in_degrees_on_the_ring_modulator(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        ring = "effects.modulation.ringModulator"
        self.assertEqual(coverage.rule_matches(laws, "Trem Phase", "00–5A", ring), ["d.phase_deg"])


class StandingClaimTest(unittest.TestCase):
    """A carried law resting on a standing claim that names the pair outranks the name rule."""

    CLAIM = "a-standing-claim"
    PAIR = ("01 42", "40 03 07")

    def _carried(self, **replaced_when) -> dict:
        row = _designed(4, "level.output", "00–7F")
        row["designed"] = {
            "basis": "carried",
            "law": "level.output",
            "from": self.CLAIM,
            "replaced_when": replaced_when or {"table_reaches": list(self.PAIR)},
        }
        return row

    def test_a_standing_claim_naming_the_pair_is_accepted_over_the_rule(self) -> None:
        result = _tally([self._carried()], claims={self.CLAIM: {self.PAIR}})
        self.assertEqual(result["basis"]["carried"], 1)
        self.assertEqual(result["names_checked"], 1)

    def test_without_the_claim_the_rule_still_decides(self) -> None:
        for claims in (None, {}, {self.CLAIM: {("01 42", "40 03 08")}}):
            with self.subTest(claims=claims), self.assertRaises(SystemExit) as caught:
                _tally([self._carried()], claims=claims)
            self.assertIn("d.unit", str(caught.exception.code))

    def test_table_reaches_names_a_type_and_an_address(self) -> None:
        claims = {self.CLAIM: {self.PAIR}}
        for bad in (["01 42"], ["01 42", "07"], "01 42", ["1 42", "40 03 07"]):
            with self.subTest(bad=bad), self.assertRaises(SystemExit) as caught:
                _tally([self._carried(table_reaches=bad)], claims=claims)
            self.assertIn("table_reaches", str(caught.exception.code))

    def test_claim_pairs_are_read_from_standing_claims_with_a_quantity(self) -> None:
        def claim(state: str, quantities: list[str]) -> dict:
            return {
                "inference": {
                    "state": state,
                    "about": {
                        "types": ["03 00"],
                        "addresses": ["40 03 07"],
                        "quantities": quantities,
                    },
                },
                "rests_on": [
                    "data/units/u/efx-params/40-03-07-x.json",
                    "data/units/u/x/01-42-07.json",
                ],
            }

        with tempfile.TemporaryDirectory() as name:
            root = Path(name) / "inferences" / coverage.CLAIM_UNIT
            root.mkdir(parents=True)
            for stem, body in (
                ("standing", claim("standing", ["rate_hz"])),
                ("parked", claim("parked", ["rate_hz"])),
                ("no-quantity", claim("standing", [])),
            ):
                (root / f"{stem}.json").write_text(json.dumps(body), encoding="utf-8")
            pairs = coverage.load_claims(Path(name))
        self.assertEqual(pairs["standing"], {("02 0C", "40 03 07"), ("01 42", "40 03 07")})
        self.assertEqual(pairs["parked"], set())
        self.assertEqual(pairs["no-quantity"], set())


class CentRatioTest(unittest.TestCase):
    """A fine-tune printed in cents is a translated ratio, read on table 2 as printed."""

    def test_fine_is_a_cent_ratio(self) -> None:
        laws = coverage.load_laws(coverage.DEFAULT_LAWS)
        self.assertEqual(coverage.rule_matches(laws, "PS Fine", "0E–72"), ["ratio.cent"])
        self.assertEqual(bindings_header.BINDING_LAWS["ratio"].index("cent"), 2)


if __name__ == "__main__":
    unittest.main()
