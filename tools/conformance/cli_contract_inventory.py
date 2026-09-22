"""Reconciling a built CLI's self-reported option inventory against the manifest.

Owns the ``--dump-cli-contract`` schema check, the classified-path
reconciliation, and the maintenance snapshot of canonical shared options.
"""

from __future__ import annotations

from typing import Any

from cli_contract_schema import (
    _exact,
    _normalized_option_inventory,
    _strings,
    _validate_option,
)


def _expected_paths(
    commands: dict[str, Any],
    surface: str,
    disabled_features: frozenset[str] = frozenset(),
) -> set[str]:
    """Every command path @p surface must carry, for the build in front of us.

    ``disabled_features`` names the build options this binary was compiled
    without. A command that declares one of them in ``requires`` is not demanded
    here: the gate either drops it from the registry or leaves it answering
    NOT_SUPPORTED, and the first shape is indistinguishable from a command that
    was never implemented. It stays demanded of a binary that has the feature,
    so an empty set -- a default full build, which is what CI runs -- asks
    exactly what it asked before.

    Not demanded is not the same as forbidden: the stub shape leaves the command
    listed, so @ref _tolerated_paths carries the other half and the two must be
    read together.
    """
    return {
        path
        for path, record in commands.items()
        if record["classification"] in {"shared", "intentional_variant", f"{surface}_only"}
        and not (disabled_features & set(record.get("requires") or ()))
    }


def _tolerated_paths(
    commands: dict[str, Any],
    surface: str,
    disabled_features: frozenset[str] = frozenset(),
) -> set[str]:
    """Paths a gated-off build may still list without being unclassified.

    The two gate shapes differ in exactly this: a dropped command is absent and
    a stubbed one is present, answering NOT_SUPPORTED. Which shape a given gate
    takes is an implementation detail that has already changed once, so the
    manifest records only the dependency and both shapes are accepted.
    """
    if not disabled_features:
        return set()
    return {
        path
        for path, record in commands.items()
        if record["classification"] in {"shared", "intentional_variant", f"{surface}_only"}
        and (disabled_features & set(record.get("requires") or ()))
    }


def _accepted_names(commands: dict[str, dict[str, Any]]) -> set[str]:
    """Every command name a surface answers to, canonical spelling or alias.

    A deprecated spelling is one registry row's alias rather than a row of its
    own, so it never appears as a ``path``.  Presence is what the classified-path
    check is about, and a name the CLI accepts is present however it resolves;
    comparing paths alone would demand a duplicate row per alias, which is the
    shape that lets two spellings of one command drift apart.
    """

    names = set(commands)
    for command in commands.values():
        aliases = command.get("aliases")
        if isinstance(aliases, list):
            names.update(alias for alias in aliases if isinstance(alias, str))
    return names


def _validate_inventory(value: Any, surface: str) -> tuple[list[str], dict[str, dict[str, Any]]]:
    errors: list[str] = []
    if not _exact(value, {"schema_version", "surface", "commands"}, f"inventory.{surface}", errors):
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
                        errors.append(f"{option_label}: duplicate option {option['name']}")
                    names.add(option["name"])
        commands[path] = command
    return errors, commands


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
    for path in sorted(expected - _accepted_names(commands)):
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
        validation_errors, commands = _snapshot_inventory_validation(value, surface, manifest)
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
            errors.append(f"inventory.native.{path}: malformed option metadata cannot be compared")
        if python_options is None:
            errors.append(f"inventory.python.{path}: malformed option metadata cannot be compared")
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
        raise ValueError("shared option snapshot validation failed:\n" + "\n".join(errors))
    return snapshot


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
                    (
                        f"inventory.shared.{path}: native/Python active option schemas differ\n"
                        f"  native: {left!r}\n"
                        f"  python: {right!r}"
                    ),
                )
            )
