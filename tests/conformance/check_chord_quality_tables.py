#!/usr/bin/env python3
"""Hold every chord-quality name table to the core enum, ordinal by ordinal.

``sonare::ChordQuality`` has no name accessor on the C ABI, so each surface
carries its own hand-written ordinal-to-name table. Appending a quality is
survivable -- a table short of an ordinal fails on the value it cannot map.
Renumbering is not: reorder two enumerators and every table relabels at once,
each still complete, each now naming the wrong chord. Nothing else sees it.
Parity cannot: it models parameter enum value sets, and this is a result-side
name table.

So the comparison here is BY ORDINAL. A sorted-name-set comparison passes
through a reorder unchanged, which is the one defect this exists to catch.

``kChordQualityCount`` is the anchor: ``src/util/types.h`` declares it as what
every fixed-size table indexed by a quality derives its extent from, and the
tables below are the ones that derive from nothing. Each is required to hold
exactly that many entries, and the located tables are counted against a floor,
because a scanner that finds nothing agrees with every enum.

Spelling differs between the surfaces by a total rule per table -- identity,
first character lowercased, or the C macro's screaming snake -- never by a
case-insensitive compare, which would also absorb a genuine rename. The two
altered ninths the bindings spell out are listed in ``SPELLING_EXEMPTIONS``
with their reason; an exemption naming an enumerator that no longer exists is a
failure, not a leftover.

``sonare::arrangement::ChordQuality`` is a deliberately coarser eight-member
family with its own conversion, not drift. Every table here is located by path
and symbol, so that enum is never opened.
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Table(typing.NamedTuple):
    """One located-by-symbol name table and how its entries are spelled."""

    path: str
    symbol: str
    parser: str
    spelling: str
    surface: str
    note: str

    @property
    def display(self) -> str:
        return f"{self.path}:{self.symbol}"


class Exemption(typing.NamedTuple):
    spelled: str
    reason: str


class Located(typing.NamedTuple):
    """A table as read: its anchor line, its ordinal mapping, and its key set.

    ``keys`` is what a name-keyed table is looked up by, which is not the side
    the name lives on in every table -- the suffix map's values are chord
    symbols, not qualities.
    """

    table: Table
    line: int
    by_ordinal: dict[int, str]
    keys: tuple[str, ...]

    @property
    def display(self) -> str:
        return f"{self.table.path}:{self.line}"


# The enum every table below mirrors, and the two counts that must agree with it.
CORE_ENUM = Table(
    "src/util/types.h", "ChordQuality", "core_enum", "pascal", "core", "the enum itself"
)
CORE_COUNT = ("src/util/types.h", "kChordQualityCount")
C_COUNT = ("include/sonare/sonare_c_types_analysis.h", "SONARE_CHORD_QUALITY_COUNT")

# Tables keyed on an ordinal. A renumbering relabels each of them silently.
ORDINAL_TABLES = (
    Table(
        "include/sonare/sonare_c_types_analysis.h",
        "SonareChordQuality",
        "c_enum",
        "c_macro",
        "c-abi",
        "the ordinals the Python and Node tables are keyed on",
    ),
    Table(
        "src/wasm/bindings.cpp",
        "ChordQuality",
        "embind",
        "pascal",
        "wasm",
        "the enum WASM callers actually receive",
    ),
    Table(
        "bindings/wasm/src/public_types_music.ts",
        "ChordQuality",
        "ts_object",
        "pascal",
        "wasm",
        "the exported constant a WASM caller compares an ordinal against",
    ),
    Table(
        "bindings/wasm/src/sonare.js.d.ts",
        "ChordQuality",
        "ts_object",
        "pascal",
        "wasm",
        "the hand-written declaration of the embind enum",
    ),
    Table(
        "bindings/python/src/libsonare/_analysis_detection.py",
        "_QUALITY_NAMES",
        "python_dict",
        "camel",
        "python",
        "the quality string on every chord `analyze()` returns",
    ),
    Table(
        "bindings/python/src/libsonare/_analysis_music.py",
        "quality_names",
        "python_dict",
        "camel",
        "python",
        "the quality string on every chord `detect_chords()` returns",
    ),
    Table(
        "bindings/node/src/addon/sonare_wrap_utils.cpp",
        "ChordQualityName",
        "cpp_switch",
        "camel",
        "node",
        "the quality string the addon writes into every chord",
    ),
)

# Tables keyed on a name. Ordering carries nothing, but the set is a public
# surface: a name missing here is a value the facade cannot represent.
NAME_TABLES = (
    Table(
        "bindings/node/src/types_analysis.ts",
        "Chord.quality",
        "ts_union",
        "camel",
        "node",
        "the TypeScript type of the string the addon writes",
    ),
    Table(
        "bindings/python/src/libsonare/_analysis_detection.py",
        "chord_quality_str",
        "python_dict",
        "camel",
        "python",
        "accepts a quality already spelled as a name; an absent key falls back to `unknown`",
    ),
    Table(
        "bindings/python/src/libsonare/_types_analysis.py",
        "suffixes",
        "python_dict",
        "camel",
        "python",
        "the chord-symbol suffix; an absent key renders the chord with no suffix at all",
    ),
)

# A core enumerator whose bindings spell it differently, and why. Applied to
# every table, so one surface diverging from the others still fails.
SPELLING_EXEMPTIONS: dict[str, Exemption] = {
    "Dominant7b9": Exemption(
        "Dominant7Flat9",
        "the core abbreviates the altered ninth; a bare `b` in an identifier reads "
        "as a pitch letter, so every binding spells the accidental out",
    ),
    "Dominant7s9": Exemption(
        "Dominant7Sharp9",
        "`#` is not an identifier character, so the core abbreviates it and every "
        "binding spells it out to match the flat-ninth spelling",
    ),
}

# Independent of the tuples above: deleting a table entry must not lower the bar.
FLOOR = {
    "qualities": 25,
    "ordinal_tables": 7,
    "name_tables": 3,
}


def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines and its length."""
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


