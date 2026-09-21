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
import math
import sys
from collections.abc import Iterable
from typing import Any, NoReturn

from ._cli_common import EXIT_USAGE, _legacy_exit_codes
from ._cli_inventory import _cli_domain, _cli_scalar_type, _inventory_subparsers
from ._errors import SonareValueError
from ._runtime import _C_INT_MAX, _C_INT_MIN, _narrow_int


class _ContractArgumentParser(argparse.ArgumentParser):
    """Argument parser with the cross-surface usage exit contract.

    ``argparse`` normally exits with status 2 directly from ``error``.  The
    native CLI has a compatibility switch that folds every non-zero status to
    1, so parser errors need to go through the same switch as dispatch errors.
    Keeping this behavior in the parser class also covers errors raised by
    nested subparsers and typed options before command dispatch begins.

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
        return super().add_argument(*args, **kwargs)

    def error(self, message: str) -> NoReturn:
        self.print_usage(sys.stderr)
        self._print_message(f"{self.prog}: error: {message}\n", sys.stderr)
        raise SystemExit(1 if _legacy_exit_codes() else EXIT_USAGE)

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
        if self.prog.endswith(" pitch") or getattr(parsed, "command", None) == "pitch":
            _validate_pitch_namespace(parsed, self)
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


def _finite_float(value: str) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError) as exc:
        raise argparse.ArgumentTypeError("must be a finite number") from exc
    if not math.isfinite(parsed):
        raise argparse.ArgumentTypeError("must be a finite number")
    return parsed


def _nonnegative_finite_float(value: str) -> float:
    parsed = _finite_float(value)
    if parsed < 0.0:
        raise argparse.ArgumentTypeError("must be greater than or equal to 0")
    return parsed


def _positive_pitch_frequency(value: str) -> float:
    parsed = _finite_float(value)
    if parsed <= 0.0:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return parsed


def _pitch_threshold(value: str) -> float:
    parsed = _finite_float(value)
    if parsed <= 0.0 or parsed > 1.0:
        raise argparse.ArgumentTypeError("must be greater than 0 and at most 1")
    return parsed


def _c_int_option(value: str, requirement: str) -> int:
    """Read an option value as a C ``int``, refusing in the type argparse expects.

    The native CLI parses every integer option through ``std::stoi`` before
    dispatch, so a value past the signed range is a usage error on both surfaces.
    """
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise argparse.ArgumentTypeError(requirement) from exc
    try:
        return _narrow_int(parsed, "value", _C_INT_MIN, _C_INT_MAX)
    except SonareValueError as exc:
        raise argparse.ArgumentTypeError("must fit in a signed 32-bit integer") from exc


def _positive_int(value: str) -> int:
    # Type half shared, semantic half local: "positive" is this option's own rule.
    parsed = _c_int_option(value, "must be a positive integer")
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def _candidate_count(value: str) -> int:
    """Parse ``--candidates``, accepting the native CLI's ``true`` shorthand.

    The native handler reads the raw string and maps ``true`` to the top five,
    so a script written against it reaches the Python CLI with that literal.
    """
    if value == "true":
        return 5
    return _c_int_option(value, "must be an integer or 'true'")


# The contract type of a ``type=`` callable that stands in for a builtin. Left
# unstated, the inventory guesses from the default, and an option defaulting to
# None has no default to guess from.
_cli_scalar_type(_finite_float, "number")
_cli_scalar_type(_candidate_count, "integer")

# The accepted value set each of the checkers above enforces, in the shape the
# published inventory uses.  A ``type=`` callable is opaque to argparse, so the
# domain it applies is recorded on the callable itself: the declaration then
# lives next to the code that enforces it, and the cross-surface checker can
# compare it with the native CLI's registry instead of assuming the two agree.
_cli_domain(_nonnegative_finite_float, minimum=0.0)
_cli_domain(_positive_pitch_frequency, minimum=0.0, exclusive_minimum=True)
_cli_domain(_pitch_threshold, minimum=0.0, exclusive_minimum=True, maximum=1.0)
_cli_domain(_positive_int, minimum=0.0, exclusive_minimum=True)


def _add_wav_bits_argument(parser: argparse.ArgumentParser) -> argparse.Action:
    """Add ``--bits`` with the accepted set ``_wav_bits`` enforces.

    argparse accepts any integer here; the handler refuses anything but 16 or
    24 after parsing, which is why the domain records the invalid-parameter
    class rather than the usage one.
    """
    return _cli_domain(
        parser.add_argument("--bits", type=int, default=16),
        choices=(16, 24),
        reject_exit="invalid_parameter",
    )


def _validate_pitch_namespace(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    fmin = getattr(args, "fmin", None)
    fmax = getattr(args, "fmax", None)
    if fmin is not None and fmax is not None and fmax <= fmin:
        # The effective values, not just the rule: the offending half is
        # commonly the option the caller never typed, and `--fmin 3000` against
        # the 2093 Hz default `--fmax` reads as an unexplained refusal without
        # them.
        parser.error(f"--fmax must be greater than --fmin (--fmin {fmin:g}, --fmax {fmax:g})")


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
    elif command == "normalize" and getattr(args, "target_db", None) is None:
        # Preserve the historical peak default while honoring the RMS
        # normalizer's distinct -20 dB reference level.
        args.target_db = -20.0 if getattr(args, "mode", "peak") == "rms" else 0.0
    elif command == "estimate-room" and getattr(args, "n_octave_bands", None) is None:
        args.n_octave_bands = 0
    elif (
        command == "trim-silence"
        and getattr(args, "threshold_db", None) is None
        and getattr(args, "top_db", None) is None
    ):
        # ``--threshold-db`` and ``--top-db`` select different handler paths.
        # Keep the threshold action dynamic so an explicit top-db does not
        # appear to conflict with an implicit threshold; only the no-selector
        # path receives the historical -60 dB fallback.
        args.threshold_db = -60.0


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
        "polyphonic-render",
        "pitch-shift",
        "time-stretch",
        "normalize",
        "trim-silence",
        "resample",
        "voice-change",
        "synthesize-rir",
        "room-morph",
        "mastering",
        "eq",
        "mastering-processor",
        "mastering-pair-processor",
        "mastering-chain",
        "master",
        "declip",
        "repair",
        "midi-render",
        "mix",
        "mix-strip",
        "project",
        "transcribe",
    }
)


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
        parser.error(f"{command} does not produce an audio file; remove --output")

    # The project parser intentionally accepts project-wide flags before the
    # leaf subcommand so ``project --json validate ...`` remains valid.  A
    # parent ``--output`` must nevertheless be rejected for stdout-only leaves
    # such as ``abi`` and ``compile`` at the same parser boundary.
    if (
        command == "project"
        and getattr(args, "project_command", "")
        in {
            "abi",
            "compile",
            "synth-presets",
        }
        and getattr(args, "output", None) is not None
    ):
        parser.error(
            f"project {args.project_command} does not produce an output file; remove --output"
        )


@dataclasses.dataclass(frozen=True)
class SharedParsers:
    """The parent parsers each command family draws its common options from.

    They are passed as one object rather than as five arguments so a family
    registrar's signature does not change when a new parent is added.
    """

    common: argparse.ArgumentParser
    stdout_options: argparse.ArgumentParser
    fft_options: argparse.ArgumentParser
    fft_stdout_options: argparse.ArgumentParser
    mel_options: argparse.ArgumentParser


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

    fft_options = _ContractArgumentParser(add_help=False, parents=[common])
    fft_options.add_argument("--n-fft", type=int, default=2048, help="FFT size (default: 2048)")
    fft_options.add_argument(
        "--hop-length", type=int, default=512, help="Hop length (default: 512)"
    )
    fft_stdout_options = _ContractArgumentParser(add_help=False, parents=[stdout_options])
    fft_stdout_options.add_argument(
        "--n-fft", type=int, default=2048, help="FFT size (default: 2048)"
    )
    fft_stdout_options.add_argument(
        "--hop-length", type=int, default=512, help="Hop length (default: 512)"
    )
    mel_options = _ContractArgumentParser(add_help=False, parents=[fft_stdout_options])
    mel_options.add_argument(
        "--n-mels", type=int, default=128, help="Number of mel bands (default: 128)"
    )
    return SharedParsers(
        common=common,
        stdout_options=stdout_options,
        fft_options=fft_options,
        fft_stdout_options=fft_stdout_options,
        mel_options=mel_options,
    )
