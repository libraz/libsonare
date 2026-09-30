"""Command-line interface for libsonare: note and pitch editing commands."""

from __future__ import annotations

import argparse
import math
import sys
from typing import TYPE_CHECKING, cast

from ._cli_common import (
    _MAX_PROJECT_OR_MIDI_BYTES,
    EXIT_INVALID_PARAMETER,
    _emit_effect_result,
    _legacy_exit_codes,
    _read_bounded,
    _strict_json_dumps,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import SharedParsers, _ContractArgumentParser, _finite_float
from ._effects_note_ops import _UNMATCHED_POLICIES
from ._runtime import _C_INT_MAX, _C_INT_MIN

if TYPE_CHECKING:
    from .analyzer import NoteEdit


def cmd_pitch_correct(args: argparse.Namespace) -> int:
    from . import pitch_correct_to_midi

    samples, sr = _load_audio(args.file)
    current_midi = getattr(args, "current_midi", 69.0)
    target_midi = getattr(args, "target_midi", 69.0)
    # The constant-pitch facade is the API that preserves the command's
    # requested-pitch behavior.  It has no hop-length control; the parser and
    # native registry must not advertise --hop-length until a matching facade
    # exists.
    result = pitch_correct_to_midi(
        samples,
        sample_rate=sr,
        current_midi=current_midi,
        target_midi=target_midi,
    )

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={"current_midi": current_midi, "target_midi": target_midi},
        label="Pitch correct",
    )


def cmd_pitch_correct_timevarying(args: argparse.Namespace) -> int:
    from . import pitch_correct_timevarying, pitch_pyin

    samples, sr = _load_audio(args.file)
    track = pitch_pyin(samples, sample_rate=sr, hop_length=args.hop_length)
    result = pitch_correct_timevarying(
        samples,
        track.f0,
        sample_rate=sr,
        hop_length=args.hop_length,
        mode=args.mode,
        target_midi=args.target_midi,
        scale_root=args.scale_root,
        scale_mode_mask=args.scale_mode_mask,
        reference_midi=args.reference_midi,
        voiced=[int(value) for value in track.voiced_flag],
        voiced_prob=track.voiced_prob,
    )
    return _emit_effect_result(args, [result], sr, label="Time-varying pitch correct")


def cmd_note_move(args: argparse.Namespace) -> int:
    from . import note_move

    samples, sr = _load_audio(args.file)
    result = note_move(
        samples,
        sample_rate=sr,
        onset_sample=args.onset,
        offset_sample=args.offset,
        target_onset_sample=args.target_onset,
    )
    return _emit_effect_result(args, [result], sr, label="Note move")


def cmd_scale_quantize(args: argparse.Namespace) -> int:
    from . import scale_quantize_midi

    value = scale_quantize_midi(
        args.root, args.mode_mask, args.midi, reference_midi=args.reference_midi
    )
    if args.json:
        print(_strict_json_dumps({"input_midi": args.midi, "quantized_midi": value}))
    else:
        print(f"{value:.6f}")
    return 0


def cmd_note_stretch(args: argparse.Namespace) -> int:
    from . import note_stretch

    samples, sr = _load_audio(args.file)
    result = note_stretch(
        samples,
        sample_rate=sr,
        onset_sample=args.onset,
        offset_sample=args.offset,
        stretch_ratio=args.ratio,
    )

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={
            "onset_sample": args.onset,
            "offset_sample": args.offset,
            "ratio": args.ratio,
        },
        label="Note stretch",
    )


# The policy names ``assign_note_targets`` accepts, in the order the help, the
# published domain and the refusal all name them. Read off the facade's own table
# so a policy added there cannot stay unadvertised here.
_UNMATCHED_POLICY_NAMES = tuple(sorted(_UNMATCHED_POLICIES))

# What an absent --unmatched-policy means, mirroring the facade's own default.
# Spelled rather than taken as the first name above: that order is alphabetical,
# so a policy added ahead of it would move the default with nothing to show.
_DEFAULT_UNMATCHED_POLICY = "leave"

# The hop the take's pitch track is measured at. tune-to-midi advertises no
# analysis geometry, so the value the track and the note frame rate must agree on
# is stated once.
_TUNE_HOP_LENGTH = 512


