#!/usr/bin/env python3
"""Check that every C-ABI pointer declaration states its lifecycle contract.

Three different obligations travel under the same ``*`` in a public header, and
a caller cannot tell them apart from the signature:

* **library-owned return** -- the library hands back a pointer it keeps owning
  (a thread-local buffer, a static table).  The doc owes what invalidates it and
  whether NULL is possible.
* **library-allocated out-parameter** -- the library allocates and the caller
  releases.  The doc owes the name of the ``sonare_free_*`` that releases it, or
  an explicit statement that it must not be freed.
* **caller pointer retained past the call** -- the library keeps using memory
  the caller owns after the call returns.  The doc owes the call that ends the
  retention.

A caller who cannot answer the question from the declaration either leaks, frees
with the wrong allocator, or frees something the library still owns.  None of
the three fails at the call site.

Classification is structural, from the declaration itself:

* a pointer return type puts a declaration in the owned-return class;
* a non-const pointer-to-pointer parameter puts it in the allocated class, as
  does a pointer to a result struct whose own definition carries pointer
  members;
* a function-pointer parameter, or a ``void*`` parameter, puts it in the
  retained class.

**The known common mode is the retained class**, and it is stated here rather
than papered over: retention is not visible in a signature.  A plain
``const float*`` input that the library stores and reads after the call looks
exactly like one it copies, so this scan sees only the retention that carries a
callback or an opaque user pointer.  A pointer retained without either is
invisible to the scan *and* to any cross-check built on the same classifier.

The second common mode is narrower: a section banner is not a doc block.  A
declaration that merely follows a ``====`` rule inherits nothing from it, so the
scan refuses to read one as the declaration's own contract -- otherwise a single
banner would silently certify every declaration beneath it.

``--floor`` pins how many pointer declarations the scan must classify at all,
separately from how many are unannotated: if the declaration pattern stops
matching, every consistency check passes over an empty set and certifies a dead
scanner.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
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

# A declaration's return type begins at column 0 in these headers.
_DECLARATION = re.compile(
    r"^[A-Za-z_][^\n(){};]*?\b(sonare_[A-Za-z0-9_]+)\s*\(", re.MULTILINE
)

_BANNER = re.compile(r"={6,}|-{20,}")

CLASS_OWNED_RETURN = "library-owned-return"
CLASS_ALLOCATED_OUT = "library-allocated-out-param"
CLASS_RETAINED_INPUT = "caller-pointer-retained"

# What each class's doc block has to say.  Every phrase is a contract a caller
# can act on, not a keyword: "NULL" alone answers a different question from
# "overwritten by the next call".
_RELEASE_CALL = re.compile(r"\bsonare_(?:free|release|destroy)_?\w*", re.IGNORECASE)
_NO_RELEASE = re.compile(
    r"must not be freed|do not free|does not transfer ownership|never free|"
    r"owned by the library|not the caller's to free",
    re.IGNORECASE,
)
_INVALIDATION = re.compile(
    r"overwritten|invalidat|remains valid|valid until|dangl|thread-local|"
    r"thread local|static storage|until the next",
    re.IGNORECASE,
)
_NULLABILITY = re.compile(r"\bNULL\b|\bnullptr\b", re.IGNORECASE)
_RETENTION = re.compile(
    r"must (?:remain|stay) valid|retain|outliv|until .*(?:call|close|stop|destroy|free)|"
    r"for the (?:duration|lifetime) of|not copied|copies the",
    re.IGNORECASE,
)


def _blank(match: re.Match[str]) -> str:
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


def strip_comments_and_literals(text: str) -> str:
    """Blank out comments and literals, preserving line structure."""
    return _LEXICAL.sub(_blank, text)


def match_paren(text: str, open_at: int) -> int:
    depth = 0
    for index in range(open_at, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError(f"unbalanced ( at offset {open_at}")


def split_top_level(text: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    start = 0
    for index, char in enumerate(text):
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(text[start:index])
            start = index + 1
    parts.append(text[start:])
    return [part for part in parts if part.strip()]


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def doc_block_before(text: str, offset: int) -> str:
    """The declaration's own doc block, with section banners refused.

    A banner is shared by everything under it, so reading one as a declaration's
    contract would let a single comment certify a whole section.
    """
    lines = text[:offset].split("\n")
    collected: list[str] = []
    for line in reversed(lines[:-1]):
        stripped = line.strip()
        if stripped.startswith(("///", "//", "*", "/*")) or stripped.endswith("*/"):
            collected.append(stripped)
            continue
        if not stripped and not collected:
            continue
        break
    block = "\n".join(reversed(collected))
    return "" if _BANNER.search(block) else block


def pointer_structs_and_docs(header_dir: Path) -> tuple[set[str], dict[str, str]]:
    """Typedef'd structs carrying a pointer member, and each one's doc block."""
    names: set[str] = set()
    docs: dict[str, str] = {}
    for path in sorted(header_dir.glob("*.h")):
        text = path.read_text(encoding="utf-8", errors="replace")
        code = strip_comments_and_literals(text)
        for match in re.finditer(r"typedef\s+struct\s*(?:\w+\s*)?\{", code):
            open_at = code.index("{", match.start())
            depth = 0
            end = open_at
            for index in range(open_at, len(code)):
                if code[index] == "{":
                    depth += 1
                elif code[index] == "}":
                    depth -= 1
                    if depth == 0:
                        end = index
                        break
            body = code[open_at + 1 : end]
            tail = code[end + 1 :].split(";", 1)[0].strip()
            if tail and "*" in body:
                names.add(tail)
                docs[tail] = doc_block_before(text, match.start())
    return names, docs


@dataclass
class Declaration:
    """One public function declaration and the doc block above it."""

    name: str
    file: str
    line: int
    signature: str
    doc: str
    return_type: str
    parameters: list[str]
    classes: list[str] = field(default_factory=list)
    subkinds: set[str] = field(default_factory=set)
    result_structs: set[str] = field(default_factory=set)

    @property
    def location(self) -> str:
        return f"{self.file}:{self.line}"


def parse_declarations(path: Path, text: str) -> list[Declaration]:
    """Every ``sonare_*`` declaration in one public header."""
    code = strip_comments_and_literals(text)
    found: list[Declaration] = []
    for match in _DECLARATION.finditer(code):
        open_paren = code.index("(", match.end() - 1)
        try:
            close = match_paren(code, open_paren)
        except ValueError:
            continue
        tail = code[close:].lstrip()
        if not tail.startswith(";"):
            continue  # a definition, or a function-pointer typedef
        line = code.count("\n", 0, match.start()) + 1
        found.append(
            Declaration(
                name=match.group(1),
                file=_display(path),
                line=line,
                signature=" ".join(code[match.start() : close].split()),
                doc=doc_block_before(text, match.start()),
                return_type=code[match.start() : match.start() + (match.end() - 1 - match.start())]
                .rsplit(match.group(1), 1)[0]
                .strip(),
                parameters=split_top_level(code[open_paren + 1 : close - 1]),
            )
        )
    return found


# A release entry point takes the pointer it frees; it does not hand one over.
# Recognised by its own name shape, which is a property of the declaration, not
# a list of the functions that happen to exist today.
_RELEASE_ENTRY = re.compile(r"^sonare_(?:free|release|destroy)\w*$|_(?:free|destroy)$")

# Sub-kinds of the allocated class, recorded because they are triaged
# differently: a pointer-to-pointer has nowhere but the declaration to carry its
# contract, while a result struct has a type whose own doc is the natural home.
SUBKIND_POINTER_TO_POINTER = "pointer-to-pointer"
SUBKIND_RESULT_STRUCT = "result-struct"


def classify(declaration: Declaration, pointer_structs: set[str]) -> list[str]:
    """The lifecycle classes a declaration belongs to, from its signature."""
    classes: list[str] = []
    if "*" in declaration.return_type:
        classes.append(CLASS_OWNED_RETURN)
    if _RELEASE_ENTRY.search(declaration.name):
        # Its pointer parameter is what the caller hands back, not what it
        # receives, so neither of the parameter classes applies.
        return classes
    for parameter in declaration.parameters:
        text = parameter.strip()
        if re.search(r"\(\s*\*\s*\w*\s*\)\s*\(", text) or re.search(r"\bvoid\s*\*", text):
            if CLASS_RETAINED_INPUT not in classes:
                classes.append(CLASS_RETAINED_INPUT)
            continue
        if re.match(r"^\s*const\b", text):
            continue
        if re.search(r"\*\s*\*", text):
            declaration.subkinds.add(SUBKIND_POINTER_TO_POINTER)
            if CLASS_ALLOCATED_OUT not in classes:
                classes.append(CLASS_ALLOCATED_OUT)
            continue
        struct = re.match(r"^\s*(\w+)\s*\*", text)
        if struct and struct.group(1) in pointer_structs:
            declaration.subkinds.add(SUBKIND_RESULT_STRUCT)
            declaration.result_structs.add(struct.group(1))
            if CLASS_ALLOCATED_OUT not in classes:
                classes.append(CLASS_ALLOCATED_OUT)
    return classes


def missing_contract(
    declaration: Declaration, klass: str, struct_docs: dict[str, str]
) -> str | None:
    """What the doc block fails to say for this class, or None."""
    doc = declaration.doc
    if klass == CLASS_OWNED_RETURN:
        missing = []
        if not _INVALIDATION.search(doc) and not _RELEASE_CALL.search(doc):
            missing.append("what invalidates the returned pointer")
        if not _NULLABILITY.search(doc):
            missing.append("whether NULL is possible")
        return " and ".join(missing) if missing else None
    if klass == CLASS_ALLOCATED_OUT:
        if _RELEASE_CALL.search(doc) or _NO_RELEASE.search(doc):
            return None
        # A result struct's own doc is the canonical home for its arrays'
        # ownership, and the type name is written in the declaration, so the
        # caller reaches it without consulting a sibling declaration.
        if SUBKIND_POINTER_TO_POINTER not in declaration.subkinds and all(
            _RELEASE_CALL.search(struct_docs.get(name, ""))
            or _NO_RELEASE.search(struct_docs.get(name, ""))
            for name in declaration.result_structs
        ):
            return None
        return "the sonare_free_* that releases the out-parameter"
    if klass == CLASS_RETAINED_INPUT:
        if _RETENTION.search(doc):
            return None
        return "how long the library retains the caller's pointer"
    raise AssertionError(klass)


@dataclass
class Finding:
    """One declaration whose doc does not state its class's contract."""

    klass: str
    subkinds: list[str]
    name: str
    file: str
    line: int
    missing: str

    def as_line(self) -> str:
        detail = f"/{','.join(self.subkinds)}" if self.subkinds else ""
        return f"[{self.klass}{detail}] {self.file}:{self.line}  {self.name}  needs {self.missing}"


