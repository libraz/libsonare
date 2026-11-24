"""Shared argument grammar for the Python command-line interface.

The parser class installs three cross-surface behaviours every command
inherits without opting in: the usage exit contract, the record of which
options argv actually spelled, and the finite-float check. The scalar
converters below carry their accepted value set on the callable itself, so a
domain is declared next to the code enforcing it and the cross-surface checker
can read it rather than assume.
"""

from __future__ import annotations

import argparse
import dataclasses
import importlib
import inspect
import math
import re
import struct
import sys
from collections.abc import Callable, Iterable, Sequence
from typing import Any, NoReturn, cast

from ._cli_common import (
    EXIT_USAGE,
    _cli_mode_names,
    _numeric_params_refusal,
    _params_grammar_refusal,
    _parse_key_profile,
    _parse_meter_candidates,
    _parse_mode,
    _parse_modes,
    _parse_pitch_class,
)
from ._cli_inventory import (
    _REQUIRED_OPTIONS,
    _action_domain,
    _canonical_option_string,
    _cli_domain,
    _cli_integer_ceiling,
    _cli_names,
    _cli_scalar_type,
    _inventory_subparsers,
    _option_type,
    _rendered_default,
    _stated_default,
    _token_of,
)
from ._narrowing import _FLOAT32_MAX, _INT64_MAX, _INT64_MIN
from ._runtime import _C_INT_MAX, _C_INT_MIN

# The native CLI's numeric grammar: leading ASCII space, a sign, decimal digits
# with an optional point and exponent (or inf / infinity / nan), nothing after.
# Python's own int() and float() also read digit separators, trailing space and
# non-ASCII digits, which the native reader refuses.
_NUMBER_TEXT = re.compile(
    r"[ \t\n\v\f\r]*[+-]?(?:(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?|inf|infinity|nan)",
    re.ASCII | re.IGNORECASE,
)
_INTEGER_TEXT = re.compile(r"[ \t\n\v\f\r]*[+-]?\d+", re.ASCII)


class _NumericTextError(ValueError):
    """A numeric value refused by requirement alone ("must be a number"), never echoing the text."""


def _read_number_text(value: str, *, float32: bool = True) -> float:
    """Read a finite decimal, applying the float32 transport limit when requested."""
    if not _NUMBER_TEXT.fullmatch(value):
        raise _NumericTextError("must be a number")
    parsed = float(value)
    # The library's own bound for a float argument: finite, and no larger than FLT_MAX.
    if not math.isfinite(parsed) or (float32 and abs(parsed) > _FLOAT32_MAX):
        raise _NumericTextError("must be finite")
    return parsed


def _read_integer_text(value: str, minimum: int = _C_INT_MIN, maximum: int = _C_INT_MAX) -> int:
    """Read ``value`` as a decimal integer in ``[minimum, maximum]``."""
    if not _INTEGER_TEXT.fullmatch(value):
        raise _NumericTextError("must be an integer")
    parsed = int(value)
    if not minimum <= parsed <= maximum:
        raise _NumericTextError(f"must be an integer in [{minimum}, {maximum}]")
    return parsed


