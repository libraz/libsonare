# ruff: noqa: F405
"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import math
import sys
from collections.abc import Iterable
from typing import Any, NoReturn

from ._cli_advanced import *  # noqa: F403
from ._cli_analysis import *  # noqa: F403
from ._cli_common import (
    _SONARE_CODE_TO_EXIT as _SONARE_CODE_TO_EXIT,
)
from ._cli_common import (
    EXIT_CANCELLED as EXIT_CANCELLED,
)
from ._cli_common import (
    EXIT_DECODE_FAILED as EXIT_DECODE_FAILED,
)
from ._cli_common import (
    EXIT_ERROR as EXIT_ERROR,
)
from ._cli_common import (
    EXIT_FILE_NOT_FOUND as EXIT_FILE_NOT_FOUND,
)
from ._cli_common import (
    EXIT_INVALID_FORMAT as EXIT_INVALID_FORMAT,
)
from ._cli_common import (
    EXIT_INVALID_PARAMETER as EXIT_INVALID_PARAMETER,
)
from ._cli_common import (
    EXIT_INVALID_STATE as EXIT_INVALID_STATE,
)
from ._cli_common import (
    EXIT_NOT_SUPPORTED as EXIT_NOT_SUPPORTED,
)
from ._cli_common import (
    EXIT_OUT_OF_MEMORY as EXIT_OUT_OF_MEMORY,
)
from ._cli_common import (
    EXIT_SUCCESS as EXIT_SUCCESS,
)
from ._cli_common import (
    EXIT_USAGE as EXIT_USAGE,
)
from ._cli_common import (
    MODE_NAMES as MODE_NAMES,
)
from ._cli_common import (
    PITCH_NAMES as PITCH_NAMES,
)
from ._cli_common import (
    _apply_voice_sets as _apply_voice_sets,
)
from ._cli_common import (
    _array_stats as _array_stats,
)
from ._cli_common import (
    _atomic_write_bytes as _atomic_write_bytes,
)
from ._cli_common import (
    _emit_effect_result as _emit_effect_result,
)
from ._cli_common import (
    _exit_code_for as _exit_code_for,
)
from ._cli_common import (
    _float_sequence as _float_sequence,
)
from ._cli_common import (
    _format_time as _format_time,
)
from ._cli_common import (
    _legacy_exit_codes as _legacy_exit_codes,
)
from ._cli_common import (
    _load_audio as _load_audio,
)
from ._cli_common import (
    _load_json_object as _load_json_object,
)
from ._cli_common import (
    _load_voice_preset_pack as _load_voice_preset_pack,
)
from ._cli_common import (
    _parse_json_config as _parse_json_config,
)
from ._cli_common import (
    _parse_json_list as _parse_json_list,
)
from ._cli_common import (
    _parse_key_profile as _parse_key_profile,
)
from ._cli_common import (
    _parse_kv_params as _parse_kv_params,
)
from ._cli_common import (
    _parse_mode as _parse_mode,
)
from ._cli_common import (
    _parse_modes as _parse_modes,
)
from ._cli_common import (
    _parse_pitch_class as _parse_pitch_class,
)
from ._cli_common import (
    _parse_voice_set_value as _parse_voice_set_value,
)
from ._cli_common import (
    _pcm16 as _pcm16,
)
from ._cli_common import (
    _read_bounded as _read_bounded,
)
from ._cli_common import (
    _resample as _resample,
)
from ._cli_common import (
    _resample_linear as _resample_linear,
)
from ._cli_common import (
    _set_nested_value as _set_nested_value,
)
from ._cli_common import (
    _write_project_bounce_wav as _write_project_bounce_wav,
)
from ._cli_common import (
    _write_wav as _write_wav,
)
from ._cli_common import (
    _write_wav_stereo as _write_wav_stereo,
)
from ._cli_common import (
    cmd_doctor as cmd_doctor,
)
from ._cli_effects import *  # noqa: F403
from ._cli_inventory import (
    _cli_domain as _cli_domain,
)
from ._cli_inventory import (
    _inventory_option as _inventory_option,
)
from ._cli_inventory import (
    _inventory_subparsers as _inventory_subparsers,
)
from ._cli_inventory import (
    dump_cli_contract as _dump_cli_contract_for_parser,
)
from ._cli_mastering import *  # noqa: F403
from ._cli_project import *  # noqa: F403
from ._cli_project import (
    _load_project as _load_project,
)
from ._cli_project import (
    _project_bounce as _project_bounce,
)
from ._cli_project import (
    _write_project_json as _write_project_json,
)
from ._facade import rebind_facade_exports as _rebind_facade_exports


class _ContractArgumentParser(argparse.ArgumentParser):
    """Argument parser with the cross-surface usage exit contract.

    ``argparse`` normally exits with status 2 directly from ``error``.  The
    native CLI has a compatibility switch that folds every non-zero status to
    1, so parser errors need to go through the same switch as dispatch errors.
    Keeping this behavior in the parser class also covers errors raised by
    nested subparsers and typed options before command dispatch begins.
    """

    def error(self, message: str) -> NoReturn:
        self.print_usage(sys.stderr)
        self._print_message(f"{self.prog}: error: {message}\n", sys.stderr)
        raise SystemExit(1 if _legacy_exit_codes() else EXIT_USAGE)

    def parse_known_args(
        self,
        args: Iterable[str] | None = None,
        namespace: Any = None,
    ) -> tuple[Any, list[str]]:
        parsed, extras = super().parse_known_args(args, namespace)
        _restore_parser_compat_defaults(parsed)
        _reject_stdout_output(parsed, self)
        if self.prog.endswith(" pitch") or getattr(parsed, "command", None) == "pitch":
            _validate_pitch_namespace(parsed, self)
        return parsed, extras


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


