"""Checks for the tracked capability-catalog contract."""

from __future__ import annotations

import json
from copy import deepcopy
from pathlib import Path
from runpy import run_path
from typing import Any

import pytest

ROOT = Path(__file__).resolve().parents[3]
validate_catalog = run_path(str(ROOT / "tools/generate_capability_catalog.py"))["validate_catalog"]


def _tracked_catalog() -> dict[str, Any]:
    return json.loads((ROOT / "tools/capability-catalog.json").read_text(encoding="utf-8"))


def test_tracked_catalog_matches_schema_contract() -> None:
    """The checked-in catalog carries the required cost and unique preset lists."""
    schema = json.loads(
        (ROOT / "schemas/capability-catalog.schema.json").read_text(encoding="utf-8")
    )
    processor_schema = schema["properties"]["processors"]["items"]
    assert "realtimeCost" in processor_schema["required"]
    assert processor_schema["properties"]["realtimeCost"]["enum"] == [
        "low",
        "moderate",
        "high",
        None,
    ]

    catalog = _tracked_catalog()
    validate_catalog(catalog)
    for names in catalog["presets"].values():
        assert len(names) == len(set(names))


def test_catalog_validator_rejects_missing_cost_and_duplicate_presets() -> None:
    """Generator validation stays equivalent to the schema's required fields."""
    missing_cost = deepcopy(_tracked_catalog())
    del missing_cost["processors"][0]["realtimeCost"]
    with pytest.raises(ValueError, match="missing required keys.*realtimeCost"):
        validate_catalog(missing_cost)

    invalid_cost = deepcopy(_tracked_catalog())
    invalid_cost["processors"][0]["realtimeCost"] = "expensive"
    with pytest.raises(ValueError, match="realtimeCost must be low, moderate, high, or null"):
        validate_catalog(invalid_cost)

    duplicate_preset = deepcopy(_tracked_catalog())
    duplicate_preset["presets"]["synth"].append("harp")
    with pytest.raises(ValueError, match="synth must contain unique names"):
        validate_catalog(duplicate_preset)