class _ContractArgumentParser(argparse.ArgumentParser):
    """Argument parser with the cross-surface usage exit contract.

    Parser errors exit with the usage status the native CLI reports. Keeping
    this behavior in the parser class also covers errors raised by nested
    subparsers and typed options before command dispatch begins.

    The class is also the one place two facts about argv are established: which
    options the caller actually spelled (``_supplied_options``, the counterpart
    of the native ``CliArgs::has``) and that every float-typed value is finite.
    Both are installed here rather than at the call sites, so a new option
    inherits them without opting in.
    """

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        # A unique prefix of a long option is not an accepted spelling. The
        # native CLI matches option names exactly, and an abbreviation silently
        # resolves to a different option once a longer one is added, so a script
        # that was never edited starts doing something else.
        kwargs.setdefault("allow_abbrev", False)
        super().__init__(*args, **kwargs)

    def add_argument(self, *args: Any, **kwargs: Any) -> Any:
        if kwargs.get("type") is float:
            kwargs["type"] = _finite_float
        choices = kwargs.get("choices")
        if (
            choices is not None
            and kwargs.get("type") is None
            and all(isinstance(choice, str) for choice in choices)
        ):
            kwargs["type"] = _token_of(tuple(choices))
        action = super().add_argument(*args, **kwargs)
        # The parser states its own default in --help, so the text cannot drift from the value.
        if action.default not in (None, argparse.SUPPRESS) and _stated_default(action) is not None:
            raise ValueError(
                f"{action.option_strings[-1]} states a default by hand beside its parser default"
            )
        rendered = _rendered_default(action)
        if rendered is not None:
            stated = f"(default: {rendered})"
            action.help = f"{action.help} {stated}" if action.help else stated
        return action

    def _get_value(self, action: argparse.Action, arg_string: str) -> Any:
        """Convert one value and hold it to the option's published domain.

        The domain record is the one the inventory publishes, so the value set the
        cross-surface checker compares is also the one enforced here, and every
        refusal is worded as the native option formatter words it, and refused
        through the parser as a usage failure.
        """
        option = _canonical_option_string(action) if action.option_strings else action.dest
        try:
            value = super()._get_value(action, arg_string)
        except argparse.ArgumentError as exc:
            # argparse names an aliased option by every spelling; the CLI names one.
            subject = option if action.option_strings else (action.metavar or action.dest)
            raise argparse.ArgumentError(None, f"argument {subject}: {exc.message}") from exc
        domain = _action_domain(action)
        if domain is None or not _outside_domain(domain, value):
            return value
        predicate = _domain_predicate(domain, _option_type(action, option.lstrip("-")))
        raise argparse.ArgumentError(None, f"argument {option}: {predicate}")

    def error(self, message: str) -> NoReturn:
        self.print_usage(sys.stderr)
        self._print_message(f"{self.prog}: error: {message}\n", sys.stderr)
        raise SystemExit(EXIT_USAGE)

    def parse_known_args(
        self,
        args: Iterable[str] | None = None,
        namespace: Any = None,
    ) -> tuple[Any, list[str]]:
        argv = list(sys.argv[1:] if args is None else args)
        parsed, extras = super().parse_known_args(self._expand_flag_values(argv), namespace)
        # Recorded from the ORIGINAL argv: a flag written as --flag=false was
        # still named by the caller, and presence is about what was spelled.
        self._record_supplied_options(parsed, argv)
        _restore_parser_compat_defaults(parsed)
        _reject_stdout_output(parsed, self)
        _reject_leaf_output(parsed, extras, self)
        # Run once, on the top-level parser, where a flag spelled before the
        # project subcommand has been merged into the namespace.
        path = _command_path(parsed) if " " not in self.prog else None
        if path is not None:
            for name in _REQUIRED_OPTIONS.get(path, ()):
                if getattr(parsed, name.replace("-", "_"), None) in (None, ""):
                    self.error(f"Missing required option '--{name}'")
            validator = _COMMAND_VALIDATORS.get(path)
            if validator is not None:
                validator(parsed, self)
        return parsed, extras

    def _expand_flag_values(self, argv: list[str]) -> list[str]:
        """Rewrite ``--flag=value`` into the bare flag, or drop it when false.

        The native parser accepts an inline value on every flag-arity option
        (``parse_option`` in tools/cli/sonare_cli_args.cpp), reading the same
        false literals ``is_false_flag_literal`` lists; argparse has no such
        form and rejected the spelling outright. Rewriting here rather than at
        the actions means every flag inherits it, including one added later, and
        the only ``=`` forms touched are those resolving to a store_true /
        store_false action -- a valued option keeps argparse's own handling.
        """
        if not any(token.startswith("--") and "=" in token for token in argv):
            return argv
        expanded: list[str] = []
        passthrough = False
        for token in argv:
            if passthrough or token == "--" or not token.startswith("--") or "=" not in token:
                expanded.append(token)
                passthrough = passthrough or token == "--"
                continue
            name, _, value = token.partition("=")
            action = self._action_for_token(name)
            if not isinstance(action, (argparse._StoreTrueAction, argparse._StoreFalseAction)):
                expanded.append(token)
                continue
            # An empty value is not a false literal on the native side either:
            # only the four spellings below turn a flag off.
            if value.strip().lower() in _FALSE_FLAG_LITERALS:
                continue
            expanded.append(name)
        return expanded

    def _record_supplied_options(self, args: Any, argv: list[str]) -> None:
        """Record the destination of every option present on ``argv``.

        A subparser parses its own slice into the same namespace, so the set
        accumulates across parser levels instead of being replaced.
        """
        supplied = getattr(args, "_supplied_options", None)
        if not isinstance(supplied, set):
            supplied = set()
            args._supplied_options = supplied
        for token in argv:
            if token == "--":
                break
            action = self._action_for_token(token)
            if action is not None:
                supplied.add(action.dest)

    def _action_for_token(self, token: str) -> Any:
        """Resolve one argv token to the option action argparse would choose."""
        if token in ("-", "--") or not token.startswith("-"):
            return None
        action = self._option_string_actions.get(token.split("=", 1)[0])
        if action is not None:
            return action
        if not token.startswith("--"):
            # A short option may carry its value attached, as in ``-o out.wav``.
            return self._option_string_actions.get(token[:2])
        if not self.allow_abbrev:
            return None
        name = token.split("=", 1)[0]
        matched = {
            candidate
            for option, candidate in self._option_string_actions.items()
            if option.startswith(name)
        }
        return next(iter(matched)) if len(matched) == 1 else None


# The values that turn a flag off when written inline, mirroring
# is_false_flag_literal in tools/cli/sonare_cli_args.cpp. Compared
# case-insensitively there, so lowered here before the lookup.
_FALSE_FLAG_LITERALS = frozenset({"false", "0", "no", "off"})
# Their complement, for the one place a boolean is written as a value rather than
# as a flag: ``polyphonic-render --edit N.muted=VALUE``. One spelling means the
# same thing wherever it is written.
_TRUE_FLAG_LITERALS = frozenset({"true", "1", "yes", "on"})


def _as_float32(value: float) -> float:
    return cast(float, struct.unpack("f", struct.pack("f", value))[0])