def _positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise argparse.ArgumentTypeError("must be a positive integer") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


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
        parser.error("--fmax must be greater than --fmin")


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
        "pitch-correct",
        "pitch-correct-timevarying",
        "note-move",
        "note-stretch",
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
        "mastering-chain",
        "master",
        "declip",
        "midi-render",
        "mix",
        "project",
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


def _build_parser() -> _ContractArgumentParser:
    """Build the public CLI parser without parsing argv.

    Keeping construction separate lets ``--dump-cli-contract`` inspect the
    same parser that handles normal invocations, so inventory drift cannot be
    hidden behind a second hand-written parser definition.
    """
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

    parser = _ContractArgumentParser(
        prog="sonare",
        description="libsonare - Fast audio analysis (Python CLI)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("version", parents=[stdout_options], help="Show version")
    sub.add_parser("doctor", parents=[stdout_options], help="Show build and runtime diagnostics")
    sub.add_parser("info", parents=[stdout_options], help="Show audio file information")
    sub.add_parser("bpm", parents=[stdout_options], help="Detect BPM")
    key_p = sub.add_parser("key", parents=[stdout_options], help="Detect musical key")
    # Key detection keeps a 4096-sample analysis default (better low-frequency
    # resolution) rather than the shared 2048. A None sentinel distinguishes
    # "left at the default" from an explicit value, matching the native CLI and
    # the detect_key() library default. These FFT options are declared here
    # instead of inherited from fft_options: parents= shares the actual argument
    # objects, so a set_defaults override would mutate the shared --n-fft used by
    # the other analysis commands.
    key_p.add_argument(
        "--n-fft", type=int, default=4096, help="FFT size (default: 4096 for key analysis)"
    )
    key_p.add_argument("--hop-length", type=int, default=512, help="Hop length (default: 512)")
    key_p.add_argument(
        "--candidates",
        type=int,
        default=None,
        metavar="N",
        help="Also show the top N key candidates",
    )
    key_p.add_argument(
        "--use-hpss",
        "--hpss",
        dest="use_hpss",
        action="store_true",
        help="Use harmonic audio for key chroma",
    )
    key_p.add_argument(
        "--loudness-weighted", action="store_true", help="Weight key chroma frames by RMS"
    )
    key_p.add_argument(
        "--high-pass-hz", type=float, default=0.0, help="High-pass cutoff before key analysis"
    )
    key_p.add_argument(
        "--modes",
        type=str,
        default="",
        help="Candidate modes: major-minor, all, or comma-separated mode names",
    )
    key_p.add_argument(
        "--profile",
        type=str,
        default="",
        help="Key profile: ks, temperley, shaath, edmt, edma, edmm, or bellman",
    )
    key_p.add_argument(
        "--genre-hint",
        type=str,
        default="",
        help="Genre hint for key profile selection, e.g. auto, edm, pop, classical, jazz",
    )
    sub.add_parser("beats", parents=[stdout_options], help="Detect beat times")
    sub.add_parser("downbeats", parents=[stdout_options], help="Detect downbeat times")
    sub.add_parser("onsets", parents=[stdout_options], help="Detect onset times")
    chords_p = sub.add_parser(
        "chords", parents=[fft_stdout_options], help="Detect chord progression"
    )
    chords_p.add_argument(
        "--min-duration", type=float, default=0.3, help="Minimum chord duration in seconds"
    )
    chords_p.add_argument(
        "--smoothing-window", type=float, default=2.0, help="Chroma smoothing window in seconds"
    )
    chords_p.add_argument(
        "--threshold", type=float, default=0.5, help="Chord detection confidence threshold"
    )
    chords_p.add_argument(
        "--triads-only", action="store_true", help="Restrict output to triad qualities"
    )
    chords_p.add_argument(
        "--nnls", action="store_true", help="Use NNLS chroma instead of the STFT chroma"
    )
    chords_p.add_argument(
        "--no-beat-sync", action="store_true", help="Disable beat-synchronous chroma pooling"
    )
    chords_p.add_argument(
        "--use-hmm", action="store_true", help="Decode the chord sequence with an HMM"
    )
    chords_p.add_argument("--hmm-beam-width", type=int, default=24, help="HMM decoder beam width")
    chords_p.add_argument(
        "--key-context", action="store_true", help="Bias detection with a key context"
    )
    chords_p.add_argument(
        "--key-root", default="C", help="Key-context root pitch class (e.g. C, F#)"
    )
    chords_p.add_argument(
        "--key-mode", default="major", help="Key-context mode (e.g. major, minor)"
    )
    chords_p.add_argument(
        "--detect-inversions", action="store_true", help="Report chord inversions (bass note)"
    )
    analyze_p = sub.add_parser("analyze", parents=[stdout_options], help="Full music analysis")
    analyze_p.add_argument(
        "--with-seventh", action="store_true", help="Include seventh chords in the analysis"
    )
    analyze_p.add_argument(
        "--no-hpss", action="store_true", help="Disable harmonic-percussive separation"
    )
    analyze_p.add_argument(
        "--chroma-highpass",
        type=_nonnegative_finite_float,
        default=80.0,
        help="High-pass cutoff for chroma analysis in Hz (default: 80.0)",
    )
    mel_p = sub.add_parser("mel", parents=[mel_options], help="Compute mel spectrogram")
    mel_p.add_argument("--fmin", type=float, default=0.0, help="Lowest mel band frequency in Hz")
    mel_p.add_argument(
        "--fmax", type=float, default=0.0, help="Highest mel band frequency in Hz (0 = Nyquist)"
    )
    mel_p.add_argument(
        "--htk", action="store_true", help="Use the HTK mel formula instead of Slaney"
    )
    sub.add_parser("chroma", parents=[fft_stdout_options], help="Compute chromagram")
    sub.add_parser("spectral", parents=[fft_stdout_options], help="Compute spectral features")
    # Pitch is a stdout-only analysis command. Its frequency/threshold domains
    # mirror the PitchConfig checks in the core API.
    pitch_p = sub.add_parser("pitch", parents=[stdout_options], help="Track pitch")
    # cmd_pitch raises for any other name, so the accepted set is declared here
    # even though argparse itself does not enforce it.
    _cli_domain(
        pitch_p.add_argument("--algorithm", default="pyin"),
        choices=("yin", "pyin"),
        reject_exit="invalid_parameter",
    )
    pitch_p.add_argument(
        "--threshold",
        type=_pitch_threshold,
        default=0.1,
        help="YIN threshold (> 0 and <= 1; default: 0.1)",
    )
    pitch_p.add_argument(
        "--hop-length",
        type=_positive_int,
        default=512,
        help="Hop length in samples (default: 512)",
    )
    pitch_p.add_argument(
        "--fmin",
        type=_positive_pitch_frequency,
        default=65.0,
        help="Minimum frequency in Hz (default: 65.0)",
    )
    pitch_p.add_argument(
        "--fmax",
        type=_positive_pitch_frequency,
        default=2093.0,
        help="Maximum frequency in Hz (default: 2093.0)",
    )
    hpss_p = sub.add_parser("hpss", parents=[fft_options], help="Harmonic-percussive separation")
    hpss_p.add_argument(
        "--kernel-harmonic", type=int, default=31, help="Harmonic median-filter kernel"
    )
    hpss_p.add_argument(
        "--kernel-percussive", type=int, default=31, help="Percussive median-filter kernel"
    )
    hpss_p.add_argument("--harmonic-only", action="store_true")
    hpss_p.add_argument("--percussive-only", action="store_true")
    hpss_p.add_argument("--with-residual", action="store_true")
    hpss_p.add_argument("--hard-mask", action="store_true")

    # Editing commands
    pitch_correct_p = sub.add_parser(
        "pitch-correct", parents=[common], help="Pitch-correct from a current to a target MIDI note"
    )
    pitch_correct_p.add_argument(
        "--current-midi", type=float, default=69.0, help="Current pitch as a MIDI note number"
    )
    pitch_correct_p.add_argument(
        "--target-midi", type=float, default=69.0, help="Target pitch as a MIDI note number"
    )
    pitch_tv_p = sub.add_parser(
        "pitch-correct-timevarying",
        parents=[common],
        help="Track pYIN contour and correct toward a note or scale",
    )
    pitch_tv_p.add_argument("--mode", choices=["midi", "scale"], default="midi")
    pitch_tv_p.add_argument("--target-midi", type=float, default=69.0)
    pitch_tv_p.add_argument("--hop-length", type=int, default=512)
    pitch_tv_p.add_argument("--scale-root", type=int, default=0)
    pitch_tv_p.add_argument("--scale-mode-mask", type=lambda value: int(value, 0), default=0xAB5)
    pitch_tv_p.add_argument("--reference-midi", type=float, default=69.0)
    note_move_p = sub.add_parser("note-move", parents=[common], help="Move one note region")
    note_move_p.add_argument("--onset", type=int, default=0)
    note_move_p.add_argument("--offset", type=int, default=None)
    note_move_p.add_argument("--target-onset", type=int, default=None)
    scale_quantize_p = sub.add_parser(
        "scale-quantize", parents=[stdout_options], help="Quantize one MIDI value to a scale"
    )
    scale_quantize_p.add_argument("midi", type=float)
    scale_quantize_p.add_argument("--root", type=int, default=0)
    scale_quantize_p.add_argument("--mode-mask", type=lambda value: int(value, 0), default=0xAB5)
    scale_quantize_p.add_argument("--reference-midi", type=float, default=69.0)
    note_stretch_p = sub.add_parser(
        "note-stretch", parents=[common], help="Time-stretch a single note region"
    )
    note_stretch_p.add_argument(
        "--onset", type=int, default=0, help="Start sample index of the note region"
    )
    note_stretch_p.add_argument(
        "--offset", type=int, default=0, help="End sample index of the note region"
    )
    note_stretch_p.add_argument(
        "--ratio", type=float, default=1.0, help="Stretch factor for the region (>1 lengthens)"
    )
    # Effect commands that map directly to the Python effects API. The C++ CLI
    # still exposes some low-level converters and section/melody analyses that
    # are not mirrored here; this set covers the common offline edits.
    pitch_shift_p = sub.add_parser(
        "pitch-shift", parents=[common], help="Shift pitch by a number of semitones"
    )
    pitch_shift_p.add_argument("--semitones", type=float, help="Semitones to shift (positive = up)")
    pitch_shift_p.add_argument("--n-fft", type=int, default=2048)
    pitch_shift_p.add_argument("--hop-length", type=int, default=512)
    time_stretch_p = sub.add_parser(
        "time-stretch", parents=[common], help="Time-stretch without changing pitch"
    )
    time_stretch_p.add_argument(
        "--rate", type=float, help="Stretch factor (>1 speeds up, <1 slows down)"
    )
    time_stretch_p.add_argument("--n-fft", type=int, default=2048)
    time_stretch_p.add_argument("--hop-length", type=int, default=512)
    normalize_p = sub.add_parser(
        "normalize", parents=[common], help="Peak-normalize audio to a target dB level"
    )
    normalize_p.add_argument("--mode", default="peak", help="Normalization mode (default: peak)")
    normalize_p.add_argument(
        "--target-db", type=float, default=None, help="Target peak level in dB"
    )
    trim_silence_p = sub.add_parser(
        "trim-silence", parents=[common], help="Trim leading/trailing silence"
    )
    trim_silence_p.add_argument(
        "--threshold-db", type=float, default=None, help="Silence threshold in dB (default: -60)"
    )
    # ``--threshold-db`` and ``--top-db`` select two handler paths. Leave the
    # alternate selector absent by default so the handler can distinguish the
    # default threshold mode from an explicit top-dB request.
    trim_silence_p.add_argument("--top-db", type=float, default=None)
    trim_silence_p.add_argument("--n-fft", type=int, default=2048)
    trim_silence_p.add_argument("--hop-length", type=int, default=512)
    resample_p = sub.add_parser(
        "resample", parents=[common], help="Resample audio to a target sample rate"
    )
    resample_p.add_argument(
        "--target-rate",
        "--target-sr",
        dest="target_rate",
        type=int,
        required=True,
        help="Target sample rate in Hz",
    )
    voice_change_p = sub.add_parser(
        "voice-change", parents=[common], help="Apply a voice-change effect"
    )
    voice_change_p.add_argument("--pitch-semitones", type=float, help="Pitch shift in semitones")
    voice_change_p.add_argument(
        "--formant-factor",
        type=float,
        help="Formant scaling factor (1.0 = unchanged)",
    )
    voice_change_p.add_argument("--preset", default="", help="Realtime voice changer preset id")
    voice_change_p.add_argument("--preset-json", help="Realtime voice changer preset JSON file")
    voice_change_p.add_argument(
        "--preset-pack", help="Realtime voice changer preset pack JSON file"
    )
    voice_change_p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="PATH=VALUE",
        help="Override preset JSON fields, e.g. dsp.outputGainDb=-2",
    )
    sub.add_parser(
        "voice-presets", parents=[stdout_options], help="List realtime voice changer presets"
    )
    voice_preset_p = sub.add_parser(
        "voice-preset", help="Print a realtime voice changer preset (always JSON)"
    )
    voice_preset_p.add_argument("--preset", default="neutral-monitor", help="Preset id")
    voice_preset_p.add_argument(
        "--json", action="store_true", help="No-op; a preset is always printed as JSON"
    )
    # Not a parents=[common] subcommand: it consumes a JSON preset, not audio,
    # so the analysis flags (--n-fft/--hop-length/--n-mels) do not apply and the
    # positional argument is a preset file rather than an audio file.
    voice_preset_validate_p = sub.add_parser(
        "voice-preset-validate", help="Validate and normalize voice preset JSON"
    )
    voice_preset_validate_p.add_argument("--json", action="store_true", help="Output JSON")
    voice_preset_validate_p.add_argument(
        "--preset-json",
        help="Voice preset JSON file (takes precedence over the positional path)",
    )
    voice_preset_validate_p.add_argument(
        "--preset", default="", help="Preset id when validating a pack"
    )
    voice_preset_validate_p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="PATH=VALUE",
        help="Override preset JSON fields before validation",
    )
    voice_preset_validate_p.add_argument("file", nargs="?", help="Voice preset JSON file")

    # Analysis commands
    acoustic_p = sub.add_parser(
        "acoustic", parents=[stdout_options], help="Estimate acoustic parameters"
    )
    acoustic_p.add_argument("--ir", action="store_true", help="Treat input as an impulse response")
    acoustic_p.add_argument("--n-bands", type=int, default=6)
    acoustic_p.add_argument("--min-decay-db", type=float, default=30.0)
    acoustic_p.add_argument("--noise-floor-margin-db", type=float, default=10.0)

    def _add_room_geometry(p: argparse.ArgumentParser) -> None:
        p.add_argument("--length", type=float, default=7.0, help="Room length (m)")
        p.add_argument("--width", type=float, default=5.0, help="Room width (m)")
        p.add_argument("--height", type=float, default=3.0, help="Room height (m)")
        p.add_argument("--absorption", type=float, default=0.2, help="Uniform wall absorption")
        p.add_argument("--source-x", type=float, default=1.0)
        p.add_argument("--source-y", type=float, default=1.0)
        p.add_argument("--source-z", type=float, default=1.2)
        p.add_argument("--listener-x", type=float, default=5.0)
        p.add_argument("--listener-y", type=float, default=4.0)
        p.add_argument("--listener-z", type=float, default=1.7)
        p.add_argument("--ism-order", type=int, default=3, help="Image-source reflection order")
        p.add_argument("--seed", type=int, default=1, help="Deterministic late-tail seed")
        p.add_argument(
            "--max-seconds",
            type=float,
            default=0.0,
            help="Hard cap on RIR/tail length in seconds (0 = natural length)",
        )
        p.add_argument(
            "--sabine",
            action="store_true",
            help="Use the Sabine late-reverb model (default Eyring)",
        )

    estimate_room_p = sub.add_parser(
        "estimate-room", parents=[stdout_options], help="Estimate equivalent room from a recording"
    )
    estimate_room_p.add_argument("--aspect-lw", type=float, default=1.0, help="length/width prior")
    estimate_room_p.add_argument("--aspect-lh", type=float, default=1.0, help="length/height prior")
    estimate_room_p.add_argument(
        "--reference-absorption", type=float, default=0.15, help="absorption prior"
    )
    estimate_room_p.add_argument(
        "--sabine", action="store_true", help="Use the Sabine model (default Eyring)"
    )
    estimate_room_p.add_argument(
        "--n-octave-bands",
        "--n-bands",
        type=int,
        default=None,
        dest="n_octave_bands",
        metavar="N",
        help="Analyzer octave-band count (0 = library default)",
    )

    synth_rir_p = sub.add_parser(
        "synthesize-rir", parents=[common], help="Synthesize a room impulse response from geometry"
    )
    _add_room_geometry(synth_rir_p)
    synth_rir_p.add_argument("--sample-rate", type=int, default=48000, help="Output sample rate")

    room_morph_p = sub.add_parser(
        "room-morph", parents=[common], help="Morph reverberation toward a target room"
    )
    _add_room_geometry(room_morph_p)
    room_morph_p.add_argument("--wet", type=float, default=0.5, help="Target-room mix [0,1]")
    room_morph_p.add_argument(
        "--suppression", type=float, default=0.5, help="Source-tail suppression [0,1]"
    )

    rhythm_p = sub.add_parser(
        "rhythm", parents=[fft_stdout_options], help="Analyze rhythm primitives"
    )
    rhythm_p.add_argument("--start-bpm", type=float, default=120.0)
    rhythm_p.add_argument("--bpm-min", type=float, default=60.0)
    rhythm_p.add_argument("--bpm-max", type=float, default=200.0)
    dynamics_p = sub.add_parser(
        "dynamics", parents=[stdout_options], help="Analyze dynamics/loudness"
    )
    dynamics_p.add_argument("--window-sec", type=float, default=0.4)
    # Dynamics windows the loudness series but runs no FFT, so it takes the hop
    # control without the matching --n-fft.
    dynamics_p.add_argument("--hop-length", type=int, default=512, help="Hop length (default: 512)")
    sub.add_parser("timbre", parents=[mel_options], help="Analyze timbre/spectral shape")
    lufs_p = sub.add_parser("lufs", parents=[stdout_options], help="Compute LUFS loudness")
    lufs_p.add_argument(
        "--series", action="store_true", help="Also emit momentary/short-term LUFS series"
    )
    sub.add_parser(
        "onset-envelope", parents=[mel_options], help="Compute the onset strength envelope"
    )
    nnls_p = sub.add_parser("nnls-chroma", parents=[stdout_options], help="Compute NNLS chroma")
    nnls_p.add_argument("--hop-length", type=int, default=512)
    tempogram_p = sub.add_parser(
        "tempogram", parents=[mel_options], help="Compute autocorrelation tempogram"
    )
    tempogram_p.add_argument("--win-length", type=int, default=384)
    plp_p = sub.add_parser("plp", parents=[mel_options], help="Compute predominant local pulse")
    plp_p.add_argument("--tempo-min", type=float, default=30.0)
    plp_p.add_argument("--tempo-max", type=float, default=300.0)
    plp_p.add_argument("--win-length", type=int, default=384)

    # Mastering commands
    mastering_p = sub.add_parser(
        "mastering", parents=[common], help="Loudness-normalize with a true-peak ceiling"
    )
    mastering_p.add_argument("--preset", default="")
    mastering_p.add_argument("--config", default=None)
    mastering_p.add_argument("--target-lufs", type=float, default=-14.0)
    mastering_p.add_argument("--ceiling-db", type=float, default=-1.0)
    mastering_p.add_argument("--params", default="")
    _add_wav_bits_argument(mastering_p)
    mastering_p.add_argument(
        "--true-peak-oversample", type=int, choices=(1, 2, 4, 8, 16), default=4
    )
    mastering_p.add_argument("--report", default=None, help="Write a mastering report JSON file")
    mastering_p.add_argument("--assistant", action="store_true")
    mastering_p.add_argument("--enable-repair", action="store_true")
    mastering_p.add_argument("--explain", action="store_true")
    mproc_p = sub.add_parser(
        "mastering-processor", parents=[common], help="Apply a named mastering processor"
    )
    mproc_p.add_argument("--processor", required=True, help="Processor name")
    mproc_p.add_argument("--params", default="", help="Params as k=v,k=v (floats)")
    _add_wav_bits_argument(mproc_p)
    mproc_p.add_argument("--stereo", action="store_true")
    eq_p = sub.add_parser("eq", parents=[common], help="Apply the unified equalizer")
    eq_p.add_argument("--params", default="", help="Params as k=v,k=v (overrides band shortcuts)")
    eq_p.add_argument(
        "--type",
        type=int,
        default=0,
        help=(
            "Band type enum: 0 peak, 1 low shelf, 2 high shelf, 3 low pass, "
            "4 high pass, 5 band pass, 6 notch, 7 tilt"
        ),
    )
    eq_p.add_argument("--frequency-hz", type=float, default=1000.0)
    eq_p.add_argument("--gain-db", type=float, default=0.0)
    eq_p.add_argument("--q", type=float, default=1.0)
    eq_p.add_argument("--coeff-mode", type=int, default=0, help="0 RBJ, 1 Vicanek")
    eq_p.add_argument("--slope-db-oct", type=int, default=12)
    eq_p.add_argument(
        "--placement", type=int, default=0, help="0 stereo, 1 left, 2 right, 3 mid, 4 side"
    )
    eq_p.add_argument(
        "--phase-mode", type=int, default=1, help="1 zero latency, 2 natural, 3 linear"
    )
    eq_p.add_argument(
        "--resolution",
        type=int,
        default=0,
        help="0 custom/default, 1 low, 2 medium, 3 high, 4 very high, 5 maximum",
    )
    eq_p.add_argument("--auto-gain", action="store_true")
    eq_p.add_argument("--gain-scale", type=float, default=1.0)
    eq_p.add_argument("--output-gain-db", type=float, default=0.0)
    eq_p.add_argument("--output-pan", type=float, default=0.0)
    eq_p.add_argument("--proportional-q", action="store_true")
    eq_p.add_argument("--dynamic", action="store_true")
    eq_p.add_argument("--threshold-db", type=float, default=-24.0)
    eq_p.add_argument("--auto-threshold", action="store_true")
    eq_p.add_argument("--ratio", type=float, default=2.0)
    eq_p.add_argument("--range-db", type=float, default=-6.0)
    eq_p.add_argument("--attack-ms", type=float, default=5.0)
    eq_p.add_argument("--release-ms", type=float, default=50.0)
    # "--lookahead-ms" is the flag's former (misleading) spelling; still
    # accepted, both writing to the same destination, so a stored script
    # keeps working.
    eq_p.add_argument(
        "--detector-delay-ms", "--lookahead-ms", dest="lookahead_ms", type=float, default=0.0
    )
    eq_p.add_argument("--sidechain-freq-hz", type=float, default=-1.0)
    eq_p.add_argument("--sidechain-q", type=float, default=1.0)
    _add_wav_bits_argument(eq_p)
    sub.add_parser(
        "mastering-processors", parents=[stdout_options], help="List mastering processor names"
    )
    sub.add_parser(
        "mastering-pair-processors",
        parents=[stdout_options],
        help="List two-input mastering processor names",
    )
    sub.add_parser(
        "mastering-pair-analyses",
        parents=[stdout_options],
        help="List two-input mastering analysis names",
    )
    mpa_p = sub.add_parser(
        "mastering-pair-analyze",
        parents=[stdout_options],
        help="Run a two-input mastering analysis (always JSON output)",
    )
    mpa_p.add_argument("--reference", required=True, help="Reference audio file")
    mpa_p.add_argument("--analysis", required=True, help="Analysis name")
    mpa_p.add_argument("--params", default="")
    mchain_p = sub.add_parser(
        "mastering-chain", parents=[common], help="Run a configurable mastering chain"
    )
    mchain_p.add_argument("--config", default=None, help="Chain config as a JSON object")
    mchain_p.add_argument("--config-file", default=None, help="Chain config JSON file")
    mchain_p.add_argument("--params", default="", help="Flat params as k=v,k=v (floats)")
    mchain_p.add_argument("--report", default=None, help="Write a mastering report JSON file")
    master_p = sub.add_parser("master", parents=[common], help="Apply a named mastering preset")
    master_p.add_argument("--preset", default="pop", help="Mastering preset name")
    master_p.add_argument("--config", default=None, help="Preset overrides as a JSON object")
    master_p.add_argument("--config-file", default=None, help="Preset override JSON file")
    master_p.add_argument("--params", default="", help="Flat overrides as k=v,k=v (floats)")
    master_p.add_argument("--report", default=None, help="Write a mastering report JSON file")
    mstream_p = sub.add_parser(
        "mastering-streaming",
        parents=[stdout_options],
        help="Preview streaming-platform normalization as JSON",
    )
    mstream_p.add_argument(
        "--platforms",
        default=None,
        help="Platform targets as JSON array of {name,targetLufs,ceilingDb}",
    )
    mstream_p.add_argument("--platforms-file", default=None, help="Platform targets JSON file")
    declip_p = sub.add_parser("declip", parents=[common], help="Repair clipped audio")
    declip_p.add_argument("--clip-threshold", type=float, default=0.98)
    declip_p.add_argument("--lpc-order", type=int, default=36)
    declip_p.add_argument("--iterations", type=int, default=2)
    declip_p.add_argument("--lpc-blend", type=float, default=0.65)
    sub.add_parser(
        "mastering-presets", parents=[stdout_options], help="List mastering preset names"
    )
    msuggest_p = sub.add_parser(
        "mastering-suggest", parents=[stdout_options], help="Suggest a mastering chain as JSON"
    )
    msuggest_p.add_argument("--params", default="", help="Assistant params as k=v,k=v")
    mprofile_p = sub.add_parser(
        "mastering-profile",
        parents=[stdout_options],
        help="Analyze a mastering audio profile as JSON",
    )
    mprofile_p.add_argument("--params", default="", help="Profile params as k=v,k=v")

    # Project / MIDI commands
    project_p = sub.add_parser("project", parents=[common], help="Headless project / SMF commands")
    project_sub = project_p.add_subparsers(dest="project_command", required=True)
    # The project-level parser owns the common defaults.  Child parsers must
    # accept the same flags without installing their own defaults, otherwise a
    # value before the project subcommand (for example `project --json abi`)
    # is overwritten by the child parser's false/empty default.
    project_common = _ContractArgumentParser(add_help=False, argument_default=argparse.SUPPRESS)
    project_common.add_argument("--json", action="store_true")
    project_common.add_argument("-o", "--output", type=str)
    project_stdout_common = _ContractArgumentParser(
        add_help=False, argument_default=argparse.SUPPRESS
    )
    project_stdout_common.add_argument("--json", action="store_true")

    project_sub.add_parser(
        "abi", parents=[project_stdout_common], help="Print the project ABI version"
    )
    pnew = project_sub.add_parser(
        "new", parents=[project_common], help="Create an empty project JSON"
    )
    pnew.add_argument("--sample-rate", type=int, default=0, help="Project sample rate")
    for pname in ("validate", "compile"):
        if pname == "validate":
            # Keep the active validation route's action order aligned with the
            # public contract. Child defaults remain suppressed so flags
            # supplied before the project subcommand are not overwritten by
            # child-parser defaults.
            pp = project_sub.add_parser(pname, help="Project validate")
            pp.add_argument("--json", action="store_true", default=argparse.SUPPRESS)
            pp.add_argument(
                "--strict",
                action="store_true",
                help="Fail (non-zero exit) when the project loads with diagnostics",
            )
            pp.add_argument("--in", dest="input", required=True, help="Input project JSON")
            pp.add_argument("-o", "--output", type=str, default=argparse.SUPPRESS)
        else:
            pp = project_sub.add_parser(
                pname, parents=[project_stdout_common], help="Project compile"
            )
            pp.add_argument("--in", dest="input", required=True, help="Input project JSON")
    sf2_cli_note = (
        "SF2 / SoundFont and per-destination synth JSON are not wired through this CLI command; "
        "use the Project API for SoundFont-backed bounces."
    )
    pbounce = project_sub.add_parser(
        "bounce",
        parents=[project_common],
        help="Render project to WAV",
        description=sf2_cli_note,
    )
    pbounce.add_argument("--in", dest="input", required=True, help="Input project JSON")
    pbounce.add_argument(
        "--sample-rate",
        type=int,
        default=None,
        help="Render sample rate (default: the project's own sample rate)",
    )
    pbounce.add_argument("--frames", type=int, default=0, help="Render length in frames")
    pbounce.add_argument("--block-size", type=int, default=0, help="Render block size")
    pbounce.add_argument("--channels", type=int, default=2, help="Render channel count")
    pbounce.add_argument("--instrument-latency", type=int, default=0)
    pbounce.add_argument(
        "--synth",
        nargs="?",
        const="",
        default=None,
        help=(
            "Bare flag uses GM program/channel routing and channel-10 drums; a value selects "
            "a fixed NativeSynth preset; "
            "no --sf2 or --synth-json CLI wiring"
        ),
    )
    pexport_smf = project_sub.add_parser("export-smf", parents=[project_common], help="Export SMF")
    pexport_smf.add_argument("--in", dest="input", required=True, help="Input project JSON")
    pimport_smf = project_sub.add_parser("import-smf", parents=[project_common], help="Import SMF")
    pimport_smf.add_argument("--smf", required=True, help="Input Standard MIDI File")
    pexport_midi2 = project_sub.add_parser(
        "export-midi2", parents=[project_common], help="Export MIDI 2.0 Clip File"
    )
    pexport_midi2.add_argument("--in", dest="input", required=True, help="Input project JSON")
    pimport_midi2 = project_sub.add_parser(
        "import-midi2", parents=[project_common], help="Import MIDI 2.0 Clip File"
    )
    pimport_midi2.add_argument("--midi2", required=True, help="Input MIDI 2.0 Clip File")
    project_sub.add_parser(
        "synth-presets", parents=[project_stdout_common], help="List NativeSynth presets"
    )

    midi_render_p = sub.add_parser(
        "midi-render",
        parents=[common],
        help="Render a MIDI project through NativeSynth",
        description=sf2_cli_note,
    )
    midi_render_p.add_argument("--in", dest="input", required=True, help="Input project JSON")
    midi_render_p.add_argument(
        "--sample-rate",
        type=int,
        default=None,
        help="Render sample rate (default: the project's own sample rate)",
    )
    midi_render_p.add_argument("--frames", type=int, default=0, help="Render length in frames")
    midi_render_p.add_argument("--block-size", type=int, default=0, help="Render block size")
    midi_render_p.add_argument("--channels", type=int, default=2, help="Render channel count")
    midi_render_p.add_argument("--instrument-latency", type=int, default=0)
    midi_render_p.add_argument(
        "--synth",
        default="",
        help=(
            "NativeSynth preset (default: GM program/channel routing); "
            "no --sf2 or --synth-json CLI wiring"
        ),
    )

    # Mixing commands
    sub.add_parser(
        "mixing-presets", parents=[stdout_options], help="List built-in mixer scene presets"
    )
    mixing_preset_p = sub.add_parser(
        "mixing-preset", parents=[stdout_options], help="Print a built-in mixer scene preset"
    )
    mixing_preset_p.add_argument(
        "--preset", default="vocalReverbSend", help="Built-in scene preset name"
    )

    mix_p = sub.add_parser(
        "mix",
        parents=[common],
        help="Load a mixer scene (JSON file or preset) and optionally render inputs",
        description=(
            "Input WAVs are loaded as mono (stereo files are downmixed) and each "
            "mono input is duplicated across both output channels. Inputs at a "
            "different sample rate are resampled to --sample-rate before mixing."
        ),
    )
    mix_group = mix_p.add_mutually_exclusive_group(required=True)
    mix_group.add_argument("--scene", default="", help="Path to a scene JSON file")
    mix_group.add_argument("--preset", default="", help="Built-in scene preset name")
    mix_p.add_argument(
        "--input",
        action="append",
        default=[],
        metavar="WAV",
        help=(
            "Per-strip input WAV (repeat once per strip); loaded as mono and "
            "resampled to --sample-rate; requires --output to render"
        ),
    )
    mix_p.add_argument(
        "--sample-rate", type=int, default=48000, help="Mixer sample rate (default: 48000)"
    )
    mix_p.add_argument(
        "--block-size", type=int, default=512, help="Mixer max block size (default: 512)"
    )

    # Add file argument to all subcommands that need it
    for name in [
        "info",
        "bpm",
        "key",
        "beats",
        "downbeats",
        "onsets",
        "chords",
        "analyze",
        "mel",
        "chroma",
        "spectral",
        "pitch",
        "hpss",
        "pitch-correct",
        "pitch-correct-timevarying",
        "note-move",
        "note-stretch",
        "pitch-shift",
        "time-stretch",
        "normalize",
        "trim-silence",
        "resample",
        "voice-change",
        "acoustic",
        "estimate-room",
        "room-morph",
        "rhythm",
        "dynamics",
        "timbre",
        "lufs",
        "onset-envelope",
        "nnls-chroma",
        "tempogram",
        "plp",
        "mastering",
        "eq",
        "mastering-processor",
        "mastering-pair-analyze",
        "mastering-chain",
        "master",
        "mastering-streaming",
        "declip",
        "mastering-suggest",
        "mastering-profile",
    ]:
        sub.choices[name].add_argument("file", help="Audio file path")

    return parser


