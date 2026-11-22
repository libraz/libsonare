"""Stdlib self-tests for the token-table check.

Each case builds a small tree holding one closed set in every source kind the
check reads, confirms the clean tree passes, then plants one defect and requires
the check to name it. The reorder case keeps every source's token set intact, so
it fails only through the ordinal comparison, not through a set comparison.
"""

from __future__ import annotations

import importlib.util
import tempfile
import typing
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
  if (value == "softKnee") return Law::SoftKnee;
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
    refused_forms=("soft",),
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
            {"src/core.cpp": ('value == "softKnee")', 'value == "softKnee" || value == "s")')}
        )
        self.assertEqual(problems, ["law: law_from_string accepts undeclared 's'"])

    def test_a_refused_form_still_accepted_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"src/core.cpp": ('value == "softKnee")', 'value == "softKnee" || value == "soft")')}
        )
        self.assertEqual(problems, ["law: law_from_string still accepts refused form 'soft'"])

    def test_a_refused_form_still_written_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"src/core.cpp": ('return "softKnee";\n    case', 'return "soft";\n    case')}
        )
        self.assertIn("law: refused form 'soft' is still written", problems)

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

Room room_from_text(const std::string& value) {
  if (value == room_name(Room::Living)) return Room::Living;
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



_NAME_TABLE_CORE = """
constexpr const char* kLawNames[] = {"softKnee", "hard4.5"};
constexpr size_t kLawCount = sizeof(kLawNames) / sizeof(kLawNames[0]);

const char* to_string(Law law) {
  const auto index = static_cast<size_t>(law);
  return index < kLawCount ? kLawNames[index] : "unknown";
}

Law law_from_string(const std::string& value) {
  for (size_t i = 0; i < kLawCount; ++i) {
    if (value == kLawNames[i]) return static_cast<Law>(i);
  }
  throw SonareException(ErrorCode::InvalidFormat, "unknown law: " + value);
}
"""


class NameTableTests(unittest.TestCase):
    """A writer and parser that index one name table are read through the table."""

    def run_with(self, core: str = _NAME_TABLE_CORE) -> tuple[list[str], int, tuple[int, int]]:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for rel, text in {**_FILES, "src/core.cpp": core}.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text)
            problems, _, compared = check.evaluate(root, (_ROW,))
            return problems, compared, check.name_table_reads(root, (_ROW,))

    def test_the_table_is_the_written_and_accepted_set(self) -> None:
        problems, compared, reads = self.run_with()
        self.assertEqual(problems, [])
        self.assertEqual(compared, len(_ROW.sources))
        self.assertEqual(reads, (1, 2))
        self.assertEqual(check.written_tokens(_NAME_TABLE_CORE, "Law"), ["softKnee", "hard4.5"])

    def test_the_out_of_range_fallback_is_not_a_token(self) -> None:
        self.assertNotIn("unknown", check.written_tokens(_NAME_TABLE_CORE, "Law") or [])

    def test_a_wrong_table_entry_is_named(self) -> None:
        problems, _, _ = self.run_with(_NAME_TABLE_CORE.replace('"hard4.5"}', '"hard"}'))
        self.assertIn("law: core token 'hard' matches no SONARE_LAW_* enumerator", problems)
        self.assertIn("law: node Law (ts/types.ts): extra token 'hard4.5'", problems)

    def test_a_table_the_file_does_not_define_is_not_read_as_clean(self) -> None:
        core = _NAME_TABLE_CORE.replace(
            'constexpr const char* kLawNames[] = {"softKnee", "hard4.5"};\n', ""
        )
        problems, _, reads = self.run_with(core)
        self.assertNotEqual(problems, [])
        self.assertEqual(reads, (0, 0))

