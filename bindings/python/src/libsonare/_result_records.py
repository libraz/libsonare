"""Private helpers shared by the result-record parsers and their JSON form.

A native result arrives as a camelCase JSON document; each record type has one
private parser that reads it once into frozen dataclasses with snake_case
attributes. :func:`_result_to_json_dict` is the inverse view the command line
prints, so the CLI JSON contract has one definition rather than one per command.
"""

from __future__ import annotations

import dataclasses
import re
from collections.abc import Mapping
from typing import Any

import numpy as np

from ._null_means import NULL_MEANS


def _snake(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def _camel(name: str) -> str:
    head, *rest = name.split("_")
    return head + "".join(part.capitalize() for part in rest)


def _fields(values: Mapping[str, Any]) -> dict[str, Any]:
    return {_snake(key): value for key, value in values.items()}


def _record(record_type: type, values: Mapping[str, Any]) -> Any:
    """Build a record of measurements from a wire object; a JSON integer reads as a float.

    A measurement the core could not take is an absent key, so a field typed
    ``... | None`` that the wire leaves out is ``None``; any other missing field
    is still an error.
    """
    given = {
        key: float(value) if type(value) is int else value for key, value in _fields(values).items()
    }
    for item in dataclasses.fields(record_type):
        if item.name not in given and str(item.type).endswith("| None"):
            given[item.name] = None
    return record_type(**given)


def _optional_float(value: Any) -> float | None:
    """A measurement that may not have been taken: ``None`` when its key is absent."""
    return None if value is None else float(value)


def _fill_null_ends(document: Any, family: str) -> Any:
    """Replace each JSON null at a path ``family`` declares with the infinity it stands for.

    The writer emits an infinite level as ``null``; the core names the end per
    path, and :mod:`._null_means` carries that table. Returns ``document``,
    edited in place.
    """
    for path, end in NULL_MEANS[family].items():
        _fill_at(document, path.split("."), end)
    return document


def _fill_at(node: Any, segments: list[str], end: float) -> None:
    if not isinstance(node, dict):
        return
    head, *rest = segments
    element = head.endswith("[]")
    key = head[:-2] if element else head
    if key not in node:
        return
    value = node[key]
    if element and isinstance(value, list):
        if rest:
            for item in value:
                _fill_at(item, rest, end)
        else:
            node[key] = [end if item is None else item for item in value]
    elif not element:
        if rest:
            _fill_at(value, rest, end)
        elif value is None:
            node[key] = end


def _result_to_json_dict(value: Any) -> Any:
    """Render a result record as the JSON value the command line prints.

    The CLI contract: keys are the records' snake_case attribute names, an
    attribute that is ``None`` (a measurement that was not taken) is omitted
    rather than written as ``null``, an ndarray becomes a list, and a nested
    record or list of records is rendered the same way. A plain ``dict`` member
    is a document (a chain config, a mixer scene) and keeps its own keys.
    """
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return {
            item.name: _result_to_json_dict(member)
            for item in dataclasses.fields(value)
            if (member := getattr(value, item.name)) is not None
        }
    if isinstance(value, np.ndarray):
        return value.tolist()
    if isinstance(value, (list, tuple)):
        return [_result_to_json_dict(item) for item in value]
    return value