def _dump_cli_contract() -> None:
    """Emit the parser inventory consumed by the cross-surface checker."""
    _dump_cli_contract_for_parser(_build_parser())


def main() -> None:
    """CLI entry point."""
    if len(sys.argv) == 2 and sys.argv[1] == "--dump-cli-contract":
        _dump_cli_contract()
        return

    parser = _build_parser()
    args = parser.parse_args()

    if not args.command:
        # Defensive fallback for callers that supply a custom namespace or
        # parser implementation.  Normal parsing enforces this through the
        # required subparser and reaches the same usage-error path.
        parser.error("the following arguments are required: command")

    commands = {
        "version": cmd_version,
        "doctor": cmd_doctor,
        "info": cmd_info,
        "bpm": cmd_bpm,
        "key": cmd_key,
        "beats": cmd_beats,
        "downbeats": cmd_downbeats,
        "onsets": cmd_onsets,
        "chords": cmd_chords,
        "analyze": cmd_analyze,
        "mel": cmd_mel,
        "chroma": cmd_chroma,
        "spectral": cmd_spectral,
        "pitch": cmd_pitch,
        "hpss": cmd_hpss,
        "pitch-correct": cmd_pitch_correct,
        "pitch-correct-timevarying": cmd_pitch_correct_timevarying,
        "note-move": cmd_note_move,
        "scale-quantize": cmd_scale_quantize,
        "note-stretch": cmd_note_stretch,
        "pitch-shift": cmd_pitch_shift,
        "time-stretch": cmd_time_stretch,
        "normalize": cmd_normalize,
        "trim-silence": cmd_trim_silence,
        "resample": cmd_resample,
        "voice-change": cmd_voice_change,
        "voice-presets": cmd_voice_presets,
        "voice-preset": cmd_voice_preset,
        "voice-preset-validate": cmd_voice_preset_validate,
        "acoustic": cmd_acoustic,
        "estimate-room": cmd_estimate_room,
        "synthesize-rir": cmd_synthesize_rir,
        "room-morph": cmd_room_morph,
        "rhythm": cmd_rhythm,
        "dynamics": cmd_dynamics,
        "timbre": cmd_timbre,
        "lufs": cmd_lufs,
        "onset-envelope": cmd_onset_envelope,
        "nnls-chroma": cmd_nnls_chroma,
        "tempogram": cmd_tempogram,
        "plp": cmd_plp,
        "mastering": cmd_mastering,
        "eq": cmd_eq,
        "mastering-processor": cmd_mastering_processor,
        "mastering-processors": cmd_mastering_processors,
        "mastering-pair-processors": cmd_mastering_pair_processors,
        "mastering-pair-analyses": cmd_mastering_pair_analyses,
        "mastering-pair-analyze": cmd_mastering_pair_analyze,
        "mastering-chain": cmd_mastering_chain,
        "master": cmd_master,
        "mastering-streaming": cmd_mastering_streaming,
        "declip": cmd_declip,
        "mastering-presets": cmd_mastering_presets,
        "mastering-suggest": cmd_mastering_suggest,
        "mastering-profile": cmd_mastering_profile,
        "project": cmd_project,
        "midi-render": cmd_midi_render,
        "mixing-presets": cmd_mixing_presets,
        "mixing-preset": cmd_mixing_preset,
        "mix": cmd_mix,
    }

    handler = commands.get(args.command)
    if not handler:
        print(f"Unknown command: {args.command}", file=sys.stderr)
        sys.exit(1 if _legacy_exit_codes() else EXIT_USAGE)

    try:
        # `common` supplies -o to every parser for a uniform CLI shape, but an
        # analysis result has no audio artifact to write. That rejection happens
        # at the parser boundary (`_reject_stdout_output`), which sees every
        # command and reports one exit code for all of them.
        sys.exit(handler(args))
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(_exit_code_for(e))


_rebind_facade_exports(globals(), "libsonare._cli_", "libsonare.cli")
del _rebind_facade_exports


if __name__ == "__main__":
    main()