class SourceKindTests(unittest.TestCase):
    """Field unions, annotated aliases, subset sources, a left-over legacy writer and second writers."""

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

    def test_a_legacy_writer_left_in_the_core_is_named(self) -> None:
        legacy = (
            "\nRoom room_from_text(",
            '\nconst char* legacy_room_name(Room room) {\n  return "living_room";\n}\n\nRoom room_from_text(',
        )
        problems, _, _ = self.run_with({"src/room.cpp": legacy})
        self.assertEqual(
            problems, ["room: src/room.cpp still has legacy_room_name; an earlier form is refused"]
        )

    def test_a_fallback_past_a_switch_is_not_a_token(self) -> None:
        # room_name returns "livingRoom" past its switch; an "unknown" there names no enumerator.
        problems, _, _ = self.run_with(
            {"src/room.cpp": ('  }\n  return "livingRoom";', '  }\n  return "unknown";')}
        )
        self.assertEqual(problems, [])

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

    def test_a_derived_const_tuple_is_a_set(self) -> None:
        tuple_set = "export const ROOMS = ['livingRoom', 'homeTheater'] as const;\n"
        problems, sets, _ = self.run_with(
            {"bindings/node/src/law.ts": ("export type Tap", tuple_set + "export type Tap")}
        )
        self.assertEqual(sets, 4)
        self.assertEqual(
            problems,
            [
                (
                    "ROOMS (bindings/node/src/law.ts) spells the tokens of room_name(Room) in "
                    "src/room.cpp and no row pins it"
                )
            ],
        )

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
  static const KeySet kKnown = known_keys("inserts[]");
  insert.processor = string_or_earlier(object, path, "processor", "processor_name", "");
}
void insert_to_value(JsonValue& object, const Insert& insert) {
  util::json::put(object, "processor", insert.processor);
}
""",
    "bindings/wasm/src/mirror.ts": "const processor = typeof object.processor === 'string' ? object.processor : '';\n",
}

_KEY_FORM = check.KeyForm(
    "insert processor", "src/scene.cpp", "processor", "processor_name", ("bindings/wasm/src/mirror.ts",)
)


class RefusedKeyTests(unittest.TestCase):
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

    def test_a_core_that_stops_refusing_is_named(self) -> None:
        problems, _, matched = self.run_with(
            {"src/scene.cpp": ('"processor", "processor_name", ""', '"processor", ""')}
        )
        self.assertEqual(
            problems,
            ["insert processor: src/scene.cpp does not refuse 'processor_name' beside 'processor'"],
        )
        self.assertEqual(matched, 1)

    def test_a_core_that_reads_the_earlier_key_again_is_named(self) -> None:
        problems, _, _ = self.run_with({"src/scene.cpp": ("string_or_earlier(", "string_or_legacy(")})
        self.assertIn("insert processor: src/scene.cpp still reads 'processor_name'", problems)
        self.assertIn(
            "insert processor: src/scene.cpp does not refuse 'processor_name' beside 'processor'",
            problems,
        )

    def test_an_earlier_key_declared_known_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"src/scene.cpp": ('known_keys("inserts[]")', 'known_keys("inserts[]", {"processor_name"})')}
        )
        self.assertEqual(
            problems,
            ["insert processor: 'processor_name' appears outside the refusal at src/scene.cpp:3"],
        )

    def test_an_earlier_key_written_by_the_core_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"src/scene.cpp": ('put(object, "processor",', 'put(object, "processor_name",')}
        )
        self.assertEqual(
            problems,
            [
                "insert processor: src/scene.cpp writes no 'processor'",
                "insert processor: 'processor_name' appears outside the refusal at src/scene.cpp:7",
            ],
        )

    def test_an_earlier_key_read_by_a_facade_is_named(self) -> None:
        problems, _, _ = self.run_with(
            extra={"bindings/node/src/scene.ts": "const name = object['processor_name'];\n"}
        )
        self.assertEqual(
            problems,
            [
                (
                    "insert processor: 'processor_name' appears outside the refusal at "
                    "bindings/node/src/scene.ts:1"
                )
            ],
        )

    def test_a_mirror_that_reads_the_earlier_key_is_named(self) -> None:
        problems, _, _ = self.run_with(
            {"bindings/wasm/src/mirror.ts": ("object.processor : ''", "object.processor : object['processor_name']")}
        )
        self.assertEqual(
            problems,
            [
                (
                    "insert processor: 'processor_name' appears outside the refusal at "
                    "bindings/wasm/src/mirror.ts:1"
                )
            ],
        )

    def test_a_mirror_that_drops_the_current_key_is_named(self) -> None:
        problems, _, matched = self.run_with(
            {"bindings/wasm/src/mirror.ts": ("typeof object.processor === 'string' ? object.processor : ''", "''")}
        )
        self.assertEqual(
            problems, ["insert processor: bindings/wasm/src/mirror.ts does not read 'processor'"]
        )
        self.assertEqual(matched, 1)

    def test_the_shipped_key_forms_are_not_empty(self) -> None:
        self.assertGreater(len(check.KEY_FORMS), 0)


class NotComparedTests(unittest.TestCase):
    def test_an_entry_a_row_now_names_is_named(self) -> None:
        declared = {("src/law.cpp", "law_text", "Law"): "fixture"}
        problems, lines = check.not_compared(Path("."), _POP_ROWS, declared)
        self.assertEqual(problems, ["not-compared entry law_text(Law) in src/law.cpp is named by a row"])
        self.assertEqual(lines, ["law_text(Law) in src/law.cpp: fixture"])

    def test_the_shipped_entries_carry_a_reason_and_name_no_row(self) -> None:
        problems, _ = check.not_compared(Path("."))
        self.assertEqual(problems, [])
        for key, reason in check.NOT_COMPARED.items():
            self.assertTrue(reason.strip(), key)


_FAMILY_FILES = {
    "src/tables.h": """
