"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections.abc import Iterable
from typing import TYPE_CHECKING, Any, cast

from ._cli_common import (
    _MAX_PROJECT_OR_MIDI_BYTES,
    EXIT_INVALID_PARAMETER,
    _apply_voice_sets,
    _emit_effect_result,
    _legacy_exit_codes,
    _load_channels_or_downmix,
    _load_voice_preset_pack,
    _read_bounded,
    _resample,
    _strict_json_dumps,
    _write_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import SharedParsers, _finite_float
from ._effects_note_ops import _UNMATCHED_POLICIES
from ._runtime import _C_INT_MAX, _C_INT_MIN

if TYPE_CHECKING:
    from .analyzer import NoteEdit


def _print_diagnostic_warnings(result: Any) -> None:
    """Prints one stderr line per warning the acoustic synthesis reported.

    One line per entry rather than one joined line, so the two front-ends print
    the same thing: the native binary walks the diagnostic list the same way, and
    a message holding the join separator would be unreadable on either.
    """
    # Direct attribute access rather than a getattr default: a result type that
    # stopped carrying the field should fail here, not print nothing.
    for diagnostic in result.diagnostics:
        if diagnostic.severity != "warning":
            continue
        print(f"warning: {diagnostic.code}: {diagnostic.message}", file=sys.stderr)


def cmd_hpss(args: argparse.Namespace) -> int:
    from . import hpss, hpss_with_residual

    if not args.output:
        print("Error: hpss requires an output file (-o/--output)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER

    modes = [
        name
        for name in ("harmonic_only", "percussive_only", "with_residual")
        if bool(getattr(args, name, False))
    ]
    if len(modes) > 1:
        raise ValueError("hpss output modes are mutually exclusive: " + ", ".join(modes))

    kernel_harmonic = getattr(args, "kernel_harmonic", 31)
    kernel_percussive = getattr(args, "kernel_percussive", 31)
    n_fft = getattr(args, "n_fft", 2048)
    hop_length = getattr(args, "hop_length", 512)
    hard_mask = bool(getattr(args, "hard_mask", False))

    samples, sr = _load_audio(args.file)

    if modes == ["with_residual"]:
        separated = hpss_with_residual(
            samples,
            sample_rate=sr,
            kernel_harmonic=kernel_harmonic,
            kernel_percussive=kernel_percussive,
            n_fft=n_fft,
            hop_length=hop_length,
            hard_mask=hard_mask,
        )
        harmonic = [float(value) for value in cast(Iterable[float], separated["harmonic"])]
        percussive = [float(value) for value in cast(Iterable[float], separated["percussive"])]
        residual = [float(value) for value in cast(Iterable[float], separated["residual"])]
        output_sr = int(cast(int, separated.get("sampleRate", sr)))
    else:
        result = hpss(
            samples,
            sample_rate=sr,
            kernel_harmonic=kernel_harmonic,
            kernel_percussive=kernel_percussive,
            n_fft=n_fft,
            hop_length=hop_length,
            hard_mask=hard_mask,
        )
        harmonic = [float(value) for value in result.harmonic]
        percussive = [float(value) for value in result.percussive]
        residual = []
        output_sr = int(result.sample_rate)

    def _mean_abs(values: list[float]) -> float:
        return sum(abs(value) for value in values) / len(values) if values else 0.0

    h_energy = _mean_abs(harmonic)
    p_energy = _mean_abs(percussive)

    harmonic_path = ""
    percussive_path = ""
    residual_path = ""
    if args.output:
        base = args.output[:-4] if args.output.lower().endswith(".wav") else args.output
        if modes == ["harmonic_only"]:
            harmonic_path = f"{base}.wav"
            _write_wav(harmonic_path, harmonic, output_sr)
        elif modes == ["percussive_only"]:
            percussive_path = f"{base}.wav"
            _write_wav(percussive_path, percussive, output_sr)
        else:
            harmonic_path = f"{base}_harmonic.wav"
            percussive_path = f"{base}_percussive.wav"
            _write_wav(harmonic_path, harmonic, output_sr)
            _write_wav(percussive_path, percussive, output_sr)
            if modes == ["with_residual"]:
                residual_path = f"{base}_residual.wav"
                _write_wav(residual_path, residual, output_sr)

    if args.json:
        # Each component's energy is reported only when that component is part
        # of the requested output, so a single-component run publishes the same
        # keys on both CLIs -- the native one never separates the other half.
        payload: dict[str, object] = {
            "length": len(percussive) if modes == ["percussive_only"] else len(harmonic),
            "sample_rate": output_sr,
        }
        if modes != ["percussive_only"]:
            payload["harmonic_energy"] = round(h_energy, 6)
        if modes != ["harmonic_only"]:
            payload["percussive_energy"] = round(p_energy, 6)
        if residual:
            payload["residual_energy"] = round(_mean_abs(residual), 6)
        if harmonic_path:
            payload["harmonic"] = harmonic_path
        if percussive_path:
            payload["percussive"] = percussive_path
        if residual_path:
            payload["residual"] = residual_path
        print(_strict_json_dumps(payload))
    else:
        print(f"  HPSS: {len(harmonic)} samples")
        print(f"  Harmonic energy:   {h_energy:.6f}")
        print(f"  Percussive energy: {p_energy:.6f}")
        if residual:
            print(f"  Residual energy:   {_mean_abs(residual):.6f}")
        paths = [path for path in (harmonic_path, percussive_path, residual_path) if path]
        if paths:
            print(f"  Wrote: {', '.join(paths)}")
    return 0


def cmd_decompose_stems(args: argparse.Namespace) -> int:
    from . import decompose_stems

    if not args.output:
        print("Error: decompose-stems requires an output file (-o/--output)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER
    # The C side reads any other name as its own default rather than refusing it,
    # so an unrecognised initialiser is caught here or not at all.
    if args.init not in {"random", "nndsvd"}:
        raise ValueError("--init must be 'random' or 'nndsvd'")

    samples, sr = _load_audio(args.file)
    result = decompose_stems(
        samples,
        sample_rate=sr,
        n_components=args.n_components,
        n_fft=args.n_fft,
        hop_length=args.hop_length,
        n_iter=args.n_iter,
        beta=args.beta,
        init=args.init,
        mask_power=args.mask_power,
    )
    components = [
        [float(value) for value in component]
        for component in cast(Iterable[Iterable[float]], result["components"])
    ]
    output_sr = int(cast(int, result["sample_rate"]))

    # One file per component, numbered from 1, in the base-name shape `hpss`
    # uses for its two: the destination names the set, not a single file.
    base = args.output[:-4] if args.output.lower().endswith(".wav") else args.output
    paths = [f"{base}_component{index}.wav" for index in range(1, len(components) + 1)]
    for path, component in zip(paths, components, strict=True):
        _write_wav(path, component, output_sr)

    def _mean_abs(values: list[float]) -> float:
        return sum(abs(value) for value in values) / len(values) if values else 0.0

    energies = [round(_mean_abs(component), 6) for component in components]
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "count": len(components),
                    "length": len(components[0]) if components else 0,
                    "sample_rate": output_sr,
                    "energies": energies,
                    "components": paths,
                }
            )
        )
    else:
        print(f"  Stems: {len(components)} components")
        for index, (path, energy) in enumerate(zip(paths, energies, strict=True), start=1):
            print(f"    {index:2d}. energy {energy:.6f}  {path}")
        if paths:
            print(f"  Wrote: {', '.join(paths)}")
    return 0


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


