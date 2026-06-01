"""Extract the three WASM-internal surfaces for the intra-binding consistency check.

The WASM binding is wired across three files that must stay consistent with each
other (this is a SEPARATE axis from the cross-binding facade-vs-C-API checks --
here we compare the WASM binding against ITSELF):

* ``src/wasm/bindings.cpp``            -- embind registrations: the C++ -> JS
  exposure truth (``function("analyzeMelody", &js_analyze_melody);``).
* ``bindings/wasm/src/sonare.js.d.ts`` -- the ``SonareModule`` TS interface: the
  type through which the facade calls the raw module.
* ``bindings/wasm/src/index.ts``       -- the public facade: calls
  ``module.X`` / ``requireModule().X``.

A free function must be (a) declared in ``SonareModule`` so TypeScript can call
it, and (b) wrapped in ``index.ts`` so users can reach it. A break in any leg is
a wiring bug invisible to the cross-binding checks, which read ``index.ts``
ALONE -- e.g. P0-4: ``analyzeSections`` was registered in embind but absent from
both the ``SonareModule`` type and the ``index.ts`` facade. This extractor
returns the three name sets (with source locations) so
``compare._wasm_internal_drift`` can cross-validate them.

Only FREE-FUNCTION embind registrations are collected. Class-method
registrations (``.function("addBus", ...)`` inside a ``class_<T>()`` chain)
belong to bound class types declared in their own interfaces, not
``SonareModule``; they are distinguished by the leading ``.`` and excluded.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

_BINDINGS_REL = "src/wasm/bindings.cpp"
_DTS_REL = "bindings/wasm/src/sonare.js.d.ts"
_INDEX_REL = "bindings/wasm/src/index.ts"

# A FREE-function embind registration ``function("name", ...)``. The negative
# lookbehind drops the class-method form ``.function("name", ...)`` (leading dot)
# and any identifier that merely ends in "function"; a qualified
# ``emscripten::function("name", ...)`` (leading ``:``) is still a free function
# and is kept.
_EMBIND_FREE_RE = re.compile(r'(?<![.\w])function\(\s*"([A-Za-z_]\w*)"')

# A ``module.X`` / ``requireModule().X`` member access in the facade. ``\s*``
# spans newlines so the chained ``module\n    .analyzeSections(...)`` form is
# captured (that exact spelling is why a single-line ``module\.X`` regex misses
# real usages).
_MODULE_REF_RE = re.compile(
    r"(?:\bmodule\b|requireModule\(\s*\))\s*\.\s*([A-Za-z_]\w*)"
)


@dataclass
class WasmInternal:
    """The three WASM-internal name sets, with source locations."""

    embind: dict[str, int] = field(
        default_factory=dict
    )  # free fn name -> bindings.cpp line
    iface: set[str] = field(default_factory=set)  # SonareModule member names
    refs: dict[str, int] = field(default_factory=dict)  # name -> first index.ts line
    bindings_file: str = _BINDINGS_REL
    dts_file: str = _DTS_REL
    index_file: str = _INDEX_REL
    available: bool = False  # True only when all three sources were found


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _embind_free(text: str) -> dict[str, int]:
    """Map each free-function embind registration name to its line number."""
    out: dict[str, int] = {}
    for m in _EMBIND_FREE_RE.finditer(text):
        out.setdefault(m.group(1), _line_of(text, m.start()))
    return out


def _module_refs(text: str) -> dict[str, int]:
    """Map each ``module.X`` / ``requireModule().X`` name to its first line."""
    out: dict[str, int] = {}
    for m in _MODULE_REF_RE.finditer(text):
        out.setdefault(m.group(1), _line_of(text, m.start()))
    return out


def _interface_body(text: str, name: str) -> str | None:
    """Return the brace-delimited body of ``interface <name>`` or None."""
    m = re.search(rf"\binterface\s+{re.escape(name)}\b", text)
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


def _iface_members(text: str) -> set[str]:
    """Top-level member names of the ``SonareModule`` interface.

    Walks the interface body tracking ``{`` / ``(`` / ``[`` nesting (NOT ``<>`` --
    the ``>`` of an arrow ``=>`` would corrupt the depth) and records an
    identifier as a member only when it sits at depth 0 and is immediately
    followed by ``:`` , ``?`` or ``(`` -- i.e. a ``name:`` property / arrow-type
    or a ``name(`` method shorthand. Nested object-type keys (depth > 0) and
    param names inside a signature are skipped.
    """
    body = _interface_body(text, "SonareModule")
    if body is None:
        return set()
    members: set[str] = set()
    depth = 0
    n = len(body)
    for m in re.finditer(r"[A-Za-z_]\w*|[{}()\[\]]", body):
        tok = m.group(0)
        if tok in "{([":
            depth += 1
            continue
        if tok in "})]":
            depth = max(0, depth - 1)
            continue
        if depth != 0:
            continue
        j = m.end()
        while j < n and body[j] in " \t\r\n":
            j += 1
        if j < n and body[j] in ":?(":
            members.add(tok)
    return members


def extract(root: Path) -> WasmInternal:
    bindings = root / _BINDINGS_REL
    dts = root / _DTS_REL
    index = root / _INDEX_REL
    if not (bindings.exists() and dts.exists() and index.exists()):
        return WasmInternal(available=False)
    return WasmInternal(
        embind=_embind_free(bindings.read_text(encoding="utf-8")),
        iface=_iface_members(dts.read_text(encoding="utf-8")),
        refs=_module_refs(index.read_text(encoding="utf-8")),
        available=True,
    )
