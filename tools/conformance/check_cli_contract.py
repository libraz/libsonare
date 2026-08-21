#!/usr/bin/env python3
"""Validate and exercise the cross-surface CLI JSON contract.

The manifest is deliberately independent of either CLI implementation.  That
lets a surface author land the inventory and semantic behavior in a separate
change while this checker already provides a stable, machine-readable target.
``--schema`` and ``--list`` never execute a CLI; the maintenance-only
``--emit-shared-option-snapshot`` mode executes only the two inventory dumps.
Invoking the script without executables is also a schema-only check.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "tests" / "conformance" / "cli_contract_v2.json"

_TOP_LEVEL_KEYS = {
    "schema_version",
    "contract",
    "surfaces",
    "exit_codes",
    "comparison",
    "inventory",
    "commands",
    "active_paths",
    "parser_cases",
    "fixtures",
}
_CLASSIFICATIONS = {"shared", "native_only", "python_only", "intentional_variant"}
_STATUSES = {"active", "pending", "native_only", "python_only", "intentional_variant"}
_OPTION_TYPES = {"boolean", "integer", "number", "string", "path"}
_POSITIONAL_TYPES = {"boolean", "integer", "number", "string", "path"}
_EXIT_CODES = {0, 2, 3, 4, 5, 9}
_ARRAY_TYPES = {"array:string", "array:number:12"}
_PAYLOAD_TYPES = _OPTION_TYPES | _ARRAY_TYPES | {"null", "any"}
_SCHEMA_METADATA_KEYS = {"optional", "required"}
_SECTION_TYPE_RE = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*\Z")
_ANALYZE_TOP_LEVEL_KEYS = {
    "bpm",
    "bpm_confidence",
    "key",
    "time_signature",
    "beats",
    "downbeat_indices",
    "downbeat_phase",
    "chords",
    "sections",
    "timbre",
    "dynamics",
    "rhythm",
    "form",
}
_ANALYZE_KEY_KEYS = {"root", "mode", "confidence", "name"}
_ANALYZE_TIME_SIGNATURE_KEYS = {"numerator", "denominator", "confidence"}
_ANALYZE_BEAT_KEYS = {"time", "strength"}
_ANALYZE_CHORD_KEYS = {"name", "start", "end", "confidence"}
_ANALYZE_SECTION_KEYS = {"type", "start", "end"}
_ANALYZE_TIMBRE_KEYS = {
    "brightness",
    "warmth",
    "density",
    "roughness",
    "complexity",
}
_ANALYZE_DYNAMICS_KEYS = {
    "dynamic_range_db",
    "loudness_range_db",
    "crest_factor",
    "is_compressed",
}
_ANALYZE_RHYTHM_KEYS = {"syncopation", "groove_type", "pattern_regularity"}
_RHYTHM_PAYLOAD_SCHEMA = {
    "keys": {
        "bpm": "number",
        "time_signature": {
            "keys": {
                "numerator": "integer",
                "denominator": "integer",
                "confidence": "number",
            }
        },
        "groove_type": "string",
        "syncopation": "number",
        "pattern_regularity": "number",
        "tempo_stability": "number",
        "beat_intervals": {
            "keys": {
                "count": "integer",
                "mean": "number",
                "std": "number",
                "min": "number",
                "max": "number",
            }
        },
    }
}
_PITCH_PAYLOAD_SCHEMA = {
    "keys": {
        "algorithm": "string",
        "n_frames": "integer",
        "voiced_count": "integer",
        "voiced_ratio": "number|null",
        "median_f0": "number|null",
        "mean_f0": "number|null",
    }
}
_EQ_PAYLOAD_SCHEMA = {
    "keys": {
        "processor": "string",
        "input_lufs": "number",
        "output_lufs": "number",
        "applied_gain_db": "number",
        "latency_samples": "integer",
        "sample_rate": "integer",
        "output": "path",
    }
}
_MASTERING_PROCESSOR_PAYLOAD_SCHEMA = {
    "keys": {
        "processor": "string",
        "stereo": "boolean",
        "input_lufs": "number",
        "output_lufs": "number",
        "applied_gain_db": "number",
        "latency_samples": "integer",
        "sample_rate": "integer",
        "output": "path",
    }
}
_SPECTRAL_FEATURES = {"centroid", "bandwidth", "rolloff", "flatness", "zcr", "rms"}
_SPECTRAL_STATS = {"mean", "std", "min", "max"}
_VOICE_COMMON_KEYS = {
    "output",
    "length",
    "duration",
    "sample_rate",
    "latency_samples",
}
_VOICE_SIMPLE_KEYS = _VOICE_COMMON_KEYS | {"pitch_semitones", "formant_factor"}
_VOICE_PRESET_KEYS = _VOICE_COMMON_KEYS | {"preset"}
_PROJECT_COMPILE_KEYS = {"has_timeline", "diagnostic_count", "diagnostics", "messages"}
_PROJECT_COMPILE_DIAGNOSTIC_KEYS = {"code", "severity", "target_id", "message"}
_MASTERING_REPORT_KEYS = {
    "input_lufs",
    "output_lufs",
    "applied_gain_db",
    "target_lufs",
    "ceiling_db",
    "true_peak_oversample",
    "latency_samples",
    "loudness_target_limited",
    "sample_rate",
    "output",
}
_SYNTHESIZE_RIR_KEYS = {"output", "sample_rate", "samples"}


def _is_bool(value: Any) -> bool:
    return isinstance(value, bool)


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: Any) -> bool:
    return (isinstance(value, int) and not isinstance(value, bool)) or (
        isinstance(value, float) and math.isfinite(value)
    )


def _exact(value: Any, keys: set[str], label: str, errors: list[str]) -> bool:
    if not isinstance(value, dict):
        errors.append(f"{label}: expected an object")
        return False
    actual = set(value)
    missing = sorted(keys - actual)
    unknown = sorted(actual - keys)
    if missing:
        errors.append(f"{label}: missing keys {', '.join(missing)}")
    if unknown:
        errors.append(f"{label}: unknown keys {', '.join(unknown)}")
    return not missing and not unknown


def _strings(value: Any, label: str, errors: list[str]) -> bool:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        errors.append(f"{label}: expected an array of strings")
        return False
    if len(set(value)) != len(value):
        errors.append(f"{label}: duplicate values are not allowed")
        return False
    return True


def _argv(value: Any, label: str, errors: list[str]) -> bool:
    """Check a case's argument vector.

    Unlike an option or alias list, an argv legitimately repeats a token: a
    repeatable option is exercised by spelling it more than once, and two
    occurrences may even carry the same value.
    """
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        errors.append(f"{label}: expected an array of strings")
        return False
    return True


def _type_token(value: Any, label: str, errors: list[str]) -> bool:
    if not isinstance(value, str) or value not in _OPTION_TYPES | _ARRAY_TYPES:
        errors.append(f"{label}: unknown type token {value!r}")
        return False
    return True


_TEXT_EXPECTATION_KEYS = ("stdout_contains", "stdout_excludes", "stderr_contains")


def _declared_text_keys(case: Any) -> set[str]:
    """The optional content-assertion keys a case actually declares.

    Kept optional so the vast majority of cases, which assert JSON payloads or
    empty output, keep their exact key set.
    """
    if not isinstance(case, dict):
        return set()
    return {key for key in _TEXT_EXPECTATION_KEYS if key in case}


def _validate_text_expectations(case: Any, label: str, errors: list[str]) -> None:
    if not isinstance(case, dict):
        return
    for key in _TEXT_EXPECTATION_KEYS:
        if key not in case:
            continue
        value = case[key]
        if not isinstance(value, list) or not value:
            errors.append(f"{label}.{key}: expected a non-empty array")
            continue
        if not _strings(value, f"{label}.{key}", errors):
            continue
        if any(not needle for needle in value):
            errors.append(f"{label}.{key}: substrings must not be empty")
    if case.get("stdout") == "text" and "stdout_contains" not in case:
        errors.append(
            f"{label}: a text stdout case must declare stdout_contains, "
            "otherwise it asserts nothing about the output"
        )


def _validate_option(option: Any, label: str, errors: list[str]) -> None:
    if not _exact(
        option,
        {"name", "type", "default", "aliases", "repeatable", "required"},
        label,
        errors,
    ):
        return
    if not isinstance(option["name"], str) or not option["name"]:
        errors.append(f"{label}.name: expected a non-empty string")
    type_is_valid = _type_token(option["type"], f"{label}.type", errors)
    if not _strings(option["aliases"], f"{label}.aliases", errors):
        pass
    repeatable = option["repeatable"]
    required = option["required"]
    repeatable_is_bool = _is_bool(repeatable)
    required_is_bool = _is_bool(required)
    if not repeatable_is_bool:
        errors.append(f"{label}.repeatable: expected a boolean")
    if not required_is_bool:
        errors.append(f"{label}.required: expected a boolean")
    default = option["default"]
    if not type_is_valid or not repeatable_is_bool or not required_is_bool:
        return
    # Inventory v2 makes these two parser states explicit.  A required value
    # has no parser default, while a repeatable value starts with an empty
    # collection; accepting either legacy scalar spelling would hide a
    # surface-level registry drift.
    if required and repeatable:
        errors.append(f"{label}: required and repeatable options are incompatible")
    if required:
        if default is not None:
            errors.append(f"{label}.default: required option needs a null default")
        return
    if repeatable:
        if default != []:
            errors.append(
                f"{label}.default: repeatable option needs an empty array default"
            )
        return
    if default is None:
        return
    if option["type"] == "boolean" and not _is_bool(default):
        errors.append(f"{label}.default: boolean option needs a boolean default")
    elif option["type"] == "integer" and not _is_int(default):
        errors.append(f"{label}.default: integer option needs an integer default")
    elif option["type"] == "number" and not _is_number(default):
        errors.append(f"{label}.default: number option needs a finite number default")
    elif (
        option["type"] in {"string", "path"}
        and default is not None
        and not isinstance(default, str)
    ):
        errors.append(
            f"{label}.default: string/path option needs a string or null default"
        )


def _validate_schema_alternatives(value: Any, label: str, errors: list[str]) -> None:
    if not isinstance(value, list) or not value:
        errors.append(f"{label}: expected a non-empty array of schemas")
        return
    for index, schema in enumerate(value):
        _validate_payload_schema(schema, f"{label}[{index}]", errors)


def _validate_object_schema(
    value: dict[str, Any], label: str, errors: list[str]
) -> None:
    """Validate the object form of the recursive payload schema.

    ``keys`` is the compact legacy spelling and makes every field required.
    New schemas may use ``required`` and ``optional`` maps, or the familiar
    JSON-Schema ``properties`` plus a list of required field names.  The
    latter is useful when a response has a stable object shape but a handful
    of fields are intentionally optional.
    """

    allowed = {
        "keys",
        "required",
        "optional",
        "properties",
        "additional_properties",
        "additionalProperties",
    }
    unknown = sorted(set(value) - {"type", *allowed})
    if unknown:
        errors.append(f"{label}: unknown keys {', '.join(unknown)}")

    field_maps: list[tuple[str, Any]] = []
    if "keys" in value:
        field_maps.append(("keys", value["keys"]))
    if "properties" in value:
        field_maps.append(("properties", value["properties"]))
    required = value.get("required")
    optional = value.get("optional")
    if isinstance(required, dict):
        field_maps.append(("required", required))
    elif required is not None and not isinstance(required, list):
        errors.append(f"{label}.required: expected an object or array of field names")
    if isinstance(optional, dict):
        field_maps.append(("optional", optional))
    elif optional is not None and not isinstance(optional, list):
        errors.append(f"{label}.optional: expected an object or array of field names")

    field_schemas: dict[str, Any] = {}
    field_sources: dict[str, str] = {}
    for source, fields in field_maps:
        if not isinstance(fields, dict):
            errors.append(f"{label}.{source}: expected an object")
            continue
        for key, schema in fields.items():
            if not isinstance(key, str) or not key:
                errors.append(
                    f"{label}.{source}: field names must be non-empty strings"
                )
                continue
            if key in field_schemas and field_sources[key] != source:
                errors.append(f"{label}: field {key!r} is declared more than once")
                continue
            field_schemas[key] = schema
            field_sources[key] = source
            _validate_payload_schema(schema, f"{label}.{source}.{key}", errors)

    if isinstance(required, list):
        for key in required:
            if not isinstance(key, str) or not key:
                errors.append(
                    f"{label}.required: field names must be non-empty strings"
                )
            elif key not in field_schemas:
                errors.append(f"{label}.required: unknown property {key!r}")
    if isinstance(optional, list):
        for key in optional:
            if not isinstance(key, str) or not key:
                errors.append(
                    f"{label}.optional: field names must be non-empty strings"
                )
            elif key not in field_schemas:
                errors.append(f"{label}.optional: unknown property {key!r}")

    if isinstance(required, list) and isinstance(optional, list):
        overlap = sorted(set(required) & set(optional))
        if overlap:
            errors.append(
                f"{label}: fields cannot be both required and optional: {', '.join(overlap)}"
            )

    for key in field_schemas:
        if isinstance(required, list) and key not in required:
            if not isinstance(optional, list) or key not in optional:
                errors.append(
                    f"{label}: property {key!r} is neither required nor optional"
                )

    for key in ("additional_properties", "additionalProperties"):
        if key in value and not _is_bool(value[key]):
            errors.append(f"{label}.{key}: expected a boolean")
    if "additional_properties" in value and "additionalProperties" in value:
        if value["additional_properties"] != value["additionalProperties"]:
            errors.append(f"{label}: additional property flags disagree")


def _validate_payload_schema(value: Any, label: str, errors: list[str]) -> None:
    """Validate one recursive JSON payload schema node.

    The compact Batch-0 form remains ``{"keys": {"name": "string"}}``.
    Recursive nodes additionally support object/array nodes, optional object
    fields, and alternatives expressed as a schema list, ``one_of``/``any_of``
    (plus their camelCase aliases), or a list-valued ``type``.
    """

    if isinstance(value, str):
        alternatives = value.split("|")
        for token in alternatives:
            if token not in _PAYLOAD_TYPES:
                errors.append(f"{label}: unknown payload type token {token!r}")
        return
    if isinstance(value, list):
        _validate_schema_alternatives(value, label, errors)
        return
    if not isinstance(value, dict):
        errors.append(f"{label}: expected a schema token or object")
        return

    # A field-level optional/required flag is accepted as a convenient wrapper
    # around any schema.  Object nodes use maps/lists with the same names, so
    # only boolean metadata is treated as a wrapper here.
    metadata = {
        key: value[key]
        for key in _SCHEMA_METADATA_KEYS
        if key in value and isinstance(value[key], bool)
    }
    core = {key: item for key, item in value.items() if key not in metadata}
    if metadata and not core:
        errors.append(f"{label}: optional/required metadata needs a schema")
        return

    alternative_keys = [
        key for key in ("one_of", "any_of", "oneOf", "anyOf", "variants") if key in core
    ]
    if alternative_keys:
        if len(alternative_keys) != 1 or set(core) - set(alternative_keys):
            errors.append(f"{label}: alternative schema has unknown or duplicate keys")
            return
        _validate_schema_alternatives(
            core[alternative_keys[0]], f"{label}.{alternative_keys[0]}", errors
        )
        return

    if "type" not in core:
        if "keys" in core:
            if not isinstance(core["keys"], dict) or not core["keys"]:
                errors.append(f"{label}.keys: expected a non-empty object")
                return
            if set(core) - {
                "keys",
                "optional",
                "additional_properties",
                "additionalProperties",
            }:
                errors.append(
                    f"{label}: unknown keys {', '.join(sorted(set(core) - {'keys', 'optional', 'additional_properties', 'additionalProperties'}))}"
                )
            _validate_object_schema({"type": "object", **core}, label, errors)
            return
        errors.append(f"{label}: expected a type, keys, or alternative schema")
        return

    schema_type = core["type"]
    if isinstance(schema_type, list):
        if set(core) - {"type"}:
            errors.append(f"{label}: list-valued type schemas cannot have extra keys")
        _validate_schema_alternatives(schema_type, f"{label}.type", errors)
        return
    if not isinstance(schema_type, str):
        errors.append(f"{label}.type: expected a string or array of schema types")
        return
    if schema_type in {"variant", "union"}:
        if set(core) != {"type", "variants"}:
            errors.append(f"{label}: {schema_type} schema expects only variants")
        else:
            _validate_schema_alternatives(core["variants"], f"{label}.variants", errors)
        return
    if "|" in schema_type:
        if set(core) != {"type"}:
            errors.append(f"{label}: alternative type schemas cannot have extra keys")
        _validate_schema_alternatives(schema_type.split("|"), f"{label}.type", errors)
        return
    if schema_type == "object":
        _validate_object_schema(core, label, errors)
        return
    if schema_type == "array":
        allowed = {"type", "items", "min_items", "max_items", "length"}
        unknown = sorted(set(core) - allowed)
        if unknown:
            errors.append(f"{label}: unknown keys {', '.join(unknown)}")
        if "items" not in core:
            errors.append(f"{label}.items: missing array item schema")
        else:
            _validate_payload_schema(core["items"], f"{label}.items", errors)
        for key in ("min_items", "max_items", "length"):
            if key in core and (not _is_int(core[key]) or core[key] < 0):
                errors.append(f"{label}.{key}: expected a non-negative integer")
        if (
            isinstance(core.get("min_items"), int)
            and isinstance(core.get("max_items"), int)
            and core["min_items"] > core["max_items"]
        ):
            errors.append(f"{label}: min_items cannot exceed max_items")
        return
    if schema_type not in _PAYLOAD_TYPES:
        errors.append(f"{label}.type: unknown payload type token {schema_type!r}")
    elif set(core) != {"type"}:
        errors.append(
            f"{label}: scalar schema has unknown keys {', '.join(sorted(set(core) - {'type'}))}"
        )


def _schema_keys(schema: Any) -> set[str] | None:
    """Return the exact field names from the compact closed-object spelling."""

    if not isinstance(schema, dict) or not isinstance(schema.get("keys"), dict):
        return None
    return set(schema["keys"])


def _validate_closed_payload_schema(value: Any, label: str, errors: list[str]) -> None:
    """Reject optional/open object nodes for active cross-surface payloads.

    The generic schema language supports optional fields for future contracts,
    but the active CLI paths intentionally use closed JSON documents.  Keeping
    this check separate from ``_validate_payload_schema`` preserves that useful
    generic capability without allowing an active path to hide drift behind an
    optional field or ``additionalProperties``.
    """

    if isinstance(value, list):
        for index, item in enumerate(value):
            _validate_closed_payload_schema(item, f"{label}[{index}]", errors)
        return
    if isinstance(value, str):
        return
    if not isinstance(value, dict):
        return
    if "optional" in value:
        errors.append(f"{label}: active payload schemas cannot declare optional fields")
    if value.get("additional_properties", value.get("additionalProperties", False)):
        errors.append(
            f"{label}: active payload schemas must reject additional properties"
        )
    if isinstance(value.get("keys"), dict):
        for key, child in value["keys"].items():
            _validate_closed_payload_schema(child, f"{label}.keys.{key}", errors)
    if isinstance(value.get("properties"), dict):
        for key, child in value["properties"].items():
            _validate_closed_payload_schema(child, f"{label}.properties.{key}", errors)
    for key in ("required", "optional"):
        if isinstance(value.get(key), dict):
            for child_name, child in value[key].items():
                _validate_closed_payload_schema(
                    child, f"{label}.{key}.{child_name}", errors
                )
    if "items" in value:
        _validate_closed_payload_schema(value["items"], f"{label}.items", errors)
    for key in ("one_of", "any_of", "oneOf", "anyOf", "variants"):
        if isinstance(value.get(key), list):
            _validate_closed_payload_schema(value[key], f"{label}.{key}", errors)


def _require_schema_keys(
    schema: Any, expected: set[str], label: str, errors: list[str]
) -> dict[str, Any] | None:
    fields = _schema_keys(schema)
    if fields is None:
        errors.append(f"{label}: expected a closed object schema with a keys map")
        return None
    if fields != expected:
        missing = sorted(expected - fields)
        replaced = sorted(fields - expected)
        details: list[str] = []
        if missing:
            details.append(f"missing {', '.join(missing)}")
        if replaced:
            details.append(f"unexpected {', '.join(replaced)}")
        errors.append(f"{label}: canonical keys differ ({'; '.join(details)})")
    return schema["keys"]


def _require_array_item_schema(
    schema: Any, label: str, errors: list[str]
) -> Any | None:
    if (
        not isinstance(schema, dict)
        or schema.get("type") != "array"
        or "items" not in schema
    ):
        errors.append(f"{label}: expected an array schema with item schema")
        return None
    return schema["items"]


def _require_schema_shape(
    schema: Any, expected: Any, label: str, errors: list[str]
) -> None:
    """Require a canonical schema node's keys and primitive types.

    ``_validate_payload_schema`` deliberately accepts the generic recursive
    schema language.  Promoted cross-surface paths need one exact spelling,
    though: a field renamed to another field with the same broad JSON shape,
    or a number changed to a string, is contract drift.  This helper keeps
    that stricter check local to the canonical paths.
    """

    if isinstance(expected, str):
        if schema != expected:
            errors.append(
                f"{label}: canonical type differs (expected {expected!r}, got {schema!r})"
            )
        return
    if not isinstance(expected, dict):
        errors.append(f"{label}: invalid canonical schema expectation")
        return

    if expected.get("type") == "array":
        if not isinstance(schema, dict) or schema.get("type") != "array":
            errors.append(f"{label}: canonical type differs (expected array)")
            return
        _require_schema_shape(
            schema.get("items"), expected.get("items"), f"{label}.items", errors
        )
        return

    expected_keys = expected.get("keys")
    if not isinstance(expected_keys, dict):
        errors.append(f"{label}: invalid canonical object expectation")
        return
    fields = _require_schema_keys(schema, set(expected_keys), label, errors)
    if fields is None:
        return
    for key, child in expected_keys.items():
        if key in fields:
            _require_schema_shape(fields[key], child, f"{label}.{key}", errors)


def _validate_canonical_payload_schema(
    path: str, payloads: dict[str, Any], label: str, errors: list[str]
) -> None:
    """Pin canonical keys and field types for promoted semantic paths."""

    success = payloads.get("success")
    if path == "analyze":
        top = _require_schema_keys(
            success, _ANALYZE_TOP_LEVEL_KEYS, f"{label}.success", errors
        )
        if top is None:
            return
        nested = {
            "key": _ANALYZE_KEY_KEYS,
            "time_signature": _ANALYZE_TIME_SIGNATURE_KEYS,
            "timbre": _ANALYZE_TIMBRE_KEYS,
            "dynamics": _ANALYZE_DYNAMICS_KEYS,
            "rhythm": _ANALYZE_RHYTHM_KEYS,
        }
        for name, expected in nested.items():
            _require_schema_keys(
                top.get(name), expected, f"{label}.success.{name}", errors
            )
        arrays = {
            "beats": _ANALYZE_BEAT_KEYS,
            "chords": _ANALYZE_CHORD_KEYS,
            "sections": _ANALYZE_SECTION_KEYS,
        }
        for name, expected in arrays.items():
            item = _require_array_item_schema(
                top.get(name), f"{label}.success.{name}", errors
            )
            if item is not None:
                _require_schema_keys(
                    item, expected, f"{label}.success.{name}.items", errors
                )
    elif path == "spectral":
        top = _require_schema_keys(
            success, {"n_frames", "features"}, f"{label}.success", errors
        )
        if top is None:
            return
        feature_schemas = _require_schema_keys(
            top.get("features"), _SPECTRAL_FEATURES, f"{label}.success.features", errors
        )
        if feature_schemas is not None:
            for name in sorted(_SPECTRAL_FEATURES):
                _require_schema_keys(
                    feature_schemas.get(name),
                    _SPECTRAL_STATS,
                    f"{label}.success.features.{name}",
                    errors,
                )
    elif path == "voice-change":
        expected_by_name = {
            "simple": _VOICE_SIMPLE_KEYS,
            "preset": _VOICE_PRESET_KEYS,
            "custom": _VOICE_COMMON_KEYS,
        }
        for name, expected in expected_by_name.items():
            schema = payloads.get(name)
            if schema is None:
                errors.append(f"{label}.payloads: missing canonical {name!r} schema")
                continue
            _require_schema_keys(schema, expected, f"{label}.{name}", errors)
    elif path == "project.compile":
        success = payloads.get("success")
        top = _require_schema_keys(
            success, _PROJECT_COMPILE_KEYS, f"{label}.success", errors
        )
        if top is not None:
            item = _require_array_item_schema(
                top.get("diagnostics"), f"{label}.success.diagnostics", errors
            )
            if item is not None:
                _require_schema_keys(
                    item,
                    _PROJECT_COMPILE_DIAGNOSTIC_KEYS,
                    f"{label}.success.diagnostics.items",
                    errors,
                )
    elif path == "synthesize-rir":
        _require_schema_keys(
            payloads.get("success"), _SYNTHESIZE_RIR_KEYS, f"{label}.success", errors
        )
    elif path == "mastering":
        _require_schema_keys(
            payloads.get("success"), _MASTERING_REPORT_KEYS, f"{label}.success", errors
        )
    elif path == "rhythm":
        _require_schema_shape(
            success, _RHYTHM_PAYLOAD_SCHEMA, f"{label}.success", errors
        )
    elif path == "pitch":
        _require_schema_shape(
            success, _PITCH_PAYLOAD_SCHEMA, f"{label}.success", errors
        )
    elif path == "eq":
        _require_schema_shape(success, _EQ_PAYLOAD_SCHEMA, f"{label}.success", errors)
    elif path == "mastering-processor":
        _require_schema_shape(
            success,
            _MASTERING_PROCESSOR_PAYLOAD_SCHEMA,
            f"{label}.success",
            errors,
        )


def validate_manifest(manifest: Any) -> list[str]:
    """Return schema errors for a decoded manifest (empty means valid)."""

    errors: list[str] = []
    if not _exact(manifest, _TOP_LEVEL_KEYS, "manifest", errors):
        return errors
    if manifest["schema_version"] != 2:
        errors.append("manifest.schema_version: expected 2")
    if manifest["contract"] != "cli-json-v2":
        errors.append("manifest.contract: expected cli-json-v2")
    if manifest["surfaces"] != ["native", "python"]:
        errors.append("manifest.surfaces: expected [native, python]")

    exit_codes = manifest["exit_codes"]
    if _exact(
        exit_codes,
        {
            "success",
            "usage",
            "invalid_parameter",
            "file_not_found",
            "invalid_format",
            "invalid_state",
            "legacy",
        },
        "manifest.exit_codes",
        errors,
    ):
        expected = {
            "success": 0,
            "usage": 2,
            "invalid_parameter": 3,
            "file_not_found": 4,
            "invalid_format": 5,
            "invalid_state": 9,
            "legacy": 1,
        }
        if exit_codes != expected:
            errors.append(f"manifest.exit_codes: expected {expected!r}")

    comparison = manifest["comparison"]
    if _exact(comparison, {"absolute", "relative"}, "manifest.comparison", errors):
        for key in comparison:
            if not _is_number(comparison[key]) or comparison[key] <= 0:
                errors.append(
                    f"manifest.comparison.{key}: expected a positive finite number"
                )

    inventory = manifest["inventory"]
    if _exact(
        inventory,
        {"schema_version", "command_fields", "option_fields", "expected_options"},
        "manifest.inventory",
        errors,
    ):
        if inventory["schema_version"] != 2:
            errors.append("manifest.inventory.schema_version: expected 2")
        if inventory["command_fields"] != ["path", "aliases", "options"]:
            errors.append("manifest.inventory.command_fields: unexpected field order")
        if inventory["option_fields"] != [
            "name",
            "type",
            "default",
            "aliases",
            "repeatable",
            "required",
        ]:
            errors.append("manifest.inventory.option_fields: unexpected field order")
        expected_options = inventory["expected_options"]
        if not isinstance(expected_options, dict):
            errors.append("manifest.inventory.expected_options: expected an object")
        else:
            command_records = manifest.get("commands")
            active_option_paths = (
                {
                    path
                    for path, record in command_records.items()
                    if isinstance(record, dict)
                    and record.get("option_status") == "active"
                }
                if isinstance(command_records, dict)
                else set()
            )
            missing = sorted(active_option_paths - set(expected_options))
            unknown = sorted(set(expected_options) - active_option_paths)
            if missing:
                errors.append(
                    "manifest.inventory.expected_options: missing paths "
                    + ", ".join(missing)
                )
            if unknown:
                errors.append(
                    "manifest.inventory.expected_options: unknown paths "
                    + ", ".join(unknown)
                )
            for path, options in expected_options.items():
                option_label = f"manifest.inventory.expected_options.{path}"
                if not isinstance(options, list):
                    errors.append(f"{option_label}: expected an array")
                    continue
                names: set[str] = set()
                for option_index, option in enumerate(options):
                    label = f"{option_label}[{option_index}]"
                    _validate_option(option, label, errors)
                    if isinstance(option, dict) and isinstance(option.get("name"), str):
                        if option["name"] in names:
                            errors.append(
                                f"{label}: duplicate option name {option['name']}"
                            )
                        names.add(option["name"])

    commands = manifest["commands"]
    command_paths: set[str] = set()
    active_from_commands: set[str] = set()
    if not isinstance(commands, dict) or not commands:
        errors.append("manifest.commands: expected a non-empty object")
    else:
        for path, record in commands.items():
            label = f"manifest.commands.{path}"
            if not isinstance(path, str) or not path:
                errors.append(
                    "manifest.commands: command paths must be non-empty strings"
                )
                continue
            command_paths.add(path)
            if not _exact(
                record, {"classification", "status", "option_status"}, label, errors
            ):
                continue
            classification = record["classification"]
            status = record["status"]
            option_status = record["option_status"]
            if classification not in _CLASSIFICATIONS:
                errors.append(
                    f"{label}.classification: unknown classification {classification!r}"
                )
            if status not in _STATUSES:
                errors.append(f"{label}.status: unknown status {status!r}")
            if option_status not in {"active", "pending"}:
                errors.append(
                    f"{label}.option_status: expected active or pending, got {option_status!r}"
                )
            if status in {"active", "pending"} and classification != "shared":
                errors.append(f"{label}: active/pending commands must be shared")
            if (
                status in {"native_only", "python_only", "intentional_variant"}
                and classification != status
            ):
                errors.append(f"{label}: status must match classification")
            if status == "active":
                active_from_commands.add(path)

    active = manifest["active_paths"]
    active_paths: set[str] = set()
    if not isinstance(active, list) or not active:
        errors.append("manifest.active_paths: expected a non-empty array")
    else:
        for index, contract in enumerate(active):
            label = f"manifest.active_paths[{index}]"
            if not _exact(
                contract,
                {"path", "options", "positionals", "payloads", "cases", "artifacts"},
                label,
                errors,
            ):
                continue
            path = contract["path"]
            if not isinstance(path, str) or not path:
                errors.append(f"{label}.path: expected a non-empty string")
            elif path in active_paths:
                errors.append(f"{label}.path: duplicate active path {path}")
            else:
                active_paths.add(path)
            options = contract["options"]
            if not isinstance(options, list):
                errors.append(f"{label}.options: expected an array")
            else:
                names: set[str] = set()
                for option_index, option in enumerate(options):
                    option_label = f"{label}.options[{option_index}]"
                    _validate_option(option, option_label, errors)
                    if isinstance(option, dict) and isinstance(option.get("name"), str):
                        if option["name"] in names:
                            errors.append(
                                f"{option_label}: duplicate option name {option['name']}"
                            )
                        names.add(option["name"])
            positionals = contract["positionals"]
            if not isinstance(positionals, list):
                errors.append(f"{label}.positionals: expected an array")
            else:
                names = set()
                for positional_index, positional in enumerate(positionals):
                    positional_label = f"{label}.positionals[{positional_index}]"
                    if not _exact(
                        positional,
                        {"name", "type", "required"},
                        positional_label,
                        errors,
                    ):
                        continue
                    if (
                        not isinstance(positional["name"], str)
                        or not positional["name"]
                    ):
                        errors.append(
                            f"{positional_label}.name: expected a non-empty string"
                        )
                    if positional["type"] not in _POSITIONAL_TYPES:
                        errors.append(
                            f"{positional_label}.type: unknown type {positional['type']!r}"
                        )
                    if not _is_bool(positional["required"]):
                        errors.append(
                            f"{positional_label}.required: expected a boolean"
                        )
                    if positional.get("name") in names:
                        errors.append(
                            f"{positional_label}: duplicate positional name {positional['name']}"
                        )
                    names.add(positional.get("name"))
            payloads = contract["payloads"]
            if not isinstance(payloads, dict) or not payloads:
                errors.append(f"{label}.payloads: expected a non-empty object")
            else:
                for payload_name, payload in payloads.items():
                    _validate_payload_schema(
                        payload, f"{label}.payloads.{payload_name}", errors
                    )
                    _validate_closed_payload_schema(
                        payload, f"{label}.payloads.{payload_name}", errors
                    )
                if path in {
                    "analyze",
                    "spectral",
                    "voice-change",
                    "project.compile",
                    "mastering",
                    "synthesize-rir",
                    "rhythm",
                    "pitch",
                    "eq",
                    "mastering-processor",
                }:
                    _validate_canonical_payload_schema(path, payloads, label, errors)
            cases = contract["cases"]
            case_ids: set[str] = set()
            if not isinstance(cases, list) or not cases:
                errors.append(f"{label}.cases: expected a non-empty array")
            else:
                for case_index, case in enumerate(cases):
                    case_label = f"{label}.cases[{case_index}]"
                    if not _exact(
                        case,
                        {
                            "id",
                            "argv",
                            "exit",
                            "legacy_exit",
                            "payload",
                            "artifact",
                            "stdout",
                        }
                        | (
                            {"stderr"}
                            if isinstance(case, dict) and "stderr" in case
                            else set()
                        )
                        | _declared_text_keys(case),
                        case_label,
                        errors,
                    ):
                        continue
                    case_id = case["id"]
                    if not isinstance(case_id, str) or not case_id:
                        errors.append(f"{case_label}.id: expected a non-empty string")
                    elif case_id in case_ids:
                        errors.append(f"{case_label}.id: duplicate case id {case_id}")
                    case_ids.add(case_id)
                    _argv(case["argv"], f"{case_label}.argv", errors)
                    if case["exit"] not in _EXIT_CODES:
                        errors.append(
                            f"{case_label}.exit: unsupported exit {case['exit']!r}"
                        )
                    if case["legacy_exit"] not in {0, 1}:
                        errors.append(f"{case_label}.legacy_exit: expected 0 or 1")
                    elif (case["exit"] == 0) != (case["legacy_exit"] == 0):
                        errors.append(
                            f"{case_label}: legacy exit must fold non-zero to 1"
                        )
                    if case["payload"] != "none" and case["payload"] not in payloads:
                        errors.append(
                            f"{case_label}.payload: unknown payload {case['payload']!r}"
                        )
                    if not isinstance(case["artifact"], str):
                        errors.append(f"{case_label}.artifact: expected a string")
                    if case["stdout"] not in {"json", "empty", "text"}:
                        errors.append(
                            f"{case_label}.stdout: expected json, empty or text"
                        )
                    if case.get("stderr", "empty") not in {"empty", "nonempty"}:
                        errors.append(
                            f"{case_label}.stderr: expected empty or nonempty"
                        )
                    _validate_text_expectations(case, case_label, errors)
            artifacts = contract["artifacts"]
            if not isinstance(artifacts, dict):
                errors.append(f"{label}.artifacts: expected an object")
            else:
                for artifact_name, artifact in artifacts.items():
                    artifact_label = f"{label}.artifacts.{artifact_name}"
                    if isinstance(artifact, dict) and artifact.get("kind") == "wav":
                        if not _exact(
                            artifact,
                            {"placeholder", "kind", "sample_rate_key", "sha256"},
                            artifact_label,
                            errors,
                        ):
                            continue
                        for key in ("placeholder", "sample_rate_key"):
                            if not isinstance(artifact[key], str) or not artifact[key]:
                                errors.append(
                                    f"{artifact_label}.{key}: expected a non-empty string"
                                )
                    elif isinstance(artifact, dict) and artifact.get("kind") == "json":
                        if not _exact(
                            artifact,
                            {"placeholder", "kind", "keys", "sha256"},
                            artifact_label,
                            errors,
                        ):
                            continue
                        if not _strings(
                            artifact["keys"], f"{artifact_label}.keys", errors
                        ):
                            pass
                        if not artifact["keys"]:
                            errors.append(
                                f"{artifact_label}.keys: expected a non-empty array"
                            )
                    else:
                        if not _exact(
                            artifact,
                            {"placeholder", "encoding", "bytes_key", "sha256"},
                            artifact_label,
                            errors,
                        ):
                            continue
                        for key in ("placeholder", "encoding", "bytes_key", "sha256"):
                            if not isinstance(artifact[key], str) or not artifact[key]:
                                errors.append(
                                    f"{artifact_label}.{key}: expected a non-empty string"
                                )
                    digest = artifact.get("sha256")
                    if (
                        not isinstance(digest, str)
                        or len(digest) != 64
                        or any(char not in "0123456789abcdef" for char in digest)
                    ):
                        errors.append(
                            f"{artifact_label}.sha256: expected a 64-character lowercase SHA-256"
                        )
            if isinstance(cases, list) and isinstance(artifacts, dict):
                for case_index, case in enumerate(cases):
                    if isinstance(case, dict) and isinstance(case.get("artifact"), str):
                        artifact_name = case["artifact"]
                        if artifact_name != "none" and artifact_name not in artifacts:
                            message = (
                                f"{label}.cases[{case_index}].artifact: "
                                f"unknown artifact {artifact_name!r}"
                            )
                            errors.append(message)

    if (
        isinstance(inventory, dict)
        and isinstance(inventory.get("expected_options"), dict)
        and isinstance(commands, dict)
    ):
        canonical_options = inventory["expected_options"]
        for contract in active if isinstance(active, list) else []:
            if not isinstance(contract, dict):
                continue
            path = contract.get("path")
            record = commands.get(path)
            if not isinstance(record, dict) or record.get("option_status") != "active":
                continue
            expected = canonical_options.get(path)
            if expected is None:
                continue
            expected_normalized = _normalized_option_inventory(expected)
            contract_normalized = _normalized_option_inventory(contract.get("options"))
            if (
                expected_normalized is not None
                and contract_normalized is not None
                and expected_normalized != contract_normalized
            ):
                errors.append(
                    f"manifest.active_paths.{path}.options: differs from "
                    "manifest.inventory.expected_options"
                )

    if active_paths != active_from_commands:
        errors.append(
            "manifest.active_paths: paths must exactly match commands with status active "
            f"(active_paths={sorted(active_paths)!r}, commands={sorted(active_from_commands)!r})"
        )
    parser_cases = manifest["parser_cases"]
    if not isinstance(parser_cases, list) or not parser_cases:
        errors.append("manifest.parser_cases: expected a non-empty array")
    else:
        parser_ids: set[str] = set()
        for index, case in enumerate(parser_cases):
            label = f"manifest.parser_cases[{index}]"
            if not _exact(
                case,
                {"id", "argv", "exit", "legacy_exit", "stdout", "payload"}
                | _declared_text_keys(case),
                label,
                errors,
            ):
                continue
            if not isinstance(case["id"], str) or not case["id"]:
                errors.append(f"{label}.id: expected a non-empty string")
            elif case["id"] in parser_ids:
                errors.append(f"{label}.id: duplicate parser case id {case['id']}")
            parser_ids.add(case["id"])
            _argv(case["argv"], f"{label}.argv", errors)
            if case["exit"] not in _EXIT_CODES:
                errors.append(f"{label}.exit: unsupported exit {case['exit']!r}")
            if case["legacy_exit"] not in {0, 1}:
                errors.append(f"{label}.legacy_exit: expected 0 or 1")
            elif (case["exit"] == 0) != (case["legacy_exit"] == 0):
                errors.append(f"{label}: legacy exit must fold non-zero to 1")
            if case["stdout"] not in {"json", "empty", "text"}:
                errors.append(f"{label}.stdout: expected json, empty or text")
            _validate_text_expectations(case, label, errors)
            if case["payload"] != "none":
                errors.append(
                    f"{label}.payload: parser cases must not declare a JSON payload"
                )

    fixtures = manifest["fixtures"]
    if _exact(fixtures, {"audio", "projects", "presets"}, "manifest.fixtures", errors):
        audio = fixtures["audio"]
        if _exact(
            audio,
            {"sample_rate", "frames", "frequency_hz", "amplitude"},
            "manifest.fixtures.audio",
            errors,
        ):
            if not _is_int(audio["sample_rate"]) or audio["sample_rate"] <= 0:
                errors.append(
                    "manifest.fixtures.audio.sample_rate: expected a positive integer"
                )
            if not _is_int(audio["frames"]) or audio["frames"] <= 0:
                errors.append(
                    "manifest.fixtures.audio.frames: expected a positive integer"
                )
            for key in ("frequency_hz", "amplitude"):
                if not _is_number(audio[key]):
                    errors.append(
                        f"manifest.fixtures.audio.{key}: expected a finite number"
                    )
        projects = fixtures["projects"]
        if _exact(
            projects,
            {"clean", "warning", "malformed"},
            "manifest.fixtures.projects",
            errors,
        ):
            for key in ("clean", "warning", "malformed"):
                if not isinstance(projects[key], str):
                    errors.append(
                        f"manifest.fixtures.projects.{key}: expected a string"
                    )
            for key in ("clean", "warning"):
                if isinstance(projects.get(key), str):
                    try:
                        json.loads(projects[key])
                    except json.JSONDecodeError as exc:
                        errors.append(
                            f"manifest.fixtures.projects.{key}: invalid JSON: {exc.msg}"
                        )
        presets = fixtures["presets"]
        if _exact(
            presets,
            {"valid", "invalid", "custom"},
            "manifest.fixtures.presets",
            errors,
        ):
            for key in ("valid", "invalid", "custom"):
                if not isinstance(presets[key], dict):
                    errors.append(
                        f"manifest.fixtures.presets.{key}: expected an object"
                    )

    if command_paths and len(command_paths) != len(commands):
        errors.append("manifest.commands: duplicate command path")
    return errors


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read manifest {path}: {exc}") from exc
    errors = validate_manifest(value)
    if errors:
        raise ValueError(
            "manifest schema validation failed:\n"
            + "\n".join(f"- {error}" for error in errors)
        )
    return value


def _payload_type(value: Any, token: str) -> bool:
    """Return whether a value matches a primitive Batch-0 payload token."""

    if token == "boolean":
        return _is_bool(value)
    if token == "integer":
        return _is_int(value)
    if token == "number":
        return _is_number(value)
    if token in {"string", "path"}:
        return isinstance(value, str)
    if token == "null":
        return value is None
    if token == "any":
        return True
    if token == "array:string":
        return isinstance(value, list) and all(isinstance(item, str) for item in value)
    if token == "array:number:12":
        return (
            isinstance(value, list)
            and len(value) == 12
            and all(_is_number(item) for item in value)
        )
    return False


def _schema_alternatives(schema: Any) -> list[Any] | None:
    """Return alternative schemas, or ``None`` when *schema* is one node."""

    if isinstance(schema, list):
        return schema
    if isinstance(schema, str) and "|" in schema:
        return schema.split("|")
    if not isinstance(schema, dict):
        return None
    for key in ("one_of", "any_of", "oneOf", "anyOf", "variants"):
        if key in schema:
            return schema[key]
    schema_type = schema.get("type")
    if isinstance(schema_type, list):
        return schema_type
    if isinstance(schema_type, str) and "|" in schema_type:
        return schema_type.split("|")
    if schema_type in {"variant", "union"}:
        return schema.get("variants")
    return None


def _schema_metadata(schema: dict[str, Any]) -> dict[str, Any]:
    """Drop field-level optional/required metadata from a schema node."""

    return {
        key: value
        for key, value in schema.items()
        if not (key in _SCHEMA_METADATA_KEYS and isinstance(value, bool))
    }


def _object_schema_parts(
    schema: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any], bool]:
    """Normalize all supported object spellings to required/optional maps."""

    core = _schema_metadata(schema)
    required: dict[str, Any] = {}
    optional: dict[str, Any] = {}

    def add_field(target: dict[str, Any], key: str, value: Any) -> None:
        # A field-level marker is useful for compact ``keys`` objects, while
        # the required/optional maps and lists remain the unambiguous form for
        # larger schemas. Explicit map/list membership wins over the marker.
        if isinstance(value, dict) and value.get("optional") is True:
            optional[key] = value
        elif isinstance(value, dict) and value.get("required") is True:
            required[key] = value
        else:
            target[key] = value

    if "keys" in core and isinstance(core["keys"], dict):
        for key, value in core["keys"].items():
            add_field(required, key, value)
    if "properties" in core and isinstance(core["properties"], dict):
        properties = core["properties"]
    else:
        properties = {}
    if isinstance(core.get("required"), dict):
        for key, value in core["required"].items():
            required[key] = value
    if isinstance(core.get("optional"), dict):
        for key, value in core["optional"].items():
            optional[key] = value
    if isinstance(core.get("required"), list):
        required_names = set(core["required"])
        for key, value in properties.items():
            if key in required_names:
                required[key] = value
            else:
                add_field(optional, key, value)
    else:
        for key, value in properties.items():
            if key not in required:
                add_field(optional, key, value)
    if isinstance(core.get("optional"), list):
        for key in core["optional"]:
            if key in required:
                optional[key] = required.pop(key)
    additional = core.get(
        "additional_properties", core.get("additionalProperties", False)
    )
    return required, optional, bool(additional)


def _schema_matches(value: Any, schema: Any, path: str) -> list[str]:
    """Validate *value* against one recursive payload schema node."""

    alternatives = _schema_alternatives(schema)
    if alternatives is not None:
        if not isinstance(alternatives, list) or not alternatives:
            return [f"{path}: expected a non-empty alternative schema"]
        alternative_errors = [
            _schema_matches(value, item, path) for item in alternatives
        ]
        if any(not errors for errors in alternative_errors):
            return []
        expected = ", ".join(_schema_description(item) for item in alternatives)
        return [f"{path}: expected one of {expected}, got {type(value).__name__}"]

    if isinstance(schema, str):
        if _payload_type(value, schema):
            return []
        return [f"{path}: expected {schema}, got {type(value).__name__}"]
    if not isinstance(schema, dict):
        return [f"{path}: invalid payload schema"]

    core = _schema_metadata(schema)
    schema_type = core.get("type")
    if schema_type is None and "keys" in core:
        schema_type = "object"
    if schema_type == "object":
        if not isinstance(value, dict):
            return [f"{path}: expected object, got {type(value).__name__}"]
        required, optional, additional = _object_schema_parts(core)
        errors: list[str] = []
        expected = set(required) | set(optional)
        missing = sorted(set(required) - set(value))
        unknown = sorted(set(value) - expected)
        if missing:
            errors.append(f"{path}: missing keys {', '.join(missing)}")
        if unknown and not additional:
            errors.append(f"{path}: unknown keys {', '.join(unknown)}")
        for key, field_schema in (*required.items(), *optional.items()):
            if key in value:
                errors.extend(
                    _schema_matches(value[key], field_schema, f"{path}.{key}")
                )
        return errors
    if schema_type == "array":
        if not isinstance(value, list):
            return [f"{path}: expected array, got {type(value).__name__}"]
        errors = []
        if "length" in core and len(value) != core["length"]:
            errors.append(
                f"{path}: expected array length {core['length']}, got {len(value)}"
            )
        if "min_items" in core and len(value) < core["min_items"]:
            errors.append(
                f"{path}: expected at least {core['min_items']} items, got {len(value)}"
            )
        if "max_items" in core and len(value) > core["max_items"]:
            errors.append(
                f"{path}: expected at most {core['max_items']} items, got {len(value)}"
            )
        if "items" in core:
            for index, item in enumerate(value):
                errors.extend(_schema_matches(item, core["items"], f"{path}[{index}]"))
        return errors
    if isinstance(schema_type, str):
        if _payload_type(value, schema_type):
            return []
        return [f"{path}: expected {schema_type}, got {type(value).__name__}"]
    return [f"{path}: invalid payload schema"]


def _schema_description(schema: Any) -> str:
    if isinstance(schema, str):
        return schema
    if isinstance(schema, dict):
        if isinstance(schema.get("type"), str):
            return schema["type"]
        if "keys" in schema or schema.get("type") == "object":
            return "object"
        return "schema"
    return "schema"


def validate_payload(payload: Any, schema: Any, label: str) -> list[str]:
    return _schema_matches(payload, schema, label)


def parse_single_json(stdout: str) -> Any:
    text = stdout.strip()
    if not text:
        raise ValueError("stdout is empty")
    decoder = json.JSONDecoder()
    try:
        value, end = decoder.raw_decode(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"stdout is not JSON: {exc.msg}") from exc
    if text[end:].strip():
        raise ValueError("stdout contains more than one JSON document")
    return value


def _compare_values(
    left: Any, right: Any, path: str, absolute: float, relative: float
) -> str | None:
    if isinstance(left, bool) or isinstance(right, bool):
        return None if left == right else f"{path}: {left!r} != {right!r}"
    if _is_number(left) and _is_number(right):
        difference = abs(float(left) - float(right))
        limit = max(absolute, relative * max(abs(float(left)), abs(float(right))))
        return (
            None
            if difference <= limit
            else f"{path}: {left!r} != {right!r} (delta {difference:g} > {limit:g})"
        )
    if type(left) is not type(right):
        return f"{path}: {type(left).__name__} != {type(right).__name__}"
    if isinstance(left, dict):
        if set(left) != set(right):
            return f"{path}: object keys differ ({sorted(left)!r} != {sorted(right)!r})"
        for key in left:
            mismatch = _compare_values(
                left[key], right[key], f"{path}.{key}", absolute, relative
            )
            if mismatch:
                return mismatch
        return None
    if isinstance(left, list):
        if len(left) != len(right):
            return f"{path}: array lengths differ ({len(left)} != {len(right)})"
        for index, (left_item, right_item) in enumerate(zip(left, right, strict=True)):
            mismatch = _compare_values(
                left_item, right_item, f"{path}[{index}]", absolute, relative
            )
            if mismatch:
                return mismatch
        return None
    return None if left == right else f"{path}: {left!r} != {right!r}"


def _compare_payloads(
    path: str, left: Any, right: Any, manifest: dict[str, Any], case_id: str
) -> str | None:
    absolute = float(manifest["comparison"]["absolute"])
    relative = float(manifest["comparison"]["relative"])
    if path == "version":
        if not isinstance(left, dict) or not isinstance(right, dict):
            return f"{path}.{case_id}: expected objects for surface comparison"
        for key in ("cli_version", "lib_version"):
            mismatch = _compare_values(
                left.get(key), right.get(key), f"{path}.{key}", absolute, relative
            )
            if mismatch:
                return mismatch
        return None
    if path == "voice-preset-validate" and case_id == "failure":
        if left.get("ok") is not False or right.get("ok") is not False:
            return f"{path}.{case_id}: both failure payloads must set ok=false"
        return None
    if path == "voice-preset-validate" and case_id == "success":
        if left.get("ok") is not True or right.get("ok") is not True:
            return f"{path}.{case_id}: both success payloads must set ok=true"
        try:
            left_json = json.loads(left["normalized_json"])
            right_json = json.loads(right["normalized_json"])
        except (TypeError, json.JSONDecodeError) as exc:
            return f"{path}.{case_id}: normalized_json is not valid JSON: {exc}"
        return _compare_values(
            left_json, right_json, f"{path}.normalized_json", absolute, relative
        )
    if path == "synthesize-rir":
        if not isinstance(left, dict) or not isinstance(right, dict):
            return f"{path}.{case_id}: expected objects for surface comparison"
        for key in ("sample_rate", "samples"):
            mismatch = _compare_values(
                left.get(key), right.get(key), f"{path}.{key}", absolute, relative
            )
            if mismatch:
                return mismatch
        return None
    return _compare_values(left, right, f"{path}.{case_id}", absolute, relative)


def _write_wav(path: Path, fixture: dict[str, Any]) -> None:
    sample_rate = int(fixture["sample_rate"])
    frames = int(fixture["frames"])
    frequency = float(fixture["frequency_hz"])
    amplitude = float(fixture["amplitude"])
    samples = bytearray()
    for index in range(frames):
        sample = int(
            round(
                max(
                    -1.0,
                    min(
                        1.0,
                        amplitude
                        * math.sin(2.0 * math.pi * frequency * index / sample_rate),
                    ),
                )
                * 32767.0
            )
        )
        samples.extend(struct.pack("<h", sample))
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(bytes(samples))


def _write_fixtures(directory: Path, manifest: dict[str, Any]) -> dict[str, str]:
    fixtures = manifest["fixtures"]
    paths: dict[str, str] = {}
    audio_path = directory / "contract.wav"
    _write_wav(audio_path, fixtures["audio"])
    paths["audio"] = str(audio_path)
    for name, text in fixtures["projects"].items():
        project_path = directory / f"project_{name}.json"
        project_path.write_text(text, encoding="utf-8")
        paths[f"project_{name}"] = str(project_path)
    for name, value in fixtures["presets"].items():
        preset_path = directory / f"preset_{name}.json"
        preset_path.write_text(
            json.dumps(value, separators=(",", ":")), encoding="utf-8"
        )
        paths[f"preset_{name}"] = str(preset_path)
    paths["project_warning_output"] = str(directory / "canonical_project.json")
    paths["mastering_report"] = str(directory / "mastering-report.json")
    paths["preset_missing"] = str(directory / "preset-does-not-exist.json")
    paths["output"] = str(directory / "rejected-output.wav")
    paths["rir_output"] = str(directory / "rir-output.wav")
    return paths


def _resolve_argv(argv: list[str], paths: dict[str, str]) -> list[str]:
    return [
        paths.get(token[1:-1], token)
        if token.startswith("{") and token.endswith("}")
        else token
        for token in argv
    ]


def _resolve_executable(executable: str) -> str:
    """Resolve repository-relative executable paths before a surface changes cwd.

    Bare command names intentionally remain bare so callers can select an
    executable through PATH (for example ``python3`` or an installed
    ``sonare``).  A path such as ``bindings/python/.venv/bin/python`` instead
    denotes a repository artifact and must retain that meaning when the Python
    surface runs from ``bindings/python``.
    """
    separators = {os.sep}
    if os.altsep:
        separators.add(os.altsep)
    if any(separator in executable for separator in separators):
        # Do not resolve symlinks here. Virtual-environment launchers are often
        # symlinks whose *spelling* determines discovery of pyvenv.cfg; resolving
        # them would invoke the base interpreter and drop its dependencies.
        return (
            os.path.abspath(os.path.join(ROOT, executable))
            if not Path(executable).is_absolute()
            else executable
        )
    return executable


def _resolved_python_library(python_executable: str, timeout: float) -> str | None:
    """Ask the Python surface which shared library it will load.

    The search order (``SONARE_LIB_PATH``, then the build tree, then the
    package-adjacent copy) belongs to the surface, and it does not follow this
    checker's ``--native`` directory, so the answer is taken from the
    interpreter that is about to run rather than reconstructed from a path.
    Returns None when the surface cannot be asked, which leaves the comparison
    to proceed exactly as before.
    """
    if Path(python_executable).name.startswith("sonare"):
        return None
    cwd = ROOT / "bindings" / "python"
    environment = os.environ.copy()
    environment["PYTHONPATH"] = (
        str(cwd / "src") + os.pathsep + environment.get("PYTHONPATH", "")
    )
    try:
        completed = subprocess.run(
            [
                python_executable,
                "-c",
                "from libsonare._ffi import resolved_library_path;"
                "print(resolved_library_path())",
            ],
            cwd=str(cwd),
            env=environment,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip() or None


def _straddling_source(older: float, newer: float) -> Path | None:
    """Return a core source file last modified between two artifact link times."""
    for directory in ("src", "include"):
        root = ROOT / directory
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            try:
                mtime = path.stat().st_mtime
            except OSError:
                continue
            if older < mtime <= newer:
                return path
    return None


def _count_sources_after(mtime: float) -> int:
    """Count core source files last modified after a given time."""
    count = 0
    for directory in ("src", "include"):
        root = ROOT / directory
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            try:
                if path.stat().st_mtime > mtime:
                    count += 1
            except OSError:
                continue
    return count


def _check_artifact_skew(
    native_executable: str,
    python_executable: str,
    timeout: float,
    report: list[tuple[str, str]],
) -> None:
    """Name the compared artifacts, and reject two built from different cores.

    Every run prints which two artifacts were compared. Whether the Python
    surface loads the build tree, a ``SONARE_LIB_PATH`` override, or the
    package-adjacent copy is not visible from the invocation, so a passing
    contract run that does not say leaves the reader to assume it.

    The rejection is narrower than the announcement. The two surfaces are
    separate link products and the targets that refresh them are separate as
    well: ``build-shared`` rebuilds only the shared library, so a core edit
    followed by ``build-shared`` alone leaves the native CLI a generation
    behind, and comparing across that gap reports a difference belonging to the
    build rather than to either surface. Two equally stale artifacts still agree
    with each other, so staleness alone is reported and not rejected; what is
    rejected is a core source change landing between the two link times.
    """
    library = _resolved_python_library(python_executable, timeout)
    if library is None:
        return
    try:
        native_mtime = os.path.getmtime(native_executable)
        library_mtime = os.path.getmtime(library)
    except OSError:
        return
    provenance = f"cli contract v2: comparing {native_executable} and {library}"
    pending = _count_sources_after(max(native_mtime, library_mtime))
    if pending:
        provenance += f" ({pending} core source file(s) are newer)"
    print(provenance)
    straddling = _straddling_source(*sorted((native_mtime, library_mtime)))
    if straddling is None:
        return
    behind, ahead = (
        (native_executable, library)
        if native_mtime < library_mtime
        else (library, native_executable)
    )
    report.append(
        (
            "fail",
            f"artifact skew: {behind} predates {straddling.relative_to(ROOT)}, "
            f"which {ahead} already includes -- rebuild both before comparing",
        )
    )


def _run(
    surface: str, executable: str, argv: list[str], legacy: bool, timeout: float
) -> dict[str, Any]:
    environment = os.environ.copy()
    if legacy:
        environment["SONARE_LEGACY_EXIT"] = "1"
    if surface == "native":
        command = [executable, *argv]
        cwd = ROOT
    else:
        executable_name = Path(executable).name
        if executable_name.startswith("sonare"):
            command = [executable, *argv]
        else:
            command = [executable, "-m", "libsonare.cli", *argv]
        cwd = ROOT / "bindings" / "python"
        python_src = str(cwd / "src")
        environment["PYTHONPATH"] = (
            python_src + os.pathsep + environment.get("PYTHONPATH", "")
        )
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {
            "returncode": None,
            "stdout": "",
            "stderr": "",
            "error": str(exc),
            "command": command,
        }
    return {
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
        "error": "",
        "command": command,
    }


def _expected_paths(commands: dict[str, Any], surface: str) -> set[str]:
    return {
        path
        for path, record in commands.items()
        if record["classification"]
        in {"shared", "intentional_variant", f"{surface}_only"}
    }


def _validate_inventory(
    value: Any, surface: str
) -> tuple[list[str], dict[str, dict[str, Any]]]:
    errors: list[str] = []
    if not _exact(
        value, {"schema_version", "surface", "commands"}, f"inventory.{surface}", errors
    ):
        return errors, {}
    if value["schema_version"] != 2:
        errors.append(f"inventory.{surface}.schema_version: expected 2")
    if value["surface"] != surface:
        errors.append(f"inventory.{surface}.surface: expected {surface!r}")
    commands: dict[str, dict[str, Any]] = {}
    if not isinstance(value["commands"], list):
        errors.append(f"inventory.{surface}.commands: expected an array")
        return errors, commands
    for index, command in enumerate(value["commands"]):
        label = f"inventory.{surface}.commands[{index}]"
        if not _exact(command, {"path", "aliases", "options"}, label, errors):
            continue
        path = command["path"]
        if not isinstance(path, str) or not path:
            errors.append(f"{label}.path: expected a non-empty string")
            continue
        if path in commands:
            errors.append(f"{label}.path: duplicate command {path}")
        if not _strings(command["aliases"], f"{label}.aliases", errors):
            pass
        options = command["options"]
        if not isinstance(options, list):
            errors.append(f"{label}.options: expected an array")
        else:
            names: set[str] = set()
            for option_index, option in enumerate(options):
                option_label = f"{label}.options[{option_index}]"
                _validate_option(option, option_label, errors)
                if isinstance(option, dict) and isinstance(option.get("name"), str):
                    if option["name"] in names:
                        errors.append(
                            f"{option_label}: duplicate option {option['name']}"
                        )
                    names.add(option["name"])
        commands[path] = command
    return errors, commands


_OPTION_RECORD_FIELDS = {
    "name",
    "type",
    "default",
    "aliases",
    "repeatable",
    "required",
}


def _option_inventory_is_well_formed(options: Any) -> bool:
    """Return whether option records are safe to normalize.

    Manifest validation reports the detailed schema error.  This lightweight
    guard is also used by the live checker because tests and callers may invoke
    the inventory helper with an intentionally malformed candidate manifest.
    """

    return isinstance(options, list) and all(
        isinstance(option, dict)
        and set(option) == _OPTION_RECORD_FIELDS
        and isinstance(option.get("name"), str)
        and isinstance(option.get("aliases"), list)
        and all(isinstance(alias, str) for alias in option["aliases"])
        and isinstance(option.get("repeatable"), bool)
        and isinstance(option.get("required"), bool)
        for option in options
    )


def _normalized_option_inventory(
    options: Any,
) -> list[dict[str, Any]] | None:
    """Normalize only ordering for the strict shared-option comparison.

    Command-line parsers are free to register options in a different order,
    and aliases are commonly emitted in declaration order.  Every other
    option field remains untouched, so a default/type/repeatability drift is
    still a contract failure.
    """

    if not _option_inventory_is_well_formed(options):
        return None
    return [
        {
            "name": option["name"],
            "type": option["type"],
            "default": option["default"],
            "aliases": sorted(option["aliases"]),
            "repeatable": option["repeatable"],
            "required": option["required"],
        }
        for option in sorted(options, key=lambda item: item["name"])
    ]


def _snapshot_inventory_validation(
    value: Any, surface: str, manifest: dict[str, Any]
) -> tuple[list[str], dict[str, dict[str, Any]]]:
    """Validate one live inventory for the maintenance snapshot mode.

    The normal checker compares only the paths listed by the committed
    ``inventory.expected_options`` map.  Snapshot generation deliberately
    runs before that map is updated, so it validates the complete schema and
    classified path set independently of option promotion state.
    """

    errors, commands = _validate_inventory(value, surface)
    expected = _expected_paths(manifest["commands"], surface)
    actual = set(commands)
    for path in sorted(expected - actual):
        errors.append(f"inventory.{surface}: missing classified path {path}")
    for path in sorted(actual - expected):
        errors.append(f"inventory.{surface}: unclassified path {path}")
    return errors, commands


def _build_shared_option_snapshot(
    native: Any, python: Any, manifest: dict[str, Any]
) -> dict[str, list[dict[str, Any]]]:
    """Build the committed option snapshot from two schema-v2 inventories.

    Every manifest path classified as ``shared`` participates, regardless of
    its current ``option_status``.  The caller receives no partial snapshot:
    malformed, missing, or mismatched input raises ``ValueError`` instead.
    """

    errors: list[str] = []
    inventories: dict[str, dict[str, dict[str, Any]]] = {}
    for surface, value in (("native", native), ("python", python)):
        validation_errors, commands = _snapshot_inventory_validation(
            value, surface, manifest
        )
        errors.extend(validation_errors)
        inventories[surface] = commands

    snapshot: dict[str, list[dict[str, Any]]] = {}
    shared_paths = sorted(
        path
        for path, record in manifest["commands"].items()
        if record["classification"] == "shared"
    )
    for path in shared_paths:
        native_command = inventories["native"].get(path)
        python_command = inventories["python"].get(path)
        if native_command is None:
            errors.append(f"inventory.native: missing shared path {path}")
        if python_command is None:
            errors.append(f"inventory.python: missing shared path {path}")
        if native_command is None or python_command is None:
            continue

        native_options = _normalized_option_inventory(native_command.get("options"))
        python_options = _normalized_option_inventory(python_command.get("options"))
        if native_options is None:
            errors.append(
                f"inventory.native.{path}: malformed option metadata cannot be compared"
            )
        if python_options is None:
            errors.append(
                f"inventory.python.{path}: malformed option metadata cannot be compared"
            )
        if native_options is None or python_options is None:
            continue
        if native_options != python_options:
            errors.append(
                f"inventory.shared.{path}: native/Python option schemas differ\n"
                f"  native: {native_options!r}\n"
                f"  python: {python_options!r}"
            )
            continue
        snapshot[path] = native_options

    if errors:
        raise ValueError(
            "shared option snapshot validation failed:\n" + "\n".join(errors)
        )
    return snapshot


def _read_inventory_dump(
    surface: str, executable: str, timeout: float
) -> tuple[Any | None, str | None]:
    """Read one fresh ``--dump-cli-contract`` document for snapshot mode."""

    result = _run(surface, executable, ["--dump-cli-contract"], False, timeout)
    if result["returncode"] != 0 or not result["stdout"].strip():
        detail = result.get("error") or f"exit {result['returncode']}"
        return (
            None,
            f"inventory.{surface}: --dump-cli-contract is not available ({detail})",
        )
    try:
        return parse_single_json(result["stdout"]), None
    except ValueError as exc:
        return (
            None,
            f"inventory.{surface}: --dump-cli-contract did not return one JSON document ({exc})",
        )


def _inventory_checks(
    surface: str,
    executable: str,
    manifest: dict[str, Any],
    timeout: float,
    report: list[tuple[str, str]],
) -> dict[str, dict[str, Any]] | None:
    result = _run(surface, executable, ["--dump-cli-contract"], False, timeout)
    if result["returncode"] != 0 or not result["stdout"].strip():
        detail = result.get("error") or f"exit {result['returncode']}"
        report.append(
            (
                "expected",
                f"inventory.{surface}: --dump-cli-contract is not available yet ({detail})",
            )
        )
        return None
    try:
        value = parse_single_json(result["stdout"])
    except ValueError as exc:
        message = f"inventory.{surface}: --dump-cli-contract did not return one JSON document ({exc})"
        report.append(
            (
                "expected",
                message,
            )
        )
        return None
    errors, commands = _validate_inventory(value, surface)
    for error in errors:
        report.append(("fail", error))
    expected = _expected_paths(manifest["commands"], surface)
    actual = set(commands)
    for path in sorted(expected - actual):
        report.append(("fail", f"inventory.{surface}: missing classified path {path}"))
    for path in sorted(actual - expected):
        report.append(("fail", f"inventory.{surface}: unclassified path {path}"))
    expected_options_by_path = manifest.get("inventory", {}).get("expected_options", {})
    if not isinstance(expected_options_by_path, dict):
        report.append(
            (
                "fail",
                f"manifest.inventory.expected_options: expected an object, got {type(expected_options_by_path).__name__}",
            )
        )
        expected_options_by_path = {}
    for path, expected_options in expected_options_by_path.items():
        if path not in commands:
            continue
        actual_options = commands[path]["options"]
        actual_normalized = _normalized_option_inventory(actual_options)
        expected_normalized = _normalized_option_inventory(expected_options)
        if actual_normalized is None:
            # _validate_inventory already emitted the detailed field-level
            # schema errors.  Keep the live checker total and avoid a KeyError
            # while still making this path fail explicitly.
            report.append(
                (
                    "fail",
                    f"inventory.{surface}.{path}: malformed option metadata cannot be compared",
                )
            )
            continue
        if expected_normalized is None:
            report.append(
                (
                    "fail",
                    f"manifest.inventory.expected_options.{path}: malformed option metadata cannot be compared",
                )
            )
            continue
        if actual_normalized != expected_normalized:
            message = (
                f"inventory.{surface}.{path}: active option schema differs\n"
                f"  expected: {expected_normalized!r}\n"
                f"  actual:   {actual_normalized!r}"
            )
            report.append(
                (
                    "fail",
                    message,
                )
            )
    return commands


def _compare_active_inventory_options(
    native: dict[str, dict[str, Any]],
    python: dict[str, dict[str, Any]],
    manifest: dict[str, Any],
    report: list[tuple[str, str]],
) -> None:
    """Compare native/Python option inventories for active shared paths.

    The manifest-to-inventory check above catches drift against the declared
    contract.  This explicit cross-surface check keeps the intended invariant
    visible and also protects against accidentally declaring two different
    active contracts for the same path.
    """

    for path, record in manifest["commands"].items():
        if record["classification"] != "shared" or record["option_status"] != "active":
            continue
        if path not in native or path not in python:
            continue
        if (
            not isinstance(native[path].get("options"), list)
            or not isinstance(python[path].get("options"), list)
            or any(
                not isinstance(option, dict) or "name" not in option
                for option in [*native[path]["options"], *python[path]["options"]]
            )
        ):
            continue
        left = _normalized_option_inventory(native[path]["options"])
        right = _normalized_option_inventory(python[path]["options"])
        if left is None or right is None:
            report.append(
                (
                    "fail",
                    f"inventory.shared.{path}: malformed option metadata cannot be compared",
                )
            )
            continue
        if left != right:
            report.append(
                (
                    "fail",
                    f"inventory.shared.{path}: native/Python active option schemas differ\n"
                    f"  native: {left!r}\n"
                    f"  python: {right!r}",
                )
            )


def _check_artifact(
    artifact_name: str,
    contract: dict[str, Any],
    payload: Any,
    paths: dict[str, str],
    label: str,
    report: list[tuple[str, str]],
) -> None:
    if artifact_name == "none":
        return
    artifact = contract["artifacts"].get(artifact_name)
    if artifact is None:
        report.append(("fail", f"{label}: unknown artifact {artifact_name}"))
        return
    path = Path(paths[artifact["placeholder"]])
    if not path.is_file():
        report.append(("fail", f"{label}: expected artifact {path} was not created"))
        return
    data = path.read_bytes()
    if artifact.get("kind") == "wav":
        try:
            with wave.open(str(path), "rb") as input_wav:
                sample_rate = input_wav.getframerate()
                frame_count = input_wav.getnframes()
        except (EOFError, wave.Error) as exc:
            report.append(("fail", f"{label}: artifact is not a readable WAV ({exc})"))
            return
        sample_rate_key = artifact["sample_rate_key"]
        if not isinstance(payload, dict) or payload.get(sample_rate_key) != sample_rate:
            payload_rate = (
                payload.get(sample_rate_key) if isinstance(payload, dict) else None
            )
            report.append(
                (
                    "fail",
                    f"{label}: payload {sample_rate_key}={payload_rate!r} does not equal WAV sample rate {sample_rate}",
                )
            )
        if isinstance(payload, dict) and isinstance(payload.get("samples"), int):
            if payload["samples"] != frame_count:
                report.append(
                    (
                        "fail",
                        f"{label}: payload samples={payload['samples']!r} does not equal WAV frames {frame_count}",
                    )
                )
        actual_digest = hashlib.sha256(data).hexdigest()
        if actual_digest != artifact["sha256"]:
            report.append(
                (
                    "fail",
                    f"{label}: artifact SHA-256 {actual_digest} does not match {artifact['sha256']}",
                )
            )
        return
    if artifact.get("kind") == "json":
        try:
            parsed_artifact = parse_single_json(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            report.append(
                ("fail", f"{label}: artifact is not one JSON document ({exc})")
            )
            return
        if not isinstance(parsed_artifact, dict) or set(parsed_artifact) != set(
            artifact["keys"]
        ):
            actual_keys = (
                sorted(parsed_artifact) if isinstance(parsed_artifact, dict) else None
            )
            report.append(
                (
                    "fail",
                    f"{label}: JSON artifact keys {actual_keys!r} do not match {artifact['keys']!r}",
                )
            )
        actual_digest = hashlib.sha256(data).hexdigest()
        if actual_digest != artifact["sha256"]:
            report.append(
                (
                    "fail",
                    f"{label}: artifact SHA-256 {actual_digest} does not match {artifact['sha256']}",
                )
            )
        return
    bytes_key = artifact["bytes_key"]
    if not isinstance(payload, dict) or payload.get(bytes_key) != len(data):
        payload_bytes = payload.get(bytes_key) if isinstance(payload, dict) else None
        message = (
            f"{label}: payload {bytes_key}={payload_bytes!r} does not equal "
            f"artifact size {len(data)}"
        )
        report.append(
            (
                "fail",
                message,
            )
        )
    actual_digest = hashlib.sha256(data).hexdigest()
    if actual_digest != artifact["sha256"]:
        report.append(
            (
                "fail",
                f"{label}: artifact SHA-256 {actual_digest} does not match "
                f"{artifact['sha256']}",
            )
        )
    try:
        parse_single_json(data.decode(artifact["encoding"]))
    except (UnicodeDecodeError, ValueError) as exc:
        report.append(("fail", f"{label}: artifact is not one JSON document ({exc})"))


def _validate_case_payload(
    path: str,
    case: dict[str, Any],
    payload: Any,
    contract: dict[str, Any],
    surface: str,
    label: str,
    report: list[tuple[str, str]],
) -> None:
    """Validate one successful JSON response, including path-specific invariants.

    The legacy exit compatibility switch changes only exit status.  Keeping all
    semantic checks here means the normal and legacy runs are deliberately held
    to the same payload contract rather than merely the same JSON shape.
    """
    payload_schema = contract["payloads"][case["payload"]]
    for error in validate_payload(payload, payload_schema, label):
        report.append(("fail", error))
    if not isinstance(payload, dict):
        return

    case_id = case["id"]
    if path == "version" and payload.get("cli") != surface:
        report.append(
            (
                "fail",
                f"{label}.cli: expected {surface!r}, got {payload.get('cli')!r}",
            )
        )
    if path == "chroma":
        if payload.get("n_chroma") != 12:
            report.append(
                (
                    "fail",
                    f"{label}.n_chroma: expected 12, got {payload.get('n_chroma')!r}",
                )
            )
        if isinstance(payload.get("n_frames"), int) and payload["n_frames"] < 0:
            report.append(("fail", f"{label}.n_frames: expected non-negative integer"))
    if path == "analyze":
        sections = payload.get("sections")
        if isinstance(sections, list):
            for index, section in enumerate(sections):
                if not isinstance(section, dict):
                    continue
                section_type = section.get("type")
                if (
                    not isinstance(section_type, str)
                    or _SECTION_TYPE_RE.fullmatch(section_type) is None
                ):
                    report.append(
                        (
                            "fail",
                            f"{label}.sections[{index}].type: expected lowercase-kebab section type, "
                            f"got {section_type!r}",
                        )
                    )
        if case_id == "with_seventh":
            chords = payload.get("chords")
            has_seventh = isinstance(chords, list) and any(
                isinstance(chord, dict)
                and isinstance(chord.get("name"), str)
                and chord["name"].endswith("7")
                for chord in chords
            )
            if not has_seventh:
                report.append(
                    (
                        "fail",
                        f"{label}.chords: --with-seventh fixture must contain a seventh chord",
                    )
                )
    if path == "mastering" and case_id == "target_within_ceiling":
        # The flag says the true-peak ceiling, not the target, decided the level.
        # Reading it as a value rather than as a type is what separates a surface
        # that computes it from one that publishes a constant. A target this far
        # below the ceiling is reached outright, so both surfaces must say so.
        limited = payload.get("loudness_target_limited")
        if limited is not False:
            report.append(
                (
                    "fail",
                    f"{label}.loudness_target_limited: expected False, got {limited!r}",
                )
            )
    if path == "mastering" and case_id == "ceiling_limits_target":
        # The opposite direction of target_within_ceiling: the fixture is a -6
        # dBFS tone, so a -6 LUFS target under a -3 dBTP ceiling cannot be
        # reached and both surfaces must say so. The cross-surface comparison
        # protects what applied_gain_db means -- the pre-limiter static gain,
        # not the bare ceiling headroom, which sits about 0.86 dB away on this
        # fixture. Omitting --report also makes this the one mastering case
        # entering through the standalone loudness helper rather than the chain.
        limited = payload.get("loudness_target_limited")
        if limited is not True:
            report.append(
                (
                    "fail",
                    f"{label}.loudness_target_limited: expected True, got {limited!r}",
                )
            )
        output_lufs = payload.get("output_lufs")
        if not _is_number(output_lufs) or float(output_lufs) > -6.5:
            report.append(
                (
                    "fail",
                    f"{label}.output_lufs: ceiling-limited run must stop short of "
                    f"its target, got {output_lufs!r}",
                )
            )
    if path == "spectral":
        n_frames = payload.get("n_frames")
        if not isinstance(n_frames, int) or n_frames <= 0:
            report.append(
                (
                    "fail",
                    f"{label}.n_frames: expected a positive integer, got {n_frames!r}",
                )
            )
        features = payload.get("features")
        if isinstance(features, dict):
            missing = sorted(_SPECTRAL_FEATURES - set(features))
            extra = sorted(set(features) - _SPECTRAL_FEATURES)
            if missing or extra:
                details = []
                if missing:
                    details.append(f"missing {', '.join(missing)}")
                if extra:
                    details.append(f"unexpected {', '.join(extra)}")
                report.append(
                    (
                        "fail",
                        f"{label}.features: canonical feature keys differ ({'; '.join(details)})",
                    )
                )
            for name in sorted(_SPECTRAL_FEATURES & set(features)):
                stats = features[name]
                if not isinstance(stats, dict):
                    continue
                missing_stats = sorted(_SPECTRAL_STATS - set(stats))
                extra_stats = sorted(set(stats) - _SPECTRAL_STATS)
                if missing_stats or extra_stats:
                    details = []
                    if missing_stats:
                        details.append(f"missing {', '.join(missing_stats)}")
                    if extra_stats:
                        details.append(f"unexpected {', '.join(extra_stats)}")
                    report.append(
                        (
                            "fail",
                            f"{label}.features.{name}: canonical statistic keys differ "
                            f"({'; '.join(details)})",
                        )
                    )
    if path == "voice-change":
        latency = payload.get("latency_samples")
        if case_id == "simple" and latency != 0:
            report.append(
                (
                    "fail",
                    f"{label}.latency_samples: simple-knob mode must report 0, got {latency!r}",
                )
            )
        if case_id == "preset" and (
            not isinstance(payload.get("preset"), str) or not payload["preset"]
        ):
            report.append(
                (
                    "fail",
                    f"{label}.preset: explicit preset mode must report its preset id",
                )
            )
        if case_id == "custom":
            if "preset" in payload:
                report.append(
                    (
                        "fail",
                        f"{label}: --preset-json without --preset must omit preset",
                    )
                )
            if not _is_int(latency) or latency <= 0:
                report.append(
                    (
                        "fail",
                        f"{label}.latency_samples: custom preset must report actual positive latency, got {latency!r}",
                    )
                )
    if path == "project.compile":
        diagnostics = payload.get("diagnostics")
        diagnostic_count = payload.get("diagnostic_count")
        if isinstance(diagnostics, list) and diagnostic_count != len(diagnostics):
            report.append(
                ("fail", f"{label}: diagnostic_count does not equal diagnostics length")
            )
        if case_id == "clean" and (
            not _is_bool(payload.get("has_timeline")) or diagnostic_count != 0
        ):
            report.append(
                (
                    "fail",
                    f"{label}: clean project must report a boolean timeline flag and no diagnostics",
                )
            )
    if path == "synthesize-rir":
        if not _is_int(payload.get("sample_rate")) or payload.get("sample_rate") <= 0:
            report.append(
                (
                    "fail",
                    f"{label}.sample_rate: expected a positive integer",
                )
            )
        if not _is_int(payload.get("samples")) or payload.get("samples") <= 0:
            report.append(("fail", f"{label}.samples: expected a positive integer"))
    if path == "rhythm":
        time_signature = payload.get("time_signature")
        if isinstance(time_signature, dict):
            for key in ("numerator", "denominator"):
                if not _is_int(time_signature.get(key)) or time_signature[key] <= 0:
                    report.append(
                        (
                            "fail",
                            f"{label}.time_signature.{key}: expected a positive integer",
                        )
                    )
            if not _is_number(time_signature.get("confidence")):
                report.append(
                    (
                        "fail",
                        f"{label}.time_signature.confidence: expected a finite number",
                    )
                )
        intervals = payload.get("beat_intervals")
        if isinstance(intervals, dict) and (
            not _is_int(intervals.get("count")) or intervals["count"] < 0
        ):
            report.append(
                (
                    "fail",
                    f"{label}.beat_intervals.count: expected a non-negative integer",
                )
            )
    if path == "pitch":
        n_frames = payload.get("n_frames")
        voiced_count = payload.get("voiced_count")
        if _is_int(n_frames) and _is_int(voiced_count) and voiced_count > n_frames:
            report.append(
                (
                    "fail",
                    f"{label}.voiced_count: cannot exceed n_frames",
                )
            )
        ratio = payload.get("voiced_ratio")
        if ratio is not None and (not _is_number(ratio) or not 0.0 <= ratio <= 1.0):
            report.append(
                (
                    "fail",
                    f"{label}.voiced_ratio: expected null or a number in [0, 1]",
                )
            )
    if path == "eq":
        if payload.get("processor") != "eq.equalizer":
            report.append(
                (
                    "fail",
                    f"{label}.processor: expected 'eq.equalizer', got {payload.get('processor')!r}",
                )
            )
        if case_id == "default" and payload.get("output") != "":
            report.append(
                (
                    "fail",
                    f"{label}.output: omitted output must serialize as an empty string",
                )
            )
        if case_id == "explicit_output" and not isinstance(payload.get("output"), str):
            report.append(
                ("fail", f"{label}.output: explicit output must serialize as a path")
            )
    if path == "mastering-processor":
        expected_stereo = case_id == "stereo"
        if payload.get("stereo") is not expected_stereo:
            report.append(
                (
                    "fail",
                    f"{label}.stereo: expected {expected_stereo!r}, got {payload.get('stereo')!r}",
                )
            )
        expected_processor = "stereo.imager" if expected_stereo else "eq.equalizer"
        if payload.get("processor") != expected_processor:
            report.append(
                (
                    "fail",
                    f"{label}.processor: expected {expected_processor!r}, got {payload.get('processor')!r}",
                )
            )
    if path == "project.validate":
        diagnostics = payload.get("diagnostics")
        diagnostic_count = payload.get("diagnostic_count")
        if isinstance(diagnostics, list) and diagnostic_count != len(diagnostics):
            report.append(
                ("fail", f"{label}: diagnostic_count does not equal diagnostics length")
            )
        if (
            case_id in {"clean", "warning", "warning_strict_artifact"}
            and payload.get("valid") is not True
        ):
            report.append(
                (
                    "fail",
                    f"{label}.valid: expected true for a successfully loaded project",
                )
            )
        if case_id == "clean" and diagnostic_count != 0:
            report.append(
                ("fail", f"{label}: clean project must not report diagnostics")
            )
        if case_id in {"warning", "warning_strict_artifact"} and (
            not isinstance(diagnostic_count, int) or diagnostic_count <= 0
        ):
            report.append(
                (
                    "fail",
                    f"{label}: warning fixture must report at least one diagnostic",
                )
            )
    if path == "voice-preset-validate":
        # The declared payload shape, not the case id, is what says whether the
        # invocation is meant to validate: several cases produce each shape.
        expected_ok = case["payload"] == "success"
        if payload.get("ok") is not expected_ok:
            report.append(
                (
                    "fail",
                    f"{label}.ok: expected {expected_ok!r}, got {payload.get('ok')!r}",
                )
            )
        if expected_ok:
            try:
                normalized = parse_single_json(payload["normalized_json"])
            except (KeyError, TypeError, ValueError) as exc:
                report.append(("fail", f"{label}.normalized_json: {exc}"))
                return
            if not isinstance(normalized, dict):
                report.append(("fail", f"{label}.normalized_json: expected an object"))
                return
            required = {"schemaVersion", "id", "name", "category"}
            missing = sorted(required - set(normalized))
            if missing:
                report.append(
                    (
                        "fail",
                        f"{label}.normalized_json: missing preset fields {', '.join(missing)}",
                    )
                )
            if ("dsp" in normalized) == ("macros" in normalized):
                report.append(
                    (
                        "fail",
                        f"{label}.normalized_json: expected exactly one of dsp or macros",
                    )
                )


def _validate_analyze_case_relationships(
    surface: str,
    payloads: dict[tuple[str, str], Any],
    manifest: dict[str, Any],
    report: list[tuple[str, str]],
) -> None:
    """Check fixture-level semantics that cannot be expressed by field types."""

    default = payloads.get(("analyze", "default"))
    explicit = payloads.get(("analyze", "chroma_highpass_explicit"))
    if default is not None and explicit is not None:
        mismatch = _compare_values(
            default,
            explicit,
            f"{surface}.analyze.default-vs-explicit-highpass",
            float(manifest["comparison"]["absolute"]),
            float(manifest["comparison"]["relative"]),
        )
        if mismatch:
            report.append(
                (
                    "fail",
                    mismatch + " (--chroma-highpass default must be canonical 80 Hz)",
                )
            )

    no_hpss = payloads.get(("analyze", "no_hpss"))
    if default is not None and no_hpss is not None:
        mismatch = _compare_values(
            default,
            no_hpss,
            f"{surface}.analyze.default-vs-no-hpss",
            float(manifest["comparison"]["absolute"]),
            float(manifest["comparison"]["relative"]),
        )
        if mismatch is None:
            report.append(
                (
                    "fail",
                    f"{surface}.analyze.no_hpss: --no-hpss did not change the canonical fixture result",
                )
            )


def _validate_voice_case_relationships(
    payloads: dict[tuple[str, str], Any],
    manifest: dict[str, Any],
    surface: str,
    report: list[tuple[str, str]],
) -> None:
    """Pin the live voice fixtures, including config-dependent latency."""

    expected_length = manifest["fixtures"]["audio"]["frames"]
    expected_rate = manifest["fixtures"]["audio"]["sample_rate"]
    for case_id in ("simple", "preset", "custom"):
        payload = payloads.get(("voice-change", case_id))
        if not isinstance(payload, dict):
            continue
        if payload.get("length") != expected_length:
            report.append(
                (
                    "fail",
                    f"{surface}.voice-change.{case_id}.length: expected {expected_length}, "
                    f"got {payload.get('length')!r}",
                )
            )
        if payload.get("sample_rate") != expected_rate:
            report.append(
                (
                    "fail",
                    f"{surface}.voice-change.{case_id}.sample_rate: expected {expected_rate}, "
                    f"got {payload.get('sample_rate')!r}",
                )
            )
    simple = payloads.get(("voice-change", "simple"))
    if isinstance(simple, dict):
        for key, expected in (("pitch_semitones", 5.0), ("formant_factor", 1.1)):
            mismatch = _compare_values(
                simple.get(key),
                expected,
                f"{surface}.voice-change.simple.{key}",
                float(manifest["comparison"]["absolute"]),
                float(manifest["comparison"]["relative"]),
            )
            if mismatch:
                report.append(("fail", mismatch))
    preset = payloads.get(("voice-change", "preset"))
    if isinstance(preset, dict):
        if preset.get("preset") != "bright-idol":
            report.append(
                (
                    "fail",
                    f"{surface}.voice-change.preset.preset: expected 'bright-idol', "
                    f"got {preset.get('preset')!r}",
                )
            )
        if preset.get("latency_samples") != 1042:
            report.append(
                (
                    "fail",
                    f"{surface}.voice-change.preset.latency_samples: expected 1042, "
                    f"got {preset.get('latency_samples')!r}",
                )
            )
    custom = payloads.get(("voice-change", "custom"))
    if isinstance(custom, dict) and custom.get("latency_samples") != 82:
        report.append(
            (
                "fail",
                f"{surface}.voice-change.custom.latency_samples: expected actual custom latency 82, "
                f"got {custom.get('latency_samples')!r}",
            )
        )


def _run_active_cases(
    surface: str,
    executable: str,
    manifest: dict[str, Any],
    paths: dict[str, str],
    timeout: float,
    report: list[tuple[str, str]],
) -> dict[tuple[str, str], Any]:
    payloads: dict[tuple[str, str], Any] = {}
    for contract in manifest["active_paths"]:
        path = contract["path"]
        for case in contract["cases"]:
            label = f"{surface}.{path}.{case['id']}"
            normal_paths = dict(paths)
            if case["artifact"] != "none":
                artifact = contract["artifacts"][case["artifact"]]
                normal_paths[artifact["placeholder"]] = str(
                    Path(paths[artifact["placeholder"]]).with_name(
                        f"{surface}_{Path(paths[artifact['placeholder']]).name}"
                    )
                )
                Path(normal_paths[artifact["placeholder"]]).unlink(missing_ok=True)
            result = _run(
                surface,
                executable,
                _resolve_argv(case["argv"], normal_paths),
                False,
                timeout,
            )
            if result["returncode"] != case["exit"]:
                detail = result.get("error") or f"got exit {result['returncode']}"
                report.append(
                    ("fail", f"{label}: expected exit {case['exit']}, {detail}")
                )
            expected_stderr = case.get("stderr")
            if expected_stderr is not None and (
                (expected_stderr == "empty") != (not result["stderr"].strip())
            ):
                report.append(
                    (
                        "fail",
                        f"{label}: expected {expected_stderr} stderr, "
                        f"got {result['stderr'][:160]!r}",
                    )
                )
            _check_text_expectations(case, result, label, report)
            parsed: Any = None
            if case["stdout"] in {"empty", "text"}:
                if case["stdout"] == "empty" and result["stdout"].strip():
                    report.append(
                        (
                            "fail",
                            f"{label}: expected empty stdout, got {result['stdout'][:160]!r}",
                        )
                    )
            else:
                try:
                    parsed = parse_single_json(result["stdout"])
                except ValueError as exc:
                    report.append(("fail", f"{label}: {exc}"))
                if parsed is not None and case["payload"] != "none":
                    _validate_case_payload(
                        path, case, parsed, contract, surface, label, report
                    )
                    payloads[(path, case["id"])] = parsed
                    _check_artifact(
                        case["artifact"], contract, parsed, normal_paths, label, report
                    )
            legacy_paths = dict(paths)
            if case["artifact"] != "none":
                artifact = contract["artifacts"][case["artifact"]]
                legacy_paths[artifact["placeholder"]] = str(
                    Path(paths[artifact["placeholder"]]).with_name(
                        f"{surface}_legacy_{Path(paths[artifact['placeholder']]).name}"
                    )
                )
                Path(legacy_paths[artifact["placeholder"]]).unlink(missing_ok=True)
            legacy_result = _run(
                surface,
                executable,
                _resolve_argv(case["argv"], legacy_paths),
                True,
                timeout,
            )
            if legacy_result["returncode"] != case["legacy_exit"]:
                detail = (
                    legacy_result.get("error")
                    or f"got exit {legacy_result['returncode']}"
                )
                report.append(
                    (
                        "fail",
                        f"{label} [legacy]: expected exit {case['legacy_exit']}, {detail}",
                    )
                )
            if expected_stderr is not None and (
                (expected_stderr == "empty") != (not legacy_result["stderr"].strip())
            ):
                report.append(
                    (
                        "fail",
                        f"{label} [legacy]: expected {expected_stderr} stderr, "
                        f"got {legacy_result['stderr'][:160]!r}",
                    )
                )
            _check_text_expectations(case, legacy_result, f"{label} [legacy]", report)
            if case["stdout"] in {"empty", "text"}:
                if case["stdout"] == "empty" and legacy_result["stdout"].strip():
                    message = (
                        f"{label} [legacy]: expected empty stdout, "
                        f"got {legacy_result['stdout'][:160]!r}"
                    )
                    report.append(
                        (
                            "fail",
                            message,
                        )
                    )
            else:
                try:
                    legacy_payload = parse_single_json(legacy_result["stdout"])
                    if case["payload"] != "none":
                        _validate_case_payload(
                            path,
                            case,
                            legacy_payload,
                            contract,
                            surface,
                            f"{label} [legacy]",
                            report,
                        )
                        if parsed is not None:
                            normal_for_compare = parsed
                            legacy_for_compare = legacy_payload
                            if case["artifact"] != "none":
                                normal_for_compare = dict(parsed)
                                legacy_for_compare = dict(legacy_payload)
                                normal_for_compare.pop("output", None)
                                legacy_for_compare.pop("output", None)
                            mismatch = _compare_values(
                                normal_for_compare,
                                legacy_for_compare,
                                f"{label} [legacy]",
                                float(manifest["comparison"]["absolute"]),
                                float(manifest["comparison"]["relative"]),
                            )
                            if mismatch:
                                report.append(
                                    (
                                        "fail",
                                        mismatch
                                        + " (legacy payload must match normal mode)",
                                    )
                                )
                        if case["artifact"] != "none":
                            _check_artifact(
                                case["artifact"],
                                contract,
                                legacy_payload,
                                legacy_paths,
                                f"{label} [legacy]",
                                report,
                            )
                except ValueError as exc:
                    report.append(("fail", f"{label} [legacy]: {exc}"))
    if any(contract["path"] == "analyze" for contract in manifest["active_paths"]):
        _validate_analyze_case_relationships(surface, payloads, manifest, report)
    if any(contract["path"] == "voice-change" for contract in manifest["active_paths"]):
        _validate_voice_case_relationships(payloads, manifest, surface, report)
    return payloads


def _check_text_expectations(
    case: dict[str, Any],
    result: dict[str, Any],
    label: str,
    report: list[tuple[str, str]],
) -> None:
    """Check the human-readable expectations a case declares.

    ``stdout: "text"`` marks a case whose output is prose rather than JSON, so
    it is asserted by content instead of being parsed.  ``stdout_contains`` and
    ``stderr_contains`` hold substrings that must appear, which is how a case
    pins a diagnostic that must be shown or a message that must name the
    option and the value it rejected.  ``stdout_excludes`` holds substrings that
    must NOT appear: a command whose text output claims success while it returns
    a failing status cannot be caught by any positive assertion, because the
    diagnostic it should print and the success line it should not print can both
    be present at once.
    """
    if case["stdout"] == "text" and not result["stdout"].strip():
        report.append(("fail", f"{label}: expected text stdout, got nothing"))
    for stream in ("stdout", "stderr"):
        expected = case.get(f"{stream}_contains")
        if not expected:
            continue
        actual = result[stream]
        for needle in expected:
            if needle not in actual:
                report.append(
                    (
                        "fail",
                        f"{label}: expected {stream} to contain {needle!r}, "
                        f"got {actual[:160]!r}",
                    )
                )
    for needle in case.get("stdout_excludes") or []:
        if needle in result["stdout"]:
            report.append(
                (
                    "fail",
                    f"{label}: expected stdout not to contain {needle!r}, "
                    f"got {result['stdout'][:160]!r}",
                )
            )


def _run_parser_cases(
    surface: str,
    executable: str,
    manifest: dict[str, Any],
    paths: dict[str, str],
    timeout: float,
    report: list[tuple[str, str]],
) -> None:
    for case in manifest["parser_cases"]:
        label = f"{surface}.parser.{case['id']}"
        argv = _resolve_argv(case["argv"], paths)
        expected_exit = case["exit"]
        result = _run(surface, executable, argv, False, timeout)
        if result["returncode"] != expected_exit:
            detail = result.get("error") or f"got exit {result['returncode']}"
            report.append(("fail", f"{label}: expected exit {expected_exit}, {detail}"))
        if case["stdout"] == "empty" and result["stdout"].strip():
            report.append(
                (
                    "fail",
                    f"{label}: expected empty stdout, got {result['stdout'][:160]!r}",
                )
            )
        _check_text_expectations(case, result, label, report)
        legacy = _run(surface, executable, argv, True, timeout)
        if legacy["returncode"] != case["legacy_exit"]:
            detail = legacy.get("error") or f"got exit {legacy['returncode']}"
            report.append(
                (
                    "fail",
                    f"{label} [legacy]: expected exit {case['legacy_exit']}, {detail}",
                )
            )
        if case["stdout"] == "empty" and legacy["stdout"].strip():
            report.append(
                (
                    "fail",
                    f"{label} [legacy]: expected empty stdout, got {legacy['stdout'][:160]!r}",
                )
            )
        _check_text_expectations(case, legacy, f"{label} [legacy]", report)


def _print_listing(manifest: dict[str, Any]) -> None:
    print(f"{manifest['contract']} schema v{manifest['schema_version']}: OK")
    commands = manifest["commands"]
    for classification in (
        "shared",
        "native_only",
        "python_only",
        "intentional_variant",
    ):
        print(f"{classification}:")
        for path, record in commands.items():
            if record["classification"] == classification:
                print(f"  {path} [{record['status']}]")
    print("active semantic paths:")
    for contract in manifest["active_paths"]:
        print(f"  {contract['path']}: {len(contract['cases'])} cases")
    print("generic parser cases:")
    for case in manifest["parser_cases"]:
        print(f"  {case['id']}: exit {case['exit']} (legacy {case['legacy_exit']})")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--native", help="native sonare-cli executable")
    parser.add_argument("--python", help="Python interpreter or sonare console script")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--schema", action="store_true", help="validate the manifest only"
    )
    parser.add_argument(
        "--list", action="store_true", help="validate and list manifest paths"
    )
    parser.add_argument(
        "--emit-shared-option-snapshot",
        action="store_true",
        help="maintenance-only: emit canonical options for every shared path",
    )
    parser.add_argument(
        "--no-inventory", action="store_true", help="skip --dump-cli-contract checks"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if args.schema and args.list:
        print("--schema and --list are mutually exclusive", file=sys.stderr)
        return 2
    if args.emit_shared_option_snapshot and (
        args.schema or args.list or args.no_inventory
    ):
        print(
            "--emit-shared-option-snapshot cannot be combined with "
            "--schema, --list, or --no-inventory",
            file=sys.stderr,
        )
        return 2
    try:
        manifest = load_manifest(args.manifest)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    if args.schema:
        print(f"{manifest['contract']} schema v{manifest['schema_version']}: OK")
        return 0
    if args.list:
        _print_listing(manifest)
        return 0
    if args.emit_shared_option_snapshot:
        if not args.native or not args.python:
            print(
                "--emit-shared-option-snapshot requires both --native and --python",
                file=sys.stderr,
            )
            return 2
        if args.timeout <= 0 or not math.isfinite(args.timeout):
            print("--timeout must be a positive finite number", file=sys.stderr)
            return 2

        native_executable = _resolve_executable(args.native)
        python_executable = _resolve_executable(args.python)
        native_inventory, native_error = _read_inventory_dump(
            "native", native_executable, args.timeout
        )
        python_inventory, python_error = _read_inventory_dump(
            "python", python_executable, args.timeout
        )
        errors = [error for error in (native_error, python_error) if error]
        if not errors:
            try:
                snapshot = _build_shared_option_snapshot(
                    native_inventory, python_inventory, manifest
                )
            except ValueError as exc:
                errors.append(str(exc))
        if errors:
            print("\n".join(errors), file=sys.stderr)
            return 1
        print(json.dumps(snapshot, indent=2, ensure_ascii=False))
        return 0
    if not args.native and not args.python:
        message = (
            f"{manifest['contract']} schema v{manifest['schema_version']}: OK "
            "(schema-only; pass --native and --python for live checks)"
        )
        print(message)
        return 0
    if not args.native or not args.python:
        print("live checks require both --native and --python", file=sys.stderr)
        return 2
    if args.timeout <= 0 or not math.isfinite(args.timeout):
        print("--timeout must be a positive finite number", file=sys.stderr)
        return 2

    native_executable = _resolve_executable(args.native)
    python_executable = _resolve_executable(args.python)

    report: list[tuple[str, str]] = []
    _check_artifact_skew(native_executable, python_executable, args.timeout, report)
    with tempfile.TemporaryDirectory(prefix="libsonare-cli-contract-") as temporary:
        paths = _write_fixtures(Path(temporary), manifest)
        native_inventory: dict[str, dict[str, Any]] | None = None
        python_inventory: dict[str, dict[str, Any]] | None = None
        if not args.no_inventory:
            native_inventory = _inventory_checks(
                "native", native_executable, manifest, args.timeout, report
            )
            python_inventory = _inventory_checks(
                "python", python_executable, manifest, args.timeout, report
            )
            if native_inventory is not None and python_inventory is not None:
                _compare_active_inventory_options(
                    native_inventory, python_inventory, manifest, report
                )
        native_payloads = _run_active_cases(
            "native", native_executable, manifest, paths, args.timeout, report
        )
        python_payloads = _run_active_cases(
            "python", python_executable, manifest, paths, args.timeout, report
        )
        for contract in manifest["active_paths"]:
            path = contract["path"]
            for case in contract["cases"]:
                key = (path, case["id"])
                if key not in native_payloads or key not in python_payloads:
                    continue
                mismatch = _compare_payloads(
                    path,
                    native_payloads[key],
                    python_payloads[key],
                    manifest,
                    case["id"],
                )
                if mismatch:
                    report.append(("fail", f"surface comparison: {mismatch}"))
                if (
                    path == "project.validate"
                    and case["id"] == "warning_strict_artifact"
                ):
                    warning_key = (path, "warning")
                    if warning_key in native_payloads:
                        mismatch = _compare_values(
                            native_payloads[key],
                            native_payloads[warning_key],
                            f"native.{path}.strict",
                            float(manifest["comparison"]["absolute"]),
                            float(manifest["comparison"]["relative"]),
                        )
                        if mismatch:
                            report.append(
                                (
                                    "fail",
                                    mismatch + " (strict must preserve the payload)",
                                )
                            )
                    if warning_key in python_payloads:
                        mismatch = _compare_values(
                            python_payloads[key],
                            python_payloads[warning_key],
                            f"python.{path}.strict",
                            float(manifest["comparison"]["absolute"]),
                            float(manifest["comparison"]["relative"]),
                        )
                        if mismatch:
                            report.append(
                                (
                                    "fail",
                                    mismatch + " (strict must preserve the payload)",
                                )
                            )
        _run_parser_cases(
            "native", native_executable, manifest, paths, args.timeout, report
        )
        _run_parser_cases(
            "python", python_executable, manifest, paths, args.timeout, report
        )

    if report:
        for kind, message in report:
            prefix = "EXPECTED-FAILURE" if kind == "expected" else "FAIL"
            print(f"[{prefix}] {message}")
        print(f"cli contract v2: {len(report)} issue(s)")
        return 1
    print("cli contract v2: live checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