def cmd_pitch_shift(args: argparse.Namespace) -> int:
    from . import pitch_shift

    if args.semitones is None:
        raise ValueError("--semitones required")
    samples, sr = _load_audio(args.file)
    result = pitch_shift(
        samples,
        sample_rate=sr,
        semitones=args.semitones,
        n_fft=getattr(args, "n_fft", 2048),
        hop_length=getattr(args, "hop_length", 512),
    )

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={"semitones": args.semitones},
        label=f"Pitch shift ({args.semitones:+.2f} semitones)",
    )


def cmd_time_stretch(args: argparse.Namespace) -> int:
    from . import time_stretch

    if args.rate is None:
        raise ValueError("--rate required")
    samples, sr = _load_audio(args.file)
    result = time_stretch(
        samples,
        sample_rate=sr,
        rate=args.rate,
        n_fft=getattr(args, "n_fft", 2048),
        hop_length=getattr(args, "hop_length", 512),
    )

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={"rate": args.rate},
        label=f"Time stretch (rate {args.rate:.4f})",
    )


def cmd_normalize(args: argparse.Namespace) -> int:
    from . import normalize, normalize_rms, normalize_rms_stereo, normalize_stereo

    planes, sr = _load_channels_or_downmix(args.file)
    mode = getattr(args, "mode", "peak")
    target_db = getattr(args, "target_db", None)
    if target_db is None:
        target_db = -20.0 if mode == "rms" else 0.0
    if mode not in ("peak", "rms"):
        raise ValueError("--mode must be 'peak' or 'rms'")

    # One gain for the pair, measured across both channels. Normalizing each
    # channel to the target on its own would drive the quieter one up by a
    # different amount and collapse the image toward the centre -- for a source
    # 12 dB apart that is a factor of four on one side.
    if len(planes) == 2:
        stereo = (normalize_rms_stereo if mode == "rms" else normalize_stereo)(
            planes[0], planes[1], sample_rate=sr, target_db=target_db
        )
        channels = [stereo.left, stereo.right]
    else:
        mono = (normalize_rms if mode == "rms" else normalize)(
            planes[0], sample_rate=sr, target_db=target_db
        )
        channels = [mono]

    return _emit_effect_result(
        args,
        channels,
        sr,
        extra={"mode": mode, "target_db": target_db},
        label=f"Normalize ({mode}, target {target_db:.2f} dB)",
    )


