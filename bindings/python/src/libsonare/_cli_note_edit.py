"""Command-line interface for libsonare: note and pitch editing commands."""

from __future__ import annotations

import argparse
import math
from typing import TYPE_CHECKING, Any, cast

from ._cli_common import (
    _MAX_PROJECT_OR_MIDI_BYTES,
    PITCH_NAMES,
    _audio_array,
    _emit_effect_result,
    _fmt_optional,
    _omit_absent,
    _read_bounded,
    _strict_json_dumps,
    _window_length_at_rate,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain, _token_of
from ._cli_options import (
    SharedParsers,
    _c_int,
    _closed_unit_float,
    _ContractArgumentParser,
    _finite_float,
    _given,
    _mode_mask,
    _NumericTextError,
    _pitch_threshold,
    _positive_finite_float,
    _positive_int,
    _read_integer_text,
    _read_number_text,
)
from ._effects_note_ops import _UNMATCHED_POLICIES
from ._narrowing import _resolve_sample_bound

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
    # A setting the caller left out is not passed, so the library chooses it. The
    # contour and the correction share one hop, as the same flag names both.
    hop = _given(args, "hop_length")
    track = pitch_pyin(
        samples,
        sample_rate=sr,
        frame_length=_window_length_at_rate(_PYIN_FRAME_LENGTH, sr),
        **hop,
    )
    result = pitch_correct_timevarying(
        samples,
        f0_hz=track.f0,
        sample_rate=sr,
        target_mode=args.mode,
        target_midi=args.target_midi,
        **hop,
        **_given(
            args,
            "scale_root",
            "scale_mode_mask",
            "retune_amount",
            "max_correction_semitones",
            "retune_speed_ms",
            "vibrato_threshold_cents",
            scale_reference_midi="reference_midi",
        ),
        voiced=[int(value) for value in track.voiced_flag],
        voiced_prob=track.voiced_prob,
    )
    return _emit_effect_result(args, [result], sr, label="Time-varying pitch correct")


def _note_bound_sample(args: argparse.Namespace, bound: str, sample_rate: int) -> int | None:
    """Resolve ``--<bound>-sample`` / ``--<bound>-sec`` to a sample index, or None if absent."""
    attr = bound.replace("-", "_")
    return _resolve_sample_bound(
        getattr(args, f"{attr}_sample"),
        getattr(args, f"{attr}_sec"),
        sample_rate,
        f"--{bound}-sample",
        f"--{bound}-sec",
    )


def cmd_note_move(args: argparse.Namespace) -> int:
    from . import note_move

    samples, sr = _load_audio(args.file)
    result = note_move(
        samples,
        sample_rate=sr,
        onset_sample=_note_bound_sample(args, "onset", sr),
        offset_sample=_note_bound_sample(args, "offset", sr),
        target_onset_sample=_note_bound_sample(args, "target-onset", sr),
    )
    return _emit_effect_result(args, [result], sr, label="Note move")


def cmd_scale_quantize(args: argparse.Namespace) -> int:
    from . import scale_quantize_midi

    # The flag keeps the command line's 0 = A4.
    value = scale_quantize_midi(
        args.root, args.mode_mask, args.midi, scale_reference_midi=args.reference_midi or None
    )
    if args.json:
        print(_strict_json_dumps({"input_midi": args.midi, "quantized_midi": value}))
    else:
        print(f"{value:.6f}")
    return 0


def cmd_note_stretch(args: argparse.Namespace) -> int:
    from . import note_stretch

    samples, sr = _load_audio(args.file)
    onset = _note_bound_sample(args, "onset", sr)
    onset = 0 if onset is None else onset
    # An omitted offset means the end of the buffer, as for note-move.
    offset = _note_bound_sample(args, "offset", sr)
    offset = len(samples) if offset is None else offset
    result = note_stretch(
        samples,
        sample_rate=sr,
        onset_sample=onset,
        offset_sample=offset,
        stretch_ratio=args.ratio,
    )

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={
            "onset_sample": onset,
            "offset_sample": offset,
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
# pYIN frame length at the analysis rate, rescaled to the input rate like the native CLI.
_PYIN_FRAME_LENGTH = 2048


def cmd_tune_to_midi(args: argparse.Namespace) -> int:
    """Tune a take to the reference melody a MIDI file carries."""
    from . import (
        assign_note_targets,
        extract_notes,
        note_targets_from_smf,
        pitch_pyin,
        render_notes,
    )

    # Only what the caller spelled is forwarded: 0 is legal for both, so neither
    # can double as the sentinel that asks for the library default.
    options: dict[str, Any] = _given(args, "unmatched_policy")
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
    pitch = pitch_pyin(
        samples,
        sample_rate=sr,
        frame_length=_window_length_at_rate(_PYIN_FRAME_LENGTH, sr),
        hop_length=_TUNE_HOP_LENGTH,
    )
    notes = extract_notes(
        samples,
        sr,
        pitch.f0,
        sr / _TUNE_HOP_LENGTH,
        voiced=[int(value) for value in pitch.voiced_flag],
    )
    assignment = assign_note_targets(
        notes,
        sr,
        targets,
        **options,
    )
    tuned, assigned_count = assignment.notes, assignment.assigned_count
    rendered = render_notes(samples, sr, tuned)

    return _emit_effect_result(
        args,
        [_audio_array(rendered)],
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
# The command line keeps the native CLI's field tokens; these two name a
# differently spelled NoteEdit attribute.
_POLYPHONIC_EDIT_ATTRIBUTES = {
    "pitch_shift_semitones": "pitch_semitones",
    "formant_shift_semitones": "formant_semitones",
}


def _parse_polyphonic_edit_float(field: str, value: str) -> float:
    """Read one ``--edit`` value with the CLI's numeric reader, the field as the subject."""
    try:
        return _read_number_text(value)
    except _NumericTextError as exc:
        raise ValueError(f"argument --edit: {field} {exc}") from None


def _parse_polyphonic_edit_int(subject: str, value: str) -> int:
    """Read one ``--edit`` integer, bounded to a C ``int``.

    That is the whole reachable range: the chain refuses audio longer than an
    int can index, so a wider offset only ever moves a note off the end.
    """
    try:
        return _read_integer_text(value)
    except _NumericTextError as exc:
        raise ValueError(f"argument --edit: {subject} {exc}") from None


def _parse_polyphonic_edit_bool(field: str, value: str) -> bool:
    """Read one ``--edit`` value as a flag, using the parser's own literal sets."""
    from .cli import _FALSE_FLAG_LITERALS, _TRUE_FLAG_LITERALS

    lowered = value.lower()
    if lowered in _TRUE_FLAG_LITERALS:
        return True
    if lowered in _FALSE_FLAG_LITERALS:
        return False
    raise ValueError(f"argument --edit: {field} must be one of true, 1, yes, on, false, 0, no, off")


def _apply_polyphonic_note_edit(edits: list[NoteEdit], assignment: str) -> int:
    """Apply one ``NOTE.FIELD=VALUE`` assignment in place, returning the note index.

    One occurrence is one assignment, as ``--set`` does, with the note index as
    the dot path's first element: an edit addresses a note rather than a JSON
    document, and nothing else about the spelling has to differ.
    """
    path, separator, value = assignment.partition("=")
    if not separator or not path:
        raise ValueError("argument --edit: must be NOTE.FIELD=VALUE")
    index_text, dot, field = path.rpartition(".")
    if not dot or not index_text or not field:
        raise ValueError("argument --edit: must be NOTE.FIELD=VALUE")
    index = _parse_polyphonic_edit_int("NOTE", index_text)
    if not edits:
        raise ValueError("argument --edit: the analysis found no notes")
    if index < 0 or index >= len(edits):
        raise ValueError(f"argument --edit: NOTE must be an integer in [0, {len(edits) - 1}]")
    edit = edits[index]
    if field == "time_offset_samples":
        edit.time_offset_samples = _parse_polyphonic_edit_int(field, value)
    elif field == "muted":
        edit.muted = _parse_polyphonic_edit_bool(field, value)
    elif field in _POLYPHONIC_EDIT_FLOAT_FIELDS:
        setattr(
            edit,
            _POLYPHONIC_EDIT_ATTRIBUTES.get(field, field),
            _parse_polyphonic_edit_float(field, value),
        )
    else:
        # The accepted set is named at the refusal because the help carries no
        # per-field text, so this is the only place a caller can read it.
        raise ValueError(
            f"argument --edit: FIELD must be one of {', '.join(_POLYPHONIC_EDIT_FIELDS)}"
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
        frame_count = analysis.frame_count
        polyphony = [int(count) for count in analysis.polyphony()]
        rows: list[dict[str, object]] = [
            _omit_absent(
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
                },
                "median_hz",
            )
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
                f"{_fmt_optional(cast('float | None', row['median_hz']), '.2f', ' Hz')}  "
                f"stability {cast(float, row['f0_stability']):.3f}"
            )
    return 0


# Each analysis setting a vocal session takes at creation, as (argparse dest,
# VocalEditSession keyword). A setting the caller left out is not
# passed, so the session's own default applies.
_VOCAL_NOTES_SETTINGS = (
    ("segmentation_threshold_cents", "segmentation_threshold_cents"),
    ("min_note_ms", "min_note_ms"),
    ("frame_length_samples", "frame_length_samples"),
    ("hop_length_samples", "hop_length_samples"),
    ("f0_min_hz", "fmin_hz"),
    ("f0_max_hz", "fmax_hz"),
    ("yin_threshold", "yin_threshold"),
    ("voiced_threshold", "voiced_threshold"),
    ("reference_hz", "reference_hz"),
)


def _nearest_note(midi: float) -> tuple[str, float]:
    """Return the nearest equal-tempered note name and the signed cents to it.

    Ties round up, as the native CLI does.
    """
    nearest = math.floor(midi + 0.5)
    return f"{PITCH_NAMES[nearest % 12]}{nearest // 12 - 1}", (midi - nearest) * 100.0


def cmd_vocal_notes(args: argparse.Namespace) -> int:
    """List the notes a vocal-edit session finds in a take; nothing is edited or rendered."""
    from . import VocalEditSession

    settings: dict[str, Any] = {
        keyword: getattr(args, dest)
        for dest, keyword in _VOCAL_NOTES_SETTINGS
        if getattr(args, dest, None) is not None
    }
    if getattr(args, "no_centered", False):
        settings["centered"] = False

    samples, sr = _load_audio(args.file)
    session = VocalEditSession(samples, sr, **settings)
    try:
        notes = session.notes().notes
    finally:
        session.close()

    rows: list[dict[str, object]] = []
    for index, note in enumerate(notes):
        row: dict[str, object] = {
            "index": index,
            "id": note.id,
            "source_start_sample": note.source_start_sample,
            "source_end_sample": note.source_end_sample,
            "start_sec": note.source_start_sample / sr,
            "end_sec": note.source_end_sample / sr,
            "analysis_frame_start": note.analysis_frame_start,
            "analysis_frame_end": note.analysis_frame_end,
        }
        # An unpitched note has no pitch to report, so its pitch keys are omitted.
        if note.center_midi is not None:
            name, cents = _nearest_note(note.center_midi)
            row.update(
                median_hz=note.median_hz,
                center_midi=note.center_midi,
                f0_stability=note.f0_stability,
                note_name=name,
                cents=cents,
            )
        row["amplitude"] = [float(value) for value in note.amplitude]
        rows.append(row)

    if args.json:
        print(_strict_json_dumps({"sample_rate": sr, "note_count": len(rows), "notes": rows}))
    else:
        print(f"Vocal notes: {len(rows)}")
        for row in rows:
            span = f"{cast(float, row['start_sec']):.3f}-{cast(float, row['end_sec']):.3f} s"
            if "center_midi" in row:
                print(
                    f"  [{row['index']}] {span}  "
                    f"{cast(str, row['note_name']):<4s} {cast(float, row['cents']):+6.1f} cents  "
                    f"midi {cast(float, row['center_midi']):.2f}  "
                    f"{cast(float, row['median_hz']):.2f} Hz  "
                    f"stability {cast(float, row['f0_stability']):.3f}"
                )
            else:
                print(f"  [{row['index']}] {span}  unpitched")
    return 0


def cmd_polyphonic_render(args: argparse.Namespace) -> int:
    """Re-render a polyphonic analysis with whatever ``--edit`` assigned."""
    from . import PolyphonicAnalysis

    # Checked before the analysis rather than in _emit_effect_result: the chain is
    # the expensive part of the command and a missing destination is already known.

    assignments = list(getattr(args, "edit", None) or [])
    samples, sr = _load_audio(args.file)
    with PolyphonicAnalysis.analyze(samples, sr) as analysis:
        note_count = analysis.note_count
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
        result = _audio_array(analysis.render())

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={"note_count": note_count, "edits": len(assignments)},
        label="Polyphonic render",
    )


def _position(action: argparse.Action) -> argparse.Action:
    """Publish a note-region position's domain; the handler refuses a negative one."""
    return _cli_domain(action, minimum=0)


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
        description=(
            "Track pYIN contour and correct toward a note or scale. In --mode scale the "
            "target is chosen per frame, so a note whose vibrato crosses the midpoint "
            "between two scale notes is pulled toward both and its vibrato widens."
        ),
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
    )
    _cli_domain(
        pitch_tv_p.add_argument(
            "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
        ),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        pitch_tv_p.add_argument(
            "--scale-root", type=_c_int, default=None, help="Scale root pitch class (default: 0)"
        ),
        minimum=0,
        maximum=11,
    )
    _cli_domain(
        pitch_tv_p.add_argument(
            "--scale-mode-mask",
            type=_mode_mask,
            default=None,
            help="12-bit scale mask or a mode name such as major or dorian (default: 2741)",
        ),
        minimum=1,
        maximum=4095,
    )
    pitch_tv_p.add_argument(
        "--reference-midi",
        type=_finite_float,
        default=None,
        help="MIDI note the scale grid is anchored at (default: 69.0)",
    )
    # The library refuses these outside the ranges below, so each is declared
    # with the class of exit that refusal carries.
    _cli_domain(
        pitch_tv_p.add_argument(
            "--retune-amount",
            type=_finite_float,
            default=None,
            help="Correction strength, 0 to 1 (default: 1.0)",
        ),
        minimum=0,
        maximum=1,
    )
    _cli_domain(
        pitch_tv_p.add_argument(
            "--max-correction-semitones",
            type=_finite_float,
            default=None,
            help="Largest correction in semitones (default: 12.0)",
        ),
        minimum=0,
    )
    _cli_domain(
        pitch_tv_p.add_argument(
            "--retune-speed-ms",
            type=_finite_float,
            default=None,
            help="Retune time constant in ms (default: 50.0)",
        ),
        minimum=0,
    )
    _cli_domain(
        pitch_tv_p.add_argument(
            "--vibrato-threshold-cents",
            type=_finite_float,
            default=None,
            help="Vibrato below this depth is preserved, in cents (default: 20.0)",
        ),
        minimum=0,
    )
    note_move_p = sub.add_parser("note-move", parents=[common], help="Move one note region")
    _position(
        note_move_p.add_argument(
            "--onset-sample",
            type=_c_int,
            help="Start of the note region (omitted: the start of the input)",
        )
    )
    _position(
        note_move_p.add_argument(
            "--onset-sec",
            type=_finite_float,
            help="Start of the note region (omitted: the start of the input)",
        )
    )
    _position(
        note_move_p.add_argument(
            "--offset-sample",
            type=_c_int,
            help="End of the note region (omitted: the end of the input)",
        )
    )
    _position(
        note_move_p.add_argument(
            "--offset-sec",
            type=_finite_float,
            help="End of the note region (omitted: the end of the input)",
        )
    )
    _position(
        note_move_p.add_argument(
            "--target-onset-sample",
            type=_c_int,
            help="Where the start of the region moves to (omitted: the start of the input)",
        )
    )
    _position(
        note_move_p.add_argument(
            "--target-onset-sec",
            type=_finite_float,
            help="Where the start of the region moves to (omitted: the start of the input)",
        )
    )
    scale_quantize_p = sub.add_parser(
        "scale-quantize", parents=[stdout_options], help="Quantize one MIDI value to a scale"
    )
    scale_quantize_p.add_argument("midi", type=_finite_float)
    # The parser accepts all three and the library refuses them after parsing,
    # so their accepted sets are declared here rather than left for a reader to
    # infer from where the refusal happens to live. ``--reference-midi`` is
    # closed at 0 because the anchor is a MIDI note; the C ABI reads 0 as the
    # library default, which is the same grid as any other whole-number anchor.
    _cli_domain(
        scale_quantize_p.add_argument("--root", type=_c_int, default=0),
        minimum=0,
        maximum=11,
    )
    _cli_domain(
        scale_quantize_p.add_argument(
            "--mode-mask",
            type=_mode_mask,
            default=0xAB5,
            help="12-bit scale mask or a mode name such as major or dorian",
        ),
        minimum=1,
        maximum=4095,
    )
    _cli_domain(
        scale_quantize_p.add_argument("--reference-midi", type=_finite_float, default=69.0),
        minimum=0,
        maximum=127,
    )
    note_stretch_p = sub.add_parser(
        "note-stretch", parents=[common], help="Time-stretch a single note region"
    )
    _position(
        note_stretch_p.add_argument(
            "--onset-sample",
            type=_c_int,
            help="Start of the note region (omitted: the start of the input)",
        )
    )
    _position(
        note_stretch_p.add_argument(
            "--onset-sec",
            type=_finite_float,
            help="Start of the note region (omitted: the start of the input)",
        )
    )
    _position(
        note_stretch_p.add_argument(
            "--offset-sample",
            type=_c_int,
            help="End of the note region (omitted: the end of the input)",
        )
    )
    _position(
        note_stretch_p.add_argument(
            "--offset-sec",
            type=_finite_float,
            help="End of the note region (omitted: the end of the input)",
        )
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
        metavar="PATH",
        help="Standard MIDI File holding the reference melody",
    )
    # Indexes the MIDI-bearing tracks, not the SMF's own numbering, so a file
    # whose first track is a conductor track has its melody at 0.
    _cli_domain(
        tune_to_midi_p.add_argument(
            "--track",
            type=_c_int,
            default=0,
            metavar="N",
            help="MIDI-bearing track the melody is read from",
        ),
        minimum=0,
    )
    # Declared rather than given to argparse as choices=, so the refusal states the
    # library's predicate like the command's other value domains.
    _cli_domain(
        tune_to_midi_p.add_argument(
            "--unmatched-policy",
            type=_token_of(_UNMATCHED_POLICY_NAMES),
            default=None,
            metavar="{" + ",".join(_UNMATCHED_POLICY_NAMES) + "}",
            help=(
                "What to do with a note that has a pitch and no target: "
                + ", ".join(_UNMATCHED_POLICY_NAMES)
                + f" (default: {_DEFAULT_UNMATCHED_POLICY})"
            ),
        ),
        choices=_UNMATCHED_POLICY_NAMES,
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
    )
    sub.add_parser(
        "polyphonic-notes",
        parents=[stdout_options],
        help="List the notes a polyphonic analysis found",
    )
    # The analysis settings a vocal session takes at creation. None carries a CLI
    # default: an omitted one reaches the session unspelled, so its own default
    # applies and the help names it rather than the parser restating it.
    vocal_notes_p = sub.add_parser(
        "vocal-notes",
        parents=[stdout_options],
        help="List the notes a vocal-edit session finds in a take",
    )
    vocal_notes_p.add_argument(
        "--segmentation-threshold-cents",
        type=_positive_finite_float,
        default=None,
        metavar="CENTS",
        help="Pitch change that starts a new note (session default: 50)",
    )
    vocal_notes_p.add_argument(
        "--min-note-ms",
        type=_positive_finite_float,
        default=None,
        metavar="MS",
        help="Shortest note kept (session default: 30)",
    )
    vocal_notes_p.add_argument(
        "--frame-length-samples",
        type=_positive_int,
        default=None,
        metavar="N",
        help="Analysis frame length in samples (session default: 2048)",
    )
    vocal_notes_p.add_argument(
        "--hop-length-samples",
        type=_positive_int,
        default=None,
        metavar="N",
        help="Analysis hop length in samples (session default: 512)",
    )
    vocal_notes_p.add_argument(
        "--f0-min-hz",
        type=_positive_finite_float,
        default=None,
        metavar="HZ",
        help="Lowest tracked frequency (session default: 65)",
    )
    vocal_notes_p.add_argument(
        "--f0-max-hz",
        type=_positive_finite_float,
        default=None,
        metavar="HZ",
        help="Highest tracked frequency, above --f0-min-hz (session default: 2093)",
    )
    vocal_notes_p.add_argument(
        "--yin-threshold",
        type=_pitch_threshold,
        default=None,
        metavar="T",
        help="YIN threshold (> 0 and <= 1; session default: 0.1)",
    )
    vocal_notes_p.add_argument(
        "--voiced-threshold",
        type=_closed_unit_float,
        default=None,
        metavar="P",
        help="Voicing probability a frame needs (0 to 1; session default: 0.5)",
    )
    vocal_notes_p.add_argument(
        "--no-centered",
        action="store_true",
        help="Analyse without padding the take by half a frame (session default: padded)",
    )
    vocal_notes_p.add_argument(
        "--reference-hz",
        type=_positive_finite_float,
        default=None,
        metavar="HZ",
        help="Frequency of A4 that note numbers are measured from (session default: 440)",
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