def _bare_exponent(text: str) -> str:
    """``bare_exponent`` in util/refusal_predicates.h: "3.4028235e+38" -> "3.4028235e38"."""
    mantissa, e, exponent = text.partition("e")
    if not e:
        return text
    sign = "-" if exponent.startswith("-") else ""
    return f"{mantissa}e{sign}{exponent.lstrip('+-').lstrip('0') or '0'}"


def _integer_bound_text(value: float) -> str:
    """``integer_bound_text`` in util/refusal_predicates.h: a whole bound in plain digits."""
    if math.isfinite(value) and float(value).is_integer() and abs(value) < 2.0**63:
        return str(int(value))
    return _bound_text(value)


def _bound_text(value: float) -> str:
    """``bound_text`` in util/refusal_predicates.h: the shortest decimal reading back as ``value``.

    A bound a 32-bit float holds exactly is written as that float's shortest decimal.
    """
    narrowed = _as_float32(value)
    if math.isfinite(value) and narrowed == value:
        for digits in range(6, 10):
            text = f"{value:.{digits}g}"
            if _as_float32(float(text)) == narrowed:
                return _bare_exponent(text)
    for digits in range(6, 17):
        text = f"{value:.{digits}g}"
        if float(text) == value:
            return _bare_exponent(text)
    return _bare_exponent(f"{value:.17g}")


def _domain_predicate(domain: dict[str, Any], scalar_type: str) -> str:
    """``domain_predicate`` in sonare_cli_option_validation.cpp: the library's predicate words.

    An integer domain's exclusive end is the next integer in, as the library states it.
    """
    if domain["choices"]:
        return "must be one of " + ", ".join(domain["choices"])
    minimum, maximum = domain["minimum"], domain["maximum"]
    if scalar_type == "integer" and minimum is not None:
        low = minimum + 1 if domain["exclusive_minimum"] else minimum
        if maximum is None:
            return f"must be an integer of at least {int(low)}"
        high = maximum - 1 if domain["exclusive_maximum"] else maximum
        return f"must be an integer in [{_integer_bound_text(low)}, {_integer_bound_text(high)}]"
    if minimum is not None and maximum is not None:
        opening = "(" if domain["exclusive_minimum"] else "["
        closing = ")" if domain["exclusive_maximum"] else "]"
        return (
            f"must be finite and in {opening}{_bound_text(minimum)}, "
            f"{_bound_text(maximum)}{closing}"
        )
    if minimum is not None:
        word = "greater than" if domain["exclusive_minimum"] else "at least"
        return f"must be finite and {word} {_bound_text(minimum)}"
    word = "less than" if domain["exclusive_maximum"] else "at most"
    return f"must be finite and {word} {_bound_text(maximum)}"


def _outside_domain(domain: dict[str, Any], value: object) -> bool:
    """Whether a converted value falls outside ``domain``, as the native check decides.

    A numeric value matches a choice by number; a bound reads a value that is not a
    number (``--tempo-bpm auto``) as no verdict.
    """
    numeric = isinstance(value, (int, float)) and not isinstance(value, bool)
    if domain["choices"]:
        if numeric:
            return not any(_number_or_none(choice) == value for choice in domain["choices"])
        return str(value) not in domain["choices"]
    number = value if numeric else _number_or_none(value)
    if number is None:
        return False
    minimum, maximum = domain["minimum"], domain["maximum"]
    below = minimum is not None and (
        number <= minimum if domain["exclusive_minimum"] else number < minimum
    )
    above = maximum is not None and (
        number >= maximum if domain["exclusive_maximum"] else number > maximum
    )
    return below or above


def _number_or_none(value: object) -> float | None:
    try:
        return float(str(value))
    except ValueError:
        return None


def _finite_float(value: str) -> float:
    try:
        return _read_number_text(value)
    except _NumericTextError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from None


# The domain-carrying converters below parse only: the accepted set is the record
# `_cli_domain` attaches to each, which the parser enforces for every option alike.


def _nonnegative_finite_float(value: str) -> float:
    return _finite_float(value)


def _positive_pitch_frequency(value: str) -> float:
    return _finite_float(value)


def _positive_finite_float(value: str) -> float:
    return _finite_float(value)


def _tempo_bpm(value: str) -> float:
    """A tempo flag's value on every command that takes one: finite and greater than 0."""
    return _finite_float(value)


def _given(args: argparse.Namespace, *same: str, **renamed: str) -> dict[str, Any]:
    """The facade keywords for the options the caller spelled; the library owns the rest.

    ``same`` names options whose destination is also the facade keyword;
    ``renamed`` maps a facade keyword to the option destination.
    """
    mapping = {name: name for name in same} | renamed
    return {
        keyword: getattr(args, dest)
        for keyword, dest in mapping.items()
        if getattr(args, dest, None) is not None
    }


def _effective(
    args: argparse.Namespace, facade: Callable[..., Any], name: str, dest: str | None = None
) -> Any:
    """The value ``facade`` runs with for keyword ``name``: the option given, else its own default.

    ``dest`` names the option destination when it differs from the keyword.
    """
    value = getattr(args, name if dest is None else dest, None)
    return inspect.signature(facade).parameters[name].default if value is None else value


