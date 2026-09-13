"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections.abc import Iterable
from typing import TYPE_CHECKING, Any, cast

from ._cli_common import (
    EXIT_INVALID_PARAMETER,
    _apply_voice_sets,
    _emit_effect_result,
    _legacy_exit_codes,
    _load_voice_preset_pack,
    _resample,
    _strict_json_dumps,
    _write_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._runtime import _C_INT_MAX, _C_INT_MIN

if TYPE_CHECKING:
    from .analyzer import NoteEdit


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
        result,
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
    return _emit_effect_result(args, result, sr, label="Time-varying pitch correct")


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
    return _emit_effect_result(args, result, sr, label="Note move")


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
        result,
        sr,
        extra={
            "onset_sample": args.onset,
            "offset_sample": args.offset,
            "ratio": args.ratio,
        },
        label="Note stretch",
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
        result,
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
        result,
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
        result,
        sr,
        extra={"rate": args.rate},
        label=f"Time stretch (rate {args.rate:.4f})",
    )


def cmd_normalize(args: argparse.Namespace) -> int:
    from . import normalize, normalize_rms

    samples, sr = _load_audio(args.file)
    mode = getattr(args, "mode", "peak")
    target_db = getattr(args, "target_db", None)
    if target_db is None:
        target_db = -20.0 if mode == "rms" else 0.0
    if mode == "peak":
        result = normalize(samples, sample_rate=sr, target_db=target_db)
    elif mode == "rms":
        result = normalize_rms(samples, sample_rate=sr, target_db=target_db)
    else:
        raise ValueError("--mode must be 'peak' or 'rms'")

    return _emit_effect_result(
        args,
        result,
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
        result,
        sr,
        extra=extra,
        label=label,
        # trim-silence doubles as analysis (it reports the trimmed length), so an
        # output file is optional here, matching the native CLI.
        requires_output=False,
    )


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
        result,
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
                    "rt60": round(result.rt60, 4),
                    "edt": round(result.edt, 4),
                    "c50": result.c50,
                    "c80": result.c80,
                    "d50": result.d50,
                    "confidence": round(result.confidence, 4),
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
                    "volume": round(est.volume, 3),
                    "length": round(est.length, 3),
                    "width": round(est.width, 3),
                    "height": round(est.height, 3),
                    "drr_db": round(est.drr_db, 3),
                    "confidence": round(est.confidence, 4),
                    "rt60_bands": [round(b, 4) for b in est.rt60_bands],
                    "absorption_bands": [round(b, 4) for b in est.absorption_bands],
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
    warning = getattr(result, "warning_message", "")
    if warning:
        print(f"warning: {warning}", file=sys.stderr)
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
    _write_wav(args.output, result, sr)
    if args.json:
        print(_strict_json_dumps({"output": args.output, "samples": len(result)}))
    else:
        print(f"  Saved morphed audio ({len(result)} samples) to {args.output}")
    return 0