def cmd_trim_silence(args: argparse.Namespace) -> int:
    from . import trim, trim_silence

    samples, sr = _load_audio(args.file)
    threshold_db = getattr(args, "threshold_db", None)
    top_db = getattr(args, "top_db", None)
    if threshold_db is not None and top_db is not None:
        raise ValueError("--threshold-db and --top-db are mutually exclusive")
    n_fft = getattr(args, "n_fft", 2048)
    hop_length = getattr(args, "hop_length", 512)
    if top_db is not None:
        result, _, _ = trim_silence(
            samples,
            top_db=top_db,
            frame_length=n_fft,
            hop_length=hop_length,
        )
        extra = {"top_db": top_db, "n_fft": n_fft, "hop_length": hop_length}
        label = f"Trim silence (top {top_db:.1f} dB)"
    else:
        if threshold_db is None:
            threshold_db = -60.0
        result = trim(
            samples,
            sample_rate=sr,
            threshold_db=threshold_db,
            frame_length=n_fft,
            hop_length=hop_length,
        )
        extra = {"threshold_db": threshold_db, "n_fft": n_fft, "hop_length": hop_length}
        label = f"Trim silence (threshold {threshold_db:.1f} dB)"

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra=extra,
        label=label,
        # trim-silence doubles as analysis (it reports the trimmed length), so an
        # output file is optional here, matching the native CLI.
        requires_output=False,
    )


def _split_silence_takes(args: argparse.Namespace) -> tuple[list[list[float]], int]:
    """Load the takes ``split-silence`` measures together, positional first."""
    samples, sr = _load_audio(args.file)
    takes = [samples]
    for path in getattr(args, "input", None) or []:
        take, take_sr = _load_audio(path)
        # Frame indices from two rates are not comparable, so a mismatch would
        # report intervals in units the caller cannot map back onto either take.
        if take_sr != sr:
            raise ValueError(
                f"take sample rate differs: {path} is {take_sr} Hz, the first take is {sr} Hz"
            )
        takes.append(take)
    return takes, sr


def _split_silence_slice(take: list[float], start: int, end: int) -> list[float]:
    """One interval of one take, padded where the take ended before it.

    A take shorter than the interval is silent past its end -- the rule the union
    was built on -- so the slice is padded rather than shortened and every take's
    file for one interval carries the same sample count.
    """
    window = take[start:end]
    if len(window) < end - start:
        window = window + [0.0] * (end - start - len(window))
    return window


def cmd_split_silence(args: argparse.Namespace) -> int:
    from . import split_silence_common_with_report

    top_db = getattr(args, "top_db", 60.0)
    n_fft = getattr(args, "n_fft", 2048)
    hop_length = getattr(args, "hop_length", 512)

    takes, sr = _split_silence_takes(args)
    # Always asked for, only sometimes printed: one call site rather than two
    # that could come to refuse differently.
    intervals, report = split_silence_common_with_report(
        takes,
        top_db=top_db,
        frame_length=n_fft,
        hop_length=hop_length,
    )

    write_takes = getattr(args, "write_takes", None)
    if write_takes:
        for take_index, take in enumerate(takes, start=1):
            for interval_index, (start, end) in enumerate(intervals, start=1):
                _write_wav(
                    f"{write_takes}{take_index:02d}_{interval_index:03d}.wav",
                    _split_silence_slice(take, start, end),
                    sr,
                )

    with_report = bool(getattr(args, "report", False))
    interval_rows = [{"start_sample": start, "end_sample": end} for start, end in intervals]
    if args.json:
        # --report wraps the result rather than replacing it, so without the flag
        # the payload is the bare array it has always been.
        if with_report:
            print(
                _strict_json_dumps(
                    {
                        "intervals": interval_rows,
                        "report": {
                            "silence_ceiling_db": report.silence_ceiling_db,
                            "max_signal_intervals": report.max_signal_intervals,
                            "min_signal_intervals": report.min_signal_intervals,
                        },
                    }
                )
            )
        else:
            print(_strict_json_dumps(interval_rows))
    else:
        print(f"Non-silent intervals: {len(intervals)}")
        for start, end in intervals:
            print(f"  {start} - {end}")
        if with_report:
            # Read against the --top-db in use, which is the whole rule: a ceiling
            # under it means the threshold was too loose for the quiet these takes
            # have, and one at or over it means the quiet is there and they do not
            # share it.
            print(
                f"  silence ceiling {report.silence_ceiling_db:.2f} dB "
                f"at --top-db {float(top_db):.2f}"
            )
            print(
                f"  intervals per signal: {report.min_signal_intervals}.."
                f"{report.max_signal_intervals}"
            )
        if write_takes:
            print(f"Wrote {len(takes) * len(intervals)} take files with prefix {write_takes}")
    return 0


