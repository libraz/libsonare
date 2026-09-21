# ruff: noqa: F405
"""Command-line interface for libsonare."""

from __future__ import annotations

import sys

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
from ._cli_effects import (
    _DEFAULT_UNMATCHED_POLICY as _DEFAULT_UNMATCHED_POLICY,
)
from ._cli_effects import (
    _POLYPHONIC_EDIT_FIELDS as _POLYPHONIC_EDIT_FIELDS,
)
from ._cli_effects import (
    _UNMATCHED_POLICY_NAMES as _UNMATCHED_POLICY_NAMES,
)
from ._cli_inventory import (
    _cli_domain as _cli_domain,
)
from ._cli_inventory import (
    _cli_scalar_type as _cli_scalar_type,
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
from ._cli_mastering import (
    _EQ_ENUM_BOUNDS as _EQ_ENUM_BOUNDS,
)
from ._cli_mixing import *  # noqa: F403
from ._cli_options import (
    _FALSE_FLAG_LITERALS as _FALSE_FLAG_LITERALS,
)
from ._cli_options import (
    _OUTPUT_CAPABLE_COMMANDS as _OUTPUT_CAPABLE_COMMANDS,
)
from ._cli_options import (
    _TRUE_FLAG_LITERALS as _TRUE_FLAG_LITERALS,
)
from ._cli_options import (
    _add_wav_bits_argument as _add_wav_bits_argument,
)
from ._cli_options import (
    _c_int_option as _c_int_option,
)
from ._cli_options import (
    _candidate_count as _candidate_count,
)
from ._cli_options import (
    _ContractArgumentParser as _ContractArgumentParser,
)
from ._cli_options import (
    _finite_float as _finite_float,
)
from ._cli_options import (
    _nonnegative_finite_float as _nonnegative_finite_float,
)
from ._cli_options import (
    _pitch_threshold as _pitch_threshold,
)
from ._cli_options import (
    _positive_int as _positive_int,
)
from ._cli_options import (
    _positive_pitch_frequency as _positive_pitch_frequency,
)
from ._cli_options import (
    _reject_stdout_output as _reject_stdout_output,
)
from ._cli_options import (
    _restore_parser_compat_defaults as _restore_parser_compat_defaults,
)
from ._cli_options import (
    _stdout_only_commands as _stdout_only_commands,
)
from ._cli_options import (
    _validate_pitch_namespace as _validate_pitch_namespace,
)
from ._cli_options import (
    build_shared_parsers as build_shared_parsers,
)
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


def _build_parser() -> _ContractArgumentParser:
    """Build the public CLI parser without parsing argv.

    Keeping construction separate lets ``--dump-cli-contract`` inspect the
    same parser that handles normal invocations, so inventory drift cannot be
    hidden behind a second hand-written parser definition.
    """
    shared = build_shared_parsers()

    parser = _ContractArgumentParser(
        prog="sonare",
        description="libsonare - Fast audio analysis (Python CLI)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # Called in the order the help output lists the commands: a family's
    # subcommands appear where its registrar runs, not where its module sorts.
    register_analysis_parsers(sub, shared)
    register_effects_parsers(sub, shared)
    register_advanced_parsers(sub, shared)
    register_mastering_parsers(sub, shared)
    register_project_parsers(sub, shared)
    register_mixing_parsers(sub, shared)

    # Add file argument to all subcommands that need it
    for name in [
        "info",
        "bpm",
        "key",
        "beats",
        "downbeats",
        "onsets",
        "chords",
        "sections",
        "analyze",
        "mel",
        "chroma",
        "spectral",
        "pitch",
        "hpss",
        "decompose-stems",
        "pitch-correct",
        "pitch-correct-timevarying",
        "note-move",
        "note-stretch",
        "tune-to-midi",
        "polyphonic-notes",
        "polyphonic-render",
        "pitch-shift",
        "time-stretch",
        "normalize",
        "trim-silence",
        "split-silence",
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
        "mastering-pair-processor",
        "mastering-stereo-analyze",
        "mastering-chain",
        "master",
        "mastering-streaming",
        "declip",
        "repair",
        "mastering-suggest",
        "mastering-profile",
        "mix-strip",
        "transcribe",
    ]:
        sub.choices[name].add_argument("file", help="Audio file path")

    return parser


def _dump_cli_contract() -> None:
    """Emit the parser inventory consumed by the cross-surface checker."""
    _dump_cli_contract_for_parser(_build_parser())


def main() -> None:
    """CLI entry point.

    One reporting path covers every failure, and it wraps parsing as well as
    dispatch: Ctrl-C is a BaseException, so an interrupt anywhere inside used to
    reach the interpreter's traceback printer instead of the one-line Error
    every other failure reports.
    """
    try:
        _dispatch()
    except (Exception, KeyboardInterrupt) as exc:
        message = "cancelled" if isinstance(exc, KeyboardInterrupt) else str(exc)
        print(f"Error: {message}", file=sys.stderr)
        sys.exit(_exit_code_for(exc))


def _dispatch() -> None:
    """Build the parser, route argv to a command handler, and exit with its status."""
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
        "sections": cmd_sections,
        "analyze": cmd_analyze,
        "mel": cmd_mel,
        "chroma": cmd_chroma,
        "spectral": cmd_spectral,
        "pitch": cmd_pitch,
        "hpss": cmd_hpss,
        "decompose-stems": cmd_decompose_stems,
        "pitch-correct": cmd_pitch_correct,
        "pitch-correct-timevarying": cmd_pitch_correct_timevarying,
        "note-move": cmd_note_move,
        "scale-quantize": cmd_scale_quantize,
        "note-stretch": cmd_note_stretch,
        "tune-to-midi": cmd_tune_to_midi,
        "polyphonic-notes": cmd_polyphonic_notes,
        "polyphonic-render": cmd_polyphonic_render,
        "pitch-shift": cmd_pitch_shift,
        "time-stretch": cmd_time_stretch,
        "normalize": cmd_normalize,
        "trim-silence": cmd_trim_silence,
        "split-silence": cmd_split_silence,
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
        "mastering-pair-processor": cmd_mastering_pair_processor,
        "mastering-stereo-analyze": cmd_mastering_stereo_analyze,
        "mastering-chain": cmd_mastering_chain,
        "master": cmd_master,
        "mastering-streaming": cmd_mastering_streaming,
        "declip": cmd_declip,
        "repair": cmd_repair,
        "mastering-presets": cmd_mastering_presets,
        "mastering-suggest": cmd_mastering_suggest,
        "mastering-profile": cmd_mastering_profile,
        "project": cmd_project,
        "midi-render": cmd_midi_render,
        "transcribe": cmd_transcribe,
        "suggest-mix": cmd_suggest_mix,
        "mixing-presets": cmd_mixing_presets,
        "mixing-preset": cmd_mixing_preset,
        "mix": cmd_mix,
        "mix-strip": cmd_mix_strip,
    }

    handler = commands.get(args.command)
    if not handler:
        print(f"Unknown command: {args.command}", file=sys.stderr)
        sys.exit(1 if _legacy_exit_codes() else EXIT_USAGE)

    # `common` supplies -o to every parser for a uniform CLI shape, but an
    # analysis result has no audio artifact to write. That rejection happens at
    # the parser boundary (`_reject_stdout_output`), which sees every command and
    # reports one exit code for all of them.
    sys.exit(handler(args))


_rebind_facade_exports(globals(), "libsonare._cli_", "libsonare.cli")
del _rebind_facade_exports


if __name__ == "__main__":
    main()
