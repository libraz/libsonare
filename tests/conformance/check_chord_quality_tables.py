"""Hold every chord-quality name table to the core enum, ordinal by ordinal.

``sonare::ChordQuality`` has no name accessor on the C ABI, so each surface
carries its own hand-written ordinal-to-name table. The core carries one too,
for the in-tree tools that link it directly; it is a copy like the rest and is
held to the enum here alongside them. Appending a quality is
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

There are two vocabularies here and they are not interchangeable. Everything
above holds **vocabulary I**: the camelCase enumerator name a caller switches on
(``halfDim7``). Below it are the **vocabulary II** tables -- the chord-symbol
suffix a chord is *spelled* with (``m7b5``). Four sites carry one, and they do
not all spell the same thing, so they are not folded into one comparison:

* ``chord_quality_to_string`` labels a template, where the quality is always
  visible, so a major chord is ``maj``.
* ``Chord::to_string`` and the Python ``suffixes`` map spell a chord symbol,
  where major is implied and written bare. Those two are required to agree at
  every ordinal, ordinal 0 included; theirs is the only full value comparison
  here, and an exception to it is declared per ordinal with its reason.
* The Roman-numeral builder is a third vocabulary, not a third copy. The
  numeral's own case carries the minor third, so its suffix drops the minor
  marker (``m7`` -> ``7``) and substitutes the figured symbols (``dim`` -> the
  degree sign, ``m7b5`` -> the half-diminished one). Comparing its spelling
  against a chord symbol would declare more than half its arms an exception,
  and an exception list that long is the table copied out a second time. It is
  held for coverage instead: every ordinal the chord-symbol tables spell must
  have an arm here, so a quality added without one fails rather than rendering
  as a bare degree.

The suffix map is keyed on a vocabulary-I name rather than on an ordinal, and
``suffixes.get(quality, '')`` returns the empty suffix for a key it does not
have -- which is also the spelling of a major chord. Comparing the values it
does resolve would therefore be satisfied by a map whose key set had gone
entirely stale: every chord would render as a bare root, and that reads as a
plausible result rather than as a failure. So the key set is held against
vocabulary I in its own right, and a key resolving to no ordinal is reported
rather than dropped from the value comparison.

``src/util/types.h`` repeats a suffix in the doc comment on some enumerators
(``///< mM7``). Those are not held here and cannot be: the set is partial, and
``Major6`` and ``Minor6`` are annotated ``maj6`` and ``min6`` while every table
spells them ``6`` and ``m6``, so the comments are disambiguating labels rather
than a copy of a suffix table. Correcting a table therefore has to be grepped
for in the prose; nothing mechanical will find it.

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


class SuffixTable(typing.NamedTuple):
    """One chord-symbol suffix table and which vocabulary it spells."""

    path: str
    symbol: str
    parser: str
    group: str
    note: str

    @property
    def display(self) -> str:
        return f"{self.path}:{self.symbol}"


class SuffixException(typing.NamedTuple):
    """A vocabulary-II divergence that is present, and why it is not drift."""

    reason: str


class LocatedSuffix(typing.NamedTuple):
    """A suffix table as read, with the keys that resolved to no ordinal.

    ``unresolved`` is empty for a table keyed on an enumerator. It is the whole
    hazard for one keyed on a quality name: a key nothing resolves carries a
    suffix no comparison reaches, and the lookup that misses it succeeds.
    """

    table: SuffixTable
    line: int
    by_ordinal: dict[int, str]
    unresolved: tuple[str, ...]

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
    Table(
        "src/analysis/chord_templates.cpp",
        "chord_quality_name",
        "cpp_core_switch",
        "camel",
        "core",
        "the quality string the native CLI writes into every chord",
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

# Vocabulary II. `group` decides what a table is compared against, and the three
# groups are not one set -- the module docstring says why. Two of these share a
# file: the Roman-numeral table is a sibling of the chord-symbol one with nothing
# putting the two in front of the same reader.
SUFFIX_TABLES = (
    SuffixTable(
        "src/analysis/chord_templates.cpp",
        "chord_quality_to_string",
        "cpp_core_switch",
        "label",
        "the suffix a template's own label carries, where the quality is always visible",
    ),
    SuffixTable(
        "src/analysis/chord_analyzer.cpp",
        "Chord::to_string",
        "cpp_switch_appends",
        "symbol",
        "the chord symbol every core-side chord name is built from",
    ),
    SuffixTable(
        "bindings/python/src/libsonare/_types_analysis.py",
        "suffixes",
        "python_dict",
        "symbol",
        "the chord symbol `Chord.name` renders on the Python surface",
    ),
    SuffixTable(
        "src/analysis/chord_analyzer.cpp",
        "ChordAnalyzer::chord_to_roman_numeral",
        "cpp_switch_appends",
        "numeral",
        "the figured suffix on a Roman numeral, appended by a second switch in the "
        "file the chord-symbol table lives in",
    ),
)

# A vocabulary-II divergence that is present and accounted for, keyed by the
# comparison and the ordinal it happens at. That is the granularity of the
# divergence itself, so an entry cannot drift into excusing a different one.
# Each must suppress a finding in a comparison that ran; one that suppresses
# nothing is a failure, not a leftover.
SUFFIX_EXCEPTIONS: dict[tuple[str, int], SuffixException] = {
    ("symbol-group", 9): SuffixException(
        "Unknown: both render paths return `N.C.` before they reach their table, so "
        "neither spelling is reachable and neither is what a caller sees"
    ),
    ("label-vs-symbol", 0): SuffixException(
        "Major: a template label spells the quality, so `Cmaj` names the template it "
        "labels, while a chord symbol leaves major implied -- stated on "
        "`chord_quality_to_string` in src/analysis/chord_templates.h"
    ),
    ("numeral-coverage", 7): SuffixException(
        "Sus2: the numeral builder appends no sus marker, so a sus2 chord renders as "
        "the bare degree, indistinguishable from the plain triad built on it"
    ),
    ("numeral-coverage", 8): SuffixException(
        "Sus4: the numeral builder appends no sus marker, so a sus4 chord renders as "
        "the bare degree, indistinguishable from the plain triad built on it"
    ),
}

# Independent of the tuples above: deleting a table entry must not lower the bar.
# The two suffix floors count located tables rather than spellings, so neither
# moves when a suffix is corrected. The second is the one that carries the
# weight: the only full value comparison here is between the chord-symbol
# tables, and it is vacuous with fewer than two of them in it.
FLOOR = {
    "qualities": 25,
    "ordinal_tables": 8,
    "name_tables": 3,
    "suffix_tables": 4,
    "suffix_symbol_tables": 2,
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


def cpp_core_switch(text: str, table: Table) -> tuple[int, dict[str, str]] | None:
    """``case ChordQuality::Enumerator: return "name";`` inside the named function.

    Returns name keyed by enumerator; the caller resolves the ordinal from the
    core enum, which is what the switch dispatches on.
    """
    anchor = re.compile(rf"\b{re.escape(table.symbol)}\s*\([^)]*\)\s*\{{")
    found = block(text, anchor)
    if found is None:
        return None
    line, body = found
    pairs = re.findall(
        rf"case\s+{re.escape(CORE_ENUM.symbol)}::(\w+)\s*:\s*return\s+\"([^\"]*)\"\s*;", body
    )
    return line, dict(pairs)


_QUALIFIERS = r"(?:\s*\b(?:const|noexcept|override|final)\b)*"
_SWITCH = re.compile(r"\bswitch\s*\(")


def definition(symbol: str) -> re.Pattern[str]:
    """Anchor on the definition of @p symbol: a declaration or a call ends in `;`."""
    return re.compile(rf"\b{re.escape(symbol)}\s*\([^)]*\){_QUALIFIERS}\s*\{{")


def switch_body(body: str) -> str | None:
    """Body of the one ``switch (...) {...}`` in @p body, or None.

    A second switch in the same function returns None rather than a guess, so
    the table reads as one this check cannot locate instead of as a short one.
    """
    skeleton = strip_lexical(body)
    matches = list(_SWITCH.finditer(skeleton))
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
                return body[opened + 1 : index]
    return None


_SWITCH_ARM = re.compile(
    rf"""(?P<case>\bcase\s+{re.escape(CORE_ENUM.symbol)}::(?P<enumerator>\w+)\s*:)
       | (?P<default>\bdefault\s*:)
       | (?P<append>\+=\s*"(?P<text>[^"\n]*)")
       | (?P<stop>\bbreak\s*; | \breturn\b)""",
    re.VERBOSE,
)


def cpp_switch_appends(text: str, table: SuffixTable) -> tuple[int, dict[str, str]] | None:
    """``case ChordQuality::X: <var> += "suffix"; break;`` inside the named function.

    Returns the suffix keyed by enumerator. An arm appending nothing before it
    breaks carries the empty suffix, which is a spelling and not an absence;
    an enumerator with no arm at all is absent, and the two stay apart because
    the caller compares against the core enum's ordinals rather than against
    this table's own extent. Consecutive labels share the one body following
    them. An escape sequence in a literal returns None: this reads source text,
    and a suffix left encoded would compare unequal to the same suffix read out
    of Python by ``ast``. A body whose appends run past a further label returns
    None for the same reason -- it is not a flat table and guessing would
    attribute a suffix to an arm that never appends it. So does an append whose
    right-hand side is not a literal: it would otherwise read as an arm that
    appends nothing, which is the empty suffix here and a spelling rather than
    an absence, so the table would come back complete and quietly wrong.
    """
    found = block(text, definition(table.symbol))
    if found is None:
        return None
    line, body = found
    inner = switch_body(body)
    if inner is None:
        return None
    by_enumerator: dict[str, str] = {}
    pending: list[str] = []
    parts: list[str] = []
    appends = 0
    for arm in _SWITCH_ARM.finditer(inner):
        if arm.group("case") or arm.group("default"):
            if parts:
                return None
            pending.append(arm.group("enumerator") or "")
        elif arm.group("append"):
            if "\\" in arm.group("text"):
                return None
            appends += 1
            parts.append(arm.group("text"))
        else:
            for name in pending:
                if name:
                    by_enumerator[name] = "".join(parts)
            pending = []
            parts = []
    if appends != len(re.findall(r"\+=", inner)):
        return None
    return (line, by_enumerator) if by_enumerator else None


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
    values += re.findall(rf"^\s*#\s*define\s+{re.escape(name)}\s+(-?\d+)", stripped, re.MULTILINE)
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
    elif table.parser == "cpp_core_switch":
        found = cpp_core_switch(text, table)
        if found is None or core is None:
            return None
        line, by_enumerator = found
        ordinals = {name: ordinal for ordinal, name in core.items()}
        if any(name not in ordinals for name in by_enumerator):
            return None
        result = (line, {ordinals[name]: exported for name, exported in by_enumerator.items()})
    elif table.parser == "ts_union":
        found = ts_union(text, table)
        if found is None:
            return None
        keys = tuple(found[1])
        result = (found[0], dict(enumerate(found[1])))
    else:
        raise ValueError(f"unknown parser: {table.parser}")
    return Located(table, result[0], result[1], keys) if result else None


def locate_suffix(
    root: Path, table: SuffixTable, core: dict[int, str] | None
) -> LocatedSuffix | None:
    """Read one suffix table and key it on the ordinal each of its keys resolves to.

    A table that parses to nothing is returned as None so it reads as one this
    check could not locate: an empty suffix table agrees with every other one.
    """
    text = _read(root, table.path)
    if text is None or not core:
        return None
    if table.parser == "cpp_core_switch":
        found = cpp_core_switch(text, table)
    elif table.parser == "cpp_switch_appends":
        found = cpp_switch_appends(text, table)
    elif table.parser == "python_dict":
        found = python_dict(text, table)
    else:
        raise ValueError(f"unknown suffix parser: {table.parser}")
    if not found or not found[1]:
        return None
    line, entries = found
    if table.parser == "python_dict":
        ordinals = {expected_name(name, "camel"): ordinal for ordinal, name in core.items()}
    else:
        ordinals = {name: ordinal for ordinal, name in core.items()}
    by_ordinal: dict[int, str] = {}
    unresolved: list[str] = []
    for key, value in entries.items():
        if not isinstance(value, str):
            return None
        if key in ordinals:
            by_ordinal[ordinals[key]] = value
        else:
            unresolved.append(str(key))
    return LocatedSuffix(table, line, by_ordinal, tuple(sorted(unresolved)))


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
        self.suffix = {t: locate_suffix(root, t, self.core) for t in SUFFIX_TABLES}
        # Filled by the vocabulary-II comparison: how many ordinals each of its
        # three comparisons reached. An ordinal excused in one comparison is
        # not reached by the ones downstream of it, and a tally is the only
        # place that shows.
        self.compared: dict[str, int] = {}


def _self_check(scan: Scan, floor: dict) -> list[str]:
    """A scan that locates nothing agrees with every enum, so size it first."""
    measured = {
        "qualities": len(scan.core),
        "ordinal_tables": sum(1 for v in scan.ordinal.values() if v is not None),
        "name_tables": sum(1 for v in scan.named.values() if v is not None),
        "suffix_tables": sum(1 for v in scan.suffix.values() if v is not None),
        "suffix_symbol_tables": sum(
            1 for t, v in scan.suffix.items() if v is not None and t.group == "symbol"
        ),
    }
    return [
        f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- the scan has "
        "stopped locating what it is sized for, and an empty table matches every enum"
        for name, minimum in floor.items()
        if measured.get(name, 0) < minimum
    ]


def _show(suffix: str | None) -> str:
    """How a suffix reads in a message, with absence and emptiness kept apart."""
    if suffix is None:
        return "no arm at all"
    return "the empty suffix" if not suffix else f"`{suffix}`"


def _suffix_failures(scan: Scan) -> list[tuple[str, list[str]]]:
    """Every vocabulary-II failure class, as (heading, lines)."""
    failures: list[tuple[str, list[str]]] = []
    core = scan.core
    if not core:
        return failures
    located = [found for found in scan.suffix.values() if found is not None]

    missing = [
        f"  {table.display} -- {table.note}"
        for table, found in scan.suffix.items()
        if found is None
    ]
    if missing:
        failures.append(
            (
                ("These suffix tables could not be read. A suffix table nothing reads is "
                "a spelling nothing compares, and the chord names it builds go on "
                "looking like chord names -- re-point the entry at the table"),
                missing,
            )
        )

    stray = [
        f"  {found.display}: `{key}` is not a chord quality name, so the suffix it "
        f"carries is keyed to no ordinal and is compared against nothing -- "
        f"{found.table.note}"
        for found in located
        for key in found.unresolved
    ]
    if stray:
        failures.append(
            (
                ("These suffix tables are looked up by a quality name, and a key that is "
                "not one can never be hit. The lookup falls back to the empty suffix, "
                "which is also the spelling of a major chord, so a key set gone stale "
                "renders every chord as a bare root -- a plausible-looking result rather "
                "than a failure, and one that comparing the values that still resolve "
                "cannot see"),
                stray,
            )
        )

    symbol = [found for found in located if found.table.group == "symbol"]
    label = next((found for found in located if found.table.group == "label"), None)
    numeral = next((found for found in located if found.table.group == "numeral"), None)

    ran: set[tuple[str, int]] = set()
    used: set[tuple[str, int]] = set()

    def excused(comparison: str, ordinal: int) -> bool:
        """Record that @p comparison ran at @p ordinal, and whether it is excused."""
        ran.add((comparison, ordinal))
        if (comparison, ordinal) not in SUFFIX_EXCEPTIONS:
            return False
        used.add((comparison, ordinal))
        return True

    consensus: dict[int, str] = {}
    split: list[str] = []
    if len(symbol) >= 2:
        for ordinal in sorted(core):
            spelled = {found.display: found.by_ordinal.get(ordinal) for found in symbol}
            agreed = set(spelled.values())
            if len(agreed) == 1 and None not in agreed:
                ran.add(("symbol-group", ordinal))
                consensus[ordinal] = agreed.pop()
                continue
            if excused("symbol-group", ordinal):
                continue
            split.append(
                f"  ordinal {ordinal} (`{core[ordinal]}`): "
                + "; ".join(
                    f"{where} spells it {_show(suffix)}"
                    for where, suffix in sorted(spelled.items())
                )
            )
    if split:
        failures.append(
            (
                ("These ordinals are spelled differently by two chord-symbol tables. Both "
                "build a chord name a caller reads, so one chord is named two ways "
                "depending on which surface produced it, and neither name is wrong on "
                "its face"),
                split,
            )
        )

    drift: list[str] = []
    if label is not None:
        for ordinal in sorted(consensus):
            if label.by_ordinal.get(ordinal) == consensus[ordinal]:
                ran.add(("label-vs-symbol", ordinal))
                continue
            if excused("label-vs-symbol", ordinal):
                continue
            drift.append(
                f"  {label.display}: ordinal {ordinal} (`{core[ordinal]}`) is "
                f"{_show(label.by_ordinal.get(ordinal))}, and the chord-symbol tables "
                f"spell it {_show(consensus[ordinal])} -- {label.table.note}"
            )
    if drift:
        failures.append(
            (
                ("The template-label table diverges from the chord symbols somewhere it "
                "is not allowed to. It is a vocabulary of its own only at the declared "
                "ordinals; everywhere else the two spell one suffix, and a template "
                "labelled with a suffix no chord is ever named with matches nothing a "
                "caller can look it up by"),
                drift,
            )
        )

    # A Roman numeral lower-cases itself for a minor chord, so a quality whose
    # whole chord symbol is the minor marker is already spelled by the numeral
    # and needs no arm. The marker is read off the chord-symbol tables' own
    # entry for Minor rather than spelled here, so this stays one vocabulary.
    minor = next((o for o, name in core.items() if name == "Minor"), None)
    carried_by_case = consensus.get(minor) if minor is not None else None

    gaps: list[str] = []
    if numeral is not None:
        for ordinal in sorted(consensus):
            if not consensus[ordinal] or consensus[ordinal] == carried_by_case:
                continue
            if ordinal in numeral.by_ordinal:
                ran.add(("numeral-coverage", ordinal))
                continue
            if excused("numeral-coverage", ordinal):
                continue
            gaps.append(
                f"  {numeral.display}: ordinal {ordinal} (`{core[ordinal]}`) has no arm, "
                f"and the chord-symbol tables spell it {_show(consensus[ordinal])}, so "
                f"the numeral carries no mark of the quality at all -- {numeral.table.note}"
            )
    if gaps:
        failures.append(
            (
                ("The Roman-numeral table has no arm for a quality the chord-symbol "
                "tables spell. Its suffixes are a vocabulary of their own and are not "
                "compared for spelling, but a quality it omits falls to the default and "
                "renders as the bare scale degree -- the numeral of a plain triad"),
                gaps,
            )
        )

    scan.compared = {
        comparison: sum(1 for name, _ in ran if name == comparison)
        for comparison in ("symbol-group", "label-vs-symbol", "numeral-coverage")
    }

    stale = [
        f"  {comparison} at ordinal {ordinal}: {exception.reason}"
        for (comparison, ordinal), exception in SUFFIX_EXCEPTIONS.items()
        if ordinal not in core
        or ((comparison, ordinal) in ran and (comparison, ordinal) not in used)
    ]
    if stale:
        failures.append(
            (
                ("These suffix exceptions excused nothing. An exception outlives its "
                "divergence as a reviewed decision about a spelling, so the next "
                "divergence to land at that ordinal inherits it unexamined -- delete the "
                "exception in the change that removes what it excused. An exception "
                "whose comparison never ran is not listed here; that is reported as a "
                "table this check could not read"),
                stale,
            )
        )
    return failures


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
                ("These spelling exemptions name an enumerator the core no longer has. An "
                "exemption that matches nothing still blesses a spelling, so the next "
                "enumerator to take that name inherits it unexamined -- delete the "
                "exemption in the change that removes its enumerator"),
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
                ("These tables could not be located. The core enum has no name accessor on "
                "the C ABI, so each surface carries its own copy and a copy this check "
                "cannot read is a copy nothing compares -- re-point the entry at the table"),
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
                ("These tables disagree with the core enum at an ordinal. A table is read "
                "by ordinal, so a wrong entry labels a detected chord as a different "
                "chord rather than failing -- restore the mapping, or renumber nothing"),
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
                ("These tables are keyed by name, and their key set no longer matches the "
                "core enum. A quality the set omits is one the facade silently renders as "
                "something else"),
                sets,
            )
        )

    failures.extend(_suffix_failures(scan))

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

    for table, found in scan.suffix.items():
        print(f"  suffix  {table.display}: "
              f"{len(found.by_ordinal) if found else 'not found'}")
        if args.list and found:
            for ordinal in sorted(found.by_ordinal):
                print(f"    {ordinal:3} {found.by_ordinal[ordinal] or '(bare)'}")

    _suffix_failures(scan)
    print("  suffix ordinals compared: " + ", ".join(
        f"{comparison} {reached}/{len(scan.core)}"
        for comparison, reached in scan.compared.items()))

    failures = evaluate(args.root)
    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