def _library_catalog(name: str) -> Callable[[], Sequence[str]]:
    """The package's catalog function ``name``, imported only when it is read."""

    def read() -> Sequence[str]:
        package = importlib.import_module(__name__.rpartition(".")[0])
        return cast(Sequence[str], getattr(package, name)())

    return read


def _closed_unit_float(value: str) -> float:
    return _finite_float(value)


def _pitch_threshold(value: str) -> float:
    return _finite_float(value)


def _c_int(value: str) -> int:
    """Parse an integer option the native CLI reads as a C ``int``."""
    try:
        return _read_integer_text(value)
    except _NumericTextError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from None


def _int64(value: str) -> int:
    """Parse an integer option whose domain reaches past a C ``int``; the domain bounds it."""
    try:
        return _read_integer_text(value, _INT64_MIN, _INT64_MAX)
    except _NumericTextError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from None


def _positive_int(value: str) -> int:
    return _c_int(value)


def _candidate_count(value: str) -> int:
    """Parse ``--candidates``, accepting the native CLI's ``true`` shorthand.

    The native handler reads the raw string and maps ``true`` to the top five,
    so a script written against it reaches the Python CLI with that literal.
    """
    if value == "true":
        return 5
    return _c_int(value)


def _mode_mask(value: str) -> int:
    """Parse a scale mask given as an integer or as a mode name.

    A name is read through the CLI's mode table (aliases included) and resolves
    through ``scale_mask_for_mode``, the facade's own mask table over the C ABI.
    """
    try:
        return int(value, 0)
    except ValueError:
        pass
    from ._features_metering import scale_mask_for_mode

    try:
        mode = _parse_mode(value)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"must be an integer mask or one of {', '.join(_cli_mode_names())}"
        ) from None
    return scale_mask_for_mode(0, mode)


# The contract type of a ``type=`` callable that stands in for a builtin. Left
# unstated, the inventory guesses from the default, and an option defaulting to
# None has no default to guess from.
_cli_scalar_type(_finite_float, "number")
_cli_scalar_type(_candidate_count, "integer")
_cli_scalar_type(_mode_mask, "integer")
_cli_names(_mode_mask, _cli_mode_names)
_cli_scalar_type(_positive_finite_float, "number")
_cli_scalar_type(_tempo_bpm, "number")
_cli_scalar_type(_closed_unit_float, "number")
_cli_scalar_type(_pitch_threshold, "number")
_cli_scalar_type(_nonnegative_finite_float, "number")
_cli_scalar_type(_positive_pitch_frequency, "number")
_cli_scalar_type(_c_int, "integer")
_cli_scalar_type(_int64, "integer")
_cli_scalar_type(_positive_int, "integer")
_cli_integer_ceiling(_c_int, _C_INT_MAX)
_cli_integer_ceiling(_positive_int, _C_INT_MAX)

# The accepted value set each of the checkers above enforces, in the shape the
# published inventory uses.  A ``type=`` callable is opaque to argparse, so the
# domain it applies is recorded on the callable itself: the declaration then
# lives next to the code that enforces it, and the cross-surface checker can
# compare it with the native CLI's registry instead of assuming the two agree.
_cli_domain(_nonnegative_finite_float, minimum=0.0)
_cli_domain(_positive_pitch_frequency, minimum=0.0, exclusive_minimum=True)
_cli_domain(_pitch_threshold, minimum=0.0, exclusive_minimum=True, maximum=1.0)
_cli_domain(_positive_finite_float, minimum=0.0, exclusive_minimum=True)
_cli_domain(_tempo_bpm, minimum=0.0, exclusive_minimum=True)
_cli_domain(_closed_unit_float, minimum=0.0, maximum=1.0)
_cli_domain(_positive_int, minimum=0.0, exclusive_minimum=True)


def _add_wav_bits_argument(parser: argparse.ArgumentParser) -> argparse.Action:
    """Add ``--bits`` with the accepted set ``_wav_bits`` enforces.

    The declared domain refuses anything but 16 or 24 at parse time.
    """
    return _cli_domain(
        parser.add_argument("--bits", type=_c_int, default=16),
        choices=(16, 24),
    )


def _wav_bits(args: argparse.Namespace) -> int:
    """Return the requested PCM width, keeping the CLI contract intentionally small."""
    bits = int(getattr(args, "bits", 16))
    if bits not in (16, 24):
        raise ValueError("bits must be 16 or 24")
    return bits


def _option_supplied(args: argparse.Namespace, name: str) -> bool:
    """Report whether the caller spelled ``--name`` on the command line.

    The parser records the destination of every option present on argv, so an
    option carrying its documented default still counts as supplied -- the same
    answer the native ``CliArgs::has`` gives for identical argv.
    """
    supplied = getattr(args, "_supplied_options", ())
    return name.replace("-", "_") in supplied