def cmd_resample(args: argparse.Namespace) -> int:
    if not args.output:
        print("Error: resample requires an output file (-o/--output)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER

    target_rate = getattr(args, "target_rate", None)
    if target_rate is None:
        target_rate = getattr(args, "target_sr", None)
    if target_rate is None:
        raise ValueError("--target-rate required")
    samples, sr = _load_audio(args.file)
    result = _resample(samples, sr, target_rate)

    if args.output:
        _write_wav(args.output, result, target_rate)

    if args.json:
        payload: dict[str, object] = {
            "length": len(result),
            "source_rate": sr,
            "sample_rate": target_rate,
        }
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print(f"  Resample ({sr} -> {target_rate} Hz): {len(result)} samples")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_voice_change(args: argparse.Namespace) -> int:
    from . import (
        RealtimeVoiceChanger,
        realtime_voice_changer_preset_json,
        voice_change,
        voice_change_realtime,
    )

    raw_preset_id: object = getattr(args, "preset", "")
    raw_preset_json: object = getattr(args, "preset_json", None)
    raw_preset_pack: object = getattr(args, "preset_pack", None)
    preset_id = raw_preset_id if isinstance(raw_preset_id, str) else ""
    preset_json = raw_preset_json if isinstance(raw_preset_json, str) and raw_preset_json else None
    preset_pack = raw_preset_pack if isinstance(raw_preset_pack, str) and raw_preset_pack else None
    assignments = list(getattr(args, "set", None) or [])
    selectors = [
        (name, value)
        for name, value in (
            ("--preset", preset_id),
            ("--preset-json", preset_json),
        )
        if value
    ]
    if len(selectors) > 1:
        raise ValueError(
            "voice-change preset selectors are mutually exclusive: "
            + ", ".join(name for name, _ in selectors)
        )
    if preset_pack and preset_json:
        raise ValueError("--preset-pack and --preset-json are mutually exclusive")
    # A pack names the file, --preset names the entry inside it, so the pair is
    # one selector. This check precedes the ones below so that a pack without an
    # entry reports the missing --preset rather than a downstream rule that
    # reads as if no selector had been given at all.
    if preset_pack and not preset_id:
        raise ValueError("--preset-pack requires --preset to select an entry")
    pitch_semitones = getattr(args, "pitch_semitones", None)
    formant_factor = getattr(args, "formant_factor", None)
    preset_source = bool(selectors or preset_pack)
    if preset_source and (pitch_semitones is not None or formant_factor is not None):
        raise ValueError(
            "--pitch-semitones/--formant-factor cannot be combined with a realtime preset"
        )
    if assignments and not preset_source:
        raise ValueError("--set requires --preset, --preset-json, or --preset-pack")

    samples, sr = _load_audio(args.file)
    input_length = len(samples)
    uses_realtime_preset = bool(preset_source or assignments)
    latency_samples = 0
    if uses_realtime_preset:
        preset: str | dict[str, Any]
        if preset_json:
            with open(preset_json, encoding="utf-8") as fh:
                preset = json.load(fh)
        elif preset_pack:
            preset = _load_voice_preset_pack(preset_pack, preset_id)
        elif assignments:
            preset = json.loads(realtime_voice_changer_preset_json(preset_id))
        else:
            preset = preset_id
        preset = _apply_voice_sets(preset, assignments)
        # Probe the same prepared C-ABI configuration used by the one-shot
        # realtime entry point.  Latency depends on the resolved config and
        # sample rate, so it must not be a hard-coded preset constant.
        with RealtimeVoiceChanger(sr, preset, max_block_size=128, channels=1) as changer:
            latency_samples = max(int(changer.latency_samples()), 0)
        result = [
            float(sample)
            for sample in voice_change_realtime(samples, sample_rate=sr, preset=preset)
        ]
        mode_metadata: dict[str, object] = {}
        if preset_id and not preset_json and not preset_pack:
            mode_metadata["preset"] = preset_id
    else:
        result = voice_change(
            samples,
            sample_rate=sr,
            pitch_semitones=pitch_semitones if pitch_semitones is not None else 0.0,
            formant_factor=formant_factor if formant_factor is not None else 1.0,
        )
        mode_metadata = {
            "pitch_semitones": pitch_semitones if pitch_semitones is not None else 0.0,
            "formant_factor": formant_factor if formant_factor is not None else 1.0,
        }

    # Spectral pitch/formant processing can differ by one sample at a frame
    # boundary.  The CLI contract is sample-preserving for both modes; trim a
    # longer result and zero-pad a shorter one before writing/serializing.
    result = [float(sample) for sample in result[:input_length]]
    if len(result) < input_length:
        result.extend([0.0] * (input_length - len(result)))

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={"latency_samples": latency_samples, **mode_metadata},
        label="Voice change",
    )


