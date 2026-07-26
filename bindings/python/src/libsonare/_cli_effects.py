"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

from ._cli_common import (
    EXIT_INVALID_FORMAT,
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


def cmd_hpss(args: argparse.Namespace) -> int:
    from . import hpss

    if not args.output:
        print("Error: hpss requires an output file (-o/--output)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER

    samples, sr = _load_audio(args.file)
    result = hpss(samples, sample_rate=sr)

    h_energy = sum(abs(x) for x in result.harmonic) / len(result.harmonic)
    p_energy = sum(abs(x) for x in result.percussive) / len(result.percussive)

    harmonic_path = ""
    percussive_path = ""
    if args.output:
        base = args.output[:-4] if args.output.lower().endswith(".wav") else args.output
        harmonic_path = f"{base}_harmonic.wav"
        percussive_path = f"{base}_percussive.wav"
        _write_wav(harmonic_path, result.harmonic, result.sample_rate)
        _write_wav(percussive_path, result.percussive, result.sample_rate)

    if args.json:
        payload: dict[str, object] = {
            "length": result.length,
            "sample_rate": result.sample_rate,
            "harmonic_energy": round(h_energy, 6),
            "percussive_energy": round(p_energy, 6),
        }
        if harmonic_path:
            payload["harmonic"] = harmonic_path
            payload["percussive"] = percussive_path
        print(_strict_json_dumps(payload))
    else:
        print(f"  HPSS: {result.length} samples")
        print(f"  Harmonic energy:   {h_energy:.6f}")
        print(f"  Percussive energy: {p_energy:.6f}")
        if harmonic_path:
            print(f"  Wrote: {harmonic_path}, {percussive_path}")
    return 0


def cmd_pitch_correct(args: argparse.Namespace) -> int:
    from . import pitch_correct_to_midi

    samples, sr = _load_audio(args.file)
    result = pitch_correct_to_midi(
        samples,
        sample_rate=sr,
        current_midi=args.current_midi,
        target_midi=args.target_midi,
    )

    return _emit_effect_result(args, result, sr, label="Pitch correct")


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

    return _emit_effect_result(args, result, sr, label="Note stretch")


def cmd_pitch_shift(args: argparse.Namespace) -> int:
    from . import pitch_shift

    samples, sr = _load_audio(args.file)
    result = pitch_shift(samples, sample_rate=sr, semitones=args.semitones)

    return _emit_effect_result(
        args,
        result,
        sr,
        extra={"semitones": args.semitones},
        label=f"Pitch shift ({args.semitones:+.2f} semitones)",
    )


def cmd_time_stretch(args: argparse.Namespace) -> int:
    from . import time_stretch

    samples, sr = _load_audio(args.file)
    result = time_stretch(samples, sample_rate=sr, rate=args.rate)

    return _emit_effect_result(
        args,
        result,
        sr,
        extra={"rate": args.rate},
        label=f"Time stretch (rate {args.rate:.4f})",
    )


def cmd_normalize(args: argparse.Namespace) -> int:
    from . import normalize

    samples, sr = _load_audio(args.file)
    result = normalize(samples, sample_rate=sr, target_db=args.target_db)

    return _emit_effect_result(
        args,
        result,
        sr,
        extra={"target_db": args.target_db},
        label=f"Normalize (target {args.target_db:.2f} dB)",
    )


def cmd_trim_silence(args: argparse.Namespace) -> int:
    from . import trim

    samples, sr = _load_audio(args.file)
    result = trim(samples, sample_rate=sr, threshold_db=args.threshold_db)

    return _emit_effect_result(
        args,
        result,
        sr,
        extra={"threshold_db": args.threshold_db},
        label=f"Trim silence (threshold {args.threshold_db:.1f} dB)",
        # trim-silence doubles as analysis (it reports the trimmed length), so an
        # output file is optional here, matching the native CLI.
        requires_output=False,
    )


def cmd_resample(args: argparse.Namespace) -> int:
    if not args.output:
        print("Error: resample requires an output file (-o/--output)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER

    samples, sr = _load_audio(args.file)
    result = _resample(samples, sr, args.target_rate)

    if args.output:
        _write_wav(args.output, result, args.target_rate)

    if args.json:
        payload: dict[str, object] = {
            "length": len(result),
            "source_rate": sr,
            "sample_rate": args.target_rate,
        }
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print(f"  Resample ({sr} -> {args.target_rate} Hz): {len(result)} samples")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_voice_change(args: argparse.Namespace) -> int:
    from . import realtime_voice_changer_preset_json, voice_change, voice_change_realtime

    samples, sr = _load_audio(args.file)
    if args.preset or args.preset_json or args.preset_pack or args.set:
        preset: str | dict[str, Any]
        if args.preset_json:
            with open(args.preset_json, encoding="utf-8") as fh:
                preset = json.load(fh)
        elif args.preset_pack:
            preset = _load_voice_preset_pack(args.preset_pack, args.preset or "neutral-monitor")
        elif args.set:
            preset = json.loads(
                realtime_voice_changer_preset_json(args.preset or "neutral-monitor")
            )
        else:
            preset = args.preset
        preset = _apply_voice_sets(preset, args.set)
        result = [
            float(sample)
            for sample in voice_change_realtime(samples, sample_rate=sr, preset=preset)
        ]
    else:
        result = voice_change(
            samples,
            sample_rate=sr,
            pitch_semitones=args.pitch_semitones,
            formant_factor=args.formant_factor,
        )

    return _emit_effect_result(args, result, sr, label="Voice change")


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

    if args.preset:
        preset = _load_voice_preset_pack(args.file, args.preset)
        updated_preset = _apply_voice_sets(preset, args.set)
        text = json.dumps(updated_preset)
    else:
        with open(args.file, encoding="utf-8") as fh:
            text = fh.read()
        text_or_preset = _apply_voice_sets(text, args.set)
        text = json.dumps(text_or_preset) if isinstance(text_or_preset, dict) else text_or_preset
    result = validate_realtime_voice_changer_preset_json(text)
    if not result.get("ok") or not result.get("normalizedJson"):
        print(
            _strict_json_dumps(result) if args.json else result.get("error", "invalid voice preset")
        )
        return 1 if _legacy_exit_codes() else EXIT_INVALID_FORMAT
    print(
        _strict_json_dumps(result)
        if args.json
        else result.get("normalizedJson", result.get("error", ""))
    )
    return 0


def cmd_acoustic(args: argparse.Namespace) -> int:
    from . import analyze_impulse_response, detect_acoustic

    samples, sr = _load_audio(args.file)
    if args.ir:
        result = analyze_impulse_response(samples, sample_rate=sr)
    else:
        result = detect_acoustic(samples, sample_rate=sr)

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
    est = estimate_room(
        samples,
        sample_rate=sr,
        aspect_hint_lw=args.aspect_lw,
        aspect_hint_lh=args.aspect_lh,
        reference_absorption=args.reference_absorption,
        prefer_eyring=not args.sabine,
        n_octave_bands=args.n_octave_bands,
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
        print("Error: invalid room geometry (source/listener outside the room)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER
    _write_wav(args.output, result.rir, result.sample_rate)
    if args.json:
        print(_strict_json_dumps({"output": args.output, "samples": len(result.rir)}))
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
