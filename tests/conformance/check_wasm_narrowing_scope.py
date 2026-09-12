#!/usr/bin/env python3
"""Keep every JS-to-C++ numeric narrowing in ``src/wasm`` accounted for.

Two populations, both enforced here, both drifting for the same reason -- a new
translation unit can convert a JS value however it likes and nothing notices:

* **File-local readers.**  A function taking an ``emscripten::val`` and returning
  a numeric type is a reader.  Readers belong in ``bindings/common/``; a second
  copy elsewhere means a fix to the conversion contract reaches some call sites
  and not others.  The Node addon has this rule already, enforced by its own
  test; this is the WASM half.
* **Inline narrowings.**  ``val::as<int>()`` SATURATES out of range, so every
  value past the maximum arrives as ``INT_MAX`` -- a plausible in-domain number
  that any positivity guard waves through.  A narrowing therefore has to go
  through the shared range-checked reader or be recorded with the mechanism that
  makes it harmless.

The records live in ``wasm_narrowing_records.json`` and are read as data.  A
record that matches nothing is itself an error: a stale one keeps asserting a
reviewed decision about a site that no longer exists, and the next narrowing to
land on that spelling inherits the blessing unexamined.

TWO SCANS, BY DIFFERENT ROUTES, WHOSE DISAGREEMENT IS AN ASSERTION
------------------------------------------------------------------
A blind spot in a regex is invisible from inside the regex, so re-reading it
proves nothing; only a second measurement by another route disagrees.  The
population is therefore counted twice and the disagreement is an error, not a
log line:

* **Scan A, top-down over declarations.**  Every parameter list mentioning
  ``val`` that opens a body, named function and lambda alike.  The body's extent
  comes from brace balance, never from a fixed character window -- a window stops
  inside the first long entry point and everything past it goes uncounted.  Each
  container's own count is taken by re-reading its body text with directly
  nested container bodies blanked out.
* **Scan B, bottom-up over expressions.**  Every cast match by absolute offset,
  assigned to the innermost container span that contains it.

Neither route can produce the other's answer by construction, so:

* every cast has a container -- an orphan means the declaration pattern is too
  narrow to see the function the cast sits in;
* the two per-container counts reconcile -- a disagreement means a body span was
  mis-measured;
* the scan finds a known-nonzero population.  Two empty sets agree perfectly:
  with the cast pattern made to match nothing, both checks above go green and
  certify a scanner that has stopped working, so a floor is pinned.

KNOWN COMMON MODE
-----------------
Both scans classify a cast through the one type list below.  A narrowing whose
type is spelled outside that list -- an alias, a typedef, a template parameter --
is invisible to both scans AND to their agreement, so the reconciliation cannot
detect it.  Widening the list is the only remedy; nothing here will report it.
The same is true of the declaration side for a parameter type that is an alias
for ``val``.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WASM_TREE = ROOT / "src" / "wasm"
SHARED_READER_DIR = WASM_TREE / "bindings" / "common"
RECORDS = Path(__file__).resolve().parent / "wasm_narrowing_records.json"

_SOURCE_SUFFIXES = (".cpp", ".cc", ".h", ".hpp")

# The common mode above lives here. Spelled out rather than matched loosely,
# because a loose pattern would sweep `as<float>` and `as<std::string>` into a
# population that is about integer saturation.
INTEGER_TYPES = (
    "int",
    "unsigned",
    "unsigned int",
    "short",
    "unsigned short",
    "long",
    "unsigned long",
    "long long",
    "unsigned long long",
    "size_t",
    "std::size_t",
    "ssize_t",
    "ptrdiff_t",
    "int8_t",
    "uint8_t",
    "int16_t",
    "uint16_t",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
    "std::int8_t",
    "std::uint8_t",
    "std::int16_t",
    "std::uint16_t",
    "std::int32_t",
    "std::uint32_t",
    "std::int64_t",
    "std::uint64_t",
)

# Return types that make a val-taking function a reader. `bool` is deliberately
# out: a boolean read cannot saturate, so a file-local one duplicates a helper
# without duplicating the defect this exists for.
NUMERIC_RETURNS = INTEGER_TYPES + ("float", "double", "long double")

# An embind registration with a JS name. A function reached this way is an entry
# point the module exports, not a reader -- structurally the two are identical
# (both take a val and return a number) and only the registration tells them
# apart.
_EXPORTED = re.compile(r"\bfunction\s*\(\s*\"[^\"]*\"\s*,\s*&\s*([\w:]+)")

_TYPE_ALTERNATION = "|".join(
    re.escape(name) for name in sorted(INTEGER_TYPES, key=len, reverse=True)
)
_CAST = re.compile(rf"\.\s*as\s*<\s*({_TYPE_ALTERNATION})\s*>\s*\(\s*\)")

# The qualified-name alternative is load-bearing: three headers spell the
# parameter `emscripten::val`, and without it every container in them disappears
# and their casts read as orphans.
_VAL_PARAM = re.compile(r"\b(?:emscripten\s*::\s*)?val\b")

# Trailing specifiers between a parameter list and the body it opens.
_AFTER_PARAMS = re.compile(
    r"(?:\s|const\b|noexcept\b|override\b|final\b|mutable\b|->|[\w:<>,&*\[\]()])*"
)

_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/""",
    re.DOTALL | re.VERBOSE,
)
_LITERAL = re.compile(
    r"""R"([^()\\ ]*)\(.*?\)\1"
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)


def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines and its length."""
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


def prepare(text: str) -> tuple[str, str]:
    """Return (text without comments, that text with literals blanked too).

    Both are the same length as the input, so an offset means the same position
    in either.  Structure -- brace balance, receiver extraction -- is read from
    the second, since a brace or a bracket inside a string literal would break
    it; the readable receiver text comes from the first.
    """
    readable = _LEXICAL.sub(_blank, text)
    return readable, _LITERAL.sub(_blank, readable)


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _match_forward(code: str, start: int, opener: str, closer: str) -> int:
    """Index just past the delimiter matching the one at ``start``, or -1."""
    depth = 0
    for i in range(start, len(code)):
        if code[i] == opener:
            depth += 1
        elif code[i] == closer:
            depth -= 1
            if depth == 0:
                return i + 1
    return -1


def _match_backward(code: str, end: int, opener: str, closer: str) -> int:
    """Index of the delimiter matching the closer at ``end``, or -1."""
    depth = 0
    for i in range(end, -1, -1):
        if code[i] == closer:
            depth += 1
        elif code[i] == opener:
            depth -= 1
            if depth == 0:
                return i
    return -1


def receiver_of(readable: str, code: str, dot: int) -> str:
    """The expression text a cast at ``dot`` is taken on.

    Walked backwards over subscripts, calls and member accesses so the key a
    field is read under survives into the record -- ``options["nFft"]`` rather
    than ``options``.
    """
    i = dot
    while i > 0:
        j = i - 1
        while j >= 0 and code[j] in " \t\n":
            j -= 1
        if j < 0:
            break
        if code[j] in "])":
            opener = "[" if code[j] == "]" else "("
            closer = code[j]
            start = _match_backward(code, j, opener, closer)
            if start < 0:
                break
            i = start
            continue
        if code[j].isalnum() or code[j] in "_":
            while j >= 0 and (code[j].isalnum() or code[j] == "_"):
                j -= 1
            i = j + 1
            # A member access or a qualification continues the expression.
            k = i - 1
            while k >= 0 and code[k] in " \t\n":
                k -= 1
            if k >= 0 and (code[k] == "." or (k > 0 and code[k - 1 : k + 1] in ("::", "->"))):
                i = k + 1 if code[k] == "." else k
                continue
            break
        break
    return " ".join(readable[i:dot].split())


class Container:
    """One val-taking function or lambda body, located by brace balance."""

    def __init__(self, path: Path, name: str, returns: str, body: tuple[int, int]) -> None:
        self.path = path
        self.name = name
        self.returns = returns
        self.start, self.end = body

    @property
    def is_lambda(self) -> bool:
        return self.name == "<lambda>"

    def contains(self, offset: int) -> bool:
        return self.start <= offset < self.end


def _declaration_before(readable: str, code: str, open_paren: int) -> tuple[str, str]:
    """The (name, return type) preceding a parameter list, or a lambda marker."""
    j = open_paren - 1
    while j >= 0 and code[j] in " \t\n":
        j -= 1
    if j >= 0 and code[j] == "]":
        return "<lambda>", ""
    end = j + 1
    while j >= 0 and (code[j].isalnum() or code[j] == "_"):
        j -= 1
    name = readable[j + 1 : end].strip()
    if not name:
        return "<lambda>", ""
    line_start = code.rfind("\n", 0, j + 1) + 1
    prefix = readable[line_start : j + 1]
    # `inline`, `static` and the like are not the return type; the last type-ish
    # token before the name is.
    tokens = [t for t in re.split(r"[\s*&]+", prefix.strip()) if t]
    skip = {"inline", "static", "constexpr", "extern", "virtual", "explicit", "template"}
    returns = next((t for t in reversed(tokens) if t not in skip), "")
    return name, returns


def containers_of(path: Path, readable: str, code: str) -> list[Container]:
    """Scan A: every val-taking body in one file, by brace balance."""
    found: list[Container] = []
    for open_paren in (i for i, ch in enumerate(code) if ch == "("):
        close = _match_forward(code, open_paren, "(", ")")
        if close < 0:
            continue
        if not _VAL_PARAM.search(code[open_paren:close]):
            continue
        tail = _AFTER_PARAMS.match(code, close)
        brace = tail.end() if tail else close
        if brace >= len(code) or code[brace] != "{":
            continue
        body_end = _match_forward(code, brace, "{", "}")
        if body_end < 0:
            continue
        name, returns = _declaration_before(readable, code, open_paren)
        found.append(Container(path, name, returns, (brace, body_end)))
    return found


class Site:
    """One narrowing, with everything a record is keyed on."""

    def __init__(self, path: Path, offset: int, line: int, cast_type: str, receiver: str) -> None:
        self.path = path
        self.offset = offset
        self.line = line
        self.type = cast_type
        self.receiver = receiver
        self.container: Container | None = None

    @property
    def display(self) -> str:
        return f"{_display(self.path)}:{self.line}"

    @property
    def key(self) -> tuple[str, str, str]:
        return (str(self.path.relative_to(WASM_TREE)), self.receiver, self.type)


class Scan:
    """Both scans over the whole tree, plus what their disagreement says."""

    def __init__(self, tree: Path = WASM_TREE, *, body_window: int | None = None) -> None:
        self.tree = tree
        # A fixed window instead of brace balance -- kept as an option so the
        # cross-check's power can be demonstrated rather than argued.
        self.body_window = body_window
        self.containers: list[Container] = []
        self.sites: list[Site] = []
        self.readers: list[Container] = []
        self.exported: set[str] = set()
        self.declared_counts: dict[int, int] = {}
        self._run()

    def _files(self) -> list[Path]:
        return sorted(
            p
            for p in self.tree.rglob("*")
            if p.suffix in _SOURCE_SUFFIXES and p.is_file()
        )

    def _run(self) -> None:
        prepared = {}
        for path in self._files():
            prepared[path] = prepare(path.read_text(encoding="utf-8", errors="replace"))
            self.exported.update(
                name.rsplit("::", 1)[-1] for name in _EXPORTED.findall(prepared[path][0])
            )
        for path, (readable, code) in prepared.items():
            containers = containers_of(path, readable, code)
            self.containers.extend(containers)
            for container in containers:
                if (
                    not container.is_lambda
                    and container.returns in NUMERIC_RETURNS
                    and container.name not in self.exported
                ):
                    self.readers.append(container)

            # Scan A: each container's own count, taken by re-reading its body
            # text with directly nested container bodies blanked.
            for container in containers:
                read_to = container.end
                if self.body_window is not None:
                    read_to = min(container.start + self.body_window, container.end)
                body = list(code[container.start : read_to])
                for other in containers:
                    if other is container or not container.contains(other.start):
                        continue
                    if any(
                        mid is not container
                        and mid is not other
                        and mid.contains(other.start)
                        and container.contains(mid.start)
                        for mid in containers
                    ):
                        continue
                    lo = other.start - container.start
                    hi = min(other.end, read_to) - container.start
                    for i in range(max(lo, 0), max(hi, 0)):
                        if body[i] != "\n":
                            body[i] = " "
                self.declared_counts[id(container)] = len(
                    _CAST.findall("".join(body))
                )

            # Scan B: each cast by absolute offset, then innermost container.
            for match in _CAST.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                site = Site(
                    path,
                    match.start(),
                    line,
                    match.group(1),
                    receiver_of(readable, code, match.start()),
                )
                enclosing = [c for c in containers if c.contains(site.offset)]
                if enclosing:
                    site.container = max(enclosing, key=lambda c: c.start)
                self.sites.append(site)

    @property
    def orphans(self) -> list[Site]:
        return [site for site in self.sites if site.container is None]

    def reconcile(self) -> list[str]:
        """Containers whose two counts disagree, named with both numbers."""
        observed: dict[int, int] = {}
        for site in self.sites:
            if site.container is not None:
                observed[id(site.container)] = observed.get(id(site.container), 0) + 1
        disagreements = []
        for container in self.containers:
            declared = self.declared_counts.get(id(container), 0)
            counted = observed.get(id(container), 0)
            if declared != counted:
                disagreements.append(
                    f"{_display(container.path)} {container.name}: "
                    f"scan A saw {declared}, scan B assigned {counted}"
                )
        return sorted(disagreements)


def load_records(path: Path = RECORDS) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


class Records:
    """The recorded-benign data, and which of its entries matched anything."""

    def __init__(self, data: dict) -> None:
        self.shapes = data.get("shapes", [])
        self.pending_types = {e["type"]: e for e in data.get("pending_types", [])}
        self.narrowings = data.get("narrowings", [])
        self.readers = data.get("readers", [])
        self.used: set[str] = set()
        self._shape_patterns = {
            shape["name"]: re.compile(shape["receiver_pattern"]) for shape in self.shapes
        }

    def covers(self, site: Site) -> bool:
        for shape in self.shapes:
            if self._shape_patterns[shape["name"]].search(site.receiver):
                self.used.add(f"shape:{shape['name']}")
                return True
        if site.type in self.pending_types:
            self.used.add(f"pending_type:{site.type}")
            return True
        for entry in self.narrowings:
            if (entry["file"], entry["receiver"], entry["type"]) == site.key:
                self.used.add(f"narrowing:{entry['file']}:{entry['receiver']}")
                return True
        return False

    def covers_reader(self, container: Container) -> bool:
        relative = str(container.path.relative_to(WASM_TREE))
        for entry in self.readers:
            if entry["file"] == relative and entry["symbol"] == container.name:
                self.used.add(f"reader:{relative}:{container.name}")
                return True
        return False

    def pending_count(self) -> int:
        """Records that enumerate a site without grading what it does."""
        return sum(
            1
            for entry in self.narrowings + self.readers
            if entry.get("triage") == "pending"
        )

    def unused(self) -> list[str]:
        every = (
            [f"shape:{s['name']}" for s in self.shapes]
            + [f"pending_type:{t}" for t in self.pending_types]
            + [f"narrowing:{e['file']}:{e['receiver']}" for e in self.narrowings]
            + [f"reader:{e['file']}:{e['symbol']}" for e in self.readers]
        )
        return sorted(name for name in every if name not in self.used)


def _self_check(scan: Scan, floor: dict) -> list[str]:
    """The mandatory one. Two empty sets agree perfectly.

    With the cast pattern matching nothing, the orphan check and the
    reconciliation both pass and certify a scanner that has stopped working, so
    the population's own size is asserted before either of them is believed.
    """
    failures = []
    measured = {
        "files": len({site.path for site in scan.sites}),
        "containers": len(scan.containers),
        "narrowings": len(scan.sites),
        "readers": len(scan.readers),
    }
    for name, minimum in floor.items():
        if measured.get(name, 0) < minimum:
            failures.append(
                f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- "
                "the scan has stopped matching, and an empty population agrees "
                "with everything"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, default=WASM_TREE)
    parser.add_argument("--records", type=Path, default=RECORDS)
    parser.add_argument(
        "--body-window",
        type=int,
        default=None,
        help="measure bodies with a fixed character window instead of brace "
        "balance, to show what the reconciliation catches",
    )
    parser.add_argument(
        "--no-qualified-val",
        action="store_true",
        help="stop matching an `emscripten::val` parameter, to show what the "
        "orphan check catches",
    )
    args = parser.parse_args()

    global _VAL_PARAM
    if args.no_qualified_val:
        _VAL_PARAM = re.compile(r"\bval\b(?<!::val)")

    data = load_records(args.records)
    records = Records(data)
    scan = Scan(args.tree, body_window=args.body_window)

    print(f"val-taking bodies: {len(scan.containers)}")
    print(f"integer narrowings: {len(scan.sites)}")
    print(f"val-taking numeric-returning functions: {len(scan.readers)}")
    # The standing debt, printed every run rather than left in the file. A
    # record marked pending says the site was enumerated and NOT graded, which
    # is a different claim from benign and must not read as one.
    pending = records.pending_count()
    if pending:
        print(f"records carrying an ungraded site: {pending}")

    failures: list[tuple[str, list[str]]] = []

    self_check = _self_check(scan, data["floor"])
    if self_check:
        failures.append(("The scan no longer finds the population it is sized for", self_check))

    orphans = scan.orphans
    if orphans:
        failures.append(
            (
                "These narrowings sit in no val-taking body, so the declaration "
                "pattern is too narrow to see the function they are in",
                [f"  {site.display}  {site.receiver}.as<{site.type}>()" for site in orphans],
            )
        )

    disagreements = scan.reconcile()
    if disagreements:
        failures.append(
            (
                "The two scans disagree on these bodies, so a body span was "
                "mis-measured and one of the counts is wrong",
                [f"  {line}" for line in disagreements],
            )
        )

    local_readers = [
        container
        for container in scan.readers
        if SHARED_READER_DIR not in container.path.parents
        and not records.covers_reader(container)
    ]
    if local_readers:
        failures.append(
            (
                "These file-local readers convert a JS value outside the shared "
                "reader family, so a change to the conversion contract does not "
                "reach their call sites",
                [
                    f"  {_display(c.path)}  {c.returns} {c.name}(val)"
                    for c in sorted(local_readers, key=lambda c: (str(c.path), c.name))
                ],
            )
        )

    unrecorded = [site for site in scan.sites if not records.covers(site)]
    if unrecorded:
        failures.append(
            (
                "These narrowings are neither performed by the shared reader nor "
                "recorded with the mechanism that makes them harmless",
                [
                    f"  {site.display}  {site.receiver}.as<{site.type}>()"
                    for site in unrecorded
                ],
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

    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