def cmd_voice_presets(args: argparse.Namespace) -> int:
    from . import realtime_voice_changer_preset_names

    names = realtime_voice_changer_preset_names()
    if args.json:
        print(_strict_json_dumps({"presets": names}))
    else:
        for name in names:
            print(name)
    return 0


def cmd_voice_preset(args: argparse.Namespace) -> int:
    from . import realtime_voice_changer_preset_json

    # The library returns a JSON preset regardless of --json; print it as-is.
    print(realtime_voice_changer_preset_json(args.preset))
    return 0


def cmd_voice_preset_validate(args: argparse.Namespace) -> int:
    from . import validate_realtime_voice_changer_preset_json

    path = args.preset_json or args.file
    if not path:
        raise FileNotFoundError("voice-preset-validate requires a JSON file")
    if args.preset:
        preset = _load_voice_preset_pack(path, args.preset)
        updated_preset = _apply_voice_sets(preset, args.set)
        text = json.dumps(updated_preset)
    else:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        text_or_preset = _apply_voice_sets(text, args.set)
        text = json.dumps(text_or_preset) if isinstance(text_or_preset, dict) else text_or_preset
    result = validate_realtime_voice_changer_preset_json(text)
    normalized = result.get("normalizedJson")
    if not result.get("ok") or not normalized:
        error = str(result.get("error", "invalid voice preset"))
        payload = {"ok": False, "error": error}
        print(_strict_json_dumps(payload) if args.json else error)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER
    payload = {"ok": True, "normalized_json": str(normalized)}
    print(_strict_json_dumps(payload) if args.json else str(normalized))
    return 0


def cmd_acoustic(args: argparse.Namespace) -> int:
    from . import analyze_impulse_response, detect_acoustic

    samples, sr = _load_audio(args.file)
    n_bands = cast(int | None, getattr(args, "n_bands", None))
    if n_bands is None:
        n_bands = cast(int, getattr(args, "n_octave_bands", 6))
    min_decay_db = getattr(args, "min_decay_db", 30.0)
    noise_floor_margin_db = getattr(args, "noise_floor_margin_db", 10.0)
    if args.ir:
        result = analyze_impulse_response(
            samples,
            sample_rate=sr,
            n_octave_bands=n_bands,
            min_decay_db=min_decay_db,
        )
    else:
        result = detect_acoustic(
            samples,
            sample_rate=sr,
            n_octave_bands=n_bands,
            min_decay_db=min_decay_db,
            noise_floor_margin_db=noise_floor_margin_db,
        )

    if args.json:
        print(
            _strict_json_dumps(
                {
                    "rt60": result.rt60,
                    "edt": result.edt,
                    "c50": result.c50,
                    "c80": result.c80,
                    "d50": result.d50,
                    "confidence": result.confidence,
                    "is_blind": result.is_blind,
                    "rt60_bands": [float(value) for value in result.rt60_bands],
                    "edt_bands": [float(value) for value in result.edt_bands],
                    "c50_bands": [float(value) for value in result.c50_bands],
                    "c80_bands": [float(value) for value in result.c80_bands],
                }
            )
        )
    else:
        mode = "impulse response" if args.ir else "blind"
        print(f"  Acoustic ({mode}):")
        print(f"    RT60:       {result.rt60:.3f} s")
        print(f"    EDT:        {result.edt:.3f} s")
        print(f"    C50:        {result.c50:.2f} dB")
        print(f"    C80:        {result.c80:.2f} dB")
        print(f"    D50:        {result.d50:.3f}")
        print(f"    Confidence: {result.confidence:.1%}")
        print(f"    Blind:      {result.is_blind}")
    return 0


