"""Public type names the Python package shares with the Node and WASM packages mean one shape.

The JS suites pin their interfaces to ``tests/conformance/shared_type_shapes.json``;
this pins the Python dataclass of the same name to the same field list, spelled in
snake case. Equality in both directions: a missing, extra or renamed field fails.
"""

from __future__ import annotations

import dataclasses
import json
import re
from pathlib import Path

import pytest

import libsonare

_SHAPES = json.loads(
    (Path(__file__).resolve().parents[3] / "tests/conformance/shared_type_shapes.json").read_text()
)


def _snake(field: str) -> str:
    return re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", field).lower()


def test_the_shared_shape_list_is_read() -> None:
    assert len(_SHAPES["types"]) >= 11
    assert not _SHAPES["aliases"], "an alias entry needs a Python mapping before it is read"


@pytest.mark.parametrize("name", sorted(_SHAPES["types"]))
def test_python_type_has_the_shared_fields(name: str) -> None:
    cls = getattr(libsonare, name)
    assert dataclasses.is_dataclass(cls), f"{name} is not a dataclass"
    fields = {field.name for field in dataclasses.fields(cls)}
    assert fields == {_snake(field) for field in _SHAPES["types"][name]}
