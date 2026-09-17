"""Check that a public type lives in the header whose declarations use it.

A header can define a type none of its own declarations mention and hand it to a
sibling through an include.  Everything compiles, every gate stays green, and the
type's home is whichever file it was first typed into.  The reader who then looks
for the type under the API that takes it does not find it there.

The invariant is one-directional, so it stays narrow:

    a type defined in a header that declares functions, used by none of that
    header's own declarations, and taken by a declaration in a sibling header

**A type defined and used in the same header is never reported**, which is the
overwhelming majority and the population this check must stay off.  A type no
sibling uses is not reported either -- that is a dead-type question, not a
placement one.

Two exclusions carry the whole check, and both were found on the site this scan
was calibrated against, where each on its own would have hidden the defect:

* **a mention in a doc comment is not a use.**  The misplaced type was named in
  an ``@ref`` three lines below its own definition.
* **a mention inside ``static_assert`` is not a use.**  The same type's layout
  assertion sits beside it.

Neither is a declaration taking the type, so neither makes the header the type's
home; counting either one certifies exactly the arrangement this looks for.

**Headers that declare no functions are exempt**, and the test is structural
rather than a name list: defining types for siblings to take is what a types
header is for, so it cannot be evidence against one.  A header that declares a
surface and defines a type outside it is the shape this reports.

``--floor`` pins how many typedefs the scan resolves at all, separately from how
many it reports: a definition pattern that stopped matching would otherwise
certify a clean tree over an empty set.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_HEADER_DIR = ROOT / "include" / "sonare"

_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)

# A layout assertion names the type it guards, which is not the type being used
# by a declaration.  Matched by name so both spellings are covered.
_ASSERTION = re.compile(r"\b_?[Ss]tatic_assert\s*\(")

# A declaration's return type begins at column 0 in these headers.
_DECLARATION = re.compile(r"^[A-Za-z_][^\n(){};]*?\b(\w+)\s*\([^;{]*\)\s*;", re.MULTILINE | re.DOTALL)

_TYPEDEF_HEAD = re.compile(r"\btypedef\s+(?:struct|enum|union)\b")


def _blank(match: re.Match[str]) -> str:
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


def strip_comments_and_literals(text: str) -> str:
    """Blank out comments and literals, preserving line structure."""
    return _LEXICAL.sub(_blank, text)


def _match_delimiter(text: str, open_at: int) -> int:
    """Index just past the delimiter closing the one at ``open_at``."""
    pairs = {"(": ")", "{": "}"}
    opener = text[open_at]
    closer = pairs[opener]
    depth = 0
    for index in range(open_at, len(text)):
        if text[index] == opener:
            depth += 1
        elif text[index] == closer:
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError(f"unbalanced {opener} at offset {open_at}")


def strip_assertions(code: str) -> str:
    """Blank out every ``static_assert(...)``, balanced, keeping line structure."""
    out = list(code)
    for match in _ASSERTION.finditer(code):
        open_at = code.index("(", match.end() - 1)
        try:
            end = _match_delimiter(code, open_at)
        except ValueError:
            continue
        for index in range(match.start(), end):
            if out[index] != "\n":
                out[index] = " "
    return "".join(out)


@dataclass
class Definition:
    """One typedef'd type and the span of the statement that defines it."""

    name: str
    header: str
    line: int
    start: int
    end: int


def parse_definitions(name: str, code: str) -> list[Definition]:
    """Every ``typedef struct/enum/union`` in one header, by brace balance.

    The body is taken by brace balance rather than by a nesting-limited pattern:
    a regex that models one level of nesting stops early inside a deeper struct
    and reads a member name as the type name, which produces one-letter types.
    """
    found: list[Definition] = []
    for head in _TYPEDEF_HEAD.finditer(code):
        semicolon = code.find(";", head.end())
        brace = code.find("{", head.end())
        if brace != -1 and (semicolon == -1 or brace < semicolon):
            try:
                after_body = _match_delimiter(code, brace)
            except ValueError:
                continue
            semicolon = code.find(";", after_body)
        if semicolon == -1:
            continue
        tail = code[:semicolon]
        alias = re.search(r"(\w+)\s*$", tail)
        if alias is None:
            continue
        found.append(
            Definition(
                alias.group(1),
                name,
                code.count("\n", 0, head.start()) + 1,
                head.start(),
                semicolon + 1,
            )
        )
    return found