constexpr const char* enum_choice_name(sonare::fx::Shape value) {
  switch (value) {
    case sonare::fx::Shape::Sine:
      return "sine";
    case sonare::fx::Shape::SawUp:
      return "sawUp";
    case sonare::fx::Shape::Square:
      return "square";
  }
  return "sine";
}
constexpr const char* enum_choice_name(
    sonare::fx::Config::Detector value) {
  switch (value) {
    case sonare::fx::Config::Detector::Peak:
      return "peak";
    case sonare::fx::Config::Detector::Rms:
      return "rms";
  }
  return "peak";
}
""",
    "bindings/node/src/fx.ts": (
        "export type Shape = 'sine' | 'sawUp' | 'square';\n"
        "export type Detector = 'peak' | 'rms';\n"
        "export type Synth = 'sine' | 'noise' | 'default';\n"
    ),
    "bindings/python/src/pkg/fx.py": 'Shape = Literal["sine", "sawUp", "square"]\n',
}


class FamilyTests(unittest.TestCase):
    def run_with(
        self,
        edits: dict[str, tuple[str, str]] | None = None,
        exempt: dict[tuple[str, str], str] | None = None,
        stages: dict[tuple[str, str], check.StageReader] | None = None,
        extra: dict[str, str] | None = None,
    ) -> tuple[list[str], int, int]:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for rel, text in {**_FAMILY_FILES, **(extra or {})}.items():
                old, new = (edits or {}).get(rel, ("", ""))
                if old:
                    self.assertIn(old, text, f"fixture edit does not apply to {rel}")
                    text = text.replace(old, new)
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text)
            return check.evaluate_family(root, (), exempt or {}, stages or {})

    def test_clean_tree_compares_every_set_drawing_on_a_qualified_overload(self) -> None:
        # Synth shares one token only and is a different vocabulary, so it draws on nothing.
        self.assertEqual(self.run_with(), ([], 3, 2))

    def test_a_respelled_union_is_named(self) -> None:
        problems, _, _ = self.run_with({"bindings/node/src/fx.ts": ("'sawUp' | 'square'", "'saw_up' | 'square'")})
        self.assertEqual(
            problems,
            [
                (
                    "Shape (bindings/node/src/fx.ts) disagrees with enum_choice_name(sonare::fx::Shape) in "
                    "src/tables.h: missing ['sawUp'], extra ['saw_up']"
                )
            ],
        )

    def test_a_union_missing_a_token_is_named(self) -> None:
        problems, _, _ = self.run_with({"bindings/python/src/pkg/fx.py": (', "square"', "")})
        self.assertEqual(
            problems,
            [
                (
                    "Shape (bindings/python/src/pkg/fx.py) disagrees with enum_choice_name(sonare::fx::Shape) in "
                    "src/tables.h: missing ['square'], extra []"
                )
            ],
        )

    def test_a_generated_block_is_not_read(self) -> None:
        generated = (
            "// BEGIN GENERATED fx (make processor-types)\n"
            "export type Generated = 'sine' | 'sawUp';\n"
            "// END GENERATED fx\n"
        )
        problems, sets, _ = self.run_with({"bindings/node/src/fx.ts": ("export type Synth", generated + "export type Synth")})
        self.assertEqual((problems, sets), ([], 3))

    def test_an_exempt_set_is_not_compared_and_a_stale_exemption_is_named(self) -> None:
        exempt = {("bindings/node/src/fx.ts", "Detector"): "fixture", ("bindings/node/src/fx.ts", "Synth"): "fixture"}
        problems, sets, _ = self.run_with(exempt=exempt)
        self.assertEqual(sets, 2)
        self.assertEqual(problems, ["family exemption for Synth (bindings/node/src/fx.ts) excuses nothing"])

    _STAGE: typing.ClassVar[dict[str, str]] = {
        "src/stage.cpp": (
            "void validate(const Config& config) {\n"
            "  if (config.shape != sonare::fx::Shape::Sine &&\n"
            "      config.shape != sonare::fx::Shape::Square) {\n"
            '    throw SonareException(ErrorCode::InvalidParameter, "stage shape not supported");\n'
            "  }\n"
            "}\n"
        ),
        "bindings/node/src/stage.ts": "export interface StageConfig {\n  shape?: 'sine' | 'square';\n}\n",
    }
    _STAGE_KEY: typing.ClassVar[tuple[str, str]] = ("bindings/node/src/stage.ts", "StageConfig.shape")

    def test_a_stage_set_is_compared_against_what_its_reader_accepts(self) -> None:
        stages = {self._STAGE_KEY: check.StageReader("src/stage.cpp", "stage shape not supported")}
        self.assertEqual(self.run_with(stages=stages, extra=self._STAGE), ([], 4, 2))

    def test_a_stage_set_wider_than_its_reader_is_named(self) -> None:
        stages = {self._STAGE_KEY: check.StageReader("src/stage.cpp", "stage shape not supported")}
        extra = {**self._STAGE, "bindings/node/src/stage.ts": "export interface StageConfig {\n  shape?: 'sine' | 'sawUp' | 'square';\n}\n"}
        problems, _, _ = self.run_with(stages=stages, extra=extra)
        self.assertEqual(
            problems,
            [
                (
                    "StageConfig.shape (bindings/node/src/stage.ts) disagrees with what src/stage.cpp accepts of "
                    "enum_choice_name(sonare::fx::Shape): missing [], extra ['sawUp']"
                )
            ],
        )

    def test_without_its_reader_a_stage_set_reads_as_family_drift(self) -> None:
        problems, _, _ = self.run_with(extra=self._STAGE)
        self.assertEqual(len(problems), 1)
        self.assertIn("missing ['sawUp']", problems[0])

    def test_a_stage_reader_whose_refusal_moved_or_names_no_set_is_named(self) -> None:
        stages = {
            self._STAGE_KEY: check.StageReader("src/stage.cpp", "a refusal nobody throws"),
            ("bindings/node/src/stage.ts", "Gone.shape"): check.StageReader("src/stage.cpp", "x"),
        }
        problems, _, _ = self.run_with(stages=stages, extra=self._STAGE)
        self.assertEqual(
            problems,
            [
                (
                    "StageConfig.shape (bindings/node/src/stage.ts): stage reader src/stage.cpp: "
                    "no condition guards the refusal 'a refusal nobody throws'"
                ),
                "stage reader for Gone.shape (bindings/node/src/stage.ts) names no facade set",
            ],
        )

    def test_the_shipped_family_exemptions_carry_a_reason(self) -> None:
        for key, reason in check.FAMILY_EXEMPT.items():
            self.assertTrue(reason.strip(), key)


_ENUM_FILES = {
    "include/c.h": "typedef enum { SONARE_TAP_PRE_FADER = 0, SONARE_TAP_POST_FADER = 1 } SonareTap;\n",
    "src/law.cpp": _POP_FILES["src/law.cpp"],
    "ts/tap.ts": "export type Tap = 'preFader' | 'postFader';\nconst TAPS: readonly Tap[] = ['preFader', 'postFader'];\n",
    "py/tap.py": 'Tap = Literal["preFader", "postFader"]\n_TAPS = {"preFader": 0, "postFader": 1}\n',
}

_ENUM_ROW = check.TokenSet(
    name="tap",
    core=None,
    c_enum=("include/c.h", "SONARE_TAP_"),
    sources=(
        check.Source("node", "ts/tap.ts", "ts_union", "Tap"),
        check.Source("node", "ts/tap.ts", "ts_array", "TAPS"),
        check.Source("python", "py/tap.py", "py_literal", "Tap"),
        check.Source("python", "py/tap.py", "py_dict", "_TAPS"),
    ),
)


class EnumOnlyRowTests(unittest.TestCase):
    """A row with no core writer takes its tokens from the C enumerators."""

    def tree(self, edits: dict[str, tuple[str, str]] | None = None) -> tempfile.TemporaryDirectory:
        d = tempfile.TemporaryDirectory()
        root = Path(d.name)
        for rel, text in _ENUM_FILES.items():
            old, new = (edits or {}).get(rel, ("", ""))
            if old:
                self.assertIn(old, text, f"fixture edit does not apply to {rel}")
                text = text.replace(old, new)
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        return d

    def run_with(self, edits: dict[str, tuple[str, str]] | None = None) -> tuple[list[str], int, int]:
        with self.tree(edits) as d:
            return check.evaluate(Path(d), (_ENUM_ROW,))

    def test_clean_tree_passes_and_compares_every_source(self) -> None:
        self.assertEqual(self.run_with(), ([], 1, len(_ENUM_ROW.sources)))

    def test_a_token_matching_no_enumerator_is_named(self) -> None:
        problems, _, _ = self.run_with({"ts/tap.ts": ("'postFader';", "'postFader' | 'preSend';")})
        self.assertEqual(problems, ["tap: node Tap (ts/tap.ts): token 'preSend' matches no SONARE_TAP_* enumerator"])

    def test_a_missing_token_is_named(self) -> None:
        problems, _, _ = self.run_with({"py/tap.py": ('"preFader", "postFader"]', '"preFader"]')})
        self.assertEqual(problems, ["tap: python Tap (py/tap.py): missing a token for SONARE_TAP_POST_FADER"])

    def test_a_swapped_ordinal_in_a_py_dict_is_named(self) -> None:
        problems, _, _ = self.run_with({"py/tap.py": ('"preFader": 0, "postFader": 1', '"preFader": 1, "postFader": 0')})
        self.assertEqual(
            problems,
            [
                "tap: python _TAPS (py/tap.py): token 'postFader' has ordinal 0, the C ABI's is 1",
                "tap: python _TAPS (py/tap.py): token 'preFader' has ordinal 1, the C ABI's is 0",
            ],
        )

    def test_a_row_without_writer_or_enum_is_named(self) -> None:
        with self.tree() as d:
            problems, _, compared = check.evaluate(Path(d), (_ENUM_ROW._replace(c_enum=None),))
        self.assertEqual((problems, compared), (["tap: a row with no core writer must name its C enum"], 0))

    def test_an_enum_pinned_set_draws_on_no_writer(self) -> None:
        # Tap shares no writer here; pin law.ts's Law to an enum row and it stops drawing on law_text.
        with self.tree({"ts/tap.ts": ("export type Tap", "export type Law = 'soft' | 'hard';\nexport type Tap")}) as d:
            root = Path(d)
            (root / "bindings/node/src").mkdir(parents=True)
            (root / "bindings/node/src/law.ts").write_text("export type Law = 'soft' | 'hard';\n")
            unpinned, _, _ = check.evaluate_population(root, (), {})
            row = _ENUM_ROW._replace(sources=(check.Source("node", "bindings/node/src/law.ts", "ts_union", "Law"),))
            pinned, sets, writers = check.evaluate_population(root, (row,), {})
        self.assertIn(
            "Law (bindings/node/src/law.ts) spells the tokens of law_text(Law) in src/law.cpp and no row pins it",
            unpinned,
        )
        self.assertEqual((pinned, sets, writers), ([], 0, 0))


_TRAITS_HEADER = """
template <>
struct Traits<Room> {
  using Enum = Room;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::Hall: return "hall";
      case Enum::Plate: return "plate";
    }
    return nullptr;
  }
};

