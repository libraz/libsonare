"""Stdlib self-tests for the token-table check.

Each case builds a small tree holding one closed set in every source kind the
check reads, confirms the clean tree passes, then plants one defect and requires
the check to name it. The reorder case keeps every source's token set intact, so
it fails only through the ordinal comparison, not through a set comparison.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_token_tables", Path(__file__).resolve().parent / "check_token_tables.py"
)
assert _SPEC and _SPEC.loader
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)

_FILES = {
    "src/core.cpp": """
const char* to_string(Law law) {
  switch (law) {
    case Law::SoftKnee:
      return "softKnee";
    case Law::Hard45:
      return "hard4.5";
  }
  return "softKnee";
}

Law law_from_string(const std::string& value) {
  if (value == "softKnee" || value == "soft") return Law::SoftKnee;
  if (value == "hard4.5") return Law::Hard45;
  throw SonareException(ErrorCode::InvalidFormat, "unknown law: " + value);
}
""",
    "include/c.h": """
typedef enum { SONARE_LAW_SOFT_KNEE = 0, SONARE_LAW_HARD_4P5 = 1 } SonareLaw;
""",
    "ts/values.ts": "const LAW_VALUES: Record<Law, number> = {\n  softKnee: 0,\n  'hard4.5': 1,\n};\n",
    "ts/list.ts": "const LAWS: readonly string[] = ['softKnee', 'hard4.5'];\n",
    "ts/types.ts": "export type Law = 'softKnee' | 'hard4.5';\n",
    "py/enums.py": 'Law = Literal["softKnee", "hard4.5"]\n_LAWS: tuple[str, ...] = get_args(Law)\n',
}

_ROW = check.TokenSet(
    name="law",
    core=check.CoreTable("src/core.cpp", "Law", "law_from_string"),
    earlier_forms=("soft",),
    c_enum=("include/c.h", "SONARE_LAW_"),
    c_aliases={"hard4.5": "HARD_4P5"},
    sources=(
        check.Source("node", "ts/values.ts", "ts_object", "LAW_VALUES"),
        check.Source("node", "ts/list.ts", "ts_array", "LAWS"),
        check.Source("node", "ts/types.ts", "ts_union", "Law"),
        check.Source("python", "py/enums.py", "py_literal", "Law"),
        check.Source("python", "py/enums.py", "py_tuple", "_LAWS"),
    ),
)


class TokenTableTests(unittest.TestCase):
    def run_with(
        self, edits: dict[str, tuple[str, str]] | None = None
    ) -> tuple[list[str], int, int]:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for rel, text in _FILES.items():
                old, new = (edits or {}).get(rel, ("", ""))
                if old:
                    self.assertIn(old, text, f"fixture edit does not apply to {rel}")
                    text = text.replace(old, new)
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text)
            return check.evaluate(root, (_ROW,))

    def test_clean_tree_passes_and_compares_every_source(self) -> None:
        problems, rows, compared = self.run_with()
        self.assertEqual(problems, [])
        self.assertEqual((rows, compared), (1, len(_ROW.sources)))

    def test_an_extra_token_is_named(self) -> None:
        problems, _, _ = self.run_with({"ts/types.ts": ("'hard4.5';", "'hard4.5' | 'linear';")})
        self.assertEqual(problems, ["law: node Law (ts/types.ts): extra token 'linear'"])

    def test_a_missing_token_is_named(self) -> None:
        problems, _, _ = self.run_with({"py/enums.py": ('"softKnee", "hard4.5"]', '"softKnee"]')})
        self.assertIn("law: python Law (py/enums.py): missing token 'hard4.5'", problems)
        self.assertIn("law: python _LAWS (py/enums.py): missing token 'hard4.5'", problems)

    def test_a_reordered_list_fails_on_ordinals_alone(self) -> None:
        problems, _, _ = self.run_with(
            {"ts/list.ts": ("['softKnee', 'hard4.5']", "['hard4.5', 'softKnee']")}
        )
        self.assertEqual(
            problems,
            [
                "law: node LAWS (ts/list.ts): token 'hard4.5' has ordinal 0, the C ABI's is 1",
                "law: node LAWS (ts/list.ts): token 'softKnee' has ordinal 1, the C ABI's is 0",
            ],
        )

    def test_a_swapped_ordinal_in_a_table_is_named(self) -> None:
        problems, _, _ = self.run_with({"ts/values.ts": ("softKnee: 0", "softKnee: 1")})
        self.assertEqual(
            problems,
            [
                "law: node LAW_VALUES (ts/values.ts): token 'softKnee' has ordinal 1, the C ABI's is 0"
            ],
        )

    def test_a_core_token_with_no_enumerator_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"include/c.h": ("SONARE_LAW_HARD_4P5 = 1", "SONARE_LAW_HARD = 1")}
        )
        self.assertIn("law: core token 'hard4.5' matches no SONARE_LAW_* enumerator", problems)

    def test_a_missing_core_table_is_named(self) -> None:
        problems, _, compared = self.run_with({"src/core.cpp": ("law_from_string", "law_parse")})
        self.assertEqual(problems, ["law: no tokens found in law_from_string (src/core.cpp)"])
        self.assertEqual(compared, 0)

    def test_a_missing_writer_is_named(self) -> None:
        problems, _, compared = self.run_with(
            {"src/core.cpp": ("to_string(Law law)", "name_of(Law law)")}
        )
        self.assertEqual(problems, ["law: no tokens found in to_string(Law) (src/core.cpp)"])
        self.assertEqual(compared, 0)

    def test_an_undeclared_third_spelling_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"src/core.cpp": ('value == "soft")', 'value == "soft" || value == "s")')}
        )
        self.assertEqual(problems, ["law: law_from_string accepts undeclared 's'"])

    def test_an_earlier_form_no_longer_read_is_named(self) -> None:
        problems, _, _ = self.run_with({"src/core.cpp": (' || value == "soft"', "")})
        self.assertEqual(problems, ["law: law_from_string does not accept 'soft'"])

    def test_an_earlier_form_still_written_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"src/core.cpp": ('return "softKnee";\n    case', 'return "soft";\n    case')}
        )
        self.assertIn("law: earlier form 'soft' is still written", problems)

    def test_a_missing_source_is_named(self) -> None:
        problems, _, compared = self.run_with({"ts/list.ts": ("const LAWS", "const OTHER")})
        self.assertEqual(problems, ["law: node LAWS (ts/list.ts): not found"])
        self.assertEqual(compared, len(_ROW.sources) - 1)

    def test_zero_rows_compares_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(check.evaluate(Path(d), ()), ([], 0, 0))

    def test_the_shipped_rows_are_not_empty(self) -> None:
        self.assertGreater(len(check.ROWS), 0)



_KIND_FILES = {
    "src/room.cpp": """
