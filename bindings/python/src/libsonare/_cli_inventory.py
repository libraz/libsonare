"""Argparse-backed inventory for the Python command-line contract."""

from __future__ import annotations

import argparse
from collections.abc import Iterator
from typing import Any

from ._cli_common import _strict_json_dumps

# ``argparse`` has no path type.  These names are the command contract's
# semantic path values, and are intentionally kept independent of a parser
# action's ``type`` callable (which is often left as ``None`` for strings).
_PATH_OPTION_NAMES = frozenset(
    {
        "config",
        "config-file",
        "in",
        "input",
        "midi2",
        "output",
        "preset-json",
        "preset-pack",
        "platforms-file",
        "reference",
        "report",
        "scene",
        "smf",
    }
)


def _inventory_subparsers(parser: argparse.ArgumentParser) -> dict[str, argparse.ArgumentParser]:
    """Return named subparser choices from one argparse parser."""
    for action in parser._actions:
        if isinstance(action, argparse._SubParsersAction):
            return {
                name: child
                for name, child in action.choices.items()
                if isinstance(child, argparse.ArgumentParser)
            }
    return {}


def _option_type(action: argparse.Action, canonical: str) -> str:
    """Map one argparse action to the CLI contract's semantic type."""
    if isinstance(action, (argparse._StoreTrueAction, argparse._StoreFalseAction)):
        return "boolean"
    if canonical in _PATH_OPTION_NAMES:
        return "path"
    if action.type is int:
        return "integer"
    if action.type is float:
        return "number"
    if isinstance(action.default, bool):
        return "boolean"
    if isinstance(action.default, int) and not isinstance(action.default, bool):
        return "integer"
    if isinstance(action.default, float):
        return "number"
    return "string"


def _is_repeatable(action: argparse.Action) -> bool:
    """Return whether argparse accumulates repeated occurrences."""
    append_action = getattr(argparse, "_AppendAction", None)
    extend_action = getattr(argparse, "_ExtendAction", None)
    action_types = tuple(
        action_type for action_type in (append_action, extend_action) if action_type is not None
    )
    return bool(action_types) and isinstance(action, action_types)


def _inventory_option(action: argparse.Action) -> dict[str, Any] | None:
    """Convert one argparse option action to a schema-v2 record.

    ``required`` describes argparse's unconditional parser-level requirement,
    not whether a handler later rejects a missing value.  Required options
    deliberately report ``null`` regardless of argparse's internal default;
    repeatable options report ``[]`` so their accumulation shape is explicit.
    """
    if (
        isinstance(action, argparse._HelpAction)
        or action.help is argparse.SUPPRESS
        or not action.option_strings
    ):
        return None

    long_options = [option for option in action.option_strings if option.startswith("--")]
    canonical_option = long_options[0] if long_options else action.option_strings[0]
    canonical = canonical_option.lstrip("-")
    aliases = [option.lstrip("-") for option in action.option_strings if option != canonical_option]
    required = bool(action.required)
    repeatable = _is_repeatable(action)
    option_type = _option_type(action, canonical)

    if required:
        default: object = None
    elif repeatable:
        default = []
    elif action.default is argparse.SUPPRESS:
        # Suppressed child defaults still have a stable contract shape.  The
        # project parser uses this only to avoid overwriting a parent flag;
        # expose the option's semantic default while retaining that runtime
        # merge behavior.  Other suppressed values have no default and use
        # JSON null.
        default = False if option_type == "boolean" else None
    else:
        default = action.default

    return {
        "name": canonical,
        "type": option_type,
        "default": default,
        "aliases": aliases,
        "repeatable": repeatable,
        "required": required,
    }


def _leaf_command_parsers(
    parser: argparse.ArgumentParser, prefix: str = ""
) -> Iterator[tuple[str, argparse.ArgumentParser]]:
    """Yield leaf command parsers in argparse registration order."""
    choices = _inventory_subparsers(parser)
    if not choices:
        if prefix:
            yield prefix, parser
        return
    for name, child in choices.items():
        path = f"{prefix}.{name}" if prefix else name
        yield from _leaf_command_parsers(child, path)


def _command_record(path: str, parser: argparse.ArgumentParser) -> dict[str, Any]:
    options = [
        option for action in parser._actions if (option := _inventory_option(action)) is not None
    ]
    return {"path": path, "aliases": [], "options": options}


def build_cli_contract(
    parser: argparse.ArgumentParser, *, surface: str = "python"
) -> dict[str, Any]:
    """Build the hidden inventory from the supplied argparse tree."""
    return {
        "schema_version": 2,
        "surface": surface,
        "commands": [
            _command_record(path, command_parser)
            for path, command_parser in _leaf_command_parsers(parser)
        ],
    }


def dump_cli_contract(parser: argparse.ArgumentParser, *, surface: str = "python") -> None:
    """Serialize one parser tree as the hidden CLI inventory contract."""
    print(_strict_json_dumps(build_cli_contract(parser, surface=surface)))
