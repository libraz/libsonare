#!/usr/bin/env python3
"""Keep every Python-to-C integer narrowing in the ctypes binding accounted for.

Three populations, all enforced here, all drifting for the same reason -- a new
module can convert a caller's number however it likes and nothing notices:

* **Inline argument narrowings.**  ``ctypes.c_int(value)`` applies the C
  conversion, so ``2**32`` arrives as 0 -- which every versioned config field
  reads as "keep the default" -- and ``2**32 + 1`` arrives as 1.  A wrapped value
  is always inside the target type, so no downstream range check can see it.  A
  conversion must therefore go through the shared ``_to_c_*`` family or be
  recorded with the mechanism that makes it harmless.
* **Struct field assignment.**  The same conversion happens on assignment to a
  ``ctypes.Structure`` field, and there the field's own type is the only thing
  that knows the bound, so a struct must inherit the base that reads it.
* **File-local readers.**  A helper that converts a caller's number to a ctypes
  integer is a reader, and a reader outside the shared home means a change to
  the conversion contract reaches some call sites and not others.

The records live in ``python_narrowing_records.json`` and are read as data.  A
record that matches nothing is itself an error: a stale one keeps asserting a
reviewed decision about a spelling, so the next narrowing to take that spelling
inherits the blessing unexamined.

TWO SCANS, BY DIFFERENT ROUTES, WHOSE DISAGREEMENT IS AN ASSERTION
------------------------------------------------------------------
A blind spot in one route is invisible from inside it, so re-reading it proves
nothing; only a second measurement disagrees.  The inline population is counted
twice and the disagreement is an error, not a log line:

* **Scan A, over the parsed syntax.**  Every ``ast.Call`` whose callee resolves
  to a ctypes integer type and that carries an argument, with the argument's
  source text taken from the tree.
* **Scan B, over the token stream.**  The ``NAME . NAME (`` sequence read from
  ``tokenize``, with the argument's presence decided by the tokens that follow.
  It never parses, so it sees the spelling rather than the structure, and it
  skips comments and string literals because the tokenizer classifies them
  rather than because a pattern blanked them.

Neither route can produce the other's answer by construction, so a per-file
disagreement means one of them stopped seeing a shape the other still sees --
a predicate on the tree side narrowed past a call shape the spelling still has,
or a spelling assembled in a way the tokens cannot follow.  The scan also has to
find a known-nonzero population: with either route made to match nothing, the
agreement and the record checks both go green and certify a scanner that has
stopped working, so a floor is pinned.

KNOWN COMMON MODE
-----------------
Both scans start from the ``ctypes.`` qualification and the type-name list
below.  A conversion reached under a different name -- ``from ctypes import
c_int``, a type stored in a variable, a ``functools.partial`` -- is invisible to
both scans AND to their agreement.  The import-shape check below is what closes
that one: it fails on any binding module that pulls a ctypes integer type into
its own namespace, so the qualification the scans depend on cannot be bypassed
without a report.
"""

from __future__ import annotations

import argparse
import ast
import io
import json
import re
import sys
import tokenize
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BINDING = ROOT / "bindings" / "python" / "src" / "libsonare"
RECORDS = Path(__file__).resolve().parent / "python_narrowing_records.json"

# The shared home. A narrowing here IS the implementation every other site
# routes through, so it is the one place the raw ctypes constructor belongs.
SHARED_READER_FILES = ("_runtime.py", "_cstruct.py")

# The struct base whose __setattr__ range-checks an integer field against the
# field's own declared type.
STRUCT_BASE = "CStruct"

# Spelled out rather than matched loosely: `c_float`, `c_char_p` and the pointer
# types cannot wrap, and sweeping them in would bury the population this exists
# for.
INTEGER_TYPES = (
    "c_int",
    "c_int8",
    "c_int16",
    "c_int32",
    "c_int64",
    "c_uint",
    "c_uint8",
    "c_uint16",
    "c_uint32",
    "c_uint64",
    "c_long",
    "c_ulong",
    "c_longlong",
    "c_ulonglong",
    "c_size_t",
    "c_ssize_t",
)

