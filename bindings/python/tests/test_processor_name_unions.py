"""The published processor-name types must match what the library returns.

``mastering_processor_names()`` is documented as the set of supported mastering
processors, and Node, WASM and Python each publish a ``SoloProcessor`` type
declaring that set. All three are rendered from the tracked capability catalog,
so these cases diff them against the *runtime* list rather than against each
other: comparing declarations only proves the generator ran, not that the
generated set is the one the library ships.
"""

from __future__ import annotations

import ast
import json
import re
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

ROOT = Path(__file__).resolve().parents[3]
DECLARATIONS = (
    ROOT / "bindings/node/src/types_mastering.ts",
    ROOT / "bindings/wasm/src/public_types_mastering.ts",
    ROOT / "bindings/python/src/libsonare/analyzer.pyi",
)

_TS_ARRAY = re.compile(r"export const SOLO_PROCESSORS = \[(.*?)\] as const;", re.DOTALL)
_TS_STRING = re.compile(r"'([^']+)'")


def _typescript_names(path: Path) -> list[str]:
    """Return the string members of the generated ``SOLO_PROCESSORS`` array."""
    match = _TS_ARRAY.search(path.read_text(encoding="utf-8"))
    assert match is not None, f"{path} declares no SOLO_PROCESSORS array"
    return _TS_STRING.findall(match.group(1))


def _stub_names(path: Path) -> list[str]:
    """Return the ``Literal`` members of the ``SoloProcessor`` alias in a stub."""
    module = ast.parse(path.read_text(encoding="utf-8"))
    for node in module.body:
        if not isinstance(node, ast.AnnAssign):
            continue
        if not isinstance(node.target, ast.Name) or node.target.id != "SoloProcessor":
            continue
        assert isinstance(node.value, ast.Subscript), "SoloProcessor must be a Literal alias"
        members = node.value.slice
        elements = members.elts if isinstance(members, ast.Tuple) else [members]
        names: list[str] = []
        for element in elements:
            assert isinstance(element, ast.Constant), "SoloProcessor members must be literals"
            assert isinstance(element.value, str)
            names.append(element.value)
        return names
    raise AssertionError(f"{path} declares no SoloProcessor alias")


def _declared_names(path: Path) -> list[str]:
    return _stub_names(path) if path.suffix == ".pyi" else _typescript_names(path)


def _runtime_names() -> set[str]:
    import libsonare

    names = libsonare.mastering_processor_names()
    assert names, "the library reported no mastering processors"
    if not any(name.startswith("effects.") for name in names):
        pytest.skip("library built without the creative effects suite")
    return set(names)


def test_declared_processor_types_match_the_runtime_name_list() -> None:
    """Every declared name is returnable, and every returned name is declared."""
    runtime = _runtime_names()
    for path in DECLARATIONS:
        declared = _declared_names(path)
        assert len(declared) == len(set(declared)), f"{path} declares a duplicate name"
        assert not set(declared) - runtime, f"{path} declares names the library cannot return"
        assert not runtime - set(declared), f"{path} omits names the library returns"


def test_every_shipped_effects_insert_is_a_listed_processor() -> None:
    """The one-shot apply path builds effects through the realtime insert factory.

    An effect the factory can construct but the name list omits still runs when
    its id is typed by hand, so it is reachable without being documented or
    typed. The catalog's realtime-insertable entries are that factory's set.
    """
    runtime = _runtime_names()
    catalog = json.loads((ROOT / "tools/capability-catalog.json").read_text(encoding="utf-8"))
    insertable_effects = {
        processor["id"]
        for processor in catalog["processors"]
        if processor["realtimeInsertable"] and processor["id"].startswith("effects.")
    }
    assert insertable_effects, "the catalog lists no insertable effects"
    assert not insertable_effects - runtime