def cmd_tune_to_midi(args: argparse.Namespace) -> int:
    """Tune a take to the reference melody a MIDI file carries."""
    from . import (
        assign_note_targets,
        extract_notes,
        note_targets_from_smf,
        pitch_pyin,
        render_notes,
    )

    # Refused before the pitch track and the extraction, which are what the
    # command costs: every check below is answerable from argv alone.
    if not args.output:
        print("Error: tune-to-midi requires an output file (-o/--output)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER
    if args.track < 0:
        raise ValueError(f"--track must be a non-negative track index: {args.track}")
    if args.unmatched_policy not in _UNMATCHED_POLICIES:
        raise ValueError(
            f"--unmatched-policy must be one of {', '.join(_UNMATCHED_POLICY_NAMES)}: "
            f"{args.unmatched_policy}"
        )
    if args.min_overlap_ratio is not None and not 0.0 <= args.min_overlap_ratio <= 1.0:
        raise ValueError(f"--min-overlap-ratio must be between 0 and 1: {args.min_overlap_ratio:g}")
    if args.max_correction_semitones is not None and args.max_correction_semitones < 0.0:
        raise ValueError(
            f"--max-correction-semitones must be non-negative: {args.max_correction_semitones:g}"
        )

    # Only what the caller spelled is forwarded: 0 is legal for both, so neither
    # can double as the sentinel that asks for the library default.
    options: dict[str, float] = {}
    if args.min_overlap_ratio is not None:
        options["min_overlap_ratio"] = args.min_overlap_ratio
    if args.max_correction_semitones is not None:
        options["max_correction_semitones"] = args.max_correction_semitones

    samples, sr = _load_audio(args.file)
    # Read ahead of the pitch track, which is what the command costs: the native
    # front-end receives its audio already decoded and reads the reference next,
    # so refusing an unreadable one here keeps the two orders identical.
    targets = note_targets_from_smf(
        _read_bounded(args.reference_smf, _MAX_PROJECT_OR_MIDI_BYTES),
        track_index=args.track,
    )
    pitch = pitch_pyin(samples, sample_rate=sr, hop_length=_TUNE_HOP_LENGTH)
    notes = extract_notes(
        samples,
        sr,
        pitch.f0,
        sr / _TUNE_HOP_LENGTH,
        voiced=[int(value) for value in pitch.voiced_flag],
    )
    tuned, assigned_count = assign_note_targets(
        notes,
        sr,
        targets,
        unmatched_policy=args.unmatched_policy,
        **options,
    )
    rendered = render_notes(samples, sr, tuned)

    return _emit_effect_result(
        args,
        [[float(value) for value in rendered]],
        sr,
        # Zero assigned is a legitimate answer -- a reference that does not line up
        # with the take -- so it is reported rather than raised.
        extra={"assigned_count": assigned_count, "note_count": len(notes)},
        label="Tune to MIDI",
    )


# The fields one --edit occurrence may address, in the order the refusal below
# names them. The amplitude envelope is the one NoteEdit field missing from the
# set: it is a curve, and nothing on a command line states one.
_POLYPHONIC_EDIT_FIELDS = (
    "pitch_shift_semitones",
    "gain_db",
    "time_offset_samples",
    "time_stretch_ratio",
    "formant_shift_semitones",
    "vibrato_depth_change",
    "drift_change",
    "muted",
)
_POLYPHONIC_EDIT_FLOAT_FIELDS = frozenset(_POLYPHONIC_EDIT_FIELDS) - {
    "time_offset_samples",
    "muted",
}


def _edit_value_consumed_whole(value: str) -> bool:
    """Whether the native numeric parsers would read ``value`` to its end.

    They skip leading whitespace and then require the conversion to reach the end
    of the string, so a PEP 515 underscore and a trailing space are refusals there
    rather than 10 and 3.
    """
    return "_" not in value and value == value.rstrip()


def _parse_polyphonic_edit_float(value: str) -> float:
    """Read one ``--edit`` value as a finite float, refusing anything else."""
    parsed = None
    if _edit_value_consumed_whole(value):
        try:
            parsed = float(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"invalid float value for --edit: {value}") from exc
    if parsed is None:
        raise ValueError(f"invalid float value for --edit: {value}")
    if not math.isfinite(parsed):
        raise ValueError(f"numeric value for --edit must be finite: {value}")
    return parsed


def _parse_polyphonic_edit_int(value: str) -> int:
    """Read one ``--edit`` value as an integer, refusing anything else.

    Bounded to a C ``int``, which is the whole reachable range: the chain refuses
    audio longer than an int can index, so a wider offset only ever moves a note
    off the end.
    """
    parsed = None
    if _edit_value_consumed_whole(value):
        try:
            parsed = int(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"invalid integer value for --edit: {value}") from exc
    if parsed is None or not _C_INT_MIN <= parsed <= _C_INT_MAX:
        raise ValueError(f"invalid integer value for --edit: {value}")
    return parsed


def _parse_polyphonic_edit_bool(field: str, value: str) -> bool:
    """Read one ``--edit`` value as a flag, using the parser's own literal sets."""
    from .cli import _FALSE_FLAG_LITERALS, _TRUE_FLAG_LITERALS

    lowered = value.lower()
    if lowered in _TRUE_FLAG_LITERALS:
        return True
    if lowered in _FALSE_FLAG_LITERALS:
        return False
    raise ValueError(f"invalid boolean value for --edit {field}: {value}")


def _apply_polyphonic_note_edit(edits: list[NoteEdit], assignment: str) -> int:
    """Apply one ``NOTE.FIELD=VALUE`` assignment in place, returning the note index.

    One occurrence is one assignment, as ``--set`` does, with the note index as
    the dot path's first element: an edit addresses a note rather than a JSON
    document, and nothing else about the spelling has to differ.
    """
    path, separator, value = assignment.partition("=")
    if not separator or not path:
        raise ValueError(f"invalid --edit assignment: {assignment}")
    index_text, dot, field = path.rpartition(".")
    if not dot or not index_text or not field:
        raise ValueError(f"--edit path must be NOTE.FIELD: {assignment}")
    index = _parse_polyphonic_edit_int(index_text)
    if index < 0 or index >= len(edits):
        raise ValueError(
            f"--edit note index out of range: {index_text} (the analysis found {len(edits)} notes)"
        )
    edit = edits[index]
    if field == "time_offset_samples":
        edit.time_offset_samples = _parse_polyphonic_edit_int(value)
    elif field == "muted":
        edit.muted = _parse_polyphonic_edit_bool(field, value)
    elif field in _POLYPHONIC_EDIT_FLOAT_FIELDS:
        setattr(edit, field, _parse_polyphonic_edit_float(value))
    else:
        # The accepted set is named at the refusal because the help carries no
        # per-field text, so this is the only place a caller can read it.
        raise ValueError(
            f"unknown --edit field: {field} (expected one of {', '.join(_POLYPHONIC_EDIT_FIELDS)})"
        )
    return index


# Both commands run the chain at its own defaults, so the indices polyphonic-notes
# reports are the ones polyphonic-render edits. A framing option on one of the two
# would have to be spelled identically on the other, and a caller spelling it
# differently would silently renumber the notes.
def cmd_polyphonic_notes(args: argparse.Namespace) -> int:
    """Report the notes a polyphonic analysis found, with their per-frame curves."""
    from . import PolyphonicAnalysis

    samples, sr = _load_audio(args.file)
    with PolyphonicAnalysis.analyze(samples, sr) as analysis:
        frame_count = analysis.frame_count()
        polyphony = [int(count) for count in analysis.polyphony()]
        rows: list[dict[str, object]] = [
            {
                "index": index,
                "onset_sample": note.onset_sample,
                "offset_sample": note.offset_sample,
                "frame_start": note.frame_start,
                "frame_end": note.frame_end,
                "median_hz": note.median_hz,
                "median_cents": note.median_cents,
                "f0_stability": note.f0_stability,
                "f0_hz": [float(value) for value in analysis.note_f0(index)],
                "amplitude": [float(value) for value in note.amplitude],
                "salience": [float(value) for value in analysis.note_salience(index)],
            }
            for index, note in enumerate(analysis.notes())
        ]

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "sample_rate": sr,
                    "frame_count": frame_count,
                    "note_count": len(rows),
                    "polyphony": polyphony,
                    "notes": rows,
                }
            )
        )
    else:
        print(f"Polyphonic notes: {len(rows)}")
        print(f"  Frames: {frame_count} @ {sr} Hz")
        for row in rows:
            print(
                f"  [{row['index']}] "
                f"samples {row['onset_sample']}-{row['offset_sample']}  "
                f"frames {row['frame_start']}-{row['frame_end']}  "
                f"{cast(float, row['median_hz']):.2f} Hz  "
                f"stability {cast(float, row['f0_stability']):.3f}"
            )
    return 0