const char* room_name(Room room) {
  switch (room) {
    case Room::Living:
      return "livingRoom";
    case Room::Theater:
      return "homeTheater";
  }
  return "livingRoom";
}

const char* legacy_room_name(Room room) {
  switch (room) {
    case Room::Living:
      return "living_room";
    case Room::Theater:
      return "home_theater";
  }
  return "living_room";
}

Room room_from_text(const std::string& value) {
  if (value == room_name(Room::Living) || value == legacy_room_name(Room::Living)) return Room::Living;
  return Room::Theater;
}
""",
    "src/bed.cpp": """
const char* bed_room(Room room) {
  return room == Room::Living ? "livingRoom" : "homeTheater";
}
""",
    "ts/config.ts": "export interface RoomConfig {\n  preset?: 'livingRoom' | 'homeTheater';\n}\n",
    "ts/narrow.ts": "export type NarrowRoom = 'livingRoom';\n",
    "py/config.py": 'class RoomConfig(TypedDict):\n    preset: Literal["livingRoom", "homeTheater"]\n',
    "py/alias.py": 'Room: TypeAlias = Literal["livingRoom", "homeTheater"]\n',
}

_KIND_ROW = check.TokenSet(
    name="room",
    core=check.CoreTable("src/room.cpp", "Room", writer="room_name"),
    also_written_by=(check.CoreTable("src/bed.cpp", "Room", writer="bed_room"),),
    sources=(
        check.Source("node", "ts/config.ts", "ts_field", "RoomConfig.preset"),
        check.Source("node", "ts/narrow.ts", "ts_union", "NarrowRoom", subset=True),
        check.Source("python", "py/config.py", "py_field", "RoomConfig.preset"),
        check.Source("python", "py/alias.py", "py_literal", "Room"),
    ),
)


class SourceKindTests(unittest.TestCase):
    """Field unions, annotated aliases, subset sources, legacy writers and second writers."""

    def run_with(self, edits: dict[str, tuple[str, str]] | None = None) -> tuple[list[str], int, int]:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for rel, text in _KIND_FILES.items():
                old, new = (edits or {}).get(rel, ("", ""))
                if old:
                    self.assertIn(old, text, f"fixture edit does not apply to {rel}")
                    text = text.replace(old, new)
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text)
            return check.evaluate(root, (_KIND_ROW,))

    def test_clean_tree_passes_and_reads_every_kind(self) -> None:
        self.assertEqual(self.run_with(), ([], 1, len(_KIND_ROW.sources)))

    def test_an_extra_token_in_a_ts_field_is_named(self) -> None:
        problems, _, _ = self.run_with({"ts/config.ts": ("'homeTheater';", "'homeTheater' | 'cinema';")})
        self.assertEqual(problems, ["room: node RoomConfig.preset (ts/config.ts): extra token 'cinema'"])

    def test_a_missing_token_in_a_py_field_is_named(self) -> None:
        problems, _, _ = self.run_with({"py/config.py": (', "homeTheater"', "")})
        self.assertEqual(
            problems, ["room: python RoomConfig.preset (py/config.py): missing token 'homeTheater'"]
        )

    def test_an_annotated_alias_is_read(self) -> None:
        problems, _, _ = self.run_with({"py/alias.py": ('"homeTheater"', '"home_theater"')})
        self.assertEqual(
            problems,
            [
                "room: python Room (py/alias.py): extra token 'home_theater'",
                "room: python Room (py/alias.py): missing token 'homeTheater'",
            ],
        )

    def test_a_subset_source_may_omit_but_not_add(self) -> None:
        problems, _, _ = self.run_with({"ts/narrow.ts": ("'livingRoom';", "'livingRoom' | 'cinema';")})
        self.assertEqual(problems, ["room: node NarrowRoom (ts/narrow.ts): extra token 'cinema'"])

    def test_a_legacy_writer_nobody_reads_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"src/room.cpp": (" || value == legacy_room_name(Room::Living)", "")}
        )
        self.assertEqual(problems, ["room: src/room.cpp does not read what legacy_room_name writes"])

    def test_a_second_writer_that_drifts_is_named(self) -> None:
        problems, _, _ = self.run_with({"src/bed.cpp": ('"homeTheater"', '"theater"')})
        self.assertEqual(
            problems,
            [
                (
                    "room: bed_room (src/bed.cpp) writes ['livingRoom', 'theater'], "
                    "room_name writes ['homeTheater', 'livingRoom']"
                )
            ],
        )


_POP_FILES = {
    "src/room.cpp": _KIND_FILES["src/room.cpp"],
    "src/law.cpp": """
