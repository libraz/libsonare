"""Manifest schema vocabulary for the cross-surface CLI JSON contract.

Owns the "is the manifest file itself well-formed" concern: the declared key
sets, the option and domain records, and the validators behind
``validate_manifest``.
"""

from __future__ import annotations

import json
import math
import re
from typing import Any

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
    "payload_property_exemptions",
}
_CLASSIFICATIONS = {"shared", "native_only", "python_only", "intentional_variant"}
_STATUSES = {"active", "pending", "native_only", "python_only", "intentional_variant"}
_OPTION_TYPES = {"boolean", "integer", "number", "string", "path"}
_POSITIONAL_TYPES = {"boolean", "integer", "number", "string", "path"}
_EXIT_CODES = {0, 2, 3, 4, 5, 9}
_ARRAY_TYPES = {"array:string", "array:number:12"}
_PAYLOAD_TYPES = _OPTION_TYPES | _ARRAY_TYPES | {"null", "any"}
_SCHEMA_METADATA_KEYS = {"optional", "required"}
_PAYLOAD_PROPERTY_NAME_RE = re.compile(r"^[a-z0-9]+(_[a-z0-9]+)*$")
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
        {"name", "type", "default", "aliases", "repeatable", "required", "domain"},
        label,
        errors,
    ):
        return
    if not _domain_is_well_formed(option["domain"]):
        errors.append(f"{label}.domain: expected null or a domain record")
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
        if (
            isinstance(required, list)
            and key not in required
            and (not isinstance(optional, list) or key not in optional)
        ):
            errors.append(f"{label}: property {key!r} is neither required nor optional")

    for key in ("additional_properties", "additionalProperties"):
        if key in value and not _is_bool(value[key]):
            errors.append(f"{label}.{key}: expected a boolean")
    if (
        "additional_properties" in value
        and "additionalProperties" in value
        and value["additional_properties"] != value["additionalProperties"]
    ):
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


def _validate_payload_property_names(
    value: Any,
    path: str,
    label: str,
    errors: list[str],
    exempt: frozenset[str] = frozenset(),
    pointer: str | None = None,
    suppressed: set[str] | None = None,
) -> None:
    """Require snake_case property names throughout one payload schema.

    The CLI's JSON keys are a user-visible contract shared by the native and
    Python surfaces, so a camelCase or PascalCase field is drift even when both
    surfaces emit it.  Each offender is reported as ``path:property`` so a
    rename can be traced back to the command that owns the key.

    ``exempt`` holds property pointers (``doctor.features``, or a bare command
    name for a whole payload) whose field names come from a schema shared with
    the other bindings rather than from the CLI.  The exemption covers the
    subtree below the pointer only, so a CLI-owned key beside a transported
    document is still held to the rule.  Pointers that actually suppress a
    rejection are recorded in ``suppressed`` so a stale entry can be failed.
    """

    if pointer is None:
        pointer = path
    if isinstance(value, list):
        for index, item in enumerate(value):
            _validate_payload_property_names(
                item, path, f"{label}[{index}]", errors, exempt, pointer, suppressed
            )
        return
    if not isinstance(value, dict):
        return
    inside_document = pointer in exempt
    for source in ("keys", "properties", "required", "optional"):
        fields = value.get(source)
        if not isinstance(fields, dict):
            continue
        for key, child in fields.items():
            child_pointer = f"{pointer}.{key}" if isinstance(key, str) else pointer
            if isinstance(key, str) and not _PAYLOAD_PROPERTY_NAME_RE.match(key):
                if inside_document or child_pointer in exempt:
                    if suppressed is not None:
                        suppressed.add(pointer if inside_document else child_pointer)
                else:
                    errors.append(
                        f"{path}:{key}: payload property name is not snake_case "
                        f"(at {label}.{source})"
                    )
            _validate_payload_property_names(
                child,
                path,
                f"{label}.{source}.{key}",
                errors,
                exempt,
                child_pointer,
                suppressed,
            )
    if "items" in value:
        # An array's items are the same property subtree as the array itself.
        _validate_payload_property_names(
            value["items"], path, f"{label}.items", errors, exempt, pointer, suppressed
        )
    for key in ("one_of", "any_of", "oneOf", "anyOf", "variants"):
        if isinstance(value.get(key), list):
            _validate_payload_property_names(
                value[key], path, f"{label}.{key}", errors, exempt, pointer, suppressed
            )
    if isinstance(value.get("type"), list):
        _validate_payload_property_names(
            value["type"], path, f"{label}.type", errors, exempt, pointer, suppressed
        )


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