template <>
struct Traits<Law> {
  using Enum = Law;
  static constexpr const char* name(Enum value) noexcept {
    switch (value) {
      case Enum::Soft: return "soft";
      case Enum::Hard: return "hard";
    }
    return nullptr;
  }
};
"""


class TraitsAliasTests(unittest.TestCase):
    """A writer spelled over a specialization's member alias is keyed by the specialized enum."""

    def test_each_specialization_is_its_own_writer(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "src").mkdir()
            (root / "src/traits.h").write_text(_TRAITS_HEADER)
            (root / "bindings/node/src").mkdir(parents=True)
            (root / "bindings/node/src/t.ts").write_text(
                "export type Room = 'hall' | 'plate';\nexport type Law = 'soft' | 'hard';\n"
            )
            self.assertEqual(
                check.core_writers(root),
                {
                    ("src/traits.h", "name", "Room"): {"hall", "plate"},
                    ("src/traits.h", "name", "Law"): {"soft", "hard"},
                },
            )
            room = check.TokenSet(
                name="room",
                core=check.CoreTable("src/traits.h", "Room", None, "name"),
                sources=(check.Source("node", "bindings/node/src/t.ts", "ts_union", "Room"),),
            )
            problems, _, _ = check.evaluate_population(root, (room,), {})
        self.assertEqual(check.written_tokens(_TRAITS_HEADER, "Law", "name"), ["soft", "hard"])
        self.assertEqual(
            problems,
            [
                "Law (bindings/node/src/t.ts) spells the tokens of name(Law) in src/traits.h and no row pins it",
                "name(Law) in src/traits.h writes a set a facade spells and no row names it",
            ],
        )


