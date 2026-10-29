"""Validation shared by observation report consumers."""

from collections.abc import Mapping
from typing import Any


def require_correct(item: Mapping[str, Any]) -> bool:
    value = item.get("correct")
    if type(value) is not bool:
        raise ValueError("observation 'correct' must be a boolean")
    return value
