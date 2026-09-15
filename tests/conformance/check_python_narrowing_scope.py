#!/usr/bin/env python3
"""Keep every Python-to-C numeric narrowing in the ctypes binding accounted for.

Four populations, all enforced here, all drifting for the same reason -- a new
module can convert a caller's number however it likes and nothing notices:

* **Inline argument narrowings.**  ``ctypes.c_int(value)`` applies the C
  conversion, so ``2**32`` arrives as 0 -- which every versioned config field
  reads as "keep the default" -- and ``2**32 + 1`` arrives as 1.  A wrapped value
  is always inside the target type, so no downstream range check can see it.  A
  conversion must therefore go through the shared ``_to_c_*`` family or be
  recorded with the mechanism that makes it harmless.  The array constructor
  ``(ctypes.c_uint8 * n)(*values)`` is the same conversion applied to every
  element, so it counts as a site and is keyed on the element expression.
  ``ctypes.c_float(value)`` belongs to this population for the same reason by a
  different mechanism: a Python float is an IEEE double, so ``c_float(1e40)``
  saturates to an infinity instead of wrapping and instead of raising.  That is
  not a wrap, and it is the same defect -- the caller's quantity is folded onto
  another legal value, and on every field whose contract reads a non-finite
  input as "unspecified" the folded value is what a deliberate request looks
  like.
* **Struct field assignment.**  The same conversion happens on assignment to a
  ``ctypes.Structure`` field, and there the field's own type is the only thing
  that knows the bound, so a struct must inherit the base that reads it.
* **A width mask in front of a field assignment.**  ``raw.kind = int(x) & 0xFF``
  leaves the checked base's ``__setattr__`` installed and running, and hands it a
  value already folded into range.  The reader is there; the defect is a layer in
  front of it, so the mask is the reportable shape rather than the assignment.
  A bare ``int(...)`` is not reported: truncating to an integer is deliberate.
* **File-local readers.**  A helper that converts a caller's number to a ctypes
  scalar is a reader, and a reader outside the shared family means a change to
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
  to a ctypes integer type -- directly, or through the multiplication that builds
  an array type -- and that carries an argument, with the argument's source text
  taken from the tree.
* **Scan B, over the token stream.**  The ``NAME . NAME (`` sequence read from
  ``tokenize``, plus the array run ``( NAME . NAME * ... ) (``, with the
  argument's presence decided by the tokens that follow.  It never parses, so it
  sees the spelling rather than the structure, and it skips comments and string
  literals because the tokenizer classifies them rather than because a pattern
  blanked them.

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

# The shared home is a SET OF SYMBOLS, not a file. A conversion inside the
# shared implementation IS what every other site routes through, so the raw
# ctypes constructor belongs there -- but a file grows, and a predicate written
# beside the implementation inherits its trust without anyone deciding to. The
# exemptions below therefore name what is trusted, so a new helper next to one
# is reported rather than blessed by its address.

# The struct base whose __setattr__ checks an integer or a `c_float` field
# against the field's own declared type. Its own definition is the one class
# allowed to sit on ctypes.Structure directly, because it installs that check.
STRUCT_BASE = "CStruct"

# Spelled out rather than matched loosely: `c_char_p` and the pointer types
# carry no numeric range to leave, and sweeping them in would bury the
# population this exists for.
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

# Kept in its own list because the failure differs: an integer wraps, a float
# saturates to an infinity. `c_double` is absent -- a Python float already IS a
# double, so that conversion has nothing to narrow.
FLOAT_TYPES = ("c_float",)

# What both scans match on. Every population below keys on the spelling rather
# than on which list a type came from, so the two halves cannot drift apart.
NARROWING_TYPES = INTEGER_TYPES + FLOAT_TYPES

# A mask that folds a value into a C integer width. Only these are reported in
# front of a field assignment: they pre-empt the checked base's range check,
# whereas an arbitrary arithmetic expression does not.
WIDTH_MASKS = (0xFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF)

# Bracket tokens, for walking the array type expression to its closing paren.
OPENERS = ("(", "[", "{")
CLOSERS = (")", "]", "}")

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
    "_to_c_float",
)

# The readers that ARE the shared family rather than a file-local copy of one:
# the conversions above, plus the two range checks they route through and the
# wording each shares. Every name here is required to resolve; see
# _undefined_shared_readers.
SHARED_LOCAL_READERS = SHARED_READERS + (
    "_narrow_int",
    "_narrowing_error",
    "_narrow_float",
    "_float_narrowing_error",
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

    def __init__(
        self, path: Path, line: int, ctype: str, argument: str, *, container: bool = False
    ) -> None:
        self.path = path
        self.line = line
        self.type = ctype
        self.argument = " ".join(argument.split())
        # An array constructor converts every element, so the argument recorded
        # is the element expression rather than the sequence handed to the call.
        self.container = container

    @property
    def display(self) -> str:
        return f"{_display(self.path)}:{self.line}"

    @property
    def text(self) -> str:
        """The site as the report names it, in the spelling it was written in."""
        if self.container:
            return f"(ctypes.{self.type} * n)(...)  every element: {self.argument}"
        return f"ctypes.{self.type}({self.argument})"

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
        self.masked_fields: list[tuple[Path, int, str, int]] = []
        self.local_readers: list[tuple[Path, int, str]] = []
        self.aliased_imports: list[tuple[Path, int, str]] = []
        self._run()

    def _files(self) -> list[Path]:
        return sorted(p for p in self.tree.rglob("*.py") if p.is_file())

    def _run(self) -> None:
        for path in self._files():
            source = path.read_text(encoding="utf-8")
            tree = ast.parse(source)
            exempt = _shared_reader_bodies(tree)

            # Scan A: over the parsed syntax.
            for node in ast.walk(tree):
                conversion = _ctypes_narrowing_conversion(node)
                if conversion is None or not node.args or _within(exempt, node.lineno):
                    continue
                ctype, container = conversion
                value = _converted_value(node.args[0]) if container else node.args[0]
                if self.simple_arguments_only and not isinstance(value, ast.Name):
                    continue
                self.sites.append(
                    Site(path, node.lineno, ctype, ast.unparse(value), container=container)
                )

            # Scan B: over the token stream.
            self.token_counts[path.name] = _token_scan(source, exempt)

            self._collect_structure_bases(path, tree)
            self._collect_masked_fields(path, tree)
            self._collect_local_readers(path, tree)
            self._collect_aliased_imports(path, tree)

    def _collect_structure_bases(self, path: Path, tree: ast.Module) -> None:
        """A struct off the checked base folds every integer and float field it has."""
        for node in ast.walk(tree):
            if not isinstance(node, ast.ClassDef) or node.name == STRUCT_BASE:
                continue
            for base in node.bases:
                if (
                    isinstance(base, ast.Attribute)
                    and base.attr == "Structure"
                    and isinstance(base.value, ast.Name)
                    and base.value.id == "ctypes"
                ):
                    self.plain_structs.append((path, node.lineno, node.name))

    def _collect_masked_fields(self, path: Path, tree: ast.Module) -> None:
        """A width mask hands the field's own range check a value that cannot fail."""
        for node in ast.walk(tree):
            if isinstance(node, ast.Assign):
                targets, masked = node.targets, _width_mask(node.value)
            elif isinstance(node, ast.AugAssign):
                targets, masked = [node.target], _width_mask(node)
            else:
                continue
            if masked is None:
                continue
            for target in targets:
                if isinstance(target, ast.Attribute):
                    self.masked_fields.append((path, node.lineno, ast.unparse(target), masked))

    def _collect_local_readers(self, path: Path, tree: ast.Module) -> None:
        """A module-level helper whose whole job is a ctypes scalar conversion."""
        for node in ast.walk(tree):
            if not isinstance(node, ast.FunctionDef) or node.name in SHARED_LOCAL_READERS:
                continue
            returns = ast.unparse(node.returns) if node.returns is not None else ""
            if not any(f"ctypes.{name}" == returns for name in NARROWING_TYPES):
                continue
            self.local_readers.append((path, node.lineno, node.name))

    def _collect_aliased_imports(self, path: Path, tree: ast.Module) -> None:
        """The common mode: a type pulled in under a name the scans cannot see."""
        for node in ast.walk(tree):
            if not isinstance(node, ast.ImportFrom) or node.module != "ctypes":
                continue
            for alias in node.names:
                if alias.name in NARROWING_TYPES:
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