const char* to_string(Law law) {
  switch (law) {
    case Law::Soft:
      return "soft";
    case Law::Hard:
      return "hard";
    case Law::Linear:
      return "linear";
  }
  return "soft";
}

const char* law_text(Law law) {
  switch (law) {
    case Law::Soft:
      return "soft";
    case Law::Hard:
      return "hard";
  }
  return "soft";
}
""",
    "bindings/node/src/config.ts": _KIND_FILES["ts/config.ts"],
    "bindings/node/src/law.ts": "export type Law = 'soft' | 'hard';\nexport type Tap = 'pre' | 'post';\n",
    "bindings/python/src/pkg/config.py": _KIND_FILES["py/config.py"],
}

_POP_ROWS = (
    check.TokenSet(
        name="room",
        core=check.CoreTable("src/room.cpp", "Room", writer="room_name"),
        sources=(
            check.Source("node", "bindings/node/src/config.ts", "ts_field", "RoomConfig.preset"),
            check.Source("python", "bindings/python/src/pkg/config.py", "py_field", "RoomConfig.preset"),
        ),
    ),
    check.TokenSet(
        name="law",
        core=check.CoreTable("src/law.cpp", "Law", writer="law_text"),
        sources=(check.Source("node", "bindings/node/src/law.ts", "ts_union", "Law"),),
    ),
)


class PopulationTests(unittest.TestCase):
    def run_with(
        self,
        edits: dict[str, tuple[str, str]] | None = None,
        rows: tuple = _POP_ROWS,
        exempt: dict[tuple[str, str], str] | None = None,
    ) -> tuple[list[str], int, int]:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for rel, text in _POP_FILES.items():
                old, new = (edits or {}).get(rel, ("", ""))
                if old:
                    self.assertIn(old, text, f"fixture edit does not apply to {rel}")
                    text = text.replace(old, new)
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text)
            return check.evaluate_population(root, rows, exempt or {})

    def test_clean_tree_passes_and_draws_on_the_closest_writer(self) -> None:
        # Law draws on law_text, which matches exactly, not on the wider to_string.
        self.assertEqual(self.run_with(), ([], 3, 2))

    def test_an_unpinned_set_is_named(self) -> None:
        problems, _, _ = self.run_with(rows=(_POP_ROWS[1],))
        self.assertEqual(
            problems,
            [
                (
                    "RoomConfig.preset (bindings/node/src/config.ts) spells the tokens of "
                    "room_name(Room) in src/room.cpp and no row pins it"
                ),
                (
                    "RoomConfig.preset (bindings/python/src/pkg/config.py) spells the tokens of "
                    "room_name(Room) in src/room.cpp and no row pins it"
                ),
                "room_name(Room) in src/room.cpp writes a set a facade spells and no row names it",
            ],
        )

    def test_an_earlier_spelling_still_draws_on_its_writer(self) -> None:
        problems, sets, _ = self.run_with(
            {"bindings/node/src/config.ts": ("'homeTheater'", "'home_theater'")}
        )
        self.assertEqual((problems, sets), ([], 3))

    def test_a_writer_no_row_names_is_named(self) -> None:
        rows = (_POP_ROWS[0], _POP_ROWS[1]._replace(core=check.CoreTable("src/law.cpp", "Law")))
        problems, _, _ = self.run_with(rows=rows)
        self.assertEqual(
            problems, ["law_text(Law) in src/law.cpp writes a set a facade spells and no row names it"]
        )

    def test_an_exemption_that_excuses_nothing_is_named(self) -> None:
        problems, _, _ = self.run_with(exempt={("bindings/node/src/law.ts", "Tap"): "no core writer"})
        self.assertEqual(problems, ["exemption for Tap (bindings/node/src/law.ts) excuses nothing"])

    def test_an_exempt_set_is_not_drawn(self) -> None:
        problems, _, writers = self.run_with(
            rows=(_POP_ROWS[0],), exempt={("bindings/node/src/law.ts", "Law"): "fixture"}
        )
        self.assertEqual((problems, writers), ([], 1))

    def test_the_shipped_exemptions_carry_a_reason(self) -> None:
        for key, reason in check.EXEMPT.items():
            self.assertTrue(reason.strip(), key)


_KEY_FILES = {
    "src/scene.cpp": """