def _validate_pitch_namespace(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    fmin = getattr(args, "f0_min_hz", None)
    fmax = getattr(args, "f0_max_hz", None)
    if fmin is None and fmax is None:
        return
    # The bound the caller left out is the tracker's own default, read from its
    # signature so the rule is checked against the value the library will use.
    from . import pitch_pyin

    defaults = inspect.signature(pitch_pyin).parameters
    fmin = defaults["f0_min_hz"].default if fmin is None else fmin
    fmax = defaults["f0_max_hz"].default if fmax is None else fmax
    if fmax <= fmin:
        # The effective values, not just the rule: the offending half is
        # commonly the option the caller never typed, and `--f0-min-hz 3000`
        # against the 2093 Hz default `--f0-max-hz` reads as an unexplained
        # refusal without them.
        parser.error(
            f"--f0-max-hz must be greater than --f0-min-hz "
            f"(--f0-min-hz {fmin:g}, --f0-max-hz {fmax:g})"
        )


# The pitch tracker's range, which a vocal session falls back to for whichever
# of `--f0-min-hz` / `--f0-max-hz` the caller left out.
_VOCAL_DEFAULT_FMIN_HZ = 65.0
_VOCAL_DEFAULT_FMAX_HZ = 2093.0


def _validate_vocal_notes_namespace(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    fmin_given = getattr(args, "f0_min_hz", None)
    fmax_given = getattr(args, "f0_max_hz", None)
    if fmin_given is None and fmax_given is None:
        return
    fmin = _VOCAL_DEFAULT_FMIN_HZ if fmin_given is None else fmin_given
    fmax = _VOCAL_DEFAULT_FMAX_HZ if fmax_given is None else fmax_given
    if fmax <= fmin:
        parser.error(
            f"--f0-max-hz must be greater than --f0-min-hz "
            f"(--f0-min-hz {fmin:g}, --f0-max-hz {fmax:g})"
        )


# The tempo search range `analyze_rhythm` falls back to for an omitted bound.
_RHYTHM_DEFAULT_BPM_MIN = 60.0
_RHYTHM_DEFAULT_BPM_MAX = 200.0


def _validate_rhythm_namespace(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    bpm_min = getattr(args, "bpm_min", None)
    bpm_max = getattr(args, "bpm_max", None)
    bpm_min = _RHYTHM_DEFAULT_BPM_MIN if bpm_min is None else bpm_min
    bpm_max = _RHYTHM_DEFAULT_BPM_MAX if bpm_max is None else bpm_max
    if bpm_max <= bpm_min:
        parser.error(
            "--bpm-max must be greater than --bpm-min "
            f"(--bpm-min {bpm_min:g}, --bpm-max {bpm_max:g})"
        )


# The hop below which the key analyzer's HPSS cannot resynthesize
# (kMinKeyHpssHopLength, src/analysis/key_analyzer.h).
_MIN_KEY_HPSS_HOP_LENGTH = 16


def _validate_key_namespace(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    # The names are decided from the command line alone, so refused before the input is read.
    try:
        if getattr(args, "profile", None) is not None:
            _parse_key_profile(args.profile)
        if getattr(args, "modes", None) is not None:
            _parse_modes(args.modes)
    except ValueError as error:
        parser.error(str(error))
    hop_length = getattr(args, "hop_length", None)
    if not getattr(args, "hpss", False) or hop_length is None:
        return
    if hop_length < _MIN_KEY_HPSS_HOP_LENGTH:
        parser.error(f"--hop-length must be at least {_MIN_KEY_HPSS_HOP_LENGTH} with --hpss")


def _validate_chords_namespace(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    try:
        if args.key_root is not None:
            _parse_pitch_class(args.key_root)
        if args.key_mode is not None:
            _parse_mode(args.key_mode)
    except ValueError as error:
        parser.error(str(error))


def _validate_analyze_namespace(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    if args.meter_candidates is None:
        return
    try:
        _parse_meter_candidates(args.meter_candidates)
    except ValueError as error:
        parser.error(str(error))


def _validate_project_bounce(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    # The command's own module owns the checks; imported here to keep the module graph acyclic.
    from ._cli_project import project_audio_refusal, project_synth_refusal

    _validate_render_channels(args, parser)
    refusal = project_synth_refusal(args, bare_flag=True) or project_audio_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _validate_midi_render(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    from ._cli_project import project_synth_refusal

    _validate_render_channels(args, parser)
    refusal = project_synth_refusal(args, bare_flag=False)
    if refusal is not None:
        parser.error(refusal)


def _validate_project_align_takes(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    from ._cli_project import project_audio_refusal

    refusal = project_audio_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _validate_render_channels(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    channels = getattr(args, "channels", None)
    if channels is None or channels in (1, 2, 6, 8):
        return
    parser.error(
        "argument --channels: must be one of 1, 2, 6, 8; leave it out for the scene master's width"
    )


def _validate_repair_namespace(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    # The command's own module owns the checks; imported here to keep the module graph acyclic.
    from ._cli_repair import _repair_argument_refusal

    refusal = _repair_argument_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _validate_streaming_platforms(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    # A --platforms document is command-line text, so it is refused before the input is read.
    from ._cli_mastering import _streaming_platforms_refusal

    refusal = _streaming_platforms_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _validate_mastering_selection(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    # One chain, chosen by --preset, --chain-config or --assistant. Alongside --assistant,
    # --preset names the base the suggestion starts from, so only --chain-config excludes both.
    assistant = _option_supplied(args, "assistant")
    config = _option_supplied(args, "config")
    preset = _option_supplied(args, "preset")
    if config and (preset or assistant):
        parser.error("--chain-config is mutually exclusive with --preset and --assistant")
    if not (config or preset or assistant):
        parser.error(
            "mastering needs --preset or --chain-config, or --assistant (which may take "
            "--preset); loudness-only normalisation is mastering-normalize-loudness"
        )
    if assistant:
        return
    # An option that only reaches an AssistantConfig field is refused, not dropped.
    for name in (
        "target-lufs",
        "ceiling-dbtp",
        "enable-repair",
        "explain",
        "target-platform",
        "no-streaming-safe",
        "speech-mono-amount",
    ):
        if _option_supplied(args, name):
            parser.error(f"--{name} requires --assistant")


def _validate_mastering_arguments(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    from ._cli_mastering import _chain_params_refusal

    _validate_mastering_selection(args, parser)
    refusal = _chain_params_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _validate_params_grammar(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    # The entries of a numeric-only --params text are argv values, so their syntax is refused
    # before the input is read.
    raw = getattr(args, "params", "") or ""
    refusal = _params_grammar_refusal(raw) or _numeric_params_refusal(raw)
    if refusal is not None:
        parser.error(refusal)


def _validate_stereo_analysis_params(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    from ._cli_mastering import _stereo_analysis_params_refusal

    _validate_params_grammar(args, parser)
    refusal = _stereo_analysis_params_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _validate_processor_params(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    from ._cli_mastering import _processor_params_refusal

    refusal = _processor_params_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _validate_eq_params(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    from ._cli_mastering import _eq_params_refusal

    refusal = _eq_params_refusal(args)
    if refusal is not None:
        parser.error(refusal)
    if _option_supplied(args, "params"):
        refusal = _numeric_params_refusal(args.params or "")
        if refusal is not None:
            parser.error(refusal)


def _validate_assistant_params(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    from ._cli_mastering import _assistant_params_refusal

    refusal = _assistant_params_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _validate_mix_namespace(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    # A scene comes from exactly one source, and a render needs both ends.
    if args.scene is None and args.preset is None:
        parser.error("mix needs --scene or --preset")
    if args.scene is not None and args.preset is not None:
        parser.error("--scene is mutually exclusive with --preset")
    # The command's module owns the entry grammar; imported here to keep the graph acyclic.
    from ._cli_mixing import mix_input_refusal

    refusal = mix_input_refusal(args.input)
    if refusal is not None:
        parser.error(refusal)
    if args.input and not args.output:
        parser.error("mix with --input requires --output")
    if args.output and not args.input:
        parser.error("mix with --output requires at least one --input")


def _validate_suggest_mix_namespace(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    # The command's own module owns the checks; imported here to keep the module graph acyclic.
    from ._cli_mixing import suggest_mix_refusal

    refusal = suggest_mix_refusal(args)
    if refusal is not None:
        parser.error(refusal)


def _refuse_both_bound_spellings(
    args: argparse.Namespace, parser: argparse.ArgumentParser, bounds: tuple[str, ...]
) -> None:
    # A position is spelled in samples or in seconds; both at once is refused by name.
    for bound in bounds:
        attr = bound.replace("-", "_")
        if getattr(args, f"{attr}_sample") is not None and getattr(args, f"{attr}_sec") is not None:
            parser.error(f"give --{bound}-sample or --{bound}-sec, not both")


def _validate_note_move_bounds(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    _refuse_both_bound_spellings(args, parser, ("onset", "offset", "target-onset"))
    if args.target_onset_sample is None and args.target_onset_sec is None:
        parser.error("give --target-onset-sample or --target-onset-sec")


def _validate_note_stretch_bounds(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    _refuse_both_bound_spellings(args, parser, ("onset", "offset"))


def _validate_fade_sides(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    if args.fade_in_sec is None and args.fade_out_sec is None:
        parser.error("give --fade-in-sec or --fade-out-sec")


def _validate_pitch_correct_timevarying_namespace(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    if args.mode == "scale" and _option_supplied(args, "target_midi"):
        parser.error("--target-midi does not apply when --mode is scale")


def _validate_filter_frequencies(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    corner = args.type in ("high-pass", "low-pass")
    types = "high-pass or low-pass" if corner else "band-pass or notch"
    read = ("cutoff_hz",) if corner else ("center_hz", "bandwidth_hz")
    unread = ("center_hz", "bandwidth_hz") if corner else ("cutoff_hz",)
    for name in read:
        if getattr(args, name) is None:
            parser.error(f"--{name.replace('_', '-')} is required when --type is {types}")
    for name in unread:
        if getattr(args, name) is not None:
            parser.error(f"--{name.replace('_', '-')} does not apply when --type is {types}")
    if not corner and args.order not in (None, 2):
        parser.error(f"--order must be 2 when --type is {types}")


def _validate_utility_arguments(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    # The command's own module owns the checks; imported here to keep the module graph acyclic.
    from ._cli_utilities import _utility_argument_refusal

    refusal = _utility_argument_refusal(args)
    if refusal is not None:
        parser.error(refusal)


# The percentile ranks metering_dynamic_range uses when a bound is left out.
_DYNAMIC_RANGE_LOW_PERCENTILE = 0.1
_DYNAMIC_RANGE_HIGH_PERCENTILE = 0.95


def _validate_dynamic_range_percentiles(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    low = _DYNAMIC_RANGE_LOW_PERCENTILE if args.low_percentile is None else args.low_percentile
    high = _DYNAMIC_RANGE_HIGH_PERCENTILE if args.high_percentile is None else args.high_percentile
    # The C options and native CLI compare float32 ranks.
    if _as_float32(high) < _as_float32(low):
        parser.error(
            f"--high-percentile must be at least --low-percentile "
            f"(--low-percentile {low:g}, --high-percentile {high:g})"
        )


def _refusal_validator(
    module: str, function: str
) -> Callable[[argparse.Namespace, argparse.ArgumentParser], None]:
    """Build a validator running ``module.function(args)``, a refusal text or None."""

    def validate(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
        # The command's own module owns the check; imported here to keep the module graph acyclic.
        refusal = getattr(importlib.import_module(f".{module}", __package__), function)(args)
        if refusal is not None:
            parser.error(refusal)

    return validate


# Cross-option checks, keyed by command path, run once parsing has succeeded and
# before any handler reads its input -- the native registry's command validators.
# Each refuses as a usage failure: it is decided from the command line alone.
_COMMAND_VALIDATORS: dict[str, Callable[[argparse.Namespace, argparse.ArgumentParser], None]] = {
    "pitch": _validate_pitch_namespace,
    "pitch-correct-timevarying": _validate_pitch_correct_timevarying_namespace,
    "vocal-notes": _validate_vocal_notes_namespace,
    "rhythm": _validate_rhythm_namespace,
    "key": _validate_key_namespace,
    "chords": _validate_chords_namespace,
    "analyze": _validate_analyze_namespace,
    "mix": _validate_mix_namespace,
    "suggest-mix": _validate_suggest_mix_namespace,
    "note-move": _validate_note_move_bounds,
    "note-stretch": _validate_note_stretch_bounds,
    "repair": _validate_repair_namespace,
    "mastering": _validate_mastering_arguments,
    "mastering-processor": _validate_processor_params,
    "eq": _validate_eq_params,
    "mastering-suggest": _validate_assistant_params,
    "mastering-profile": _validate_params_grammar,
    "mastering-pair-processor": _validate_params_grammar,
    "mastering-pair-analyze": _validate_params_grammar,
    "mastering-stereo-analyze": _validate_stereo_analysis_params,
    "mastering-streaming": _validate_streaming_platforms,
    "project.bounce": _validate_project_bounce,
    "project.align-takes": _validate_project_align_takes,
    "midi-render": _validate_midi_render,
    "fade": _validate_fade_sides,
    "melody": _validate_pitch_namespace,
    "dynamic-range": _validate_dynamic_range_percentiles,
    "filter": _validate_filter_frequencies,
    "hpss": _refusal_validator("_cli_effects", "_hpss_refusal"),
    "normalize": _refusal_validator("_cli_effects", "_normalize_refusal"),
    "trim-silence": _refusal_validator("_cli_effects", "_trim_silence_refusal"),
    "voice-change": _refusal_validator("_cli_effects", "_voice_change_refusal"),
    "voice-preset": _refusal_validator("_cli_effects", "_voice_preset_refusal"),
    "voice-preset-validate": _refusal_validator("_cli_effects", "_voice_preset_validate_refusal"),
    "polyphonic-render": _refusal_validator("_cli_note_edit", "_polyphonic_edit_refusal"),
    "transcribe": _refusal_validator("_cli_project", "_transcribe_refusal"),
    "synthesize-rir": _refusal_validator("_cli_acoustic", "_placement_refusal"),
    "room-morph": _refusal_validator("_cli_acoustic", "_placement_refusal"),
    **{
        name: _validate_utility_arguments
        for name in (
            "chirp",
            "clicks",
            "power-to-db",
            "amplitude-to-db",
            "db-to-power",
            "db-to-amplitude",
            "frame-signal",
            "pad-center",
            "fix-length",
            "fix-frames",
            "peak-pick",
            "vector-normalize",
            "pcen",
        )
    },
}


def _command_path(args: argparse.Namespace) -> str | None:
    """The leaf a parsed namespace dispatches to (``project.bounce``), or None."""
    command = getattr(args, "command", None)
    if command == "project":
        subcommand = getattr(args, "project_command", None)
        return f"project.{subcommand}" if subcommand else None
    return command


def _restore_parser_compat_defaults(args: argparse.Namespace) -> None:
    """Keep handler-facing defaults while inventory actions expose nulls.

    A few long-standing handlers use a concrete sentinel (``0`` or ``""``)
    to mean "not supplied", while the public schema deliberately reports that
    state as JSON ``null``.  Their argparse actions use ``None`` so the
    inventory is derived from the real parser; restore the historical runtime
    sentinel only after parsing and before dispatch.
    """
    command = getattr(args, "command", "")
    if command == "key" and getattr(args, "candidates", None) is None:
        args.candidates = 0


# The one place a command declares whether it writes an audio artifact.
#
# The complement -- the commands for which ``-o`` is a usage error -- is derived
# from the parser below rather than restated. Both sets used to be written out
# by hand, one of them inline inside ``main``, so a new command had to be added
# to the right one of two lists that nothing compared; the two even rejected the
# same mistake with different exit codes.
_OUTPUT_CAPABLE_COMMANDS = frozenset(
    {
        "hpss",
        "decompose-stems",
        "pitch-correct",
        "pitch-correct-timevarying",
        "note-move",
        "note-stretch",
        "tune-to-midi",
        "playback",
        "polyphonic-render",
        "pitch-shift",
        "time-stretch",
        "normalize",
        "gain",
        "fade",
        "filter",
        "tone",
        "chirp",
        "clicks",
        "preemphasis",
        "deemphasis",
        "mel-to-audio",
        "mfcc-to-audio",
        "trim-silence",
        "resample",
        "voice-change",
        "synthesize-rir",
        "room-morph",
        "mastering",
        "mastering-normalize-loudness",
        "eq",
        "mastering-processor",
        "mastering-pair-processor",
        "repair",
        "midi-render",
        "mix",
        "mix-strip",
        "project",
        "transcribe",
    }
)


# The `project` leaves that write nothing; the native registry declares no `output` on them.
_PROJECT_STDOUT_LEAVES = frozenset({"abi", "compile", "synth-presets"})
_NO_FILE_OUTPUT_REFUSAL = "Command 'project' does not produce a file output; remove -o/--output"


def _stdout_only_commands(parser: argparse.ArgumentParser) -> frozenset[str]:
    """Commands that produce no audio artifact, derived from the parser.

    Every registered subcommand that is not output-capable belongs here, so a
    new subcommand cannot end up missing from both sets.
    """
    return frozenset(_inventory_subparsers(parser)) - _OUTPUT_CAPABLE_COMMANDS


def _reject_stdout_output(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    """Reject output destinations before a stdout-only handler can run."""
    command = getattr(args, "command", "")
    if command in _stdout_only_commands(parser) and getattr(args, "output", None) is not None:
        parser.error(f"Command '{command}' does not produce a file output; remove -o/--output")

    # The project parser intentionally accepts project-wide flags before the
    # leaf subcommand so ``project --json validate ...`` remains valid.  A
    # parent ``--output`` must nevertheless be rejected for stdout-only leaves
    # such as ``abi`` and ``compile`` at the same parser boundary.
    if (
        command == "project"
        and getattr(args, "project_command", "") in _PROJECT_STDOUT_LEAVES
        and getattr(args, "output", None) is not None
    ):
        parser.error(_NO_FILE_OUTPUT_REFUSAL)


def _reject_leaf_output(
    args: argparse.Namespace, extras: list[str], parser: argparse.ArgumentParser
) -> None:
    """Refuse -o/--output on a project leaf that declares no output option.

    The leaf's parser leaves the flag unparsed, so it arrives as an extra and
    would otherwise end as argparse's generic "unrecognized arguments".
    """
    if (
        getattr(args, "command", "") == "project"
        and getattr(args, "project_command", "") in _PROJECT_STDOUT_LEAVES
        and any(
            token in ("-o", "--output") or token.startswith(("--output=", "-o")) for token in extras
        )
    ):
        parser.error(_NO_FILE_OUTPUT_REFUSAL)


@dataclasses.dataclass(frozen=True)
class SharedParsers:
    """The parent parsers each command family draws its common options from.

    They are passed as one object rather than as four arguments so a family
    registrar's signature does not change when a new parent is added.
    """

    common: _ContractArgumentParser
    stdout_options: _ContractArgumentParser
    fft_stdout_options: _ContractArgumentParser
    mel_options: _ContractArgumentParser


def build_shared_parsers() -> SharedParsers:
    """Construct the parent parsers shared across every command family."""
    # Keep stdout-only and artifact-producing leaves on separate parents. A
    # shared output option would make ``--output`` look valid on analysis
    # commands and defer the usage error until handler dispatch.
    json_options = _ContractArgumentParser(add_help=False)
    json_options.add_argument("--json", action="store_true", help="Output JSON")
    common = _ContractArgumentParser(add_help=False, parents=[json_options])
    common.add_argument("-o", "--output", type=str, default=None, help="Output file path")

    # Accept the spelling solely long enough to issue the established
    # stdout-only diagnostic below.  It is deliberately hidden from the
    # public inventory: these commands do not produce audio artifacts.
    stdout_options = _ContractArgumentParser(add_help=False, parents=[json_options])
    stdout_options.add_argument(
        "-o", "--output", type=str, default=argparse.SUPPRESS, help=argparse.SUPPRESS
    )

    # The geometry a command leaves out is the library's own, which every command
    # fed by these parents agrees on, so one help text states it.
    fft_stdout_options = _ContractArgumentParser(add_help=False, parents=[stdout_options])
    fft_stdout_options.add_argument(
        "--n-fft", type=_c_int, default=None, help="FFT size (default: 2048)"
    )
    fft_stdout_options.add_argument(
        "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
    )
    mel_options = _ContractArgumentParser(add_help=False, parents=[fft_stdout_options])
    mel_options.add_argument(
        "--n-mels", type=_c_int, default=None, help="Number of mel bands (default: 128)"
    )
    return SharedParsers(
        common=common,
        stdout_options=stdout_options,
        fft_stdout_options=fft_stdout_options,
        mel_options=mel_options,
    )
