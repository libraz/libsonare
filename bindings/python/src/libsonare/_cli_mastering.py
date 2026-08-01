"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import sys
from typing import Any

from ._cli_common import (
    _atomic_wav_writer,
    _float_sequence,
    _parse_json_config,
    _parse_json_list,
    _parse_kv_params,
    _resample,
    _strict_json_dumps,
    _write_wav,
    _write_wav_stereo_frames,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)


def _mastering_report_payload(report: Any) -> dict[str, object]:
    """Serialize the shared chain report without depending on dataclass internals."""

    if report is None:
        raise RuntimeError("loaded libsonare did not return a mastering report")
    before = report.before
    after = report.after
    return {
        "before": {
            "integrated_lufs": before.integrated_lufs,
            "max_momentary_lufs": before.max_momentary_lufs,
            "max_short_term_lufs": before.max_short_term_lufs,
            "true_peak_dbtp": before.true_peak_dbtp,
            "loudness_range": before.loudness_range,
        },
        "after": {
            "integrated_lufs": after.integrated_lufs,
            "max_momentary_lufs": after.max_momentary_lufs,
            "max_short_term_lufs": after.max_short_term_lufs,
            "true_peak_dbtp": after.true_peak_dbtp,
            "loudness_range": after.loudness_range,
        },
        "applied_gain_db": report.applied_gain_db,
        "max_gain_reduction_db": report.max_gain_reduction_db,
        "loudness_target_limited": report.loudness_target_limited,
        "band_energy_delta_db": report.band_energy_delta_db,
    }


def _write_mastering_report(path: str, report: Any) -> None:
    with open(path, "w", encoding="utf-8") as output:
        output.write(_strict_json_dumps(_mastering_report_payload(report)))
        output.write("\n")