# The shared family every inline narrowing outside the home must route through.
SHARED_READERS = (
    "_to_c_int",
    "_to_c_int32",
    "_to_c_int64",
    "_to_c_uint",
    "_to_c_uint32",
    "_to_c_uint16",
    "_to_c_uint8",
    "_to_c_size_t",
)

def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines and its length."""
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


_LEXICAL = re.compile(
    r"""\#[^\n]*
      | '''.*?'''
      | \"\"\".*?\"\"\"
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)


def strip_lexical(text: str) -> str:
    """Comments and string literals blanked, offsets and line numbers preserved."""
    return _LEXICAL.sub(_blank, text)


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


class Site:
    """One inline narrowing, with everything a record is keyed on."""

    def __init__(self, path: Path, line: int, ctype: str, argument: str) -> None:
        self.path = path
        self.line = line
        self.type = ctype
        self.argument = " ".join(argument.split())

    @property
    def display(self) -> str:
        return f"{_display(self.path)}:{self.line}"

    @property
    def key(self) -> tuple[str, str, str]:
        return (self.path.name, self.argument, self.type)


class Scan:
    """Both scans over the binding, plus what their disagreement says."""

    def __init__(self, tree: Path = BINDING, *, simple_arguments_only: bool = False) -> None:
        self.tree = tree
        # Count only a call whose argument is a bare name -- kept as an option so
        # the cross-check's power can be demonstrated rather than argued.
        self.simple_arguments_only = simple_arguments_only
        self.sites: list[Site] = []
        self.token_counts: dict[str, int] = {}
        self.plain_structs: list[tuple[Path, int, str]] = []
        self.local_readers: list[tuple[Path, int, str]] = []
        self.aliased_imports: list[tuple[Path, int, str]] = []
        self._run()

    def _files(self) -> list[Path]:
        return sorted(p for p in self.tree.rglob("*.py") if p.is_file())

    def _run(self) -> None:
        for path in self._files():
            source = path.read_text(encoding="utf-8")
            tree = ast.parse(source)
            shared_home = path.name in SHARED_READER_FILES

            # Scan A: over the parsed syntax.
            for node in ast.walk(tree):
                if shared_home or not _is_ctypes_integer_call(node) or not node.args:
                    continue
                if self.simple_arguments_only and not isinstance(node.args[0], ast.Name):
                    continue
                self.sites.append(
                    Site(path, node.lineno, node.func.attr, ast.unparse(node.args[0]))
                )

            # Scan B: over the token stream.
            self.token_counts[path.name] = 0 if shared_home else _token_scan(source)

            self._collect_structure_bases(path, tree)
            self._collect_local_readers(path, tree, shared_home)
            self._collect_aliased_imports(path, tree)

    def _collect_structure_bases(self, path: Path, tree: ast.Module) -> None:
        """A struct not on the checked base truncates every integer field it has."""
        if path.name in SHARED_READER_FILES:
            return
        for node in ast.walk(tree):
            if not isinstance(node, ast.ClassDef):
                continue
            for base in node.bases:
                if (
                    isinstance(base, ast.Attribute)
                    and base.attr == "Structure"
                    and isinstance(base.value, ast.Name)
                    and base.value.id == "ctypes"
                ):
                    self.plain_structs.append((path, node.lineno, node.name))

    def _collect_local_readers(self, path: Path, tree: ast.Module, shared_home: bool) -> None:
        """A module-level helper whose whole job is a ctypes integer conversion."""
        if shared_home:
            return
        for node in ast.walk(tree):
            if not isinstance(node, ast.FunctionDef):
                continue
            returns = ast.unparse(node.returns) if node.returns is not None else ""
            if not any(f"ctypes.{name}" == returns for name in INTEGER_TYPES):
                continue
            self.local_readers.append((path, node.lineno, node.name))

    def _collect_aliased_imports(self, path: Path, tree: ast.Module) -> None:
        """The common mode: a type pulled in under a name the scans cannot see."""
        for node in ast.walk(tree):
            if not isinstance(node, ast.ImportFrom) or node.module != "ctypes":
                continue
            for alias in node.names:
                if alias.name in INTEGER_TYPES:
                    self.aliased_imports.append((path, node.lineno, alias.name))
            continue

    def reconcile(self) -> list[str]:
        """Files whose two counts disagree, named with both numbers."""
        observed: dict[str, int] = {}
        for site in self.sites:
            observed[site.path.name] = observed.get(site.path.name, 0) + 1
        return [
            f"{name}: the token scan saw {counted}, the tree scan saw {observed.get(name, 0)}"
            for name, counted in sorted(self.token_counts.items())
            if counted != observed.get(name, 0)
        ]