def _shared_reader_bodies(tree: ast.Module) -> list[tuple[int, int]]:
    """Line spans of the shared ``_to_c_*`` definitions -- the one exempt region.

    This is what both scans share beyond the ctypes qualification, so a defect in
    the spans is invisible to their disagreement. It is narrower than the
    filename it replaces for exactly that reason: a span covers a definition
    rather than everything written beside it.
    """
    return [
        (node.lineno, node.end_lineno or node.lineno)
        for node in ast.walk(tree)
        if isinstance(node, ast.FunctionDef) and node.name in SHARED_READERS
    ]


def _within(spans: list[tuple[int, int]], line: int) -> bool:
    return any(low <= line <= high for low, high in spans)


def _token_scan(source: str, exempt: list[tuple[int, int]]) -> int:
    """Scan B: both call spellings, read from the token stream."""
    stream = [
        token
        for token in tokenize.generate_tokens(io.StringIO(source).readline)
        if token.type not in (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE, tokenize.INDENT)
        and token.type != tokenize.DEDENT
        and not _within(exempt, token.start[0])
    ]
    return _token_calls(stream) + _token_arrays(stream)


def _token_calls(stream: list[tokenize.TokenInfo]) -> int:
    """``ctypes . <narrowing type> (`` followed by anything but ``)``."""
    count = 0
    for i in range(len(stream) - 4):
        names = [stream[i + k] for k in range(5)]
        if (
            names[0].string == "ctypes"
            and names[1].string == "."
            and names[2].string in NARROWING_TYPES
            and names[3].string == "("
            and names[4].string != ")"
        ):
            count += 1
    return count