def _validate_payload_property_exemptions(manifest: Any, errors: list[str]) -> frozenset[str]:
    """Validate the snake_case exemption registry and return its pointer set.

    An exemption is a named category rather than a per-command escape, so a
    command that transports a shared document inherits a decision that was
    already argued instead of an omission nobody notices.  The reason is
    mandatory for the same purpose: the next reader has to be able to see why
    the CLI does not own these names without reconstructing the argument.
    """

    registry = manifest.get("payload_property_exemptions")
    if not isinstance(registry, dict) or not registry:
        errors.append(
            "manifest.payload_property_exemptions: expected a non-empty object"
        )
        return frozenset()
    commands = manifest.get("commands")
    pointers: set[str] = set()
    for name, entry in registry.items():
        label = f"manifest.payload_property_exemptions.{name}"
        if not isinstance(name, str) or not _PAYLOAD_PROPERTY_NAME_RE.match(name):
            errors.append(f"{label}: category name must be snake_case")
        if not _exact(entry, {"reason", "properties"}, label, errors):
            continue
        reason = entry["reason"]
        if not isinstance(reason, str) or not reason.strip():
            errors.append(f"{label}.reason: expected a non-empty string")
        properties = entry["properties"]
        if not _strings(properties, f"{label}.properties", errors):
            continue
        if not properties:
            errors.append(f"{label}.properties: expected a non-empty array")
        for pointer in properties:
            command = pointer.split(".", 1)[0]
            # A dotted command name (project.bounce) owns the first two segments.
            if isinstance(commands, dict) and command not in commands:
                command = ".".join(pointer.split(".", 2)[:2])
            if isinstance(commands, dict) and command not in commands:
                errors.append(f"{label}.properties: unknown command in {pointer!r}")
            if pointer in pointers:
                errors.append(f"{label}.properties: duplicate pointer {pointer!r}")
            pointers.add(pointer)
    return frozenset(pointers)


def validate_manifest(manifest: Any) -> list[str]:
    """Return schema errors for a decoded manifest (empty means valid)."""

    errors: list[str] = []
    if not _exact(manifest, _TOP_LEVEL_KEYS, "manifest", errors):
        return errors
    exempt_properties = _validate_payload_property_exemptions(manifest, errors)
    suppressed_exemptions: set[str] = set()
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
            "domain",
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
                    _validate_payload_property_names(
                        payload,
                        path if isinstance(path, str) else label,
                        f"{label}.payloads.{payload_name}",
                        errors,
                        exempt_properties,
                        None,
                        suppressed_exemptions,
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
    # An exemption that suppresses nothing is not inert: it keeps asserting a
    # reviewed decision about a name, so the next property to take that name
    # inherits the blessing unexamined.  It expires with the divergence.
    for pointer in sorted(exempt_properties - suppressed_exemptions):
        errors.append(
            f"manifest.payload_property_exemptions: {pointer!r} suppressed nothing; "
            "delete the entry with the divergence"
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


_OPTION_RECORD_FIELDS = {
    "name",
    "type",
    "default",
    "aliases",
    "repeatable",
    "required",
    "domain",
}

# One option's accepted value set, published by both surfaces so the comparison
# below can see a domain that only one of them declares.  ``rejectExit`` names
# the exit-code class the refusal carries: ``usage`` (2) is a parse-time
# rejection and ``invalid_parameter`` (3) is a handler that refuses after
# parsing.  Both CLIs report some domains each way, so a shared command has to
# agree on the class as well as on the values.
_DOMAIN_RECORD_FIELDS = {
    "choices",
    "minimum",
    "exclusiveMinimum",
    "maximum",
    "exclusiveMaximum",
    "rejectExit",
}
_DOMAIN_REJECT_EXITS = {"usage", "invalid_parameter"}


def _domain_is_well_formed(domain: Any) -> bool:
    """Return whether one domain value is safe to compare."""
    if domain is None:
        return True
    return (
        isinstance(domain, dict)
        and set(domain) == _DOMAIN_RECORD_FIELDS
        and isinstance(domain["choices"], list)
        and all(isinstance(choice, str) for choice in domain["choices"])
        and (domain["minimum"] is None or isinstance(domain["minimum"], (int, float)))
        and (domain["maximum"] is None or isinstance(domain["maximum"], (int, float)))
        and isinstance(domain["exclusiveMinimum"], bool)
        and isinstance(domain["exclusiveMaximum"], bool)
        and domain["rejectExit"] in _DOMAIN_REJECT_EXITS
    )


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
        and _domain_is_well_formed(option.get("domain"))
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
            "domain": option["domain"],
        }
        for option in sorted(options, key=lambda item: item["name"])
    ]
