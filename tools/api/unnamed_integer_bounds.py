"""List the core's integer checks whose domain is written at the call instead of named.

A member's numeric domain is stated once, as a named ``NumericDomain`` constant the
check reads and ``numeric-domains.json`` dumps; a binding states the same domain when it
refuses a value before the core sees it. A check that passes constant bounds straight to
``integer_in_range`` and its siblings (or an inline ``integer_domain(...)``) has no row,
so a binding can only state the C type's range for that member. This lists those sites.

A bound that reads a runtime value (a buffer length, a prepared channel count) is a
relation between arguments rather than a domain, and is not listed.

Each site falls in one class, derived rather than tagged:
- ``enum``: the bounds are an enumeration's ordinals (a ``*_COUNT`` / ``k*Count`` limit, or
  enumerator casts); a closed set, not a numeric domain.
- ``c-only``: no Python, Node or WASM facade parameter or record field of a numeric type
  carries the subject's name, so a facade reaches it only as a token or not at all, and the
  core's own refusal states the domain.
- ``facade``: a facade reaches the subject (or the subject is not a literal, so that cannot
  be ruled out); a facade refusing a fraction there states the C type's range.

It fails on any ``facade`` site and when it scanned no integer check at all; the other
two classes are reported only.

Usage: python3 tools/api/unnamed_integer_bounds.py [--count]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

_CALL = re.compile(
    r"\b(integer_in_range|integer_at_least|power_of_two_in_range|even_integer_in_range"
    r"|odd_integer_in_range)\s*(<[^>(]*>)?\s*\("
)
_WITHIN = re.compile(r"\bwithin\s*\(")
_INLINE_DOMAIN = re.compile(r"^(\w+::)*integer_domain\w*\s*(<[^>]*>)?\s*\(")
# Spellings inside a constant expression that name a type or a cast, not a runtime value.
_TYPE_WORDS = frozenset(
    [
        "static_cast",
        "long",
        "int",
        "unsigned",
        "short",
        "char",
        "float",
        "double",
        "std",
        "numeric_limits",
        "max",
        "min",
        "lowest",
        "sizeof",
        "size_t",
        "int8_t",
        "int16_t",
        "int32_t",
        "int64_t",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "uint64_t",
    ]
)


def _arguments(text: str, start: int) -> list[str]:
    """The top-level arguments of the call whose opening parenthesis ends at ``start``."""
    depth, current, out = 0, "", []
    for c in text[start:]:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                out.append(current.strip())
                return out
            depth -= 1
        if c == "," and depth == 0:
            out.append(current.strip())
            current = ""
        else:
            current += c
    return out


def _is_constant(expr: str) -> bool:
    """Whether ``expr`` reads no runtime value: literals, k-constants, enumerators, casts."""
    stripped = re.sub(r"\b0x[0-9a-fA-F]+\b|\b\d[\w']*", " ", expr)
    for m in re.finditer(r"(::|\.|->)?\b([A-Za-z_]\w*)(\s*::)?", stripped):
        name = m.group(2)
        qualified_or_member = m.group(1) or m.group(3)
        if qualified_or_member or name in _TYPE_WORDS or name[0].isupper():
            continue
        if re.match(r"k[A-Z]", name):
            continue
        return False
    return True


def sites(root: Path = ROOT) -> list[tuple[str, int, str, str, str]]:
    """(path, line, check, bounds, subject) for every unnamed constant integer bound."""
    return scan(root)[0]


def sources(root: Path = ROOT) -> list[str]:
    """The C++ sources under src/ and include/, read from the tree so no git checkout is needed."""
    return sorted(
        file.relative_to(root).as_posix()
        for top in ("src", "include")
        for file in (root / top).rglob("*")
        if file.suffix in (".cpp", ".h") and file.is_file()
    )


def scan(root: Path = ROOT) -> tuple[list[tuple[str, int, str, str, str]], int]:
    """The unnamed bounds and the number of integer checks read."""
    scanned = 0
    out = []
    for path in sources(root):
        if path.startswith("src/wasm/") or path.endswith("util/parameter_checks.h"):
            continue
        text = re.sub(r"//[^\n]*", lambda m: " " * len(m.group()), (root / path).read_text())

        def line(offset: int, text: str = text) -> int:
            return text.count("\n", 0, offset) + 1

        for m in _WITHIN.finditer(text):
            args = _arguments(text, m.end())
            scanned += 1
            if len(args) == 3 and _INLINE_DOMAIN.match(args[1]):
                out.append((path, line(m.start()), "within", args[1], args[2]))
        for m in _CALL.finditer(text):
            args = _arguments(text, m.end())
            scanned += 1
            bounds = args[1:2] if m.group(1) == "integer_at_least" else args[1:3]
            if bounds and all(_is_constant(b) for b in bounds):
                out.append((path, line(m.start()), m.group(1), ", ".join(bounds), args[-1]))
    rows = [(p, n, check, " ".join(b.split()), " ".join(s.split())) for p, n, check, b, s in out]
    return rows, scanned


CLASSES = ("enum", "c-only", "facade")
_ENUM_LIMIT = re.compile(r"\b(SONARE_\w*_COUNT|k\w*Count)\b")
_ENUMERATOR = re.compile(r"^static_cast<\w+>\(\s*(\w+::)+[A-Z]\w*\s*\)$")
_FACADES = ("python", "node", "wasm")


def _numeric(spelled: str) -> bool:
    """Whether a facade type admits a number; an unextracted type is not ruled out."""
    return not spelled or bool(
        re.search(r"\b(int|number|bigint)\b", spelled) or re.fullmatch(r"[\d\s|]+", spelled)
    )


def facade_names(root: Path = ROOT) -> frozenset[str]:
    """Every numeric parameter and record-field name the facades expose, in snake case.

    Read through the parity extractors; a Python ctypes mirror (``_ffi*``) is not a facade.
    """
    sys.path.insert(0, str(root / "tools/parity"))
    try:
        import check_parity
    finally:
        sys.path.pop(0)
    names: set[str] = set()
    for surface in _FACADES:
        extraction = check_parity._EXTRACTORS[surface](root)
        for fn in extraction.functions:
            names.update(p.name for p in fn.params if _numeric(p.type))
        for record in extraction.records:
            if not Path(record.file).name.startswith("_ffi"):
                names.update(f.name for f in record.fields if _numeric(f.type))
    return frozenset(names)


def subject_name(subject: str) -> str | None:
    """The member name a subject expression ends in, or None when it holds no literal."""
    literals = re.findall(r'"([^"]*)"', subject)
    if not literals:
        return None
    leaf = literals[-1].rpartition(".")[2]
    return re.sub(r"\[[^]]*\]", "", leaf) or None


def classify(bounds: str, subject: str, exposed: frozenset[str]) -> str:
    parts = [b.strip() for b in bounds.split(",")]
    if _ENUM_LIMIT.search(bounds) or (len(parts) == 2 and all(_ENUMERATOR.match(b) for b in parts)):
        return "enum"
    name = subject_name(subject)
    if name is not None and name not in exposed:
        return "c-only"
    return "facade"


def classified(root: Path = ROOT) -> list[tuple[str, int, str, str, str, str]]:
    """Every site with its class appended."""
    exposed = facade_names(root)
    return [(*site, classify(site[3], site[4], exposed)) for site in sites(root)]


def main(argv: list[str]) -> int:
    found, scanned = scan()
    exposed = facade_names()
    rows = [(*site, classify(site[3], site[4], exposed)) for site in found]
    counts = {name: sum(row[5] == name for row in rows) for name in CLASSES}
    if "--count" in argv:
        for name in CLASSES:
            print(f"{name}\t{counts[name]}")
    else:
        for path, line, check, bounds, subject, cls in rows:
            print(f"{path}:{line}\t{cls}\t{check}\t{bounds}\t{subject}")
    print(
        f"unnamed integer bounds: scanned {scanned} integer checks; "
        + " ".join(f"{name}={counts[name]}" for name in CLASSES),
        file=sys.stderr,
    )
    if scanned == 0:
        print("unnamed integer bounds: no integer check was scanned", file=sys.stderr)
        return 1
    if counts["facade"]:
        print(
            f"unnamed integer bounds: {counts['facade']} facade-reached site(s) state their "
            "domain at the call; name it as a NumericDomain",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