@dataclass
class Report:
    findings: list[Finding]
    classified: dict[str, int]
    declarations: int

    @property
    def pointer_declarations(self) -> int:
        return sum(self.classified.values())


def audit(header_dir: Path) -> Report:
    """Classify and check every declaration under ``header_dir``."""
    pointer_structs, struct_docs = pointer_structs_and_docs(header_dir)
    findings: list[Finding] = []
    classified = {
        CLASS_OWNED_RETURN: 0,
        CLASS_ALLOCATED_OUT: 0,
        CLASS_RETAINED_INPUT: 0,
    }
    total = 0
    for path in sorted(header_dir.glob("*.h")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for declaration in parse_declarations(path, text):
            total += 1
            declaration.classes = classify(declaration, pointer_structs)
            for klass in declaration.classes:
                classified[klass] += 1
                missing = missing_contract(declaration, klass, struct_docs)
                if missing is not None:
                    findings.append(
                        Finding(
                            klass,
                            sorted(declaration.subkinds) if klass == CLASS_ALLOCATED_OUT else [],
                            declaration.name,
                            declaration.file,
                            declaration.line,
                            missing,
                        )
                    )
    findings.sort(key=lambda f: (f.klass, f.file, f.line))
    return Report(findings, classified, total)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header-dir", type=Path, default=DEFAULT_HEADER_DIR)
    parser.add_argument(
        "--class",
        dest="klass",
        choices=[CLASS_OWNED_RETURN, CLASS_ALLOCATED_OUT, CLASS_RETAINED_INPUT],
        help="restrict the report to one lifecycle class",
    )
    parser.add_argument(
        "--floor",
        type=int,
        default=0,
        help="minimum pointer declarations the scan must classify; guards "
        "against a declaration pattern that stopped matching",
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    report = audit(args.header_dir)
    findings = [f for f in report.findings if args.klass is None or f.klass == args.klass]

    if args.json:
        print(
            json.dumps(
                {
                    "declarations": report.declarations,
                    "classified": report.classified,
                    "findings": [f.__dict__ for f in findings],
                },
                indent=2,
            )
        )
    else:
        print(f"declarations scanned: {report.declarations}")
        for klass, count in report.classified.items():
            print(f"  {klass}: {count}")
        print(f"  declarations missing their contract: {len(findings)}")
        if findings:
            print(
                "\nThese declarations hand a pointer across the boundary without "
                "stating the contract:",
                *(f"  {finding.as_line()}" for finding in findings),
                sep="\n",
                file=sys.stderr,
            )

    if report.pointer_declarations < args.floor:
        print(
            f"\nclassified {report.pointer_declarations} pointer declarations, below "
            f"the floor of {args.floor}: the declaration pattern stopped matching, "
            "so a clean report here would certify nothing",
            file=sys.stderr,
        )
        return 2
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
