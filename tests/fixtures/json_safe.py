#!/usr/bin/env python3
"""Helpers for writing strict JSON fixture reports."""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any


def json_safe(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(key): json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_safe(item) for item in value]
    if isinstance(value, float) and not math.isfinite(value):
        if math.isnan(value):
            return "nan"
        return "inf" if value > 0 else "-inf"
    return value


def dumps_strict(value: Any, **kwargs: Any) -> str:
    return json.dumps(json_safe(value), allow_nan=False, **kwargs)


def write_json_strict(path: Path, value: Any, **kwargs: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dumps_strict(value, **kwargs) + "\n", encoding="utf-8")