def cmd_mastering(args: argparse.Namespace) -> int:
    samples, sr = _load_audio(args.file)
    report_path = getattr(args, "report", "")
    if report_path:
        from . import mastering_chain

        result = mastering_chain(
            samples,
            sample_rate=sr,
            config={
                "loudness": {
                    "enabled": True,
                    "targetLufs": args.target_lufs,
                    "ceilingDb": args.ceiling_db,
                }
            },
        )
    else:
        from .audio import Audio

        result = Audio.from_buffer(samples, sr).mastering(
            target_lufs=args.target_lufs, ceiling_db=args.ceiling_db
        )

    if args.output:
        _write_wav(args.output, result.samples, result.sample_rate)
    if report_path:
        _write_mastering_report(report_path, result.report)

    if args.json:
        payload = {
            "input_lufs": round(result.input_lufs, 4),
            "output_lufs": round(result.output_lufs, 4),
            "applied_gain_db": round(result.applied_gain_db, 4),
            "target_lufs": args.target_lufs,
            "ceiling_db": args.ceiling_db,
            "true_peak_oversample": 4,
            "latency_samples": getattr(result, "latency_samples", 0),
            "sample_rate": result.sample_rate,
            "output": args.output or "",
        }
        print(_strict_json_dumps(payload))
    else:
        print("  Mastering:")
        print(f"    Input LUFS:  {result.input_lufs:.2f}")
        print(f"    Output LUFS: {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_mastering_processor(args: argparse.Namespace) -> int:
    from . import mastering_process, mastering_process_stereo, mastering_processor_catalog

    samples, sr = _load_audio(args.file)
    params = _parse_kv_params(args.params) if args.params else {}
    stereo_only = {entry["id"] for entry in mastering_processor_catalog() if entry["stereoOnly"]}
    if args.processor in stereo_only:
        print(
            "warning: stereo-only processor preview duplicates the mono input on left/right; "
            "inspect stereo results through the Python API for production decisions",
            file=sys.stderr,
        )
        stereo = mastering_process_stereo(
            args.processor, samples, samples, sample_rate=sr, params=params
        )
        result = argparse.Namespace(
            samples=[
                0.5 * (left + right) for left, right in zip(stereo.left, stereo.right, strict=True)
            ],
            sample_rate=stereo.sample_rate,
            input_lufs=stereo.input_lufs,
            output_lufs=stereo.output_lufs,
            applied_gain_db=stereo.applied_gain_db,
            latency_samples=stereo.latency_samples,
        )
    else:
        result = mastering_process(args.processor, samples, sample_rate=sr, params=params)

    if args.output:
        _write_wav(args.output, result.samples, result.sample_rate)

    if args.json:
        payload = {
            "processor": args.processor,
            "input_lufs": round(result.input_lufs, 4),
            "output_lufs": round(result.output_lufs, 4),
            "applied_gain_db": round(result.applied_gain_db, 4),
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
        }
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print(f"  Mastering processor: {args.processor}")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_eq(args: argparse.Namespace) -> int:
    from . import mastering_process

    samples, sr = _load_audio(args.file)
    if args.params:
        params = _parse_kv_params(args.params)
    else:
        params = {
            "band0.enabled": 1.0,
            "band0.type": float(args.type),
            "band0.frequencyHz": float(args.frequency_hz),
            "band0.gainDb": float(args.gain_db),
            "band0.q": float(args.q),
            "band0.coeffMode": float(args.coeff_mode),
            "band0.slopeDbOct": float(args.slope_db_oct),
            "band0.placement": float(args.placement),
            "band0.proportionalQ": 1.0 if args.proportional_q else 0.0,
            "band0.dynamic": 1.0 if args.dynamic else 0.0,
            "band0.thresholdDb": float(args.threshold_db),
            "band0.autoThreshold": 1.0 if args.auto_threshold else 0.0,
            "band0.ratio": float(args.ratio),
            "band0.rangeDb": float(args.range_db),
            "band0.attackMs": float(args.attack_ms),
            "band0.releaseMs": float(args.release_ms),
            "band0.lookaheadMs": float(args.lookahead_ms),
            "band0.sidechainFreqHz": float(args.sidechain_freq_hz),
            "band0.sidechainQ": float(args.sidechain_q),
            "phaseMode": float(args.phase_mode),
            "resolution": float(args.resolution),
            "autoGain": 1.0 if args.auto_gain else 0.0,
            "gainScale": float(args.gain_scale),
            "outputGainDb": float(args.output_gain_db),
            "outputPan": float(args.output_pan),
        }
    result = mastering_process("eq.equalizer", samples, sample_rate=sr, params=params)

    if args.output:
        _write_wav(args.output, result.samples, result.sample_rate)

    if args.json:
        payload = {
            "processor": "eq.equalizer",
            "input_lufs": round(result.input_lufs, 4),
            "output_lufs": round(result.output_lufs, 4),
            "applied_gain_db": round(result.applied_gain_db, 4),
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
        }
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print("  Equalizer")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_mastering_processors(args: argparse.Namespace) -> int:
    from . import mastering_processor_names

    names = mastering_processor_names()
    if args.json:
        print(_strict_json_dumps({"processors": names}))
    else:
        print("  Mastering processors:")
        for name in names:
            print(f"    {name}")
    return 0


def cmd_mastering_pair_processors(args: argparse.Namespace) -> int:
    from . import mastering_pair_processor_names

    names = mastering_pair_processor_names()
    if args.json:
        print(_strict_json_dumps({"processors": names}))
    else:
        print("  Mastering pair processors:")
        for name in names:
            print(f"    {name}")
    return 0


def cmd_mastering_pair_analyses(args: argparse.Namespace) -> int:
    from . import mastering_pair_analysis_names

    names = mastering_pair_analysis_names()
    if args.json:
        print(_strict_json_dumps({"analyses": names}))
    else:
        print("  Mastering pair analyses:")
        for name in names:
            print(f"    {name}")
    return 0


def cmd_mastering_pair_analyze(args: argparse.Namespace) -> int:
    from . import mastering_pair_analyze

    source, sr = _load_audio(args.file)
    reference, ref_sr = _load_audio(args.reference)
    if ref_sr != sr:
        reference = _resample(reference, ref_sr, sr)
    result_json = mastering_pair_analyze(args.analysis, source, reference, sample_rate=sr)
    # The library returns a JSON string regardless of --json; print it as-is.
    print(result_json)
    return 0


def cmd_mastering_chain(args: argparse.Namespace) -> int:
    from . import mastering_chain

    samples, sr = _load_audio(args.file)
    config = _parse_json_config(args.config, args.config_file)
    if args.params:
        config.update(_parse_kv_params(args.params))
    result = mastering_chain(samples, sample_rate=sr, config=config)
    report_path = getattr(args, "report", "")

    if args.output:
        _write_wav(args.output, result.samples, result.sample_rate)
    if report_path:
        _write_mastering_report(report_path, result.report)

    if args.json:
        payload: dict[str, object] = {
            "input_lufs": round(result.input_lufs, 4),
            "output_lufs": round(result.output_lufs, 4),
            "applied_gain_db": round(result.applied_gain_db, 4),
            "sample_rate": result.sample_rate,
            "stages": result.stages,
        }
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print("  Mastering chain:")
        print(f"    Stages:      {', '.join(result.stages) if result.stages else '(none)'}")
        print(f"    Input LUFS:  {result.input_lufs:.2f}")
        print(f"    Output LUFS: {result.output_lufs:.2f}")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_master(args: argparse.Namespace) -> int:
    from . import master_audio

    samples, sr = _load_audio(args.file)
    overrides = _parse_json_config(args.config, args.config_file)
    if args.params:
        overrides.update(_parse_kv_params(args.params))
    result = master_audio(samples, sample_rate=sr, preset_name=args.preset, overrides=overrides)
    report_path = getattr(args, "report", "")

    if args.output:
        _write_wav(args.output, result.samples, result.sample_rate)
    if report_path:
        _write_mastering_report(report_path, result.report)

    if args.json:
        payload: dict[str, object] = {
            "preset": args.preset,
            "input_lufs": round(result.input_lufs, 4),
            "output_lufs": round(result.output_lufs, 4),
            "applied_gain_db": round(result.applied_gain_db, 4),
            "sample_rate": result.sample_rate,
            "stages": result.stages,
        }
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print(f"  Master preset: {args.preset}")
        print(f"    Stages:      {', '.join(result.stages) if result.stages else '(none)'}")
        print(f"    Input LUFS:  {result.input_lufs:.2f}")
        print(f"    Output LUFS: {result.output_lufs:.2f}")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_mastering_streaming(args: argparse.Namespace) -> int:
    from . import mastering_streaming_preview

    samples, sr = _load_audio(args.file)
    platforms = _parse_json_list(args.platforms, args.platforms_file) or None
    print(mastering_streaming_preview(samples, sample_rate=sr, platforms=platforms))
    return 0


def cmd_declip(args: argparse.Namespace) -> int:
    from . import mastering_repair_declip

    samples, sr = _load_audio(args.file)
    repaired = _float_sequence(
        mastering_repair_declip(
            samples,
            sample_rate=sr,
            clip_threshold=args.clip_threshold,
            lpc_order=args.lpc_order,
            iterations=args.iterations,
            lpc_blend=args.lpc_blend,
        )
    )

    if args.output:
        _write_wav(args.output, repaired, sr)

    if args.json:
        payload: dict[str, object] = {
            "sample_rate": sr,
            "samples": len(repaired),
            "clip_threshold": args.clip_threshold,
            "lpc_order": args.lpc_order,
            "iterations": args.iterations,
            "lpc_blend": args.lpc_blend,
        }
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print("  Declip:")
        print(f"    Samples: {len(repaired)}")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def cmd_mastering_presets(args: argparse.Namespace) -> int:
    from . import mastering_preset_names

    names = mastering_preset_names()
    print(_strict_json_dumps({"presets": names}) if args.json else "\n".join(names))
    return 0


def cmd_mastering_suggest(args: argparse.Namespace) -> int:
    from . import mastering_assistant_suggest

    samples, sr = _load_audio(args.file)
    params = _parse_kv_params(args.params) if args.params else {}
    print(mastering_assistant_suggest(samples, sample_rate=sr, params=params))
    return 0


def cmd_mastering_profile(args: argparse.Namespace) -> int:
    from . import mastering_audio_profile

    samples, sr = _load_audio(args.file)
    params = _parse_kv_params(args.params) if args.params else {}
    print(mastering_audio_profile(samples, sample_rate=sr, params=params))
    return 0


def cmd_mixing_presets(args: argparse.Namespace) -> int:
    from . import mixing_scene_preset_names

    names = mixing_scene_preset_names()
    print(_strict_json_dumps({"presets": names}) if args.json else "\n".join(names))
    return 0


def cmd_mixing_preset(args: argparse.Namespace) -> int:
    from . import mixing_scene_preset_json

    print(mixing_scene_preset_json(args.preset))
    return 0


def cmd_mix(args: argparse.Namespace) -> int:
    from . import Mixer, mixing_scene_preset_json

    if args.input and not args.output:
        raise ValueError("mix with --input requires --output")
    if args.output and not args.input:
        raise ValueError("mix with --output requires at least one --input")

    # Resolve the scene JSON from either a file or a built-in preset.
    if args.scene:
        with open(args.scene, encoding="utf-8") as fh:
            scene_json = fh.read()
    elif args.preset:
        scene_json = mixing_scene_preset_json(args.preset)
    else:
        raise ValueError("either --scene or --preset is required")

    mixer = Mixer.from_scene_json(
        scene_json, sample_rate=args.sample_rate, block_size=args.block_size
    )
    try:
        strip_count = mixer.strip_count()

        rendered_samples = 0
        if args.input:
            # Process each input WAV as one strip (mono inputs are duplicated to
            # both channels). Inputs that were captured at a different sample
            # rate are resampled to the mixer rate so a 44.1 kHz stem is not
            # played back fast at the 48 kHz default. All inputs must share a
            # length after resampling.
            channels: list[list[float]] = []
            length: int | None = None
            for path in args.input:
                samples, in_sr = _load_audio(path)
                if in_sr != args.sample_rate:
                    samples = _resample(samples, in_sr, args.sample_rate)
                if length is None:
                    length = len(samples)
                elif len(samples) != length:
                    raise ValueError("all --input files must have the same length")
                channels.append(list(samples))
            if len(channels) != strip_count:
                raise ValueError(
                    f"scene has {strip_count} strips but {len(channels)} inputs were given"
                )
            mixer.compile()
            # The mixer reports its graph latency separately. Output begins at
            # sample zero without trimming so routing alignment is preserved.
            with _atomic_wav_writer(args.output, 2, args.sample_rate) as wav:
                for offset in range(0, length or 0, args.block_size):
                    end = min(offset + args.block_size, length or 0)
                    block = [channel[offset:end] for channel in channels]
                    result = mixer.process_stereo(block, block)
                    _write_wav_stereo_frames(wav, result.left, result.right)
                    rendered_samples += len(result.left)

                tail_remaining = mixer.tail_samples()
                while tail_remaining > 0:
                    count = min(tail_remaining, args.block_size)
                    result = mixer.drain_tail_stereo(count)
                    _write_wav_stereo_frames(wav, result.left, result.right)
                    rendered_samples += len(result.left)
                    tail_remaining -= count

        if args.json:
            payload: dict[str, object] = {
                "strip_count": strip_count,
                "sample_rate": args.sample_rate,
                "block_size": args.block_size,
            }
            if args.input:
                payload["rendered_samples"] = rendered_samples
                payload["output"] = args.output
            print(_strict_json_dumps(payload))
        else:
            print("  Mixer:")
            print(f"    Strips:      {strip_count}")
            print(f"    Sample rate: {args.sample_rate} Hz")
            print(f"    Block size:  {args.block_size}")
            if args.input:
                print(f"    Rendered:    {rendered_samples} samples (stereo)")
                print(f"    Wrote: {args.output}")
    finally:
        mixer.close()
    return 0
