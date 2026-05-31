"""Extract literal field-default initializers from a C++ config struct.

Used by the core-default parity check: the canonical "design" default of a
config parameter is the field initializer in its C++ core struct (e.g.
``struct MelodyConfig { float fmin = 65.0f; ... };``). Those values are compared
against the defaults each binding facade declares.

Only SIMPLE LITERAL initializers are extracted -- a numeric literal
(``65.0f`` / ``2048`` / ``-1``) or a boolean (``true`` / ``false``). Enum
members (``WindowType::Hann``), named constants (``constants::kC1Hz``), string
literals (``"auto"``) and braced / aggregate initializers (``{Mode::Major}``)
are intentionally SKIPPED: they cannot be compared to a facade default without a
value-resolution layer, and skipping keeps the check free of false positives.
Nested scopes (method bodies, braced initializers) are dropped before matching,
so only the struct's own field initializers are seen.
"""

from __future__ import annotations

import re
from pathlib import Path

# A field initializer ``<...> <name> = <rhs>;`` where rhs has no brace (so
# aggregate / braced initializers are excluded) and no embedded ';'.
_FIELD_RE = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*([^;{}]+?)\s*;")

# A simple numeric literal (optionally signed, with C++ float / int suffixes).
_NUMERIC_RE = re.compile(r"^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[fFlLuU]*$")


def _struct_body(text: str, struct_name: str) -> str | None:
    """Return the brace-delimited body of ``struct <struct_name>`` or None."""
    m = re.search(rf"\bstruct\s+{re.escape(struct_name)}\b", text)
    if m is None:
        return None
    brace = text.find("{", m.end())
    if brace < 0:
        return None
    depth = 0
    for i in range(brace, len(text)):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1 : i]
    return None


def _depth0(body: str) -> str:
    """Keep only characters at brace-depth 0 (drop method bodies / braced inits)."""
    out: list[str] = []
    depth = 0
    for ch in body:
        if ch == "{":
            depth += 1
            continue
        if ch == "}":
            depth = max(0, depth - 1)
            continue
        if depth == 0:
            out.append(ch)
    return "".join(out)


def extract_struct_defaults(header_path: Path, struct_name: str) -> dict[str, str]:
    """Map ``field_name -> literal default`` for one C++ struct.

    Returns only fields whose initializer is a numeric or boolean literal.
    Returns an empty dict if the file or struct is not found.
    """
    if not header_path.exists():
        return {}
    body = _struct_body(header_path.read_text(encoding="utf-8"), struct_name)
    if body is None:
        return {}
    out: dict[str, str] = {}
    for name, rhs in _FIELD_RE.findall(_depth0(body)):
        rhs = rhs.strip()
        if _NUMERIC_RE.match(rhs) or rhs in ("true", "false"):
            out.setdefault(name, rhs)
    return out