class UnexpandedTests(unittest.TestCase):
    """A union composing literals with a named type is pinned, declared, or a failure."""

    def run_with(self, text: str, declared: dict[tuple[str, str], str]) -> list[str]:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "bindings/node/src").mkdir(parents=True)
            (root / "bindings/node/src/t.ts").write_text(text)
            problems, _ = check.evaluate_unexpanded(root, (), declared)
        return problems

    _COMPOSED = "export type Mode = Major | 'unknown';\nexport type Plain = 'a' | 'b';\n"
    _KEY = ("bindings/node/src/t.ts", "Mode")

    def test_an_undeclared_composed_set_is_named(self) -> None:
        self.assertEqual(
            self.run_with(self._COMPOSED, {}),
            [
                (
                    "Mode (bindings/node/src/t.ts) composes literals with a named type, which no reader "
                    "expands; pin it through a plain list or declare it in UNEXPANDED"
                )
            ],
        )

    def test_a_declared_composed_set_passes(self) -> None:
        self.assertEqual(self.run_with(self._COMPOSED, {self._KEY: "why"}), [])

    def test_an_entry_outliving_its_composition_is_named(self) -> None:
        self.assertEqual(
            self.run_with("export type Mode = 'major' | 'unknown';\n", {self._KEY: "why"}),
            ["UNEXPANDED entry for Mode (bindings/node/src/t.ts) names no composed declaration"],
        )


if __name__ == "__main__":
    unittest.main()