def cmd_polyphonic_render(args: argparse.Namespace) -> int:
    """Re-render a polyphonic analysis with whatever ``--edit`` assigned."""
    from . import PolyphonicAnalysis

    # Checked before the analysis rather than in _emit_effect_result: the chain is
    # the expensive part of the command and a missing destination is already known.
    if not args.output:
        print("Error: polyphonic-render requires an output file (-o/--output)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER

    assignments = list(getattr(args, "edit", None) or [])
    samples, sr = _load_audio(args.file)
    with PolyphonicAnalysis.analyze(samples, sr) as analysis:
        note_count = analysis.note_count()
        if assignments:
            # One NoteEdit per note, mutated in place, so several --edit
            # occurrences on one note accumulate instead of replacing each other.
            edits = [note.edit for note in analysis.notes()]
            touched: list[int] = []
            for assignment in assignments:
                index = _apply_polyphonic_note_edit(edits, assignment)
                if index not in touched:
                    touched.append(index)
            for index in touched:
                analysis.set_note_edit(index, edits[index])
        result = [float(value) for value in analysis.render()]

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={"note_count": note_count, "edits": len(assignments)},
        label="Polyphonic render",
    )


def register_note_edit_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the note and pitch editing commands."""
    common = shared.common
    stdout_options = shared.stdout_options

    pitch_correct_p = sub.add_parser(
        "pitch-correct", parents=[common], help="Pitch-correct from a current to a target MIDI note"
    )
    pitch_correct_p.add_argument(
        "--current-midi",
        type=_finite_float,
        default=69.0,
        help="Current pitch as a MIDI note number",
    )
    pitch_correct_p.add_argument(
        "--target-midi", type=_finite_float, default=69.0, help="Target pitch as a MIDI note number"
    )
    pitch_tv_p = sub.add_parser(
        "pitch-correct-timevarying",
        parents=[common],
        help="Track pYIN contour and correct toward a note or scale",
    )
    pitch_tv_p.add_argument("--mode", choices=["midi", "scale"], default="midi")
    # The scale arguments are checked whichever target mode is selected, and
    # --target-midi names a note in both, so each declares its range once rather
    # than on the branch that happens to read it. --reference-midi stays
    # undeclared: this path validates the anchor for finiteness only, and a
    # range here would refuse a value the library accepts.
    _cli_domain(
        pitch_tv_p.add_argument("--target-midi", type=_finite_float, default=69.0),
        minimum=0,
        maximum=127,
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        pitch_tv_p.add_argument("--hop-length", type=int, default=512),
        minimum=0,
        exclusive_minimum=True,
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        pitch_tv_p.add_argument("--scale-root", type=int, default=0),
        minimum=0,
        maximum=11,
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        pitch_tv_p.add_argument(
            "--scale-mode-mask", type=lambda value: int(value, 0), default=0xAB5
        ),
        minimum=1,
        maximum=4095,
        reject_exit="invalid_parameter",
    )
    pitch_tv_p.add_argument("--reference-midi", type=_finite_float, default=69.0)
    note_move_p = sub.add_parser("note-move", parents=[common], help="Move one note region")
    note_move_p.add_argument("--onset", type=int, default=0)
    note_move_p.add_argument("--offset", type=int, default=None)
    # The library's own default, not None: note_move takes an int, so an absent
    # --target-onset reached ctypes as None and the command could not run with
    # its own defaults at all. --offset keeps None because the handler resolves
    # it to the end of the buffer, which is not a number a default can spell.
    note_move_p.add_argument("--target-onset", type=int, default=0)
    scale_quantize_p = sub.add_parser(
        "scale-quantize", parents=[stdout_options], help="Quantize one MIDI value to a scale"
    )
    scale_quantize_p.add_argument("midi", type=_finite_float)
    # The parser accepts all three and the library refuses them after parsing,
    # so their accepted sets are declared here rather than left for a reader to
    # infer from where the refusal happens to live. ``--reference-midi`` takes 0
    # as the sentinel for the library default, which is why the range is closed
    # at the bottom instead of starting above it.
    _cli_domain(
        scale_quantize_p.add_argument("--root", type=int, default=0),
        minimum=0,
        maximum=11,
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        scale_quantize_p.add_argument(
            "--mode-mask", type=lambda value: int(value, 0), default=0xAB5
        ),
        minimum=1,
        maximum=4095,
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        scale_quantize_p.add_argument("--reference-midi", type=_finite_float, default=69.0),
        minimum=0,
        maximum=127,
        reject_exit="invalid_parameter",
    )
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
        "--ratio",
        type=_finite_float,
        default=1.0,
        help="Stretch factor for the region (>1 lengthens)",
    )
    tune_to_midi_p = sub.add_parser(
        "tune-to-midi",
        parents=[common],
        help="Tune a take to the reference melody a MIDI file carries",
    )
    tune_to_midi_p.add_argument(
        "--reference-smf",
        required=True,
        metavar="PATH",
        help="Standard MIDI File holding the reference melody",
    )
    # Indexes the MIDI-bearing tracks, not the SMF's own numbering, so a file
    # whose first track is a conductor track has its melody at 0.
    _cli_domain(
        tune_to_midi_p.add_argument(
            "--track",
            type=int,
            default=0,
            metavar="N",
            help="MIDI-bearing track the melody is read from (default: 0)",
        ),
        minimum=0,
        reject_exit="invalid_parameter",
    )
    # Declared rather than given to argparse as choices=: an unknown name is
    # refused by the handler, which carries the invalid-parameter class the
    # command's other value domains carry rather than argparse's usage class.
    _cli_domain(
        tune_to_midi_p.add_argument(
            "--unmatched-policy",
            default=_DEFAULT_UNMATCHED_POLICY,
            metavar="{" + ",".join(_UNMATCHED_POLICY_NAMES) + "}",
            help=(
                "What to do with a note that has a pitch and no target: "
                + ", ".join(_UNMATCHED_POLICY_NAMES)
                + f" (default: {_DEFAULT_UNMATCHED_POLICY})"
            ),
        ),
        choices=_UNMATCHED_POLICY_NAMES,
        reject_exit="invalid_parameter",
    )
    # Neither of the next two carries a CLI default: 0 is a legal value for both,
    # so an absent flag has to reach the handler as None and leave the library's
    # own default in place.
    _cli_domain(
        tune_to_midi_p.add_argument(
            "--min-overlap-ratio",
            type=_finite_float,
            default=None,
            metavar="R",
            help="Fraction of a note that must overlap a target (library default: 0.5)",
        ),
        minimum=0.0,
        maximum=1.0,
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        tune_to_midi_p.add_argument(
            "--max-correction-semitones",
            type=_finite_float,
            default=None,
            metavar="S",
            help="Where an assigned pitch shift saturates (library default: 12)",
        ),
        minimum=0.0,
        reject_exit="invalid_parameter",
    )
    sub.add_parser(
        "polyphonic-notes",
        parents=[stdout_options],
        help="List the notes a polyphonic analysis found",
    )
    polyphonic_render_p = sub.add_parser(
        "polyphonic-render",
        parents=[common],
        help="Re-render a polyphonic analysis with per-note edits",
    )
    # One assignment per occurrence, as --set does: the value reaches the field
    # parser as written, so no separator a fold could pick has to be reserved.
    polyphonic_render_p.add_argument(
        "--edit",
        action="append",
        default=[],
        metavar="NOTE.FIELD=VALUE",
        help=(
            "Edit one field of one note, repeatable; FIELD is one of "
            + ", ".join(_POLYPHONIC_EDIT_FIELDS)
        ),
    )