def cmd_estimate_room(args: argparse.Namespace) -> int:
    from . import estimate_room

    samples, sr = _load_audio(args.file)
    n_bands = cast(int | None, getattr(args, "n_bands", None))
    if n_bands is None:
        n_bands = cast(int | None, getattr(args, "n_octave_bands", None))
    if n_bands is None:
        n_bands = 0
    est = estimate_room(
        samples,
        sample_rate=sr,
        aspect_hint_lw=args.aspect_lw,
        aspect_hint_lh=args.aspect_lh,
        reference_absorption=args.reference_absorption,
        prefer_eyring=not args.sabine,
        n_octave_bands=n_bands,
        min_decay_db=getattr(args, "min_decay_db", 0.0),
        noise_floor_margin_db=getattr(args, "noise_floor_margin_db", 0.0),
    )
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "volume": est.volume,
                    "length": est.length,
                    "width": est.width,
                    "height": est.height,
                    "drr_db": est.drr_db,
                    "confidence": est.confidence,
                    "rt60_bands": [float(value) for value in est.rt60_bands],
                    "absorption_bands": [float(value) for value in est.absorption_bands],
                }
            )
        )
    else:
        print("  Room estimate:")
        print(f"    Volume:     {est.volume:.1f} m^3")
        print(f"    Dimensions: {est.length:.2f} x {est.width:.2f} x {est.height:.2f} m")
        print(f"    DRR:        {est.drr_db:.2f} dB")
        print(f"    Confidence: {est.confidence:.1%}")
    return 0


def cmd_synthesize_rir(args: argparse.Namespace) -> int:
    from . import synthesize_rir

    if not args.output:
        raise ValueError("synthesize-rir requires --output")
    result = synthesize_rir(
        args.length,
        args.width,
        args.height,
        source=(args.source_x, args.source_y, args.source_z),
        listener=(args.listener_x, args.listener_y, args.listener_z),
        absorption=args.absorption,
        sample_rate=args.sample_rate,
        ism_order=args.ism_order,
        seed=args.seed,
        max_seconds=args.max_seconds,
        prefer_eyring=not args.sabine,
    )
    if result.has_error:
        detail = getattr(result, "error_message", "")
        print(f"Error: {detail or 'invalid room geometry'}", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER
    # Warnings survive a successful synthesis and describe a RIR the caller did
    # not ask for -- a tail cut against max_seconds, a cap raised to fit the
    # direct sound, a request reduced to early reflections alone. Dropping them
    # made a truncated RIR indistinguishable from a complete one. They go to
    # stderr in every mode so the JSON document on stdout stays exactly the
    # payload both CLIs publish.
    _print_diagnostic_warnings(result)
    # 24-bit, not the 16-bit default. A synthesized RIR carries its physical
    # 1/(4*pi*d) attenuation, so its peak sits far below full scale and 16-bit
    # quantization would cost the tail roughly 36 dB of the headroom it needs;
    # half the reported samples came back exactly zero.
    _write_wav(args.output, result.rir, result.sample_rate, 24)
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "output": args.output,
                    "samples": len(result.rir),
                    "sample_rate": result.sample_rate,
                }
            )
        )
    else:
        print(f"  Saved RIR ({len(result.rir)} samples) to {args.output}")
    return 0


def cmd_room_morph(args: argparse.Namespace) -> int:
    from . import room_morph

    if not args.output:
        raise ValueError("room-morph requires --output")
    samples, sr = _load_audio(args.file)
    result = room_morph(
        samples,
        sr,
        args.length,
        args.width,
        args.height,
        source=(args.source_x, args.source_y, args.source_z),
        listener=(args.listener_x, args.listener_y, args.listener_z),
        absorption=args.absorption,
        source_tail_suppression=args.suppression,
        wet=args.wet,
        ism_order=args.ism_order,
        seed=args.seed,
        max_seconds=args.max_seconds,
        prefer_eyring=not args.sabine,
    )
    # The target RIR is synthesized by the code synthesize-rir uses, so the same
    # clamps fire and each one says the morph went through a room the caller did
    # not ask for. stderr in every mode, so the JSON document on stdout stays
    # exactly the payload both CLIs publish.
    _print_diagnostic_warnings(result)
    _write_wav(args.output, result.audio, sr)
    if args.json:
        print(_strict_json_dumps({"output": args.output, "samples": len(result.audio)}))
    else:
        print(f"  Saved morphed audio ({len(result.audio)} samples) to {args.output}")
    return 0