def _token_scan(source: str) -> int:
    """Scan B: ``ctypes . <integer type> (`` followed by anything but ``)``."""
    stream = [
        token
        for token in tokenize.generate_tokens(io.StringIO(source).readline)
        if token.type not in (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE, tokenize.INDENT)
        and token.type != tokenize.DEDENT
    ]
    count = 0
    for i in range(len(stream) - 4):
        names = [stream[i + k] for k in range(5)]
        if (
            names[0].string == "ctypes"
            and names[1].string == "."
            and names[2].string in INTEGER_TYPES
            and names[3].string == "("
            and names[4].string != ")"
        ):
            count += 1
    return count


def _is_ctypes_integer_call(node: ast.AST) -> bool:
    return (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr in INTEGER_TYPES
        and isinstance(node.func.value, ast.Name)
        and node.func.value.id == "ctypes"
    )


class Records:
    """The recorded-benign data, and which of its entries matched anything."""

    def __init__(self, data: dict) -> None:
        self.shapes = data.get("shapes", [])
        self.narrowings = data.get("narrowings", [])
        self.structs = data.get("plain_structs", [])
        self.readers = data.get("readers", [])
        self.used: set[str] = set()
        self._shape_patterns = {
            shape["name"]: re.compile(shape["argument_pattern"]) for shape in self.shapes
        }

    def covers(self, site: Site) -> bool:
        for shape in self.shapes:
            if shape.get("unsigned_only") and not site.type.startswith(("c_u", "c_size")):
                continue
            if self._shape_patterns[shape["name"]].fullmatch(site.argument):
                self.used.add(f"shape:{shape['name']}")
                return True
        for entry in self.narrowings:
            if (entry["file"], entry["argument"], entry["type"]) == site.key:
                self.used.add(f"narrowing:{entry['file']}:{entry['argument']}")
                return True
        return False

    def covers_struct(self, name: str) -> bool:
        if name in self.structs:
            self.used.add(f"struct:{name}")
            return True
        return False

    def covers_reader(self, path: Path, symbol: str) -> bool:
        for entry in self.readers:
            if entry["file"] == path.name and entry["symbol"] == symbol:
                self.used.add(f"reader:{entry['file']}:{entry['symbol']}")
                return True
        return False

    def unused(self) -> list[str]:
        every = (
            [f"shape:{s['name']}" for s in self.shapes]
            + [f"narrowing:{e['file']}:{e['argument']}" for e in self.narrowings]
            + [f"struct:{s}" for s in self.structs]
            + [f"reader:{e['file']}:{e['symbol']}" for e in self.readers]
        )
        return sorted(name for name in every if name not in self.used)


def _self_check(scan: Scan, floor: dict) -> list[str]:
    """The mandatory one. Two empty sets agree perfectly.

    With either pattern matching nothing, the reconciliation and the record
    checks both pass and certify a scanner that has stopped working, so the
    population's own size is asserted before either of them is believed.
    """
    measured = {
        "files": len(list(scan.tree.rglob("*.py"))),
        "narrowings": len(scan.sites),
        "shared_reader_calls": _shared_reader_calls(scan.tree),
    }
    return [
        f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- the scan has "
        "stopped matching, and an empty population agrees with everything"
        for name, minimum in floor.items()
        if measured.get(name, 0) < minimum
    ]


