"""List the core's integer checks whose domain is written at the call instead of named.

A member's numeric domain is stated once, as a named ``NumericDomain`` constant the
check reads and ``numeric-domains.json`` dumps; a binding states the same domain when it
refuses a value before the core sees it. A check that passes constant bounds straight to
``integer_in_range`` and its siblings (or an inline ``integer_domain(...)``) has no row,
so a binding can only state the C type's range for that member. This lists those sites.

A bound that reads a runtime value (a buffer length, a prepared channel count) is a
relation between arguments rather than a domain, and is not listed.

Usage: python3 tools/api/unnamed_integer_bounds.py [--count]
"""

from __future__ import annotations

import re
import subprocess
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
    listed = subprocess.run(
        ["git", "ls-files", "src", "include"], cwd=root, capture_output=True, text=True, check=True
    ).stdout.split()
    out = []
    for path in listed:
        if not path.endswith((".cpp", ".h")) or path.startswith("src/wasm/"):
            continue
        if path.endswith("util/parameter_checks.h"):
            continue
        text = re.sub(r"//[^\n]*", lambda m: " " * len(m.group()), (root / path).read_text())

        def line(offset: int, text: str = text) -> int:
            return text.count("\n", 0, offset) + 1

        for m in _WITHIN.finditer(text):
            args = _arguments(text, m.end())
            if len(args) == 3 and _INLINE_DOMAIN.match(args[1]):
                out.append((path, line(m.start()), "within", args[1], args[2]))
        for m in _CALL.finditer(text):
            args = _arguments(text, m.end())
            bounds = args[1:2] if m.group(1) == "integer_at_least" else args[1:3]
            if bounds and all(_is_constant(b) for b in bounds):
                out.append((path, line(m.start()), m.group(1), ", ".join(bounds), args[-1]))
    return [(p, n, check, " ".join(b.split()), " ".join(s.split())) for p, n, check, b, s in out]


def main(argv: list[str]) -> int:
    found = sites()
    if "--count" in argv:
        print(len(found))
        return 0
    for path, line, check, bounds, subject in found:
        print(f"{path}:{line}\t{check}\t{bounds}\t{subject}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