Insert insert_from_value(const JsonValue& object, const std::string& path) {
  static const KeySet kKnown = known_keys("inserts[]", {"processor_name"});
  insert.processor = string_or_legacy(object, path, "processor", "processor_name", "");
}
void insert_to_value(JsonValue& object, const Insert& insert) {
  util::json::put(object, "processor", insert.processor);
}
""",
    "bindings/wasm/src/mirror.ts": "const processor = aliasedValue(object, 'processor', 'processor_name');\n",
}

_KEY_FORM = check.KeyForm(
    "insert processor", "src/scene.cpp", "processor", "processor_name", ("bindings/wasm/src/mirror.ts",)
)


class EarlierKeyTests(unittest.TestCase):
    def run_with(
        self, edits: dict[str, tuple[str, str]] | None = None, extra: dict[str, str] | None = None
    ) -> tuple[list[str], int, int]:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for rel, text in {**_KEY_FILES, **(extra or {})}.items():
                old, new = (edits or {}).get(rel, ("", ""))
                if old:
                    self.assertIn(old, text, f"fixture edit does not apply to {rel}")
                    text = text.replace(old, new)
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text)
            return check.evaluate_key_forms(root, (_KEY_FORM,))

    def test_clean_tree_passes_and_matches_core_and_mirror(self) -> None:
        self.assertEqual(self.run_with(), ([], 1, 2))

    def test_an_earlier_key_no_longer_read_is_named(self) -> None:
        problems, _, matched = self.run_with(
            {"src/scene.cpp": ('"processor", "processor_name", ""', '"processor", ""')}
        )
        self.assertIn(
            "insert processor: src/scene.cpp does not read 'processor_name' beside 'processor'",
            problems,
        )
        self.assertEqual(matched, 1)

    def test_an_earlier_key_written_by_the_core_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"src/scene.cpp": ('put(object, "processor",', 'put(object, "processor_name",')}
        )
        self.assertIn("insert processor: src/scene.cpp writes no 'processor'", problems)
        self.assertIn(
            "insert processor: 'processor_name' appears outside a reader at src/scene.cpp:7",
            problems,
        )

    def test_an_earlier_key_written_by_a_facade_is_named(self) -> None:
        problems, _, _ = self.run_with(
            extra={"bindings/node/src/scene.ts": "out['processor_name'] = name;\n"}
        )
        self.assertEqual(
            problems,
            [
                (
                    "insert processor: 'processor_name' appears outside a reader at "
                    "bindings/node/src/scene.ts:1"
                )
            ],
        )

    def test_a_mirror_that_drops_the_earlier_key_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"bindings/wasm/src/mirror.ts": ("aliasedValue(object, 'processor', 'processor_name')", "object.processor")}
        )
        self.assertEqual(
            problems,
            [
                (
                    "insert processor: bindings/wasm/src/mirror.ts does not read "
                    "'processor_name' beside 'processor'"
                )
            ],
        )

    def test_the_shipped_key_forms_are_not_empty(self) -> None:
        self.assertGreater(len(check.KEY_FORMS), 0)

if __name__ == "__main__":
    unittest.main()