_CPP_COMMENT = re.compile(
    r"""(?P<keep>"(?:[^"\\\n]|\\.)*" | '(?:[^'\\\n]|\\.)*')
      | (?P<drop>//[^\n]* | /\*.*?\*/)""",
    re.DOTALL | re.VERBOSE,
)

_CPP_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)


def strip_comments(text: str) -> str:
    """Comments blanked, string literals kept: the names live inside the strings."""
    return _CPP_COMMENT.sub(lambda m: m.group(0) if m.group("keep") else _blank(m), text)


def strip_lexical(text: str) -> str:
    """Comments and literals both blanked, so a brace in either closes nothing."""
    return _CPP_LEXICAL.sub(_blank, text)


def block(text: str, anchor: re.Pattern[str]) -> tuple[int, str] | None:
    """Body of the one ``{...}`` @p anchor opens, or None -- never a near miss.

    Boundaries are taken from a lexically blanked copy so a brace inside a
    comment or a literal cannot close the block early; both strippers preserve
    offsets, so the span slices the comment-stripped text unchanged.
    """
    skeleton = strip_lexical(text)
    matches = list(anchor.finditer(skeleton))
    if len(matches) != 1:
        return None
    opened = skeleton.find("{", matches[0].start())
    if opened < 0:
        return None
    depth = 0
    for index in range(opened, len(skeleton)):
        if skeleton[index] == "{":
            depth += 1
        elif skeleton[index] == "}":
            depth -= 1
            if depth == 0:
                line = skeleton.count("\n", 0, matches[0].start()) + 1
                return line, strip_comments(text)[opened + 1 : index]
    return None


_ENUMERATOR = re.compile(r"^\s*(?P<name>\w+)\s*(?:=\s*(?P<value>-?\d+))?\s*$")


def core_enum(text: str, table: Table) -> tuple[int, dict[int, str]] | None:
    """Enumerators of ``enum class <symbol>``, in declaration order."""
    anchor = re.compile(rf"\benum\s+class\s+{re.escape(table.symbol)}\s*(?::[^{{;]*)?\{{")
    found = block(text, anchor)
    if found is None:
        return None
    line, body = found
    by_ordinal: dict[int, str] = {}
    ordinal = 0
    for entry in body.split(","):
        if not entry.strip():
            continue
        match = _ENUMERATOR.fullmatch(entry)
        if match is None:
            return None
        if match.group("value") is not None:
            ordinal = int(match.group("value"))
        by_ordinal[ordinal] = match.group("name")
        ordinal += 1
    return line, by_ordinal


def c_enum(text: str, table: Table) -> tuple[int, dict[int, str]] | None:
    """Enumerators of the ``typedef enum { ... } <symbol>;`` the typedef names."""
    stripped = strip_comments(text)
    pattern = re.compile(
        rf"typedef\s+enum\b[^{{]*\{{(?P<body>[^{{}}]*)\}}\s*{re.escape(table.symbol)}\s*;"
    )
    matches = list(pattern.finditer(stripped))
    if len(matches) != 1:
        return None
    line = stripped.count("\n", 0, matches[0].start()) + 1
    by_ordinal: dict[int, str] = {}
    for name, value in re.findall(r"(\w+)\s*=\s*(-?\d+)", matches[0].group("body")):
        by_ordinal[int(value)] = name
    return line, by_ordinal


