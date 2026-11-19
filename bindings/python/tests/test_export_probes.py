"""The binding never probes for a C entry point every shared build exports.

A ``hasattr(lib, "sonare_...")`` in front of an always-exported symbol is dead
code on a current build and a silent fallback on a stale one, so the binding
probes only for the symbols a feature-off shared build omits. That set is
derived here from ``src/CMakeLists.txt`` and the C-ABI sources -- a symbol is
gated when every translation unit defining it is compiled into the shared
library only under an option -- so it shrinks by itself as gated entries gain
NOT_SUPPORTED stubs. Source scan only; nothing here loads the library.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PACKAGE_DIR = REPO_ROOT / "bindings" / "python" / "src" / "libsonare"
CMAKE_LISTS = REPO_ROOT / "src" / "CMakeLists.txt"

# The loader's ABI skew check: a binary at SONARE_LIB_PATH may predate the
# symbol, and must fail as AbiMismatch rather than as an AttributeError.
EXEMPT = {("_ffi.py", "sonare_abi_version")}

# Functions whose probe takes its symbol as an argument, keyed by (file, function).
EXEMPT_HELPERS = {
    # The probe helper itself; every call site is scanned as a probe of its own.
    ("_runtime.py", "_require_export"): "helper",
    # Asks whether a token in a C message names a C entry, to strip it; not a guard.
    ("_runtime.py", "_strip_c_entry"): "token lookup",
}

_TU = re.compile(r"c_api/[\w/]+\.cpp")


def _shared_translation_units() -> dict[str, bool]:
    """C-ABI translation unit -> whether the shared library compiles it unconditionally."""
    lists: dict[str, list[str]] = {}
    current: str | None = None
    lines = [line.split("#")[0].strip() for line in CMAKE_LISTS.read_text().splitlines()]
    for line in lines:
        start = re.match(r"set\(\s*(SONARE_\w*SOURCES)\b(.*)", line)
        if start:
            current = start.group(1)
            lists[current] = _TU.findall(start.group(2))
        elif current:
            lists[current] += _TU.findall(line)
        if current and line.endswith(")"):
            current = None

    units = {tu: True for tu in lists.get("SONARE_SOURCES", [])}
    stack: list[str] = []
    appending = False
    for line in lines:
        if re.match(r"if\s*\(", line):
            stack.append(line)
        elif re.match(r"endif\s*\(", line):
            stack.pop()
        if re.match(r"(list|set)\(\s*(APPEND\s+)?SONARE_SHARED_SOURCES\b", line) or appending:
            unconditional = all("BUILD_SHARED" in cond for cond in stack)
            for tu in _TU.findall(line):
                units[tu] = unconditional
            for var in re.findall(r"\$\{(SONARE_\w+SOURCES)\}", line):
                for tu in lists.get(var, []):
                    units[tu] = unconditional
            appending = not line.endswith(")")
    return units


def _defined_symbols(tu: str) -> set[str]:
    text = re.sub(r"/\*.*?\*/", "", (REPO_ROOT / "src" / tu).read_text(), flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return set(re.findall(r"^[A-Za-z_][\w \t*]*?\b(sonare_\w+)\s*\([^;{]*\)\s*\{", text, re.M))


def _gated_symbols() -> set[str]:
    units = _shared_translation_units()
    always: set[str] = set()
    optional: set[str] = set()
    for tu, unconditional in units.items():
        (always if unconditional else optional).update(_defined_symbols(tu))
    assert len(always) >= 600, (
        f"only {len(always)} unconditionally compiled C-ABI definitions found; "
        "the CMake or definition scan broke"
    )
    return optional - always


def _constant_strings(
    node: ast.AST | None, tree: ast.Module, loop: dict[str, set[str]] | None = None
) -> set[str]:
    """String values an expression can take: constants, loop variables, one Name binding,
    and f-strings over those."""
    loop = loop or {}
    if node is None:
        return set()
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return {node.value}
    if isinstance(node, ast.JoinedStr):
        values = {""}
        for part in node.values:
            if isinstance(part, ast.Constant):
                choices = {str(part.value)}
            elif isinstance(part, ast.FormattedValue):
                choices = _constant_strings(part.value, tree, loop)
            else:
                choices = set()
            values = {prefix + choice for prefix in values for choice in choices}
        return values
    if isinstance(node, (ast.Tuple, ast.List, ast.Set)):
        found: set[str] = set()
        for element in node.elts:
            # A tuple of tuples iterates as (name, extra...) records.
            first = element.elts[0] if isinstance(element, ast.Tuple) and element.elts else element
            found |= _constant_strings(first, tree, loop)
        return found
    if isinstance(node, ast.Dict):
        return set().union(*(_constant_strings(key, tree, loop) for key in node.keys))
    if (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr in {"items", "keys"}
    ):
        return _constant_strings(node.func.value, tree, loop)
    if isinstance(node, ast.Name):
        if node.id in loop:
            return loop[node.id]
        found = set()
        for other in ast.walk(tree):
            if not isinstance(other, ast.Assign):
                continue
            for target in other.targets:
                if isinstance(target, ast.Name) and target.id == node.id:
                    found |= _constant_strings(other.value, tree)
                elif isinstance(target, ast.Tuple) and isinstance(other.value, ast.Tuple):
                    for element, value in zip(target.elts, other.value.elts, strict=False):
                        if isinstance(element, ast.Name) and element.id == node.id:
                            found |= _constant_strings(value, tree)
        return found
    return set()


def _loop_bindings(tree: ast.Module) -> dict[int, dict[str, set[str]]]:
    """id(call) -> {loop variable: strings it iterates over} for calls inside a for loop."""
    bindings: dict[int, dict[str, set[str]]] = {}
    for loop in ast.walk(tree):
        if not isinstance(loop, ast.For):
            continue
        target = loop.target.elts[0] if isinstance(loop.target, ast.Tuple) else loop.target
        if not isinstance(target, ast.Name):
            continue
        names = _constant_strings(loop.iter, tree)
        for statement in loop.body:
            for call in ast.walk(statement):
                bindings.setdefault(id(call), {})[target.id] = names
    return bindings


def _enclosing_function(tree: ast.Module, call: ast.Call) -> str | None:
    innermost: str | None = None
    for function in ast.walk(tree):
        if isinstance(function, (ast.FunctionDef, ast.AsyncFunctionDef)) and any(
            node is call for node in ast.walk(function)
        ):
            innermost = function.name
    return innermost


def _probe_sites() -> tuple[list[tuple[str, int, str | None]], set[tuple[str, str]]]:
    """Probes as (file, line, symbol or None when unresolved), and the exempt helpers seen."""
    sites: list[tuple[str, int, str | None]] = []
    helpers_seen: set[tuple[str, str]] = set()
    for path in sorted(PACKAGE_DIR.glob("*.py")):
        tree = ast.parse(path.read_text())
        loops = _loop_bindings(tree)
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                continue
            name, args = node.func.id, node.args
            probe = (
                (name == "hasattr" and len(args) == 2)
                or (name == "getattr" and len(args) == 3)
                or (name == "_require_export" and len(args) >= 2)
            )
            if not probe or not _is_library(args[0]):
                continue
            helper = (path.name, _enclosing_function(tree, node) or "")
            if helper in EXEMPT_HELPERS:
                helpers_seen.add(helper)
                continue
            symbols = _constant_strings(args[1], tree, loops.get(id(node), {}))
            c_names = sorted(symbol for symbol in symbols if symbol.startswith("sonare_"))
            if c_names:
                sites.extend((path.name, node.lineno, symbol) for symbol in c_names)
            elif not symbols:
                sites.append((path.name, node.lineno, None))
    return sites, helpers_seen


def _is_library(node: ast.expr) -> bool:
    """``lib``, ``self._lib`` or ``_get_lib()`` -- the loaded CDLL."""
    if isinstance(node, ast.Name):
        return node.id in {"lib", "_lib"}
    if isinstance(node, ast.Attribute):
        return node.attr in {"lib", "_lib"}
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
        return node.func.id == "_get_lib"
    return False


def test_binding_probes_only_for_symbols_a_feature_off_build_omits() -> None:
    gated = _gated_symbols()
    sites, helpers_seen = _probe_sites()
    exempt_seen = {(f, s) for f, _, s in sites if (f, s) in EXEMPT}
    assert exempt_seen == EXEMPT and helpers_seen == set(EXEMPT_HELPERS), (
        f"the probe scan no longer sees the exemptions {sorted(EXEMPT)} / "
        f"{sorted(EXEMPT_HELPERS)}; the scan broke rather than the binding changing"
    )
    unresolved = [(f, line) for f, line, s in sites if s is None]
    assert not unresolved, f"export probes whose symbol the scan cannot resolve: {unresolved}"
    ungated = sorted(
        (f, line, s)
        for f, line, s in sites
        if s is not None and (f, s) not in EXEMPT and s not in gated
    )
    assert not ungated, (
        "these probes test for a symbol every shared build exports (ungated, or a "
        f"NOT_SUPPORTED stub when its feature is off); call it directly: {ungated}"
    )
