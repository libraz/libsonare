#!/usr/bin/env python3
"""Assert every surface agrees on what each ``sonare::ErrorCode`` means.

Five tables translate the core error enumeration for a consumer, and no two of
them live in the same file: the C ABI's ``map_sonare_exception`` and its
inverse, the Node addon's ``CErrorFromException`` / ``ErrorCodeName``, the WASM
module's inline ``{code, codeName, message}`` switch, the two TypeScript
``ErrorCode`` enums, and the Python ``ErrorCode`` plus its name table.  A new
enumerator added to the core and missed in one of them does not fail to
compile; it reaches that surface as ``Unknown`` / 99 while every other surface
reports the real cause, and the caller branching on the code takes the wrong
branch on one runtime only.

So the assertion is driven by the enumeration itself rather than by the codes
that happen to be in use: every enumerator the core declares is looked up in
every table, and a table that does not answer is the finding.  Asserting the
current codes would pass forever after the next enumerator is added, which is
exactly the moment the check is for.

**Scope, stated because it is the checker's blind spot:** the comparison covers
the enumerators reachable through a ``SonareException``, which is all of them
except ``Ok``.  ``Ok`` is a non-error that the surfaces deliberately answer
differently -- the addon turns an exception carrying it into ``Unknown``, the
WASM module reports it as ``Ok`` -- and folding it in would assert a
disagreement that is not drift.  Anything about ``Ok`` is outside what this
test can see.

**The second blind spot is the parser.** Every table is read by locating one
named construct and matching ``case`` arms inside it.  A surface that stops
spelling its mapping that way -- a lookup table, a generated header, a map
literal -- parses as an empty table, and two empty sets agree perfectly.  The
population floor below exists for that: each table must answer for at least as
many enumerators as the core declares, checked before any agreement is.
"""

from __future__ import annotations

import re
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

CORE_ENUM = "src/util/types.h"
C_ENUM = "include/sonare/sonare_c_types_enums.h"
C_MAPPING = "src/c_api/sonare_c_internal.cpp"
C_INVERSE = "src/c_api/sonare_c_error_mapping.h"
NODE_ADDON = "bindings/node/src/addon/sonare_wrap_utils.cpp"
WASM_BINDINGS = "src/wasm/bindings.cpp"
NODE_TYPES = "bindings/node/src/errors.ts"
WASM_TYPES = "bindings/wasm/src/errors.ts"
PYTHON_RUNTIME = "bindings/python/src/libsonare/_runtime.py"

SOURCES = (
    CORE_ENUM,
    C_ENUM,
    C_MAPPING,
    C_INVERSE,
    NODE_ADDON,
    WASM_BINDINGS,
    NODE_TYPES,
    WASM_TYPES,
    PYTHON_RUNTIME,
)

# `Ok` is not reachable through an exception; see the module docstring.
NOT_AN_ERROR = "Ok"

_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/|^[ \t]*#[^\n]*", re.DOTALL | re.MULTILINE)


def _read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def _strip_comments(text: str) -> str:
    return _COMMENT.sub(" ", text)


def _braced_body(text: str, start: int) -> str:
    """The ``{ ... }`` block that opens at or after ``start``."""
    open_at = text.index("{", start)
    depth = 0
    for index in range(open_at, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_at + 1 : index]
    raise AssertionError("unbalanced block")


def _enum_values(body: str) -> dict[str, int]:
    """Enumerator name to value, filling in the implicit successors."""
    values: dict[str, int] = {}
    running = 0
    for part in body.split(","):
        match = re.match(r"\s*([A-Za-z_]\w*)\s*(?:=\s*(-?\d+))?\s*$", part)
        if match is None:
            continue
        if match.group(2) is not None:
            running = int(match.group(2))
        values[match.group(1)] = running
        running += 1
    return values


def core_enumerators(root: Path) -> dict[str, int]:
    """``sonare::ErrorCode`` as declared, the source of truth for the set."""
    text = _strip_comments(_read(root, CORE_ENUM))
    return _enum_values(_braced_body(text, text.index("enum class ErrorCode")))


def c_enumerators(root: Path) -> dict[str, int]:
    """The C ABI ``SonareError`` enum, name to numeric code."""
    text = _strip_comments(_read(root, C_ENUM))
    marker = text.index("SONARE_OK")
    return _enum_values(_braced_body(text, text.rindex("{", 0, marker) - 1))


def _case_arms(text: str, function: str, pattern: str) -> dict[str, str]:
    """``{case label: captured}`` for one function's switch arms."""
    body = _braced_body(text, text.index(function))
    return {m.group(1): m.group(2) for m in re.finditer(pattern, body)}


