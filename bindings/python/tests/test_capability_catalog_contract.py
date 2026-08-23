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


def _params() -> list[dict[str, Any]]:
    return [
        parameter
        for processor in _tracked_catalog()["processors"]
        for parameter in processor["params"]
    ]


def _first_number_param(catalog: dict[str, Any]) -> dict[str, Any]:
    for processor in catalog["processors"]:
        for parameter in processor["params"]:
            if parameter["type"] == "number":
                return parameter
    raise AssertionError("catalog publishes no numeric parameter")


def _first_boolean_param(catalog: dict[str, Any]) -> dict[str, Any]:
    for processor in catalog["processors"]:
        for parameter in processor["params"]:
            if parameter["type"] == "boolean":
                return parameter
    raise AssertionError("catalog publishes no boolean parameter")


def test_every_boolean_parameter_publishes_a_json_boolean_default() -> None:
    """0/1 would read as a number to every typed facade mirror of this shape."""
    booleans = [parameter for parameter in _params() if parameter["type"] == "boolean"]
    assert booleans
    for parameter in booleans:
        assert isinstance(parameter["default"], bool), parameter["name"]
        # A boolean cannot be out of range, so it carries no measured bounds.
        assert parameter["min"] is None and parameter["max"] is None, parameter["name"]


def test_every_published_default_sits_inside_its_published_range() -> None:
    """A default outside the accepted range would ship a rejected configuration."""
    for processor in _tracked_catalog()["processors"]:
        for parameter in processor["params"]:
            fallback = parameter["default"]
            if not isinstance(fallback, (int, float)) or isinstance(fallback, bool):
                continue
            where = f"{processor['id']}.{parameter['name']}"
            if parameter["min"] is not None:
                assert fallback >= parameter["min"], where
            if parameter["max"] is not None:
                assert fallback <= parameter["max"], where


def test_catalog_validator_rejects_malformed_parameter_metadata() -> None:
    """The shape three hand-written type mirrors promise, guarded at the source."""
    numeric_bound = deepcopy(_tracked_catalog())
    _first_number_param(numeric_bound)["max"] = "24000"
    with pytest.raises(ValueError, match="max must be a number or null"):
        validate_catalog(numeric_bound)

    inverted = deepcopy(_tracked_catalog())
    parameter = _first_number_param(inverted)
    parameter["min"], parameter["max"] = 10, 1
    with pytest.raises(ValueError, match="publishes a min above its max"):
        validate_catalog(inverted)

    numeric_boolean_default = deepcopy(_tracked_catalog())
    _first_boolean_param(numeric_boolean_default)["default"] = 0
    with pytest.raises(ValueError, match="must be a JSON boolean"):
        validate_catalog(numeric_boolean_default)

    bounded_boolean = deepcopy(_tracked_catalog())
    _first_boolean_param(bounded_boolean)["min"] = 0
    with pytest.raises(ValueError, match="boolean and must publish no bounds"):
        validate_catalog(bounded_boolean)


def test_an_unbounded_parameter_stays_valid() -> None:
    """Null is how the catalog states "no limit", not a schema hole."""
    catalog = deepcopy(_tracked_catalog())
    parameter = _first_number_param(catalog)
    parameter["min"] = None
    parameter["max"] = None
    validate_catalog(catalog)