def ts_object(text: str, table: Table) -> tuple[int, dict[int, str]] | None:
    """``Name: 0,`` and the embind declaration's ``Name: { value: 0 };``."""
    anchor = re.compile(
        rf"(?:export\s+const\s+)?\b{re.escape(table.symbol)}\s*[=:]\s*\{{"
    )
    found = block(text, anchor)
    if found is None:
        return None
    line, body = found
    by_ordinal: dict[int, str] = {}
    for name, value in re.findall(r"(\w+)\s*:\s*(?:\{\s*value\s*:\s*)?(-?\d+)", body):
        by_ordinal[int(value)] = name
    return line, by_ordinal


def embind(text: str, table: Table) -> tuple[int, dict[str, str]] | None:
    """``.value("Name", Enum::Enumerator)`` of the one matching embind chain.

    Returns exported name keyed by enumerator; the caller resolves the ordinal
    from the core enum, which is where an embind registration takes it from.
    """
    stripped = strip_comments(text)
    anchor = re.compile(rf"enum_<{re.escape(table.symbol)}>\s*\(")
    matches = list(anchor.finditer(strip_lexical(text)))
    if len(matches) != 1:
        return None
    end = strip_lexical(text).find(";", matches[0].start())
    if end < 0:
        return None
    chain = stripped[matches[0].start() : end]
    line = stripped.count("\n", 0, matches[0].start()) + 1
    pairs = re.findall(
        rf"\.value\(\s*\"(\w+)\"\s*,\s*{re.escape(table.symbol)}::(\w+)\s*\)", chain
    )
    return line, {enumerator: exported for exported, enumerator in pairs}


def cpp_switch(text: str, table: Table) -> tuple[int, dict[str, str]] | None:
    """``case CONSTANT: return "name";`` inside the named function.

    Returns name keyed by the C constant; the caller resolves the ordinal from
    the C enum, which is what the switch dispatches on.
    """
    anchor = re.compile(rf"\b{re.escape(table.symbol)}\s*\([^)]*\)\s*\{{")
    found = block(text, anchor)
    if found is None:
        return None
    line, body = found
    pairs = re.findall(r"case\s+(\w+)\s*:\s*return\s+\"([^\"]*)\"\s*;", body)
    return line, dict(pairs)


def python_dict(text: str, table: Table) -> tuple[int, dict] | None:
    """The one assignment of a dict literal to @p symbol, at any nesting depth."""
    found = []
    for node in ast.walk(ast.parse(text)):
        targets = []
        if isinstance(node, ast.Assign):
            targets = node.targets
        elif isinstance(node, ast.AnnAssign):
            targets = [node.target]
        else:
            continue
        if len(targets) != 1 or not isinstance(targets[0], ast.Name):
            continue
        if targets[0].id != table.symbol or not isinstance(node.value, ast.Dict):
            continue
        found.append(node)
    if len(found) != 1:
        return None
    node = found[0]
    entries = {}
    for key, value in zip(node.value.keys, node.value.values):
        if not isinstance(key, ast.Constant) or not isinstance(value, ast.Constant):
            return None
        entries[key.value] = value.value
    return node.lineno, entries


_TS_UNION = re.compile(r"\bquality\s*:\s*((?:\s*\|\s*'[^']+')+)\s*;")


def ts_union(text: str, table: Table) -> tuple[int, list[str]] | None:
    """The string-literal union of the ``quality`` property on the named interface."""
    interface, _, member = table.symbol.partition(".")
    anchor = re.compile(rf"\bexport\s+interface\s+{re.escape(interface)}\s*\{{")
    found = block(text, anchor)
    if found is None or member != "quality":
        return None
    line, body = found
    matches = list(_TS_UNION.finditer(body))
    if len(matches) != 1:
        return None
    return line, re.findall(r"'([^']+)'", matches[0].group(1))


def _pascal(name: str) -> str:
    return name


def _camel(name: str) -> str:
    return name[:1].lower() + name[1:]


def _c_macro(name: str) -> str:
    return "SONARE_CHORD_" + re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name).upper()


SPELLINGS = {"pascal": _pascal, "camel": _camel, "c_macro": _c_macro}