def core_to_c(root: Path, relative: str, function: str) -> dict[str, str]:
    """``ErrorCode::X -> SONARE_ERROR_Y`` out of one switch."""
    return _case_arms(
        _strip_comments(_read(root, relative)),
        function,
        r"case\s+(?:sonare::)?ErrorCode::(\w+)\s*:\s*return\s+(SONARE_\w+)\s*;",
    )


def c_to_core(root: Path) -> dict[str, str]:
    """``SONARE_ERROR_Y -> ErrorCode::X``, the inverse the facades use."""
    return _case_arms(
        _strip_comments(_read(root, C_INVERSE)),
        "error_code_from_c_error",
        r"case\s+(SONARE_\w+)\s*:\s*return\s+(?:sonare::)?ErrorCode::(\w+)\s*;",
    )


def c_to_name(root: Path) -> dict[str, str]:
    """``SONARE_ERROR_Y -> "Name"``, the canonical cross-surface spelling."""
    return _case_arms(
        _strip_comments(_read(root, NODE_ADDON)),
        "ErrorCodeName",
        r'case\s+(SONARE_\w+)\s*:\s*return\s+"(\w+)"\s*;',
    )


def wasm_mapping(root: Path) -> dict[str, tuple[int, str]]:
    """``ErrorCode::X -> (numeric code, codeName)`` from the WASM switch."""
    text = _strip_comments(_read(root, WASM_BINDINGS))
    body = _braced_body(text, text.index("js_sonare_exception_info"))
    pattern = (
        r"case\s+(?:sonare::)?ErrorCode::(\w+)\s*:\s*"
        r"code\s*=\s*(-?\d+)\s*;\s*"
        r'code_name\s*=\s*"(\w+)"\s*;'
    )
    return {m.group(1): (int(m.group(2)), m.group(3)) for m in re.finditer(pattern, body)}


def ts_enumerators(root: Path, relative: str) -> dict[str, int]:
    """A TypeScript ``ErrorCode`` enum, name to numeric code."""
    text = _strip_comments(_read(root, relative))
    body = _braced_body(text, text.index("export enum ErrorCode"))
    return {
        m.group(1): int(m.group(2))
        for m in re.finditer(r"(\w+)\s*=\s*(-?\d+)\s*,", body)
    }


def python_mapping(root: Path) -> dict[str, int]:
    """The Python binding's ``codeName -> numeric code``.

    Two declarations have to agree before the surface answers at all: the
    ``ErrorCode`` IntEnum carries the numbers under SCREAMING_CASE members, and
    the ``code_name`` table carries the canonical spelling for each.  Joining
    them here is deliberate -- a member with no entry in the table is a surface
    that cannot name its own code.
    """
    text = _read(root, PYTHON_RUNTIME)
    enum_body = text[text.index("class ErrorCode(IntEnum):") :]
    enum_body = enum_body[: enum_body.index("\n\n\n")]
    members = {
        m.group(1): int(m.group(2))
        for m in re.finditer(r"^\s{4}([A-Z][A-Z_0-9]*)\s*=\s*(-?\d+)\s*$", enum_body, re.MULTILINE)
    }
    names = {
        m.group(1): m.group(2)
        for m in re.finditer(r'ErrorCode\.([A-Z][A-Z_0-9]*)\s*:\s*"(\w+)"', text)
    }
    return {names[member]: value for member, value in members.items() if member in names}


class Tables:
    """Every surface's error table, read from one tree."""

    def __init__(self, root: Path) -> None:
        self.core = core_enumerators(root)
        self.c_enum = c_enumerators(root)
        self.c_abi = core_to_c(root, C_MAPPING, "map_sonare_exception")
        self.c_inverse = c_to_core(root)
        self.addon = core_to_c(root, NODE_ADDON, "CErrorFromException")
        self.addon_names = c_to_name(root)
        self.wasm = wasm_mapping(root)
        self.node_ts = ts_enumerators(root, NODE_TYPES)
        self.wasm_ts = ts_enumerators(root, WASM_TYPES)
        self.python = python_mapping(root)

    @property
    def reachable(self) -> list[str]:
        """Enumerators reachable through a SonareException, in declared order."""
        return [name for name in self.core if name != NOT_AN_ERROR]