def _token_arrays(stream: list[tokenize.TokenInfo]) -> int:
    """``( ctypes . <narrowing type> * ... ) (`` followed by anything but ``)``.

    The count expression between the type and the closing paren is arbitrary, so
    the run cannot be matched by a fixed window the way the call spelling is: the
    opening paren is walked to its own close and the call tested from there.
    """
    count = 0
    for i in range(1, len(stream) - 3):
        if not (
            stream[i - 1].string == "("
            and stream[i].string == "ctypes"
            and stream[i + 1].string == "."
            and stream[i + 2].string in NARROWING_TYPES
            and stream[i + 3].string == "*"
        ):
            continue
        close = _matching_close(stream, i - 1)
        if close is None or close + 2 >= len(stream):
            continue
        if stream[close + 1].string == "(" and stream[close + 2].string != ")":
            count += 1
    return count


def _matching_close(stream: list[tokenize.TokenInfo], start: int) -> int | None:
    """Index of the bracket closing the one at ``start``, or None if unbalanced."""
    depth = 0
    for j in range(start, len(stream)):
        if stream[j].string in OPENERS:
            depth += 1
        elif stream[j].string in CLOSERS:
            depth -= 1
            if depth == 0:
                return j
    return None


def _ctypes_narrowing_conversion(node: ast.AST) -> tuple[str, bool] | None:
    """The scalar type a call converts through, and whether it is an array type.

    Two spellings reach the same C conversion: ``ctypes.c_int(x)`` narrows one
    value, ``(ctypes.c_int * n)(*xs)`` narrows every element of ``xs``.
    """
    if not isinstance(node, ast.Call):
        return None
    if _is_ctypes_narrowing(node.func):
        return node.func.attr, False
    if isinstance(node.func, ast.BinOp) and isinstance(node.func.op, ast.Mult):
        for side in (node.func.left, node.func.right):
            if _is_ctypes_narrowing(side):
                return side.attr, True
    return None


def _is_ctypes_narrowing(node: ast.AST) -> bool:
    return (
        isinstance(node, ast.Attribute)
        and node.attr in NARROWING_TYPES
        and isinstance(node.value, ast.Name)
        and node.value.id == "ctypes"
    )


def _converted_value(argument: ast.expr) -> ast.expr:
    """Past a splat, the expression each converted element is built from."""
    if not isinstance(argument, ast.Starred):
        return argument
    if isinstance(argument.value, (ast.ListComp, ast.GeneratorExp)):
        return argument.value.elt
    return argument.value


