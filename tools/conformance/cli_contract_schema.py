"""Manifest schema vocabulary for the cross-surface CLI JSON contract.

Owns the vocabulary the manifest is read against: the declared key sets, the
option and domain records, and the per-record validators. The section-by-section
check that consumes them is ``cli_contract_manifest.validate_manifest``.
"""

from __future__ import annotations

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
# Why a command is not on both front-ends. ``by_design`` is a decision the
# project stands behind; ``unported`` is a gap nobody has closed yet and
# expires with the port, the way an allowlist entry expires with its drift.
_REASON_KINDS = {"by_design", "unported"}
_MIN_REASON_LENGTH = 24
# Build options that decide whether the native binary answers a command at all.
# Spelled as the capability descriptor spells them (``doctor --json`` ->
# ``features``), because that descriptor is what the checker asks a binary about
# itself -- a name here that the descriptor does not carry can never be
# answered, so the two sets are one vocabulary rather than two.
#
# The gate produces one of two shapes and a command record does not say which:
# the command is dropped from the registry (mastering, mixing, the assistant,
# the acoustic simulator, arrangement) or it stays listed and its handler
# answers NOT_SUPPORTED (the pitch editor, the voice changer). Which shape a
# gate takes has already changed once, so the manifest records the dependency
# and the checker accepts either shape.
_FEATURE_NAMES = {
    "mastering",
    "mixing",
    "mixingAssistant",
    "fx",
    "arrangement",
    "acousticSim",
    "pitchEditor",
    "voiceChanger",
}
# The two exits a gated-off command may answer with, one per gate shape:
# ErrorCode::NotImplemented from a stub handler, and the parser's own unknown
# -command code when the registry never listed it.
NOT_SUPPORTED_EXIT = 8
UNKNOWN_COMMAND_EXIT = 2
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


def _validate_melody(melody: Any, label: str, errors: list[str]) -> None:
    """Check one reference-melody fixture: the tick grid, the tempo, the notes.

    The bounds are the MIDI encoding's own, so a value the writer would have to
    truncate into a byte is refused here instead of reaching a file as some other
    note.
    """
    if not _exact(melody, {"ppq", "tempo_bpm", "notes"}, label, errors):
        return
    if not _is_int(melody["ppq"]) or not 1 <= melody["ppq"] <= 0x7FFF:
        errors.append(f"{label}.ppq: expected ticks per quarter note in 1..32767")
    if not _is_number(melody["tempo_bpm"]) or melody["tempo_bpm"] <= 0:
        errors.append(f"{label}.tempo_bpm: expected a positive finite number")
    notes = melody["notes"]
    if not isinstance(notes, list) or not notes:
        errors.append(f"{label}.notes: expected a non-empty array")
        return
    for index, note in enumerate(notes):
        note_label = f"{label}.notes[{index}]"
        if not _exact(
            note, {"midi", "velocity", "start_ticks", "length_ticks"}, note_label, errors
        ):
            continue
        for key, low, high in (
            ("midi", 0, 127),
            # Velocity 0 is a note-off in the encoding, so it cannot open a note.
            ("velocity", 1, 127),
            ("start_ticks", 0, None),
            ("length_ticks", 1, None),
        ):
            value = note[key]
            if not _is_int(value) or value < low or (high is not None and value > high):
                bound = f"{low}..{high}" if high is not None else f"at least {low}"
                errors.append(f"{note_label}.{key}: expected an integer {bound}")


def _validate_command_requires(record: Any, label: str, errors: list[str]) -> None:
    """Check the build options a command needs the native binary to carry.

    Absent means unconditional, which is most commands. Present means the
    native front-end drops or stubs the command when any named feature is off,
    so the checker stops demanding it there. The Python front-end builds its
    parser unconditionally and is never relaxed by this.
    """
    requires = record.get("requires") if isinstance(record, dict) else None
    if requires is None:
        return
    if not isinstance(requires, list) or not requires:
        errors.append(f"{label}.requires: expected a non-empty array of feature names")
        return
    if requires != sorted(set(requires)):
        errors.append(
            f"{label}.requires: expected unique feature names in sorted order, got {requires!r}"
        )
    for name in requires:
        if name not in _FEATURE_NAMES:
            errors.append(f"{label}.requires: unknown build feature {name!r}")