def _shared_reader_calls(tree: Path) -> int:
    """How many sites route through the shared family, as the population's floor."""
    pattern = re.compile(r"\b(?:" + "|".join(SHARED_READERS) + r")\(")
    return sum(
        len(pattern.findall(strip_lexical(p.read_text(encoding="utf-8"))))
        for p in tree.rglob("*.py")
    )


def evaluate(scan: Scan, records: Records, floor: dict) -> list[tuple[str, list[str]]]:
    """Every failure class, as (heading, lines).

    Separated from ``main`` so the self-tests can revert one class at a time and
    require exactly that class to fire -- a class asserted through a
    reimplementation of this function would only ever agree with itself.
    """
    failures: list[tuple[str, list[str]]] = []

    self_check = _self_check(scan, floor)
    if self_check:
        failures.append(("The scan no longer finds the population it is sized for", self_check))

    disagreements = scan.reconcile()
    if disagreements:
        failures.append(
            (
                "The two scans disagree on these files, so one of them stopped "
                "seeing a shape the other still sees",
                [f"  {line}" for line in disagreements],
            )
        )

    aliased = [
        (path, line, name)
        for path, line, name in scan.aliased_imports
        if path.name not in SHARED_READER_FILES
    ]
    if aliased:
        failures.append(
            (
                "These modules import a ctypes integer type directly, which puts "
                "a conversion beyond the qualification both scans depend on",
                [f"  {_display(p)}:{line}  from ctypes import {name}" for p, line, name in aliased],
            )
        )

    plain = [
        (path, line, name)
        for path, line, name in scan.plain_structs
        if not records.covers_struct(name)
    ]
    if plain:
        failures.append(
            (
                f"These structs subclass ctypes.Structure directly instead of "
                f"{STRUCT_BASE}, so assigning an out-of-range value to an integer "
                "field wraps it into a different legal setting",
                [f"  {_display(p)}:{line}  class {name}(ctypes.Structure)" for p, line, name in plain],
            )
        )

    local = [
        (path, line, name)
        for path, line, name in scan.local_readers
        if not records.covers_reader(path, name)
    ]
    if local:
        failures.append(
            (
                "These file-local readers convert to a ctypes integer outside the "
                "shared family, so a change to the conversion contract does not "
                "reach their call sites",
                [f"  {_display(p)}:{line}  {name}()" for p, line, name in local],
            )
        )

    unrecorded = [site for site in scan.sites if not records.covers(site)]
    if unrecorded:
        failures.append(
            (
                "These narrowings are neither performed by the shared reader nor "
                "recorded with the mechanism that makes them harmless",
                [f"  {site.display}  ctypes.{site.type}({site.argument})" for site in unrecorded],
            )
        )

    stale = records.unused()
    if stale:
        failures.append(
            (
                "These records matched nothing. A record that suppresses nothing "
                "still asserts a reviewed decision about a spelling, so the next "
                "narrowing to take it inherits the blessing unexamined",
                [f"  {name}" for name in stale],
            )
        )
    return failures


def load_records(path: Path = RECORDS) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, default=BINDING)
    parser.add_argument("--records", type=Path, default=RECORDS)
    parser.add_argument(
        "--simple-arguments-only",
        action="store_true",
        help="narrow the tree scan to calls whose argument is a bare name, to "
        "show what the reconciliation catches",
    )
    args = parser.parse_args()

    data = load_records(args.records)
    records = Records(data)
    scan = Scan(args.tree, simple_arguments_only=args.simple_arguments_only)

    print(f"inline narrowings outside the shared home: {len(scan.sites)}")
    print(f"sites routed through the shared family: {_shared_reader_calls(args.tree)}")
    print(f"structs on ctypes.Structure directly: {len(scan.plain_structs)}")

    failures = evaluate(scan, records, data["floor"])
    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
