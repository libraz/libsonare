"""Extract the three WASM-internal surfaces for the intra-binding consistency check.

The WASM binding is wired across three surfaces that must stay consistent with
each other (this is a SEPARATE axis from the cross-binding facade-vs-C-API
checks -- here we compare the WASM binding against ITSELF):

* the embind translation units under ``src/wasm/`` -- the C++ -> JS exposure
  truth (``function("analyzeMelody", &js_analyze_melody);``).
* ``bindings/wasm/src/sonare.js.d.ts`` -- the ``SonareModule`` TS interface: the
  type through which the facade calls the raw module.
* the facade modules under ``bindings/wasm/src/`` -- the public wrappers that
  call ``module.X`` / ``requireModule().X``.

A free function must be (a) declared in ``SonareModule`` so TypeScript can call
it, and (b) wrapped by a facade module so users can reach it. A break in any leg
is a wiring bug invisible to the cross-binding checks, which read the facade
ALONE -- e.g. a function registered in embind but absent from both the
``SonareModule`` type and every facade. This extractor returns the three name
sets (with source locations) so ``compare._wasm_internal_drift`` can
cross-validate them.

Only FREE-FUNCTION embind registrations are collected. Class-method
registrations (``.function("addBus", ...)`` inside a ``class_<T>()`` chain)
belong to bound class types declared in their own interfaces, not
``SonareModule``; they are distinguished by the leading ``.`` and excluded.

Both source sets are DISCOVERED by walking their root, never enumerated. A
hardcoded file list keeps its old spelling after the tree moves under it and
silently stops covering the parts that moved: this check once named a single
``src/wasm/bindings.cpp``, kept resolving after the registrations were split
into ``src/wasm/bindings/**``, and so reported clean while seeing a fraction of
them. Walking a root cannot narrow that way, and :func:`_require_scope` refuses
to return a result from a walk that collapsed, so the same failure surfaces as a
loud error rather than a quiet pass.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

# Roots that are WALKED. Anything matching the suffix beneath them is scanned,
# so a new translation unit or facade module is covered the day it is added.
_BINDINGS_ROOT = "src/wasm"
_FACADE_ROOT = "bindings/wasm/src"
_DTS_REL = "bindings/wasm/src/sonare.js.d.ts"

# Below these the walk is treated as broken rather than as a clean tree. The
# embind registrations span dozens of translation units and the facade spans
# dozens of modules, so a walk that finds one contributing file of either has
# lost the tree, not found a small one.
_MIN_BINDING_FILES = 2
_MIN_FACADE_FILES = 2

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


class WasmScopeError(RuntimeError):
    """Raised when a source walk collapsed, so no result can be trusted.

    Reporting "clean" from a walk that found nothing is indistinguishable from
    reporting "clean" from a tree that is actually consistent, which is how the
    narrowed file set survived unnoticed. Failing loudly is the point.
    """


@dataclass(frozen=True)
class Site:
    """Where a name was found, so a finding can point at the right file."""

    file: str  # repo-relative path
    line: int

    def __str__(self) -> str:
        return f"{self.file}:{self.line}"


@dataclass
class WasmInternal:
    """The three WASM-internal name sets, with per-name source locations."""

    embind: dict[str, Site] = field(default_factory=dict)  # free fn name -> site
    iface: set[str] = field(default_factory=set)  # SonareModule member names
    refs: dict[str, Site] = field(default_factory=dict)  # name -> first facade site
    dts_file: str = _DTS_REL


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _blank_comments(text: str, blank_strings: bool = False) -> str:
    """Replace comment bodies with spaces, preserving length and line breaks.

    Both patterns below match prose as readily as code, so a name mentioned in a
    doc comment would otherwise read as a registration or a facade call: the
    facade scan alone picks up ``new module.Foo(...)``, an ``index`` module
    reference and a ``{"module.processor.param": value}`` example, none of which
    are calls. Offsets are preserved so :func:`_line_of` stays correct.

    @p blank_strings additionally blanks string CONTENTS, for the facade scan
    where a quoted ``"module.x"`` is data rather than a call. It must stay off
    for the embind scan, whose registrations live inside the string literal.
    """
    out = list(text)
    i = 0
    n = len(text)
    quote = ""
    while i < n:
        ch = text[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = ""
            elif blank_strings and ch != "\n":
                out[i] = " "
            i += 1
            continue
        if ch in "\"'`":
            quote = ch
            i += 1
            continue
        if ch == "/" and i + 1 < n:
            nxt = text[i + 1]
            if nxt == "/":
                j = text.find("\n", i)
                j = n if j < 0 else j
                out[i:j] = " " * (j - i)
                i = j
                continue
            if nxt == "*":
                j = text.find("*/", i + 2)
                j = n if j < 0 else j + 2
                for k in range(i, j):
                    if out[k] != "\n":
                        out[k] = " "
                i = j
                continue
        i += 1
    return "".join(out)


def _require_scope(files: list[Path], minimum: int, root: str, suffix: str) -> None:
    """Reject a walk that found too little to be a real scan of @p root."""
    if len(files) < minimum:
        raise WasmScopeError(
            f"wasm_internal: found {len(files)} {suffix} file(s) under '{root}', "
            f"expected at least {minimum}. The source layout moved or the root is "
            "wrong; refusing to report a clean result from a scan this narrow."
        )


def _scan(
    files: list[Path],
    root: Path,
    pattern: re.Pattern[str],
    blank_strings: bool = False,
) -> dict[str, Site]:
    """Map each name @p pattern captures to the first site that declares it.

    Files are visited in sorted order so the recorded site is stable across
    runs, and comments are blanked first so prose never registers as code.
    """
    out: dict[str, Site] = {}
    for path in files:
        text = _blank_comments(path.read_text(encoding="utf-8"), blank_strings)
        rel = path.relative_to(root).as_posix()
        for m in pattern.finditer(text):
            out.setdefault(m.group(1), Site(rel, _line_of(text, m.start())))
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
    """Collect the three name sets, or raise if a walk found too little.

    @throws WasmScopeError when a root yields fewer files than a real scan of it
            would, or when the ``SonareModule`` type is missing or unparsable.
    """
    dts = root / _DTS_REL
    if not dts.exists():
        raise WasmScopeError(f"wasm_internal: '{_DTS_REL}' is missing")

    binding_files = sorted((root / _BINDINGS_ROOT).rglob("*.cpp"))
    _require_scope(binding_files, _MIN_BINDING_FILES, _BINDINGS_ROOT, "*.cpp")

    facade_files = sorted(
        p
        for p in (root / _FACADE_ROOT).rglob("*.ts")
        if not p.name.endswith(".d.ts")  # type declarations, not call sites
    )
    _require_scope(facade_files, _MIN_FACADE_FILES, _FACADE_ROOT, "*.ts")

    iface = _iface_members(dts.read_text(encoding="utf-8"))
    if not iface:
        raise WasmScopeError(
            f"wasm_internal: no members parsed from the SonareModule interface in "
            f"'{_DTS_REL}'; every registration would read as undeclared"
        )

    return WasmInternal(
        embind=_scan(binding_files, root, _EMBIND_FREE_RE),
        iface=iface,
        refs=_scan(facade_files, root, _MODULE_REF_RE, blank_strings=True),
    )
