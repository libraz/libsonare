"""Every class has one declaration, the runtime one.

A ``.pyi`` that redeclares a class defined in a runtime module gives the type
checker a second, unrelated type of the same name: a record built from the
public name is then refused by every call typed in a module without a stub.
The stubs re-export such a class (``from ._module import Name as Name``).
"""

from __future__ import annotations

import ast
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[1] / "src" / "libsonare"


def _top_level_classes(path: Path) -> set[str]:
    tree = ast.parse(path.read_text(encoding="utf-8"))
    return {node.name for node in tree.body if isinstance(node, ast.ClassDef)}


def test_no_stub_redeclares_a_runtime_class() -> None:
    stubs = sorted(PACKAGE.glob("*.pyi"))
    runtime = [
        path for path in sorted(PACKAGE.glob("*.py")) if not path.with_suffix(".pyi").exists()
    ]
    assert len(stubs) >= 5, f"expected the package stubs under {PACKAGE}, found {len(stubs)}"
    assert len(runtime) >= 50, f"expected the runtime modules under {PACKAGE}, found {len(runtime)}"
    declared_at_runtime: dict[str, str] = {}
    for path in runtime:
        for name in _top_level_classes(path):
            declared_at_runtime.setdefault(name, path.name)
    duplicates = sorted(
        f"{stub.name}: {name} (declared in {declared_at_runtime[name]})"
        for stub in stubs
        for name in _top_level_classes(stub)
        if name in declared_at_runtime
    )
    assert not duplicates, (
        "these stub classes redeclare a runtime class; re-export it with "
        "`from ._module import Name as Name` instead:\n" + "\n".join(duplicates)
    )
