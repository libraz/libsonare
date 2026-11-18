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


def _snake(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def _camel(name: str) -> str:
    head, *rest = name.split("_")
    return head + "".join(part.capitalize() for part in rest)


def _fields(values: Mapping[str, Any]) -> dict[str, Any]:
    return {_snake(key): value for key, value in values.items()}


def _record(record_type: type, values: Mapping[str, Any]) -> Any:
    """Build a record of measurements from a wire object; a JSON integer reads as a float."""
    return record_type(
        **{
            key: float(value) if type(value) is int else value
            for key, value in _fields(values).items()
        }
    )


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