def declares_functions(code: str) -> int:
    """How many function declarations a header carries."""
    return len(_DECLARATION.findall(code))


def _mentioned_outside(code: str, name: str, spans: list[tuple[int, int]]) -> bool:
    """Whether ``name`` appears anywhere but inside its own definitions."""
    pattern = re.compile(r"\b" + re.escape(name) + r"\b")
    return any(
        not any(start <= match.start() < end for start, end in spans)
        for match in pattern.finditer(code)
    )


@dataclass
class Finding:
    """One type defined by a surface header that none of its declarations use."""

    type_name: str
    header: str
    line: int
    used_by: list[str]

    def as_line(self) -> str:
        return (
            f"{self.header}:{self.line}  {self.type_name}  is defined here, used by no "
            f"declaration here, and taken by {', '.join(self.used_by)}"
        )


@dataclass
class Report:
    findings: list[Finding]
    typedefs: int
    surface_headers: int
    types_headers: int


def audit(header_dir: Path) -> Report:
    """Classify every typedef under ``header_dir`` by the header that uses it."""
    code_of: dict[str, str] = {}
    for path in sorted(header_dir.glob("*.h")):
        code_of[path.name] = strip_assertions(
            strip_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
        )

    definitions: dict[str, list[Definition]] = {}
    for name, code in code_of.items():
        for definition in parse_definitions(name, code):
            definitions.setdefault(definition.name, []).append(definition)

    surface = {name for name, code in code_of.items() if declares_functions(code) > 0}
    findings: list[Finding] = []
    for type_name, defs in sorted(definitions.items()):
        home = defs[0].header
        if any(d.header != home for d in defs):
            continue  # defined in more than one header; not a placement question
        if home not in surface:
            continue  # a types header defines for its siblings by design
        spans = [(d.start, d.end) for d in defs]
        if _mentioned_outside(code_of[home], type_name, spans):
            continue
        used_by = sorted(
            name
            for name, code in code_of.items()
            if name != home and re.search(r"\b" + re.escape(type_name) + r"\b", code)
        )
        if used_by:
            findings.append(Finding(type_name, home, defs[0].line, used_by))
    findings.sort(key=lambda f: (f.header, f.line))
    return Report(findings, len(definitions), len(surface), len(code_of) - len(surface))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header-dir", type=Path, default=DEFAULT_HEADER_DIR)
    parser.add_argument(
        "--floor",
        type=int,
        default=0,
        help="minimum typedefs the scan must resolve; guards against a definition "
        "pattern that stopped matching reporting a clean tree",
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    report = audit(args.header_dir)

    if args.json:
        print(
            json.dumps(
                {
                    "typedefs": report.typedefs,
                    "surface_headers": report.surface_headers,
                    "types_headers": report.types_headers,
                    "findings": [f.__dict__ for f in report.findings],
                },
                indent=2,
            )
        )
    else:
        print(f"typedefs resolved: {report.typedefs}")
        print(f"  headers declaring a surface: {report.surface_headers}")
        print(f"  headers defining types only, exempt: {report.types_headers}")
        print(f"  types defined away from the declarations that take them: {len(report.findings)}")
        if report.findings:
            print(
                "\nThese types are defined by a header whose own declarations never take them:",
                *(f"  {finding.as_line()}" for finding in report.findings),
                sep="\n",
                file=sys.stderr,
            )

    if report.typedefs < args.floor:
        print(
            f"\nresolved {report.typedefs} typedefs, below the floor of {args.floor}: the "
            "definition pattern stopped matching, so a clean report here would certify "
            "nothing",
            file=sys.stderr,
        )
        return 2
    return 1 if report.findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
