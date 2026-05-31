"""Extract literal field-default initializers from a C++ config struct.

Used by the core-default parity check: the canonical "design" default of a
config parameter is the field initializer in its C++ core struct (e.g.
``struct MelodyConfig { float fmin = 65.0f; ... };``). Those values are compared
against the defaults each binding facade declares.

Three kinds of field initializer are recognized:

* numeric / boolean literal (``65.0f`` / ``2048`` / ``true``) -- kept verbatim.
* named-constant reference (``constants::kC1Hz`` / ``chord_constants::kFoo``)
  -- RESOLVED to the constant's literal value via :func:`scan_constants` (only
  constants defined as a direct numeric literal are resolvable; computed ones
  fall through and are skipped).
* enum-member reference (``WindowType::Hann`` / ``TempogramMode::kAutocorrelation``)
  -- kept verbatim as ``Type::Member``; the comparison side canonicalizes it.

String literals (``"auto"``) and braced / aggregate initializers
(``{Mode::Major}``) are SKIPPED -- they cannot be compared to a facade default
without more machinery, and skipping keeps the check free of false positives.
Nested scopes (method bodies, braced initializers) are dropped before matching,
so only the struct's own field initializers are seen.

Disambiguation of the Google-style ``k``-prefix: enum members (``kAutocorrelation``)
and constants (``kC1Hz``) share a name shape, so a qualified RHS is resolved as a
CONSTANT first (looked up in the scanned constants map); only if that lookup
fails is it treated as an enum member.
"""

from __future__ import annotations

import re
from pathlib import Path

# A field initializer ``<...> <name> = <rhs>;`` where rhs has no brace (so
# aggregate / braced initializers are excluded) and no embedded ';'.
_FIELD_RE = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*([^;{}]+?)\s*;")

# A simple numeric literal (optionally signed, with C++ float / int suffixes).
_NUMERIC_RE = re.compile(r"^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[fFlLuU]*$")

# A qualified name ``Ns::...::Member`` (enum member or qualified constant ref).
_QUALIFIED_RE = re.compile(r"^(?:[A-Za-z_]\w*::)+[A-Za-z_]\w*$")

# A ``constexpr`` / ``const`` numeric constant definition: ``kName = <literal>;``.
_CONST_RE = re.compile(
    r"\bconst(?:expr)?\s+[A-Za-z_][\w:]*\s+(k[A-Za-z]\w*)\s*=\s*([^;{}]+?)\s*;"
)


def scan_constants(paths: list[Path]) -> dict[str, str]:
    """Map ``kName -> literal`` for every direct-numeric-literal constant found.

    Scans each header for ``[inline|static] const[expr] <type> kName = <num>;``.
    Computed constants (RHS not a bare numeric literal) are skipped.
    """
    out: dict[str, str] = {}
    for p in paths:
        if not p.exists():
            continue
        for name, rhs in _CONST_RE.findall(p.read_text(encoding="utf-8")):
            rhs = rhs.strip()
            if _NUMERIC_RE.match(rhs):
                out.setdefault(name, rhs)
    return out


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


def _classify_default(rhs: str, constants: dict[str, str]) -> str | None:
    """Classify a default RHS into a comparable form, or None to skip.

    Numeric / boolean literal -> verbatim; named-constant reference -> resolved
    literal (constant lookup wins over enum so a ``k``-prefixed constant is not
    mistaken for a Google-style enum member); qualified ``Type::Member`` -> raw
    enum reference. Anything else (string literal, aggregate, unresolved name).
    """
    rhs = rhs.strip()
    if _NUMERIC_RE.match(rhs) or rhs in ("true", "false"):
        return rhs
    # An empty-optional sentinel is "no value" -- equivalent to a facade ``None``
    # / ``null`` default, NOT an enum member. Fold to ``none`` so they compare
    # equal (otherwise ``std::nullopt`` would be read as enum member ``nullopt``).
    if rhs in ("std::nullopt", "nullopt", "nullptr", "std::nullptr_t"):
        return "none"
    final = rhs.split("::")[-1]
    if final in constants:
        return constants[final]
    if _QUALIFIED_RE.match(rhs):
        return rhs
    return None


def extract_struct_defaults(
    header_path: Path, struct_name: str, constants: dict[str, str] | None = None
) -> dict[str, str]:
    """Map ``field_name -> default`` for one C++ struct.

    The default is a numeric / boolean literal, a resolved named constant's
    literal, or a raw ``Type::Member`` enum reference. Fields whose initializer
    is none of these (string literal, aggregate, unresolved constant) are
    omitted. Returns an empty dict if the file or struct is not found.
    """
    constants = constants or {}
    if not header_path.exists():
        return {}
    body = _struct_body(header_path.read_text(encoding="utf-8"), struct_name)
    if body is None:
        return {}
    out: dict[str, str] = {}
    for name, rhs in _FIELD_RE.findall(_depth0(body)):
        val = _classify_default(rhs, constants)
        if val is not None:
            out.setdefault(name, val)
    return out


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def _split_top_commas(args: str) -> list[str]:
    """Split a C++ arg list on top-level commas (parens / brackets / angles)."""
    parts, depth, cur = [], 0, []
    for ch in args:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def _signature_arglists(text: str, func_name: str) -> list[str]:
    """Every balanced arg list of a DECLARATION/DEFINITION of ``func_name``.

    Skips method-call / qualified-name occurrences (a preceding word char, ``.``,
    ``>`` or ``:``), and requires the closing ``)`` to be followed by ``;`` or
    ``{`` (after trailing qualifiers) so only real signatures are captured.
    """
    out: list[str] = []
    pat = re.compile(r"(?<![\w:.>])" + re.escape(func_name) + r"\s*\(")
    for m in pat.finditer(text):
        depth, start = 0, None
        for j in range(m.end() - 1, len(text)):
            ch = text[j]
            if ch == "(":
                depth += 1
                if depth == 1:
                    start = j + 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    rest = re.sub(
                        r"^(?:const|noexcept|override|final|\s)+",
                        "",
                        text[j + 1 : j + 48].lstrip(),
                    )
                    if rest[:1] in (";", "{"):
                        out.append(text[start:j])
                    break
    return out


def extract_func_defaults(
    header_path: Path, func_name: str, constants: dict[str, str] | None = None
) -> dict[str, str]:
    """Map ``param_name -> default`` for a free function's DEFAULT ARGUMENTS.

    Scans every declaration/definition of ``func_name`` in the header (overloads
    included) and collects parameters that carry a default argument. A parameter
    whose default disagrees across overloads is dropped (ambiguous). Defaults are
    classified like struct fields (numeric / bool / resolved constant / enum).
    """
    constants = constants or {}
    if not header_path.exists():
        return {}
    text = _strip_comments(header_path.read_text(encoding="utf-8"))
    out: dict[str, str] = {}
    conflict: set[str] = set()
    for args in _signature_arglists(text, func_name):
        for part in _split_top_commas(args):
            lhs, eq, rhs = part.partition("=")
            if not eq:
                continue
            nm = re.search(r"([A-Za-z_]\w*)\s*$", lhs)
            if not nm:
                continue
            name = nm.group(1)
            val = _classify_default(rhs, constants)
            if val is None:
                continue
            if name in out and out[name] != val:
                conflict.add(name)
            else:
                out.setdefault(name, val)
    for name in conflict:
        out.pop(name, None)
    return out
