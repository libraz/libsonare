"""Audio-thread C entries are checked with ``_check_realtime``, never ``_check``.

An entry opened with ``SONARE_C_RT_API_ENTRY`` neither clears nor records the
thread-local detail message, so ``_check`` -- which prefers that message -- would
describe an earlier, unrelated call. This scans the C sources for those entries and
the facade for any function that reaches one through ``_check``.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PACKAGE = REPO / "bindings" / "python" / "src" / "libsonare"
C_API = REPO / "src" / "c_api"
# The entry name is the identifier ahead of the parameter list that precedes the macro.
_ENTRY = re.compile(r"\b(sonare_\w+)\s*\([^;{}]*\)\s*\{\s*(?://[^\n]*\n\s*)*SONARE_C_RT_API_ENTRY")


def _rt_entries() -> set[str]:
    names: set[str] = set()
    for source in C_API.glob("*.cpp"):
        names.update(_ENTRY.findall(source.read_text(encoding="utf-8")))
    return names


def _calls_named(node: ast.AST, names: set[str]) -> list[str]:
    return [
        child.func.attr
        for child in ast.walk(node)
        if isinstance(child, ast.Call)
        and isinstance(child.func, ast.Attribute)
        and child.func.attr in names
    ]


def test_the_scan_finds_the_audio_thread_entries() -> None:
    entries = _rt_entries()
    assert len(entries) >= 15
    assert "sonare_eq_non_finite_discard_count" in entries
    assert "sonare_mixer_bus_non_finite_discard_count" in entries
    assert "sonare_strip_non_finite_discard_count" in entries


def test_no_function_reaches_an_audio_thread_entry_through_check() -> None:
    entries = _rt_entries()
    offenders: list[str] = []
    reached = 0
    for path in sorted(PACKAGE.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for fn in ast.walk(tree):
            if not isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            used = _calls_named(fn, entries)
            if not used:
                continue
            reached += 1
            checks = [
                call
                for call in ast.walk(fn)
                if isinstance(call, ast.Call)
                and isinstance(call.func, ast.Name)
                and call.func.id == "_check"
            ]
            if checks:
                offenders.append(f"{path.name}:{fn.name} -> {sorted(set(used))}")
    assert reached >= 10, "the scan reached too few call sites to mean anything"
    assert not offenders, offenders