def expected_name(enumerator: str, spelling: str) -> str:
    """How @p enumerator is spelled in a table using @p spelling."""
    exemption = SPELLING_EXEMPTIONS.get(enumerator)
    base = exemption.spelled if exemption else enumerator
    return SPELLINGS[spelling](base)


def _read(root: Path, relative: str) -> str | None:
    path = root / relative
    return path.read_text(encoding="utf-8") if path.is_file() else None


def find_constant(root: Path, where: tuple[str, str]) -> int | None:
    """The single integer named by @p where, from a C++ or a C definition."""
    path, name = where
    text = _read(root, path)
    if text is None:
        return None
    stripped = strip_lexical(text)
    values = re.findall(rf"\b{re.escape(name)}\b\s*=\s*(-?\d+)", stripped)
    values += re.findall(rf"^\s*#\s*define\s+{re.escape(name)}\s+(-?\d+)", stripped, re.M)
    return int(values[0]) if len(values) == 1 else None


def locate(root: Path, table: Table, core: dict[int, str] | None, c_abi: dict[int, str] | None):
    """Read one table, resolving symbolic keys against the enum they index."""
    text = _read(root, table.path)
    if text is None:
        return None
    keys: tuple[str, ...] = ()
    if table.parser == "core_enum":
        result = core_enum(text, table)
    elif table.parser == "c_enum":
        result = c_enum(text, table)
    elif table.parser == "ts_object":
        result = ts_object(text, table)
    elif table.parser == "python_dict":
        found = python_dict(text, table)
        if found is None:
            return None
        line, entries = found
        keys = tuple(str(key) for key in entries)
        result = (line, entries)
    elif table.parser == "embind":
        found = embind(text, table)
        if found is None or core is None:
            return None
        line, by_enumerator = found
        ordinals = {name: ordinal for ordinal, name in core.items()}
        if any(name not in ordinals for name in by_enumerator):
            return None
        result = (line, {ordinals[name]: exported for name, exported in by_enumerator.items()})
    elif table.parser == "cpp_switch":
        found = cpp_switch(text, table)
        if found is None or c_abi is None:
            return None
        line, by_constant = found
        ordinals = {name: ordinal for ordinal, name in c_abi.items()}
        if any(name not in ordinals for name in by_constant):
            return None
        result = (line, {ordinals[name]: exported for name, exported in by_constant.items()})
    elif table.parser == "ts_union":
        found = ts_union(text, table)
        if found is None:
            return None
        keys = tuple(found[1])
        result = (found[0], dict(enumerate(found[1])))
    else:
        raise ValueError(f"unknown parser: {table.parser}")
    return Located(table, result[0], result[1], keys) if result else None


class Scan:
    """The core enum, its two counts, and every table located against them."""

    def __init__(self, root: Path = ROOT) -> None:
        self.root = root
        core = _read(root, CORE_ENUM.path)
        found = core_enum(core, CORE_ENUM) if core else None
        self.core_line, self.core = found if found else (0, {})
        self.core_count = find_constant(root, CORE_COUNT)
        self.c_count = find_constant(root, C_COUNT)

        c_text = _read(root, ORDINAL_TABLES[0].path)
        c_found = c_enum(c_text, ORDINAL_TABLES[0]) if c_text else None
        c_abi = c_found[1] if c_found else None

        self.ordinal = {t: locate(root, t, self.core, c_abi) for t in ORDINAL_TABLES}
        self.named = {t: locate(root, t, self.core, c_abi) for t in NAME_TABLES}


def _self_check(scan: Scan, floor: dict) -> list[str]:
    """A scan that locates nothing agrees with every enum, so size it first."""
    measured = {
        "qualities": len(scan.core),
        "ordinal_tables": sum(1 for v in scan.ordinal.values() if v is not None),
        "name_tables": sum(1 for v in scan.named.values() if v is not None),
    }
    return [
        f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- the scan has "
        "stopped locating what it is sized for, and an empty table matches every enum"
        for name, minimum in floor.items()
        if measured.get(name, 0) < minimum
    ]