def inconsistencies(tables: Tables) -> list[str]:
    """Every enumerator a surface answers for differently, or not at all."""
    problems: list[str] = []
    for name in tables.reachable:
        c_symbol = tables.c_abi.get(name)
        if c_symbol is None:
            problems.append(f"{name}: the C ABI mapping has no case for it")
            continue
        code = tables.c_enum.get(c_symbol)
        if code is None:
            problems.append(f"{name}: maps to {c_symbol}, which the C enum does not declare")
            continue

        addon_symbol = tables.addon.get(name)
        if addon_symbol is None:
            problems.append(f"{name}: the Node addon mapping has no case for it")
        elif addon_symbol != c_symbol:
            problems.append(
                f"{name}: the C ABI maps it to {c_symbol}, the Node addon to {addon_symbol}"
            )

        if tables.c_inverse.get(c_symbol) != name:
            problems.append(
                f"{name}: maps to {c_symbol}, which the inverse mapping sends to "
                f"{tables.c_inverse.get(c_symbol)!r}"
            )

        canonical = tables.addon_names.get(c_symbol)
        if canonical is None:
            problems.append(f"{name}: {c_symbol} has no canonical codeName")
            continue

        wasm = tables.wasm.get(name)
        if wasm is None:
            problems.append(f"{name}: the WASM mapping has no case for it")
        elif wasm != (code, canonical):
            problems.append(
                f"{name}: the C ABI says ({code}, {canonical!r}), WASM says {wasm!r}"
            )

        for label, table in (
            ("the Node ErrorCode enum", tables.node_ts),
            ("the WASM ErrorCode enum", tables.wasm_ts),
            ("the Python binding", tables.python),
        ):
            if canonical not in table:
                problems.append(f"{name}: {label} does not declare {canonical!r}")
            elif table[canonical] != code:
                problems.append(
                    f"{name}: {canonical!r} is {code} in the C ABI and "
                    f"{table[canonical]} in {label}"
                )
    return problems


class SourcePresenceTest(unittest.TestCase):
    """Each table must answer for the whole enumeration before it is compared.

    Two empty sets agree, so the floor is checked first and separately: if a
    surface stops spelling its mapping in a way the parser recognises, this
    fails rather than the agreement check passing over nothing.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = Tables(ROOT)

    def test_the_core_enumeration_is_nonempty(self) -> None:
        self.assertGreaterEqual(len(self.tables.core), 10)
        self.assertEqual(self.tables.core[NOT_AN_ERROR], 0)

    def test_every_table_answers_for_the_whole_enumeration(self) -> None:
        floor = len(self.tables.reachable)
        for label, size in (
            ("C ABI mapping", len(self.tables.c_abi)),
            ("C ABI inverse", len(self.tables.c_inverse)),
            ("Node addon mapping", len(self.tables.addon)),
            ("Node addon names", len(self.tables.addon_names)),
            ("WASM mapping", len(self.tables.wasm)),
            ("Node ErrorCode enum", len(self.tables.node_ts)),
            ("WASM ErrorCode enum", len(self.tables.wasm_ts)),
            ("Python binding", len(self.tables.python)),
        ):
            with self.subTest(table=label):
                self.assertGreaterEqual(
                    size, floor, f"{label} parsed as {size} entries; the parser or the table broke"
                )


class AgreementTest(unittest.TestCase):
    def test_every_enumerator_means_the_same_thing_on_every_surface(self) -> None:
        problems = inconsistencies(Tables(ROOT))
        self.assertEqual(problems, [], "\n".join(problems))


class NonVacuityTest(unittest.TestCase):
    """Removing one enumerator's mapping must fail, naming that enumerator.

    Run against a copy: the break has to be in the real text to prove the check
    reads it, and it must not be in the shared tree, where another session's
    build would silently inherit it.
    """

    def _copy(self) -> tuple[tempfile.TemporaryDirectory, Path]:
        holder = tempfile.TemporaryDirectory()
        root = Path(holder.name)
        for relative in SOURCES:
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        return holder, root

    def test_removing_an_enumerator_from_the_wasm_mapping_names_it(self) -> None:
        holder, root = self._copy()
        with holder:
            path = root / WASM_BINDINGS
            text = path.read_text(encoding="utf-8")
            arm = re.search(
                r"[ \t]*case sonare::ErrorCode::Cancelled:\n"
                r"[ \t]*code = \d+;\n"
                r'[ \t]*code_name = "\w+";\n'
                r"[ \t]*break;\n",
                text,
            )
            self.assertIsNotNone(arm, "the WASM switch arm is no longer written this way")
            assert arm is not None
            path.write_text(text[: arm.start()] + text[arm.end() :], encoding="utf-8")
            problems = inconsistencies(Tables(root))
        self.assertEqual(problems, ["Cancelled: the WASM mapping has no case for it"])

    def test_changing_one_numeric_code_names_the_surface_that_moved(self) -> None:
        holder, root = self._copy()
        with holder:
            path = root / NODE_TYPES
            text = path.read_text(encoding="utf-8")
            path.write_text(text.replace("Cancelled = 8,", "Cancelled = 88,"), encoding="utf-8")
            problems = inconsistencies(Tables(root))
        self.assertEqual(
            problems,
            ["Cancelled: 'Cancelled' is 8 in the C ABI and 88 in the Node ErrorCode enum"],
        )


if __name__ == "__main__":
    unittest.main()