def _validate_command_reason(
    record: dict[str, Any], classification: Any, label: str, errors: list[str]
) -> None:
    """Check the ``reason_kind`` / ``reason`` pair a one-sided command carries."""
    kind = record["reason_kind"]
    reason = record["reason"]
    if kind not in _REASON_KINDS:
        errors.append(f"{label}.reason_kind: unknown reason kind {kind!r}")
    elif kind == "unported" and classification == "intentional_variant":
        errors.append(
            f"{label}.reason_kind: an intentional variant is a decision, not an unclosed gap"
        )
    if not isinstance(reason, str) or len(reason.strip()) < _MIN_REASON_LENGTH:
        errors.append(
            f"{label}.reason: expected a sentence of at least "
            f"{_MIN_REASON_LENGTH} characters saying why"
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
            errors.append(f"{label}.default: repeatable option needs an empty array default")
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
        errors.append(f"{label}.default: string/path option needs a string or null default")


def _validate_schema_alternatives(value: Any, label: str, errors: list[str]) -> None:
    if not isinstance(value, list) or not value:
        errors.append(f"{label}: expected a non-empty array of schemas")
        return
    for index, schema in enumerate(value):
        _validate_payload_schema(schema, f"{label}[{index}]", errors)


def _validate_object_schema(value: dict[str, Any], label: str, errors: list[str]) -> None:
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
                errors.append(f"{label}.{source}: field names must be non-empty strings")
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
                errors.append(f"{label}.required: field names must be non-empty strings")
            elif key not in field_schemas:
                errors.append(f"{label}.required: unknown property {key!r}")
    if isinstance(optional, list):
        for key in optional:
            if not isinstance(key, str) or not key:
                errors.append(f"{label}.optional: field names must be non-empty strings")
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
        errors.append(f"{label}: active payload schemas must reject additional properties")
    if isinstance(value.get("keys"), dict):
        for key, child in value["keys"].items():
            _validate_closed_payload_schema(child, f"{label}.keys.{key}", errors)
    if isinstance(value.get("properties"), dict):
        for key, child in value["properties"].items():
            _validate_closed_payload_schema(child, f"{label}.properties.{key}", errors)
    for key in ("required", "optional"):
        if isinstance(value.get(key), dict):
            for child_name, child in value[key].items():
                _validate_closed_payload_schema(child, f"{label}.{key}.{child_name}", errors)
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


def _require_array_item_schema(schema: Any, label: str, errors: list[str]) -> Any | None:
    if not isinstance(schema, dict) or schema.get("type") != "array" or "items" not in schema:
        errors.append(f"{label}: expected an array schema with item schema")
        return None
    return schema["items"]


def _require_schema_shape(schema: Any, expected: Any, label: str, errors: list[str]) -> None:
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
        _require_schema_shape(schema.get("items"), expected.get("items"), f"{label}.items", errors)
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
        top = _require_schema_keys(success, _ANALYZE_TOP_LEVEL_KEYS, f"{label}.success", errors)
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
            _require_schema_keys(top.get(name), expected, f"{label}.success.{name}", errors)
        arrays = {
            "beats": _ANALYZE_BEAT_KEYS,
            "chords": _ANALYZE_CHORD_KEYS,
            "sections": _ANALYZE_SECTION_KEYS,
        }
        for name, expected in arrays.items():
            item = _require_array_item_schema(top.get(name), f"{label}.success.{name}", errors)
            if item is not None:
                _require_schema_keys(item, expected, f"{label}.success.{name}.items", errors)
    elif path == "spectral":
        top = _require_schema_keys(success, {"n_frames", "features"}, f"{label}.success", errors)
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
        top = _require_schema_keys(success, _PROJECT_COMPILE_KEYS, f"{label}.success", errors)
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
        _require_schema_shape(success, _RHYTHM_PAYLOAD_SCHEMA, f"{label}.success", errors)
    elif path == "pitch":
        _require_schema_shape(success, _PITCH_PAYLOAD_SCHEMA, f"{label}.success", errors)
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
        errors.append("manifest.payload_property_exemptions: expected a non-empty object")
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
