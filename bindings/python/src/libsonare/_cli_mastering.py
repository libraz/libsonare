"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import TYPE_CHECKING, Any, cast

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

if TYPE_CHECKING:
    from .analyzer import MasteringPreset, SoloProcessor


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


def _wav_bits(args: argparse.Namespace) -> int:
    """Return the requested PCM width, keeping the CLI contract intentionally small."""
    bits = int(getattr(args, "bits", 16))
    if bits not in (16, 24):
        raise ValueError("bits must be 16 or 24")
    return bits


def _option_supplied(args: argparse.Namespace, name: str, default: object) -> bool:
    """Detect a non-default option without changing argparse's public Namespace.

    The Python parser currently does not retain whether an option equal to its
    default was spelled on the command line.  A future parser may expose that
    information as ``_supplied_options`` or ``supplied_options``; when it does,
    this helper consumes it.  Until then, values differing from the documented
    default are sufficient to reject semantic no-op combinations.
    """
    explicit = getattr(args, "_supplied_options", None)
    if explicit is None:
        explicit = getattr(args, "supplied_options", None)
    if isinstance(explicit, dict):
        values = list(explicit.values())
        explicit = (
            [item for item, supplied in explicit.items() if supplied]
            if values and all(isinstance(value, bool) for value in values)
            else explicit.keys()
        )
    if explicit is not None:
        normalized = name.replace("_", "-")
        for item in explicit:
            candidate = str(item).lstrip("-").replace("_", "-")
            if candidate == normalized:
                return True
    value = getattr(args, name.replace("-", "_"), default)
    return value != default


def _mastering_config(raw: str | None) -> dict[str, Any]:
    """Load a mastering chain config from JSON text or a JSON file path."""
    if not raw:
        return {}
    if os.path.isfile(raw):
        return _parse_json_config("", raw)
    try:
        loaded = json.loads(raw)
    except (TypeError, json.JSONDecodeError) as exc:
        raise ValueError("--config must be a JSON object or an existing JSON file") from exc
    if not isinstance(loaded, dict):
        raise ValueError("--config must be a JSON object")
    return loaded


def _chain_params_config(config: dict[str, Any]) -> dict[str, Any]:
    """Unwrap the native ``{version, params}`` chain-config representation."""
    params = config.get("params")
    if isinstance(params, dict):
        return dict(params)
    return dict(config)


def _mastering_chain_payload(
    result: Any,
    *,
    mode: str,
    output: str,
    preset: str = "",
    explanation: list[str] | None = None,
    include_report_latency: bool = False,
) -> dict[str, object]:
    payload: dict[str, object] = {
        "mode": mode,
        "input_lufs": result.input_lufs,
        "output_lufs": result.output_lufs,
        "applied_gain_db": result.applied_gain_db,
        "output": output,
    }
    if preset:
        payload["preset"] = preset
    payload["stages"] = list(getattr(result, "stages", []))
    if explanation:
        payload["explanation"] = explanation
    if include_report_latency:
        payload["latency_samples"] = 0
    return payload


def _eq_shortcut_names(args: argparse.Namespace) -> list[str]:
    defaults: dict[str, object] = {
        "type": 0,
        "frequency-hz": 1000.0,
        "gain-db": 0.0,
        "q": 1.0,
        "coeff-mode": 0,
        "slope-db-oct": 12,
        "placement": 0,
        "proportional-q": False,
        "dynamic": False,
        "threshold-db": -24.0,
        "auto-threshold": False,
        "ratio": 2.0,
        "range-db": -6.0,
        "attack-ms": 5.0,
        "release-ms": 50.0,
        "lookahead-ms": 0.0,
        "sidechain-freq-hz": -1.0,
        "sidechain-q": 1.0,
        "phase-mode": 1,
        "resolution": 0,
        "auto-gain": False,
        "gain-scale": 1.0,
        "output-gain-db": 0.0,
        "output-pan": 0.0,
    }
    return [name for name, default in defaults.items() if _option_supplied(args, name, default)]