def evaluate(root: Path = ROOT, floor: dict | None = None) -> list[tuple[str, list[str]]]:
    """Every failure class, as (heading, lines). Empty means the tables hold."""
    scan = Scan(root)
    floor = FLOOR if floor is None else floor
    failures: list[tuple[str, list[str]]] = []

    anchor: list[str] = []
    if not scan.core:
        anchor.append(
            f"{CORE_ENUM.path}: no `enum class {CORE_ENUM.symbol}`. Every table below is "
            "compared against it, so a rename or a move leaves them all unmeasured."
        )
    for path, name in (CORE_COUNT, C_COUNT):
        value = find_constant(root, (path, name))
        if value is None:
            anchor.append(f"{path}: no single `{name}`, so the tables have no extent to hold to.")
        elif scan.core and value != len(scan.core):
            anchor.append(
                f"{path}: {name} is {value}, but {CORE_ENUM.symbol} has {len(scan.core)} "
                "enumerators. Every fixed-size table indexed by a quality sizes itself "
                "from this count."
            )
    if anchor:
        failures.append(("The anchor the tables are held to is not readable", anchor))

    stale = [
        f"  {enumerator} -> {exemption.spelled}: {exemption.reason}"
        for enumerator, exemption in SPELLING_EXEMPTIONS.items()
        if scan.core and enumerator not in scan.core.values()
    ]
    if stale:
        failures.append(
            (
                "These spelling exemptions name an enumerator the core no longer has. An "
                "exemption that matches nothing still blesses a spelling, so the next "
                "enumerator to take that name inherits it unexamined -- delete the "
                "exemption in the change that removes its enumerator",
                stale,
            )
        )

    missing = [
        f"  {table.display} ({table.surface}) -- {table.note}"
        for table, located in {**scan.ordinal, **scan.named}.items()
        if located is None
    ]
    if missing:
        failures.append(
            (
                "These tables could not be located. The core enum has no name accessor on "
                "the C ABI, so each surface carries its own copy and a copy this check "
                "cannot read is a copy nothing compares -- re-point the entry at the table",
                missing,
            )
        )

    drift: list[str] = []
    for table, located in scan.ordinal.items():
        if located is None or not scan.core:
            continue
        for ordinal in sorted(set(scan.core) | set(located.by_ordinal)):
            want = scan.core.get(ordinal)
            got = located.by_ordinal.get(ordinal)
            if want is None:
                drift.append(
                    f"  {located.display}: ordinal {ordinal} is `{got}`, and the core enum "
                    f"has no such ordinal ({CORE_ENUM.path}:{scan.core_line})"
                )
                continue
            spelled = expected_name(want, table.spelling)
            if got is None:
                drift.append(
                    f"  {located.display}: ordinal {ordinal} is absent; the core has "
                    f"`{want}`, spelled `{spelled}` here -- {table.note}"
                )
            elif got != spelled:
                drift.append(
                    f"  {located.display}: ordinal {ordinal} is `{got}`, but the core has "
                    f"`{want}` there, spelled `{spelled}` here -- {table.note}"
                )
    if drift:
        failures.append(
            (
                "These tables disagree with the core enum at an ordinal. A table is read "
                "by ordinal, so a wrong entry labels a detected chord as a different "
                "chord rather than failing -- restore the mapping, or renumber nothing",
                drift,
            )
        )

    sets: list[str] = []
    for table, located in scan.named.items():
        if located is None or not scan.core:
            continue
        want = {expected_name(name, table.spelling) for name in scan.core.values()}
        got = set(located.keys)
        for name in sorted(want - got):
            sets.append(f"  {located.display}: `{name}` is absent -- {table.note}")
        for name in sorted(got - want):
            sets.append(f"  {located.display}: `{name}` is not a core quality -- {table.note}")
    if sets:
        failures.append(
            (
                "These tables are keyed by name, and their key set no longer matches the "
                "core enum. A quality the set omits is one the facade silently renders as "
                "something else",
                sets,
            )
        )

    self_check = _self_check(scan, floor)
    if self_check:
        failures.append(("The scan no longer locates the population it is sized for", self_check))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--list", action="store_true", help="print every located table")
    args = parser.parse_args()

    scan = Scan(args.root)
    print(f"core enum: {len(scan.core)} enumerators, kChordQualityCount = {scan.core_count}")
    for table, located in scan.ordinal.items():
        print(f"  {table.surface:7} {table.display}: "
              f"{len(located.by_ordinal) if located else 'not found'}")
        if args.list and located:
            for ordinal in sorted(located.by_ordinal):
                print(f"    {ordinal:3} {located.by_ordinal[ordinal]}")
    for table, located in scan.named.items():
        print(f"  {table.surface:7} {table.display}: "
              f"{len(located.keys) if located else 'not found'} keys")
        if args.list and located:
            for name in sorted(located.keys):
                print(f"        {name}")

    failures = evaluate(args.root)
    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
