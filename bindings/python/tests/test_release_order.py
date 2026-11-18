"""A C result is read whole before the call that releases it.

Every ``sonare_free_*`` resets the struct it is handed, so a field read after the
release returns zero without any error. This scans the package source rather than
calling each entry point, because no single result-valued call covers every releaser.
"""

from __future__ import annotations

import ast
from pathlib import Path

_PACKAGE = Path(__file__).resolve().parents[1] / "src" / "libsonare"
_SCOPES = (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)


def _scope_nodes(node: ast.AST) -> list[ast.AST]:
    out: list[ast.AST] = []
    for child in ast.iter_child_nodes(node):
        if isinstance(child, _SCOPES):
            continue
        out.append(child)
        out.extend(_scope_nodes(child))
    return out


def _callee(call: ast.Call) -> str:
    func = call.func
    if isinstance(func, ast.Attribute):
        return func.attr
    return func.id if isinstance(func, ast.Name) else ""


def _released(call: ast.Call) -> str | None:
    if not call.args:
        return None
    arg = call.args[0]
    if isinstance(arg, ast.Call) and _callee(arg) in ("byref", "pointer") and arg.args:
        arg = arg.args[0]
    return ast.unparse(arg)


def _pos(node: ast.AST) -> tuple[int, int]:
    return (getattr(node, "lineno", 0), getattr(node, "col_offset", 0))


def _reads_after_release(source: str, label: str) -> tuple[int, list[str]]:
    """Return the release calls seen and every read of a released struct after its release."""
    releases = 0
    found: list[str] = []
    for scope in ast.walk(ast.parse(source)):
        if not isinstance(scope, _SCOPES):
            continue
        nodes = _scope_nodes(scope)
        for call in nodes:
            if not isinstance(call, ast.Call) or "free" not in _callee(call).lower():
                continue
            releases += 1
            name = _released(call)
            if name is None:
                continue
            end = (call.end_lineno or 0, call.end_col_offset or 0)
            # A later assignment to the same name starts a fresh struct.
            rebinds = [
                _pos(n)
                for n in nodes
                if isinstance(n, ast.Assign)
                and any(ast.unparse(t) == name for t in n.targets)
                and _pos(n) > end
            ]
            limit = min(rebinds, default=(1 << 30, 0))
            for read in nodes:
                if (
                    isinstance(read, (ast.Attribute, ast.Subscript))
                    and isinstance(read.ctx, ast.Load)
                    and ast.unparse(read.value) == name
                    and end < _pos(read) < limit
                ):
                    found.append(f"{label}:{read.lineno}: {ast.unparse(read)}")
    return releases, found


def test_no_result_field_is_read_after_its_release() -> None:
    releases = 0
    found: list[str] = []
    for path in sorted(_PACKAGE.glob("*.py")):
        count, sites = _reads_after_release(path.read_text(), path.name)
        releases += count
        found.extend(sites)
    # A floor, so a scan that stops matching the release calls cannot pass empty.
    assert releases >= 100
    assert found == []


def test_the_scan_flags_a_field_read_after_the_release() -> None:
    source = (
        "def bounce(lib, raw):\n"
        "    try:\n"
        "        data = copy(raw.interleaved)\n"
        "    finally:\n"
        "        lib.sonare_free_engine_bounce_result(ctypes.byref(raw))\n"
        "    return Result(data, frames=raw.frames)\n"
        "\n"
        "def rebound(lib, out):\n"
        "    lib.sonare_free_melody_result(ctypes.byref(out))\n"
        "    out = Fresh()\n"
        "    return out.points\n"
    )
    assert _reads_after_release(source, "fixture") == (2, ["fixture:6: raw.frames"])