def cmd_mastering(args: argparse.Namespace) -> int:
    samples, sr = _load_audio(args.file)
    report_path = getattr(args, "report", "") or ""
    preset = getattr(args, "preset", None) or ""
    config_raw = getattr(args, "config", None) or ""
    assistant = bool(getattr(args, "assistant", False))
    enable_repair = bool(getattr(args, "enable_repair", False))
    explain = bool(getattr(args, "explain", False))
    params_raw = getattr(args, "params", "") or ""
    bits = _wav_bits(args)

    selectors = [
        name
        for name, selected in (
            ("preset", bool(preset)),
            ("config", bool(config_raw)),
            ("assistant", assistant),
        )
        if selected
    ]
    if len(selectors) > 1:
        raise ValueError("--preset, --config, and --assistant are mutually exclusive")
    if params_raw and not selectors:
        raise ValueError("--params requires --preset, --config, or --assistant")
    if enable_repair and not assistant:
        raise ValueError("--enable-repair requires --assistant")
    if explain and not assistant:
        raise ValueError("--explain requires --assistant")

    # Native CliArgs can distinguish an explicitly supplied default-valued
    # option.  Python argparse cannot yet do so; non-default values are still
    # rejected here rather than silently discarded by a preset/config chain.
    if selectors and not assistant:
        ignored_loudness = [
            name
            for name, default in (
                ("target-lufs", -14.0),
                ("ceiling-db", -1.0),
                ("true-peak-oversample", 4),
            )
            if _option_supplied(args, name, default)
        ]
        if ignored_loudness:
            joined = ", ".join(f"--{name}" for name in ignored_loudness)
            raise ValueError(f"{joined} cannot be combined with --{selectors[0]}")

    params = _parse_kv_params(params_raw) if params_raw else {}
    result: Any
    mode = "loudness"
    explanation: list[str] = []
    if preset:
        from . import master_audio

        result = master_audio(
            samples,
            sample_rate=sr,
            preset_name=cast("MasteringPreset", preset),
            overrides=params or None,
        )
        mode = "preset"
    elif config_raw:
        from . import mastering_chain

        config = _chain_params_config(_mastering_config(config_raw))
        config.update(params)
        result = mastering_chain(samples, sample_rate=sr, config=config)
        mode = "config"
    elif assistant:
        from . import mastering_assistant_suggest, mastering_chain

        suggestion_params: dict[str, float | int | bool | str] = {
            "targetLufs": float(getattr(args, "target_lufs", -14.0)),
            "ceilingDb": float(getattr(args, "ceiling_db", -1.0)),
            "enableRepair": enable_repair,
        }
        suggestion = json.loads(
            mastering_assistant_suggest(samples, sample_rate=sr, params=suggestion_params)
        )
        if not isinstance(suggestion, dict) or not isinstance(suggestion.get("chainConfig"), dict):
            raise ValueError("mastering assistant returned an invalid chain config")
        config = _chain_params_config(suggestion["chainConfig"])
        config.update(params)
        # Match the native assistant route: an explicitly supplied shortcut
        # wins over a flat --params override; an omitted shortcut leaves the
        # assistant's suggested/default chain value intact.
        if _option_supplied(args, "true-peak-oversample", 4):
            config["loudness.truePeakOversample"] = float(getattr(args, "true_peak_oversample", 4))
        result = mastering_chain(samples, sample_rate=sr, config=config)
        explanation_value = suggestion.get("explanation", [])
        if not isinstance(explanation_value, list) or not all(
            isinstance(item, str) for item in explanation_value
        ):
            raise ValueError("mastering assistant returned invalid explanation data")
        explanation = list(explanation_value)
        mode = "assistant"
    elif report_path:
        from . import mastering_chain

        result = mastering_chain(
            samples,
            sample_rate=sr,
            config={
                "loudness": {
                    "enabled": True,
                    "targetLufs": getattr(args, "target_lufs", -14.0),
                    "ceilingDb": getattr(args, "ceiling_db", -1.0),
                    "truePeakOversample": getattr(args, "true_peak_oversample", 4),
                }
            },
        )
    else:
        from .audio import Audio

        result = Audio.from_buffer(samples, sr).mastering(
            target_lufs=getattr(args, "target_lufs", -14.0),
            ceiling_db=getattr(args, "ceiling_db", -1.0),
            true_peak_oversample=getattr(args, "true_peak_oversample", 4),
        )

    output = getattr(args, "output", "") or ""
    if output:
        _write_wav(output, result.samples, result.sample_rate, bits)
    if report_path:
        _write_mastering_report(report_path, result.report)

    if getattr(args, "json", False):
        if mode != "loudness":
            payload = _mastering_chain_payload(
                result,
                mode=mode,
                output=output,
                preset=preset,
                explanation=explanation if explain else None,
                include_report_latency=bool(report_path),
            )
        else:
            payload = {
                "input_lufs": result.input_lufs,
                "output_lufs": result.output_lufs,
                "applied_gain_db": result.applied_gain_db,
                "target_lufs": getattr(args, "target_lufs", -14.0),
                "ceiling_db": getattr(args, "ceiling_db", -1.0),
                "true_peak_oversample": getattr(args, "true_peak_oversample", 4),
                "latency_samples": getattr(result, "latency_samples", 0),
                "loudness_target_limited": bool(result.loudness_target_limited),
                "sample_rate": result.sample_rate,
                "output": output,
            }
        print(_strict_json_dumps(payload))
    else:
        print("  Mastering:" if mode == "loudness" else f"  Mastering {mode}:")
        if preset:
            print(f"    Preset:       {preset}")
        if mode != "loudness":
            stages = list(getattr(result, "stages", []))
            print(f"    Stages:       {', '.join(stages) if stages else '(none)'}")
        print(f"    Input LUFS:  {result.input_lufs:.2f}")
        print(f"    Output LUFS: {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if output:
            print(f"    Wrote: {output}")
    return 0


def cmd_mastering_processor(args: argparse.Namespace) -> int:
    from . import mastering_process, mastering_process_stereo, mastering_processor_catalog

    samples, sr = _load_audio(args.file)
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw) if params_raw else {}
    bits = _wav_bits(args)
    processor = getattr(args, "processor", None) or ""
    processor_name = cast("SoloProcessor", processor)
    stereo_only = {
        entry["id"] for entry in mastering_processor_catalog() if entry.get("stereoOnly", False)
    }
    explicit_stereo = bool(getattr(args, "stereo", False))
    use_stereo = explicit_stereo or processor in stereo_only
    result: Any
    if use_stereo:
        if processor in stereo_only and not explicit_stereo:
            print(
                "warning: stereo-only processor preview duplicates the mono input on left/right; "
                "inspect stereo results through the Python API for production decisions",
                file=sys.stderr,
            )
        stereo = mastering_process_stereo(
            processor_name, samples, samples, sample_rate=sr, params=params
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
        result = mastering_process(processor_name, samples, sample_rate=sr, params=params)

    output = getattr(args, "output", "") or ""
    if output:
        _write_wav(output, result.samples, result.sample_rate, bits)

    if getattr(args, "json", False):
        payload = {
            "processor": processor,
            "input_lufs": result.input_lufs,
            "output_lufs": result.output_lufs,
            "applied_gain_db": result.applied_gain_db,
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
            "output": output,
            "stereo": use_stereo,
        }
        print(_strict_json_dumps(payload))
    else:
        print(f"  Mastering processor: {processor}")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if output:
            print(f"    Wrote: {output}")
    return 0


def cmd_eq(args: argparse.Namespace) -> int:
    from . import mastering_process

    samples, sr = _load_audio(args.file)
    params_raw = getattr(args, "params", "") or ""
    bits = _wav_bits(args)
    if params_raw:
        conflicts = _eq_shortcut_names(args)
        if conflicts:
            joined = ", ".join(f"--{name}" for name in conflicts)
            raise ValueError(f"{joined} cannot be combined with --params")
        params = _parse_kv_params(params_raw)
    else:
        params = {
            "band0.enabled": 1.0,
            "band0.type": float(getattr(args, "type", 0)),
            "band0.frequencyHz": float(getattr(args, "frequency_hz", 1000.0)),
            "band0.gainDb": float(getattr(args, "gain_db", 0.0)),
            "band0.q": float(getattr(args, "q", 1.0)),
            "band0.coeffMode": float(getattr(args, "coeff_mode", 0)),
            "band0.slopeDbOct": float(getattr(args, "slope_db_oct", 12)),
            "band0.placement": float(getattr(args, "placement", 0)),
            "band0.proportionalQ": 1.0 if getattr(args, "proportional_q", False) else 0.0,
            "band0.dynamic": 1.0 if getattr(args, "dynamic", False) else 0.0,
            "band0.thresholdDb": float(getattr(args, "threshold_db", -24.0)),
            "band0.autoThreshold": 1.0 if getattr(args, "auto_threshold", False) else 0.0,
            "band0.ratio": float(getattr(args, "ratio", 2.0)),
            "band0.rangeDb": float(getattr(args, "range_db", -6.0)),
            "band0.attackMs": float(getattr(args, "attack_ms", 5.0)),
            "band0.releaseMs": float(getattr(args, "release_ms", 50.0)),
            "band0.detectorDelayMs": float(getattr(args, "lookahead_ms", 0.0)),
            "band0.sidechainFreqHz": float(getattr(args, "sidechain_freq_hz", -1.0)),
            "band0.sidechainQ": float(getattr(args, "sidechain_q", 1.0)),
            "phaseMode": float(getattr(args, "phase_mode", 1)),
            "resolution": float(getattr(args, "resolution", 0)),
            "autoGain": 1.0 if getattr(args, "auto_gain", False) else 0.0,
            "gainScale": float(getattr(args, "gain_scale", 1.0)),
            "outputGainDb": float(getattr(args, "output_gain_db", 0.0)),
            "outputPan": float(getattr(args, "output_pan", 0.0)),
        }
    result = mastering_process("eq.equalizer", samples, sample_rate=sr, params=params)

    output = getattr(args, "output", "") or ""
    if output:
        _write_wav(output, result.samples, result.sample_rate, bits)

    if getattr(args, "json", False):
        payload = {
            "processor": "eq.equalizer",
            "input_lufs": result.input_lufs,
            "output_lufs": result.output_lufs,
            "applied_gain_db": result.applied_gain_db,
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
            "output": output,
        }
        print(_strict_json_dumps(payload))
    else:
        print("  Equalizer")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if output:
            print(f"    Wrote: {output}")
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
        raise ValueError("reference sample rate must match input sample rate")
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw) if params_raw else {}
    result_json = mastering_pair_analyze(
        args.analysis, source, reference, sample_rate=sr, params=params or None
    )
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
    params: dict[str, float | int | bool | str] = (
        dict(_parse_kv_params(args.params)) if args.params else {}
    )
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

    # The declared argparse default is the only default: a second fallback here
    # would be a value the published contract does not name.
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