def register_effects_parsers(sub: argparse._SubParsersAction, shared: SharedParsers) -> None:
    """Register the offline effect, voice-preset and acoustic commands."""
    common = shared.common
    stdout_options = shared.stdout_options
    fft_options = shared.fft_options
    fft_stdout_options = shared.fft_stdout_options

    hpss_p = sub.add_parser("hpss", parents=[fft_options], help="Harmonic-percussive separation")
    hpss_p.add_argument(
        "--kernel-harmonic", type=int, default=31, help="Harmonic median-filter kernel"
    )
    hpss_p.add_argument(
        "--kernel-percussive", type=int, default=31, help="Percussive median-filter kernel"
    )
    hpss_p.add_argument("--harmonic-only", action="store_true")
    hpss_p.add_argument("--percussive-only", action="store_true")
    hpss_p.add_argument(
        "--with-residual",
        action="store_true",
        help="Also write what neither component claimed. Silent without --hard-mask, "
        "whose masks are the ones that leave anything over",
    )
    hpss_p.add_argument(
        "--hard-mask",
        action="store_true",
        help="Assign each cell to whichever component dominates instead of blending them",
    )

    stems_p = sub.add_parser(
        "decompose-stems",
        parents=[fft_options],
        help="Separate into NMF components that keep the original phase",
    )
    stems_p.add_argument(
        "--n-components", type=int, default=4, help="Number of NMF components (default: 4)"
    )
    stems_p.add_argument(
        "--n-iter", type=int, default=100, help="NMF update iterations (default: 100)"
    )
    stems_p.add_argument(
        "--beta",
        type=_finite_float,
        default=2.0,
        help="Beta divergence: 2 Frobenius, 1 Kullback-Leibler (default: 2.0)",
    )
    _cli_domain(
        stems_p.add_argument(
            "--init",
            default="random",
            help="NMF initialisation: random or nndsvd (default: random)",
        ),
        choices=("random", "nndsvd"),
        reject_exit="invalid_parameter",
    )
    stems_p.add_argument(
        "--mask-power",
        type=_finite_float,
        default=1.0,
        help="Soft-mask exponent, >= 1; 2 is the Wiener-style power ratio (default: 1.0)",
    )

    # Editing commands
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
    # Effect commands that map directly to the Python effects API. The C++ CLI
    # still exposes some low-level converters and section/melody analyses that
    # are not mirrored here; this set covers the common offline edits.
    pitch_shift_p = sub.add_parser(
        "pitch-shift", parents=[common], help="Shift pitch by a number of semitones"
    )
    pitch_shift_p.add_argument(
        "--semitones", type=_finite_float, help="Semitones to shift (positive = up)"
    )
    # The spectral backend repairs a non-positive geometry into the librosa
    # defaults instead of refusing it, so the refusal a caller gets is the
    # handler's; these two commands are where it is reachable from a command line.
    for _geometry_option, _geometry_default in (("--n-fft", 2048), ("--hop-length", 512)):
        _cli_domain(
            pitch_shift_p.add_argument(_geometry_option, type=int, default=_geometry_default),
            minimum=0,
            exclusive_minimum=True,
            reject_exit="invalid_parameter",
        )
    time_stretch_p = sub.add_parser(
        "time-stretch", parents=[common], help="Time-stretch without changing pitch"
    )
    time_stretch_p.add_argument(
        "--rate", type=_finite_float, help="Stretch factor (>1 speeds up, <1 slows down)"
    )
    for _geometry_option, _geometry_default in (("--n-fft", 2048), ("--hop-length", 512)):
        _cli_domain(
            time_stretch_p.add_argument(_geometry_option, type=int, default=_geometry_default),
            minimum=0,
            exclusive_minimum=True,
            reject_exit="invalid_parameter",
        )
    normalize_p = sub.add_parser(
        "normalize", parents=[common], help="Peak-normalize audio to a target dB level"
    )
    normalize_p.add_argument("--mode", default="peak", help="Normalization mode (default: peak)")
    normalize_p.add_argument(
        "--target-db", type=_finite_float, default=None, help="Target peak level in dB"
    )
    trim_silence_p = sub.add_parser(
        "trim-silence", parents=[common], help="Trim leading/trailing silence"
    )
    trim_silence_p.add_argument(
        "--threshold-db",
        type=_finite_float,
        default=None,
        help="Silence threshold in dB (default: -60)",
    )
    # ``--threshold-db`` and ``--top-db`` select two handler paths. Leave the
    # alternate selector absent by default so the handler can distinguish the
    # default threshold mode from an explicit top-dB request.
    trim_silence_p.add_argument("--top-db", type=_finite_float, default=None)
    trim_silence_p.add_argument("--n-fft", type=int, default=2048)
    trim_silence_p.add_argument("--hop-length", type=int, default=512)
    split_silence_p = sub.add_parser(
        "split-silence", parents=[fft_stdout_options], help="List non-silent intervals"
    )
    # One take per occurrence, as suggest-mix --input does. The positional is the
    # first take, so this option names the further ones.
    split_silence_p.add_argument(
        "--input",
        action="append",
        default=[],
        metavar="WAV",
        help=(
            "Another take of the same part (repeat once per take); the intervals "
            "become the union, so a cut falls only where every take is quiet"
        ),
    )
    split_silence_p.add_argument(
        "--top-db",
        type=_finite_float,
        default=60.0,
        help="Silence threshold below the peak in dB (default: 60)",
    )
    split_silence_p.add_argument(
        "--write-takes",
        metavar="PREFIX",
        help=("Write every take sliced at every interval as PREFIX{take:02d}_{interval:03d}.wav"),
    )
    split_silence_p.add_argument(
        "--report",
        action="store_true",
        help=(
            "Also say why those are the intervals: the largest --top-db at which every "
            "take still shows silence, read against the one in use. One interval covering "
            "everything answers three situations and the intervals cannot say which"
        ),
    )
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
    voice_change_p.add_argument(
        "--pitch-semitones", type=_finite_float, help="Pitch shift in semitones"
    )
    voice_change_p.add_argument(
        "--formant-factor",
        type=_finite_float,
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
    acoustic_p.add_argument("--min-decay-db", type=_finite_float, default=30.0)
    acoustic_p.add_argument("--noise-floor-margin-db", type=_finite_float, default=10.0)

    def _add_room_geometry(p: argparse.ArgumentParser) -> None:
        p.add_argument("--length", type=_finite_float, default=7.0, help="Room length (m)")
        p.add_argument("--width", type=_finite_float, default=5.0, help="Room width (m)")
        p.add_argument("--height", type=_finite_float, default=3.0, help="Room height (m)")
        # The room builder clamps rather than validates, so the refusal comes from
        # the material check beside it -- an invalid parameter, not a usage error.
        _cli_domain(
            p.add_argument(
                "--absorption", type=_finite_float, default=0.2, help="Uniform wall absorption"
            ),
            minimum=0,
            maximum=1,
            reject_exit="invalid_parameter",
        )
        p.add_argument("--source-x", type=_finite_float, default=1.0)
        p.add_argument("--source-y", type=_finite_float, default=1.0)
        p.add_argument("--source-z", type=_finite_float, default=1.2)
        p.add_argument("--listener-x", type=_finite_float, default=5.0)
        p.add_argument("--listener-y", type=_finite_float, default=4.0)
        p.add_argument("--listener-z", type=_finite_float, default=1.7)
        p.add_argument("--ism-order", type=int, default=3, help="Image-source reflection order")
        # The C field is a uint32 and 0 is its library-default sentinel, so the
        # range is closed at both ends rather than refusing only a negative.
        _cli_domain(
            p.add_argument("--seed", type=int, default=1, help="Deterministic late-tail seed"),
            minimum=0,
            maximum=0xFFFFFFFF,
            reject_exit="invalid_parameter",
        )
        p.add_argument(
            "--max-seconds",
            type=_finite_float,
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
    estimate_room_p.add_argument(
        "--aspect-lw", type=_finite_float, default=1.0, help="length/width prior"
    )
    estimate_room_p.add_argument(
        "--aspect-lh", type=_finite_float, default=1.0, help="length/height prior"
    )
    estimate_room_p.add_argument(
        "--reference-absorption", type=_finite_float, default=0.15, help="absorption prior"
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
    room_morph_p.add_argument(
        "--wet", type=_finite_float, default=0.5, help="Target-room mix [0,1]"
    )
    room_morph_p.add_argument(
        "--suppression", type=_finite_float, default=0.5, help="Source-tail suppression [0,1]"
    )
