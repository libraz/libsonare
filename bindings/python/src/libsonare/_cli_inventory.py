"""Argparse-backed inventory for the Python command-line contract."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Iterator, Sequence
from typing import Any, TypeVar

from ._cli_common import _find_cli_token, _strict_json_dumps

# The argument type _cli_domain hands straight back, so an inline wrap of an
# add_argument() result stays an argparse.Action instead of decaying to Any.
_TargetT = TypeVar("_TargetT")

# ``argparse`` has no path type.  These names are the command contract's
# semantic path values, and are intentionally kept independent of a parser
# action's ``type`` callable (which is often left as ``None`` for strings).
_PATH_OPTION_NAMES = frozenset(
    {
        "chain-config",
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
        "reference-smf",
        "report",
        "scene",
        "smf",
        "write-takes",
    }
)


# The published shape of one option's accepted value set. It has to match the
# native CLI's record field for field, because the cross-surface checker
# compares them directly: that comparison is what turns "one surface narrowed an
# option and the other did not" from an invisible drift into a failure.
_DOMAIN_ATTRIBUTE = "sonare_cli_domain"

# Options a command cannot run without, keyed by command path. One table rather
# than argparse's ``required=True`` because a parent parser shares one action
# across commands (``-o`` is required on a render, optional on ``trim-silence``),
# and because the refusal is worded as the native CLI words it. The inventory
# publishes these as required and the parser refuses a missing one before any
# handler reads its input.
_REQUIRED_OPTIONS: dict[str, tuple[str, ...]] = {
    **{
        path: ("output",)
        for path in (
            "decompose-stems",
            "hpss",
            "midi-render",
            "normalize",
            "note-move",
            "note-stretch",
            "pitch-correct",
            "pitch-correct-timevarying",
            "playback",
            "polyphonic-render",
            "project.bounce",
            "project.export-midi2",
            "project.export-smf",
            "project.import-midi2",
            "project.import-smf",
            "project.new",
            "room-morph",
            "synthesize-rir",
            "transcribe",
            "voice-change",
        )
    },
    "mastering-pair-analyze": ("analysis", "reference"),
    "mastering-pair-processor": ("processor", "reference"),
    "mastering-processor": ("processor",),
    "mastering-stereo-analyze": ("analysis", "reference"),
    "pitch-shift": ("pitch-semitones", "output"),
    "project.align-takes": ("output", "reference-source"),
    "resample": ("target-sample-rate", "output"),
    "time-stretch": ("rate", "output"),
    "tune-to-midi": ("reference-smf", "output"),
}


def _domain_record(
    *,
    choices: tuple[object, ...] | list[object] | None = None,
    minimum: float | None = None,
    exclusive_minimum: bool = False,
    maximum: float | None = None,
    exclusive_maximum: bool = False,
) -> dict[str, Any]:
    """Build one published domain record."""
    return {
        "choices": [str(choice) for choice in (choices or ())],
        "minimum": minimum,
        "exclusive_minimum": exclusive_minimum,
        "maximum": maximum,
        "exclusive_maximum": exclusive_maximum,
    }


def _cli_domain(target: _TargetT, **domain: Any) -> _TargetT:
    """Record the domain a type callable or a parser action enforces.

    Returns @p target so a caller can wrap an ``add_argument`` result inline.
    The return type mirrors the argument so an inline wrap keeps the action or
    callable type it started with rather than widening to ``Any``. A lower bound
    alone on an integer reader with a ceiling ends at that ceiling.
    """
    reader = target.type if isinstance(target, argparse.Action) else target
    ceiling = getattr(reader, _INTEGER_CEILING_ATTRIBUTE, None)
    if (
        ceiling is not None
        and domain.get("minimum") is not None
        and domain.get("maximum") is None
        and not domain.get("choices")
    ):
        domain = {**domain, "maximum": ceiling}
    setattr(target, _DOMAIN_ATTRIBUTE, _domain_record(**domain))
    choices = domain.get("choices")
    # A declared token set is read in any case, the same as an argparse ``choices=``.
    if (
        isinstance(target, argparse.Action)
        and target.type is None
        and choices
        and all(isinstance(choice, str) for choice in choices)
    ):
        target.type = _token_of(tuple(choices))
    return target


# The contract type a ``type=`` callable produces. argparse only exposes the
# callable, so a strict converter standing in for a builtin has to say which
# scalar it yields or the inventory falls back to guessing from the default --
# and an option defaulting to None has no default to guess from.
_SCALAR_TYPE_ATTRIBUTE = "sonare_cli_scalar_type"


# The largest value an integer ``type=`` callable reads: a C ``int`` reader's INT_MAX.
_INTEGER_CEILING_ATTRIBUTE = "sonare_cli_integer_ceiling"


def _cli_integer_ceiling(target: _TargetT, ceiling: int) -> _TargetT:
    """Record the largest value an integer type callable reads, returning @p target."""
    setattr(target, _INTEGER_CEILING_ATTRIBUTE, ceiling)
    return target


def _cli_scalar_type(target: _TargetT, scalar_type: str) -> _TargetT:
    """Record the contract type a type callable produces, returning @p target."""
    setattr(target, _SCALAR_TYPE_ATTRIBUTE, scalar_type)
    return target


def _token_of(tokens: Sequence[str] | Callable[[], Sequence[str]]) -> Callable[[str], str]:
    """``type=`` for a closed token set: any letter case in, the set's own spelling out.

    A value naming no token passes through unchanged, so the refusal stays with
    what already owns it -- argparse's ``choices=``, the handler or the library.
    A callable is a library catalog, read only when the flag carries a value.
    """

    def parse(value: str) -> str:
        if not value:
            return value
        found = _find_cli_token(value, tokens() if callable(tokens) else tokens)
        return value if found is None else found

    return _cli_scalar_type(parse, "string")


def _action_domain(action: argparse.Action) -> dict[str, Any] | None:
    """Return the published domain for one action, or None when unnarrowed.

    A domain declared on the action itself wins; otherwise an argparse
    ``choices=`` tuple or a domain recorded on the ``type=`` callable is published.
    """
    declared = getattr(action, _DOMAIN_ATTRIBUTE, None)
    if declared is not None:
        return dict(declared)
    if action.choices is not None and not isinstance(action.choices, dict):
        return _domain_record(choices=list(action.choices))
    type_domain = getattr(action.type, _DOMAIN_ATTRIBUTE, None)
    return dict(type_domain) if type_domain is not None else None


def _canonical_option_string(action: argparse.Action) -> str:
    """The spelling an option is published and refused under: its first long form."""
    long_options = [option for option in action.option_strings if option.startswith("--")]
    return long_options[0] if long_options else action.option_strings[0]


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
    declared = getattr(action.type, _SCALAR_TYPE_ATTRIBUTE, None)
    if isinstance(declared, str):
        return declared
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


def _stated_number(value: float) -> str:
    """A number as both CLIs state it in ``--help``: an integral value keeps one decimal place."""
    number = float(value)
    return f"{number:.1f}" if number.is_integer() else format(number, "g")


def _rendered_default(action: argparse.Action) -> str | None:
    """The parser default as ``--help`` states it, or None where there is none to state.

    The rule is the native help renderer's, ``cli_option_stated_default``: a flag, a
    required or repeatable option and an empty string state nothing.
    """
    if not action.option_strings or action.required or _is_repeatable(action):
        return None
    default = action.default
    if default is None or default is argparse.SUPPRESS or isinstance(default, bool):
        return None
    if default == "":
        return None
    option_type = _option_type(action, _canonical_option_string(action).lstrip("-"))
    if option_type == "boolean":
        return None
    if option_type == "integer":
        return str(int(default))
    if option_type == "number":
        return _stated_number(default)
    return str(default)


_STATED_PREFIX = "(default: "


def _stated_default(action: argparse.Action) -> str | None:
    """The default an option's help states, exactly as ``--help`` prints it."""
    text = action.help
    if not isinstance(text, str) or text == argparse.SUPPRESS:
        return None
    start = text.rfind(_STATED_PREFIX)
    if start < 0:
        return None
    begin = start + len(_STATED_PREFIX)
    depth = 1
    for index in range(begin, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return " ".join(text[begin:index].split())
    return None


def _inventory_option(
    action: argparse.Action, required_options: tuple[str, ...] = ()
) -> dict[str, Any] | None:
    """Convert one argparse option action to a schema-v2 record.

    ``required`` is the command's own requirement (``required_options``, from
    @ref _REQUIRED_OPTIONS) or argparse's unconditional one.  Required options
    deliberately report ``null`` regardless of argparse's internal default;
    repeatable options report ``[]`` so their accumulation shape is explicit.
    """
    if (
        isinstance(action, argparse._HelpAction)
        or action.help is argparse.SUPPRESS
        or not action.option_strings
    ):
        return None

    canonical_option = _canonical_option_string(action)
    canonical = canonical_option.lstrip("-")
    aliases = [option.lstrip("-") for option in action.option_strings if option != canonical_option]
    required = bool(action.required) or canonical in required_options
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
        "stated_default": _stated_default(action),
        "aliases": aliases,
        "repeatable": repeatable,
        "required": required,
        "domain": _action_domain(action),
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
    required_options = _REQUIRED_OPTIONS.get(path, ())
    options = [
        option
        for action in parser._actions
        if (option := _inventory_option(action, required_options)) is not None
    ]
    return {"path": path, "aliases": [], "options": options}


def build_cli_contract(
    parser: argparse.ArgumentParser, *, surface: str = "python"
) -> dict[str, Any]:
    """Build the hidden inventory from the supplied argparse tree."""
    return {
        "schema_version": 4,
        "surface": surface,
        "commands": [
            _command_record(path, command_parser)
            for path, command_parser in _leaf_command_parsers(parser)
        ],
    }


def dump_cli_contract(parser: argparse.ArgumentParser, *, surface: str = "python") -> None:
    """Serialize one parser tree as the hidden CLI inventory contract."""
    print(_strict_json_dumps(build_cli_contract(parser, surface=surface)))
