"""Schema validation of the cross-surface CLI JSON manifest file.

Owns the check that the manifest itself is well formed, section by section.
The vocabulary it validates against -- the declared key sets, the option and
domain records, and the payload-schema checks -- lives in
``cli_contract_schema``.
"""

from __future__ import annotations

import json
from typing import Any

from cli_contract_schema import (
    _CLASSIFICATIONS,
    _EXIT_CODES,
    _POSITIONAL_TYPES,
    _STATUSES,
    _TOP_LEVEL_KEYS,
    _argv,
    _declared_text_keys,
    _exact,
    _is_bool,
    _is_int,
    _is_number,
    _normalized_option_inventory,
    _strings,
    _validate_canonical_payload_schema,
    _validate_closed_payload_schema,
    _validate_command_reason,
    _validate_command_requires,
    _validate_melody,
    _validate_option,
    _validate_payload_property_exemptions,
    _validate_payload_property_names,
    _validate_payload_schema,
    _validate_text_expectations,
)


def _validate_exit_codes(exit_codes: Any, errors: list[str]) -> None:
    """The five named exit codes and the values the contract fixes them at."""
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


def _validate_comparison(comparison: Any, errors: list[str]) -> None:
    """The absolute and relative tolerances two surfaces are compared under."""
    if _exact(comparison, {"absolute", "relative"}, "manifest.comparison", errors):
        for key in comparison:
            if not _is_number(comparison[key]) or comparison[key] <= 0:
                errors.append(
                    f"manifest.comparison.{key}: expected a positive finite number"
                )


def _validate_declared_inventory(
    manifest: Any, inventory: Any, errors: list[str]
) -> None:
    """The declared command and option field lists, and the canonical shared options.

    Reads the command records straight off the manifest because the expected
    options are checked against the paths whose options are active, and that is
    the only place that says which those are.
    """
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


def _validate_commands(commands: Any, errors: list[str]) -> tuple[set[str], set[str]]:
    """Every command record. Returns the declared paths and the active subset."""
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
            base_keys = {"classification", "status", "option_status"}
            # A command both front-ends carry needs no excuse, so it may not
            # write one; anything else states why in the ledger rather than
            # leaving the divergence to be re-derived by the next reader.
            one_sided = (
                not isinstance(record, dict) or record.get("classification") != "shared"
            )
            expected_keys = (
                base_keys | {"reason_kind", "reason"} if one_sided else base_keys
            )
            # Optional: only a command the native binary can be built without
            # carries one, and most cannot.
            if isinstance(record, dict) and "requires" in record:
                expected_keys = expected_keys | {"requires"}
            if not _exact(record, expected_keys, label, errors):
                continue
            _validate_command_requires(record, label, errors)
            classification = record["classification"]
            status = record["status"]
            option_status = record["option_status"]
            if one_sided:
                _validate_command_reason(record, classification, label, errors)
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
    return command_paths, active_from_commands


def _validate_active_paths(
    active: Any, exempt_properties: frozenset[str], errors: list[str]
) -> tuple[set[str], set[str]]:
    """Each active path's options, positionals, payloads, cases and artifacts.

    Returns the paths it accepted and the payload-property exemptions that
    suppressed something, which the caller needs to retire the rest.
    """
    active_paths: set[str] = set()
    suppressed_exemptions: set[str] = set()
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
    return active_paths, suppressed_exemptions


def _validate_expected_options(
    inventory: Any, commands: Any, active: Any, errors: list[str]
) -> None:
    """The canonical shared options against the active paths that must carry them."""
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


def _validate_parser_cases(parser_cases: Any, errors: list[str]) -> None:
    """The parser-level cases and the ids they are addressed by."""
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


def _validate_fixtures(fixtures: Any, errors: list[str]) -> None:
    """The audio, melody, project and preset fixtures the cases name."""
    if _exact(
        fixtures,
        {"audio", "melodies", "projects", "presets"},
        "manifest.fixtures",
        errors,
    ):
        audio = fixtures["audio"]
        if _exact(
            audio,
            {"sample_rate", "frames", "frequency_hz", "amplitude", "take_frames"},
            "manifest.fixtures.audio",
            errors,
        ):
            if not _is_int(audio["sample_rate"]) or audio["sample_rate"] <= 0:
                errors.append(
                    "manifest.fixtures.audio.sample_rate: expected a positive integer"
                )
            for key in ("frames", "take_frames"):
                if not _is_int(audio[key]) or audio[key] <= 0:
                    errors.append(
                        f"manifest.fixtures.audio.{key}: expected a positive integer"
                    )
            # The two lengths carry the same tone and must differ: an alignment
            # reports a frame count per side, so equal lengths would let the two
            # front-ends swap reference for take and still compare equal.
            if (
                _is_int(audio["frames"])
                and _is_int(audio["take_frames"])
                and audio["frames"] == audio["take_frames"]
            ):
                errors.append(
                    "manifest.fixtures.audio.take_frames: expected a length other than frames"
                )
            for key in ("frequency_hz", "amplitude"):
                if not _is_number(audio[key]):
                    errors.append(
                        f"manifest.fixtures.audio.{key}: expected a finite number"
                    )
        melodies = fixtures["melodies"]
        if not isinstance(melodies, dict) or not melodies:
            errors.append("manifest.fixtures.melodies: expected a non-empty object")
        else:
            for name, melody in melodies.items():
                _validate_melody(melody, f"manifest.fixtures.melodies.{name}", errors)
        projects = fixtures["projects"]
        if _exact(
            projects,
            {"clean", "warning", "malformed", "takes", "takes_many", "takes_single"},
            "manifest.fixtures.projects",
            errors,
        ):
            for key in ("clean", "warning", "malformed", "takes", "takes_many", "takes_single"):
                if not isinstance(projects[key], str):
                    errors.append(
                        f"manifest.fixtures.projects.{key}: expected a string"
                    )
            for key in ("clean", "warning", "takes", "takes_many", "takes_single"):
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


def validate_manifest(manifest: Any) -> list[str]:
    """Return schema errors for a decoded manifest (empty means valid)."""
    errors: list[str] = []
    if not _exact(manifest, _TOP_LEVEL_KEYS, "manifest", errors):
        return errors
    exempt_properties = _validate_payload_property_exemptions(manifest, errors)
    if manifest["schema_version"] != 2:
        errors.append("manifest.schema_version: expected 2")
    if manifest["contract"] != "cli-json-v2":
        errors.append("manifest.contract: expected cli-json-v2")
    if manifest["surfaces"] != ["native", "python"]:
        errors.append("manifest.surfaces: expected [native, python]")

    _validate_exit_codes(manifest["exit_codes"], errors)
    _validate_comparison(manifest["comparison"], errors)

    inventory = manifest["inventory"]
    _validate_declared_inventory(manifest, inventory, errors)
    commands = manifest["commands"]
    command_paths, active_from_commands = _validate_commands(commands, errors)
    active = manifest["active_paths"]
    active_paths, suppressed_exemptions = _validate_active_paths(
        active, exempt_properties, errors
    )
    _validate_expected_options(inventory, commands, active, errors)

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
    _validate_parser_cases(manifest["parser_cases"], errors)
    _validate_fixtures(manifest["fixtures"], errors)

    if command_paths and len(command_paths) != len(commands):
        errors.append("manifest.commands: duplicate command path")
    return errors