def _width_mask(node: ast.AST) -> int | None:
    """The C integer width a ``&`` folds its value into, or None if it is not one."""
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitAnd):
        operands = (node.right, node.left)
    elif isinstance(node, ast.AugAssign) and isinstance(node.op, ast.BitAnd):
        operands = (node.value,)
    else:
        return None
    for operand in operands:
        if (
            isinstance(operand, ast.Constant)
            and not isinstance(operand.value, bool)
            and operand.value in WIDTH_MASKS
        ):
            return operand.value
    return None


class Records:
    """The recorded-benign data, and which of its entries matched anything."""

    def __init__(self, data: dict) -> None:
        self.shapes = data.get("shapes", [])
        self.narrowings = data.get("narrowings", [])
        self.structs = data.get("plain_structs", [])
        self.masked = data.get("masked_fields", [])
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

    def covers_masked_field(self, path: Path, target: str) -> bool:
        for entry in self.masked:
            if entry["file"] == path.name and entry["target"] == target:
                self.used.add(f"mask:{entry['file']}:{entry['target']}")
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
            + [f"mask:{e['file']}:{e['target']}" for e in self.masked]
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
        # Pinned apart from the total: the array spelling is a small population
        # a growing scalar count would otherwise hide the disappearance of.
        "array_narrowings": sum(1 for site in scan.sites if site.container),
        # Same reason, for the float half: it is the smaller of the two type
        # lists and the one a predicate written for integers drops silently.
        "float_narrowings": sum(1 for site in scan.sites if site.type in FLOAT_TYPES),
        "shared_float_reader_calls": _shared_reader_calls(scan.tree, ("_to_c_float",)),
        "shared_reader_calls": _shared_reader_calls(scan.tree),
    }
    return [
        f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- the scan has "
        "stopped matching, and an empty population agrees with everything"
        for name, minimum in floor.items()
        if measured.get(name, 0) < minimum
    ]


def _shared_reader_calls(tree: Path, readers: tuple[str, ...] = SHARED_READERS) -> int:
    """How many sites route through the named readers, as the population's floor."""
    pattern = re.compile(r"\b(?:" + "|".join(readers) + r")\(")
    return sum(
        len(pattern.findall(strip_lexical(p.read_text(encoding="utf-8"))))
        for p in tree.rglob("*.py")
    )


def _undefined_shared_readers(tree: Path) -> list[str]:
    """Names in SHARED_LOCAL_READERS that no longer resolve in the scanned tree.

    A name kept here after its function is gone exempts nothing, and it goes on
    blessing whatever takes the name next without that ever being reviewed. So
    an entry expires with the function it names.
    """
    defined = {
        node.name
        for path in tree.rglob("*.py")
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8")))
        if isinstance(node, ast.FunctionDef)
    }
    return [name for name in SHARED_LOCAL_READERS if name not in defined]


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

    # No exemption at all: an alias puts a conversion beyond the qualification
    # both scans depend on wherever it is written, the shared implementation
    # included, and there it would take the exempt spans with it.
    aliased = scan.aliased_imports
    if aliased:
        failures.append(
            (
                "These modules import a ctypes numeric type directly, which puts "
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

    masked = [
        (path, line, target, mask)
        for path, line, target, mask in scan.masked_fields
        if not records.covers_masked_field(path, target)
    ]
    if masked:
        failures.append(
            (
                "These width masks fold a value into range in front of a struct "
                f"field, so the {STRUCT_BASE} range check behind the assignment is "
                "handed a value that can no longer fail it",
                [
                    f"  {_display(p)}:{line}  {target} = ... & {mask:#x}"
                    for p, line, target, mask in masked
                ],
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
                [f"  {site.display}  {site.text}" for site in unrecorded],
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

    print(f"inline narrowings outside the shared family: {len(scan.sites)}")
    print(f"sites routed through the shared family: {_shared_reader_calls(args.tree)}")
    print(f"structs on ctypes.Structure directly: {len(scan.plain_structs)}")

    failures = evaluate(scan, records, data["floor"])
    # Asked of the real tree rather than inside evaluate, which the self-tests
    # call over scratch trees that define none of these names.
    undefined = _undefined_shared_readers(args.tree)
    if undefined:
        failures.append(
            (
                "These shared-family names no longer exist, so they exempt nothing",
                undefined,
            )
        )
    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
