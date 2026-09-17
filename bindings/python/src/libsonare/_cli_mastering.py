"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import TYPE_CHECKING, Any, cast

from ._cli_common import (
    _atomic_wav_writer,
    _atomic_write_bytes,
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


def _json_key_to_snake_case(key: str) -> str:
    """Rewrite one camelCase JSON key as snake_case ("gainToMatchDb" -> "gain_to_match_db")."""
    out: list[str] = []
    for char in key:
        if char.isupper():
            if out:
                out.append("_")
            out.append(char.lower())
        else:
            out.append(char)
    return "".join(out)


def _json_keys_to_snake_case(value: Any) -> Any:
    """Recursively re-key a parsed JSON payload from camelCase to snake_case."""
    if isinstance(value, dict):
        return {_json_key_to_snake_case(k): _json_keys_to_snake_case(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_json_keys_to_snake_case(item) for item in value]
    return value


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
    """Write the report through the shared artifact writer.

    Routing it there rather than through a bare ``open`` gives the report the
    same atomic replace and the same write-failure exit class as every other
    artifact the CLI produces.
    """
    payload = _strict_json_dumps(_mastering_report_payload(report)) + "\n"
    _atomic_write_bytes(path, payload.encode("utf-8"))


def _wav_bits(args: argparse.Namespace) -> int:
    """Return the requested PCM width, keeping the CLI contract intentionally small."""
    bits = int(getattr(args, "bits", 16))
    if bits not in (16, 24):
        raise ValueError("bits must be 16 or 24")
    return bits


def _option_supplied(args: argparse.Namespace, name: str) -> bool:
    """Report whether the caller spelled ``--name`` on the command line.

    The parser records the destination of every option present on argv, so an
    option carrying its documented default still counts as supplied -- the same
    answer the native ``CliArgs::has`` gives for identical argv.
    """
    supplied = getattr(args, "_supplied_options", ())
    return name.replace("-", "_") in supplied


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


# Every ``eq`` option that selects part of the band --params would otherwise
# specify in full, in the native handler's order.
_EQ_SHORTCUT_NAMES = (
    "type",
    "frequency-hz",
    "gain-db",
    "q",
    "coeff-mode",
    "slope-db-oct",
    "placement",
    "proportional-q",
    "dynamic",
    "threshold-db",
    "auto-threshold",
    "ratio",
    "range-db",
    "attack-ms",
    "release-ms",
    "lookahead-ms",
    "sidechain-freq-hz",
    "sidechain-q",
    "phase-mode",
    "resolution",
    "auto-gain",
    "gain-scale",
    "output-gain-db",
    "output-pan",
)


def _eq_shortcut_names(args: argparse.Namespace) -> list[str]:
    return [name for name in _EQ_SHORTCUT_NAMES if _option_supplied(args, name)]


# Highest accepted index of every ``eq`` option that selects an enumerator, keyed
# by its argparse destination. The enumerations are EqBandType, BiquadCoeffMode,
# StereoPlacement and PhaseMode in mastering/eq/eq_band.h plus
# LinearPhaseEqConfig::Resolution; the switches that map an index answer an
# unknown one with their first enumerator, so an unchecked `--type 999` would
# apply a peak filter and exit 0. The native CLI declares the same bounds in its
# registry and the cross-surface option-domain comparison pins the two together.
_EQ_ENUM_BOUNDS = {
    "type": 8,
    "coeff_mode": 1,
    "placement": 4,
    "phase_mode": 3,
    "resolution": 5,
}


def _reject_unknown_processor_params(processor: str, params: dict[str, float]) -> None:
    """Reject a supplied ``--params`` key the named processor does not read.

    ``mastering_insert_param_names`` builds the processor against an empty param
    map and reports every key its config builder probed, so a supplied key
    outside that set took no effect at all: ``band0.bogusKey=42`` used to ship a
    chain containing none of the edit the caller asked for, under exit 0 and a
    normal JSON payload.

    A name with no published key set is left alone rather than rejected
    wholesale: only a realtime insert can be probed this way, so an empty list
    means "not an insert" (an offline-only processor, or one whose build feature
    is off), not "reads nothing". Those ids keep the old silent-ignore
    behaviour, which is the remaining half of this gap.
    """
    from . import mastering_insert_param_names

    known = set(mastering_insert_param_names(processor))
    if not known:
        return
    unknown = [key for key in params if key not in known]
    if unknown:
        joined = ", ".join(unknown)
        raise ValueError(f"unknown --params key for {processor}: {joined}")


def _check_eq_enum_options(args: argparse.Namespace) -> None:
    """Reject an ``eq`` enumerator index outside its enumeration."""
    for dest, highest in _EQ_ENUM_BOUNDS.items():
        value = getattr(args, dest, 0)
        if not isinstance(value, int) or 0 <= value <= highest:
            continue
        option = "--" + dest.replace("_", "-")
        accepted = ", ".join(str(index) for index in range(highest + 1))
        raise ValueError(f"invalid value for {option}: {value} (expected one of {accepted})")


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
    target_platform = getattr(args, "target_platform", "streaming") or "streaming"
    no_streaming_safe = bool(getattr(args, "no_streaming_safe", False))
    speech_mono_amount = float(getattr(args, "speech_mono_amount", 1.0))
    # Every option that only reaches an AssistantConfig field, refused rather
    # than accepted and dropped -- the same list, in the same order, the native
    # handler refuses.
    if not assistant:
        for name in (
            "enable-repair",
            "explain",
            "target-platform",
            "no-streaming-safe",
            "speech-mono-amount",
        ):
            if _option_supplied(args, name):
                raise ValueError(f"--{name} requires --assistant")

    # The preset/config chain is driven entirely by its config, so a standalone
    # loudness flag would be silently discarded.
    if selectors and not assistant:
        ignored_loudness = [
            name
            for name in ("target-lufs", "ceiling-db", "true-peak-oversample")
            if _option_supplied(args, name)
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
            "enableRepair": enable_repair,
            # Resolved to its table index inside mastering_assistant_suggest, by
            # the library rather than by a mapping restated here.
            "targetPlatform": target_platform,
            "preferStreamingSafe": not no_streaming_safe,
            "speechMonoAmount": speech_mono_amount,
        }
        # Sent only when the caller named them. Supplying a key marks the field
        # as explicit for the assistant, and a delivery target only fills in what
        # the caller left alone -- so passing the default through unconditionally
        # suppressed every platform target's loudness.
        if _option_supplied(args, "target-lufs"):
            suggestion_params["targetLufs"] = float(getattr(args, "target_lufs", -14.0))
        if _option_supplied(args, "ceiling-db"):
            suggestion_params["ceilingDb"] = float(getattr(args, "ceiling_db", -1.0))
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
        if _option_supplied(args, "true-peak-oversample"):
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
    _reject_unknown_processor_params(processor, params)
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

    # Ahead of the audio load and the --params conflict check, matching the
    # native CLI, which enforces every declared option domain from its registry
    # before dispatching the handler.
    _check_eq_enum_options(args)
    samples, sr = _load_audio(args.file)
    params_raw = getattr(args, "params", "") or ""
    bits = _wav_bits(args)
    if params_raw:
        conflicts = _eq_shortcut_names(args)
        if conflicts:
            joined = ", ".join(f"--{name}" for name in conflicts)
            raise ValueError(f"{joined} cannot be combined with --params")
        params = _parse_kv_params(params_raw)
        _reject_unknown_processor_params("eq.equalizer", params)
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
    # The library returns a JSON string regardless of --json, keyed in camelCase
    # like every other core JSON producer. CLI stdout is snake_case throughout,
    # so re-key the payload rather than printing the core spelling verbatim.
    print(_strict_json_dumps(_json_keys_to_snake_case(json.loads(result_json))))
    return 0


def cmd_mastering_pair_processor(args: argparse.Namespace) -> int:
    from . import mastering_pair_process

    source, sr = _load_audio(args.file)
    reference, ref_sr = _load_audio(args.reference)
    if ref_sr != sr:
        raise ValueError("reference sample rate must match input sample rate")
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw) if params_raw else {}
    result = mastering_pair_process(
        args.processor, source, reference, sample_rate=sr, params=params or None
    )
    output = getattr(args, "output", "") or ""
    if output:
        _write_wav(output, result.samples, result.sample_rate, _wav_bits(args))
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "processor": args.processor,
                    "input_lufs": result.input_lufs,
                    "output_lufs": result.output_lufs,
                    "applied_gain_db": result.applied_gain_db,
                    "latency_samples": result.latency_samples,
                    "output": output,
                }
            )
        )
    else:
        print(f"  Mastering pair processor: {args.processor}")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        print(f"    Latency:      {result.latency_samples} samples")
        if output:
            print(f"    Wrote: {output}")
    return 0


def cmd_mastering_stereo_analyze(args: argparse.Namespace) -> int:
    from . import mastering_stereo_analyze

    left, sr = _load_audio(args.file)
    right, ref_sr = _load_audio(args.reference)
    if ref_sr != sr:
        raise ValueError("reference sample rate must match input sample rate")
    if len(right) != len(left):
        raise ValueError("reference length must match input length")
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw) if params_raw else {}
    result_json = mastering_stereo_analyze(
        args.analysis, left, right, sample_rate=sr, params=params or None
    )
    # Same core JSON producer as mastering-pair-analyze above, so the same
    # re-keying: the document arrives in camelCase and CLI stdout is snake_case.
    print(_strict_json_dumps(_json_keys_to_snake_case(json.loads(result_json))))
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
    result_json = mastering_streaming_preview(samples, sample_rate=sr, platforms=platforms)
    print(_strict_json_dumps(_json_keys_to_snake_case(json.loads(result_json))))
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


# Every ".enabled" flag build_chain_params() (chain_json.cpp) emits under a
# module other than "repair", forced off so a named preset built for a full
# master (e.g. "speech", which pairs repair.denoise with eq.tilt,
# dynamics.deesser and loudness) cannot master through `repair --preset`.
# Keep in sync with build_chain_params() if the chain gains a new module.
_NON_REPAIR_CHAIN_DISABLE_OVERRIDES: dict[str, float] = {
    "eq.tilt.enabled": 0.0,
    "dynamics.deesser.enabled": 0.0,
    "dynamics.transientShaper.enabled": 0.0,
    "dynamics.compressor.enabled": 0.0,
    "dynamics.multibandComp.enabled": 0.0,
    "saturation.tape.enabled": 0.0,
    "saturation.exciter.enabled": 0.0,
    "spectral.airBand.enabled": 0.0,
    "stereo.imager.enabled": 0.0,
    "stereo.monoMaker.enabled": 0.0,
    "maximizer.truePeakLimiter.enabled": 0.0,
    "loudness.enabled": 0.0,
}


def _parse_repair_params(raw: str) -> dict[str, float]:
    """Parse ``repair --params`` into the chain's flat ``repair.*`` dict.

    Every value crosses to C as the same double the chain-config transport
    uses for every field (``SonareMasteringParam.value``); the chain refuses a
    fractional value for an integer field there, naming the key, so this parser
    deliberately carries no second list of which fields are integral. Splitting
    that judgement across two surfaces is what would let them disagree.
    """
    params: dict[str, float] = {}
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise ValueError(f"invalid param (expected key=value): {item}")
        key, value = (part.strip() for part in item.split("=", 1))
        if not key.startswith("repair."):
            raise ValueError(f"--params key must be a repair.* field: {key}")
        params[key] = float(value)
    return params


# select_repair_stages() (suggester.cpp) names the stage at the start of every
# explanation line it writes for a repair decision; the remaining lines
# explain non-repair choices (preset/genre, EQ, dynamics) that `repair` never
# applies.
_REPAIR_EXPLANATION_PREFIXES = (
    "declip:",
    "declick:",
    "decrackle:",
    "dehum:",
    "denoise:",
    "dereverb:",
)


def _repair_explanation(explanation: list[str]) -> list[str]:
    return [
        line
        for line in explanation
        if line.startswith(_REPAIR_EXPLANATION_PREFIXES) or "repair" in line
    ]


def _repair_detection_report(defects: dict[str, Any]) -> dict[str, object]:
    """Render the assistant's defect profile as the flat, snake_case ``defects``
    object the native CLI's ``--json`` also emits, key-for-key.

    ``noise_band_measured`` and ``hum_peak_found`` are added booleans, not
    replacements: ``noiseBandPeakIndex == -1`` and
    ``humFundamentalProminence == 1.0`` are both "could not decide" sentinels
    that a raw number invites a reader to mistake for a measured value, so a
    boolean says so where it cannot be missed. When nothing was measured, no
    numeric field is emitted at all -- there must be nothing here for a
    caller to misread as "clean".
    """
    if not bool(defects.get("measured", False)):
        return {"measured": False}

    band_index = int(defects.get("noiseBandPeakIndex", -1))
    hum_prominence = float(defects.get("humFundamentalProminence", 1.0))
    return {
        "measured": True,
        "click_count": int(defects.get("clickCount", 0)),
        "click_rejected": int(defects.get("clickRejected", 0)),
        "click_longest_run_samples": int(defects.get("clickLongestRunSamples", 0)),
        "click_per_second": float(defects.get("clickPerSecond", 0.0)),
        "crackle_sample_count": int(defects.get("crackleSampleCount", 0)),
        "crackle_sample_fraction": float(defects.get("crackleSampleFraction", 0.0)),
        "crackle_per_second": float(defects.get("cracklePerSecond", 0.0)),
        "clip_sample_count": int(defects.get("clipSampleCount", 0)),
        "clip_run_count": int(defects.get("clipRunCount", 0)),
        "clip_longest_run_samples": int(defects.get("clipLongestRunSamples", 0)),
        "clip_sample_fraction": float(defects.get("clipSampleFraction", 0.0)),
        "clip_flat_run_count": int(defects.get("clipFlatRunCount", 0)),
        "clip_flat_sample_count": int(defects.get("clipFlatSampleCount", 0)),
        "clip_longest_flat_run_samples": int(defects.get("clipLongestFlatRunSamples", 0)),
        "clip_flat_level": float(defects.get("clipFlatLevel", 0.0)),
        "noise_floor_dbfs": float(defects.get("noiseFloorDbfs", 0.0)),
        "noise_band_measured": band_index >= 0,
        "noise_band_peak_dbfs": float(defects.get("noiseBandPeakDbfs", 0.0)),
        "noise_band_peak_index": band_index,
        "hum_peak_found": hum_prominence > 1.0,
        "hum_fundamental_hz": float(defects.get("humFundamentalHz", 0.0)),
        "hum_fundamental_prominence": hum_prominence,
        "hum_harmonics": int(defects.get("humHarmonics", 0)),
        "hum_fundamental_dbfs": float(defects.get("humFundamentalDbfs", 0.0)),
        "hum_peak_harmonic_dbfs": float(defects.get("humPeakHarmonicDbfs", 0.0)),
        "late_decay_ratio_db": float(defects.get("lateDecayRatioDb", 0.0)),
    }


def _print_repair_detection_report(report: dict[str, object]) -> None:
    print("  Repair detection:")
    if not report.get("measured"):
        print("    Measured:     no (nothing looked at this recording)")
        return
    print("    Measured:     yes")
    click_count = cast("int", report["click_count"])
    click_rejected = cast("int", report["click_rejected"])
    print(
        f"    Clicks:       {click_count} found, {click_rejected} rejected as undecidable, "
        f"longest run {report['click_longest_run_samples']} samples, "
        f"{cast('float', report['click_per_second']):.2f}/s"
    )
    if click_count == 0 and click_rejected > 0:
        print(
            "    Note:         click detector rejected candidates as undecidable "
            "and confirmed none -- detector uncertainty, not necessarily a clean signal"
        )
    print(
        f"    Crackle:      {report['crackle_sample_count']} samples "
        f"({cast('float', report['crackle_sample_fraction']) * 100:.3f}% of input), "
        f"{cast('float', report['crackle_per_second']):.2f}/s"
    )
    print(
        f"    Clipping:     {report['clip_sample_count']} samples in "
        f"{report['clip_run_count']} run(s) "
        f"({cast('float', report['clip_sample_fraction']) * 100:.3f}% of input), "
        f"longest run {report['clip_longest_run_samples']} samples"
    )
    if cast("int", report["clip_flat_run_count"]) > 0:
        print(
            f"    Flat tops:    {report['clip_flat_run_count']} run(s) pinned at "
            f"{cast('float', report['clip_flat_level']):.3f} "
            f"({report['clip_flat_sample_count']} samples, longest "
            f"{report['clip_longest_flat_run_samples']}) -- these survive a later gain "
            "change, so they are clipping rather than a peak that reaches the ceiling"
        )
    else:
        print(
            "    Flat tops:    none (an unclipped peak reaches the ceiling too; resampling "
            "or a lossy codec erases a real one, so this is not proof of no clipping)"
        )
    band_text = (
        f"band {report['noise_band_peak_index']}"
        if report["noise_band_measured"]
        else "no band measured"
    )
    print(
        f"    Noise floor:  {cast('float', report['noise_floor_dbfs']):.1f} dBFS, "
        f"loudest band {cast('float', report['noise_band_peak_dbfs']):.1f} dBFS ({band_text})"
    )
    if report["hum_peak_found"]:
        print(
            f"    Hum:          {cast('float', report['hum_fundamental_hz']):.1f} Hz, "
            f"{cast('float', report['hum_fundamental_prominence']):.1f}x the rest of the search, "
            f"{report['hum_harmonics']} harmonic(s), "
            f"fundamental {cast('float', report['hum_fundamental_dbfs']):.1f} dBFS, "
            f"loudest harmonic {cast('float', report['hum_peak_harmonic_dbfs']):.1f} dBFS"
        )
    else:
        print("    Hum:          search found no candidate peak (could not decide)")
    print(
        f"    Late decay:   {cast('float', report['late_decay_ratio_db']):.1f} dB "
        "(not RT60; less negative means more reverberant)"
    )


def cmd_repair(args: argparse.Namespace) -> int:
    """Measure repair defects and/or apply only the repair stages a file needs.

    Unlike ``mastering --enable-repair``, this command never masters: whatever
    non-repair stage a preset or the assistant would also turn on (EQ,
    dynamics, saturation, loudness) is dropped or forced off before the chain
    runs.
    """
    from . import (
        master_audio,
        mastering_assistant_suggest,
        mastering_audio_profile,
        mastering_chain,
    )

    detect_only = bool(getattr(args, "detect", False))
    preset = getattr(args, "preset", "") or ""
    params_raw = getattr(args, "params", "") or ""
    explain = bool(getattr(args, "explain", False))
    output = getattr(args, "output", "") or ""

    if detect_only:
        if explain:
            raise ValueError(
                "--explain has nothing to explain under --detect: no repair stage "
                "runs, so none was chosen -- drop --detect to see why the assistant "
                "would pick a stage"
            )
        for name in ("preset", "params", "bits", "output"):
            if _option_supplied(args, name):
                raise ValueError(f"--{name} cannot be combined with --detect")
    elif not output:
        raise ValueError("repair requires --output unless --detect is given")

    if preset and explain:
        raise ValueError(
            "--explain has nothing to explain under --preset: a preset's repair "
            "stages are named directly, not chosen by measurement -- drop --preset "
            "to see why the assistant would pick a stage"
        )

    samples, sr = _load_audio(args.file)

    # `defects` is part of the --json contract in every mode (and of the text
    # report whenever it prints), so it is always measured here -- never only
    # when it happens to be convenient for one mode.
    profile = json.loads(
        mastering_audio_profile(samples, sample_rate=sr, params={"detectDefects": True})
    )
    raw_defects = profile.get("defects") if isinstance(profile, dict) else None
    if not isinstance(raw_defects, dict):
        raise ValueError("libsonare did not return a defect profile")
    defects = _repair_detection_report(raw_defects)

    if detect_only:
        if getattr(args, "json", False):
            print(_strict_json_dumps({"mode": "detect", "defects": defects}))
        else:
            _print_repair_detection_report(defects)
        return 0

    params = _parse_repair_params(params_raw) if params_raw else {}
    bits = _wav_bits(args)
    explanation: list[str] = []
    result: Any

    if preset:
        overrides: dict[str, float] = dict(_NON_REPAIR_CHAIN_DISABLE_OVERRIDES)
        overrides.update(params)
        result = master_audio(
            samples,
            sample_rate=sr,
            preset_name=cast("MasteringPreset", preset),
            overrides=overrides,
        )
        mode = "preset"
    else:
        suggestion = json.loads(
            mastering_assistant_suggest(samples, sample_rate=sr, params={"enableRepair": True})
        )
        if not isinstance(suggestion, dict) or not isinstance(suggestion.get("chainConfig"), dict):
            raise ValueError("mastering assistant returned an invalid chain config")
        chain_config = _chain_params_config(suggestion["chainConfig"])
        repair_config = {
            key: value for key, value in chain_config.items() if key.startswith("repair.")
        }
        repair_config.update(params)
        result = mastering_chain(samples, sample_rate=sr, config=repair_config)
        mode = "assistant"
        explanation_value = suggestion.get("explanation", [])
        if not isinstance(explanation_value, list) or not all(
            isinstance(item, str) for item in explanation_value
        ):
            raise ValueError("mastering assistant returned invalid explanation data")
        explanation = _repair_explanation(explanation_value)

    stages = list(getattr(result, "stages", []))
    _write_wav(output, result.samples, result.sample_rate, bits)

    if getattr(args, "json", False):
        payload: dict[str, object] = {"mode": mode}
        if preset:
            payload["preset"] = preset
        payload["stages"] = stages
        if explain:
            payload["explanation"] = explanation
        payload["output"] = output
        payload["defects"] = defects
        print(_strict_json_dumps(payload))
    else:
        print(f"  Repair ({mode}):")
        if preset:
            print(f"    Preset:  {preset}")
        print(f"    Stages:  {', '.join(stages) if stages else '(none)'}")
        if explain:
            if explanation:
                for line in explanation:
                    print(f"    Why:     {line}")
            else:
                print("    Why:     (no repair stage was chosen)")
        _print_repair_detection_report(defects)
        print(f"    Wrote: {output}")
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
    suggestion = json.loads(mastering_assistant_suggest(samples, sample_rate=sr, params=params))
    # The suggested chain is fed back to the library verbatim (`mastering --config`),
    # so the names under it belong to the chain param schema rather than to CLI
    # stdout, and re-keying them would make the document the library rejects.
    # Everything beside it is measurement output and follows the snake_case rule.
    chain_config = suggestion.pop("chainConfig", None)
    payload = _json_keys_to_snake_case(suggestion)
    if chain_config is not None:
        payload["chain_config"] = chain_config
    print(_strict_json_dumps(payload))
    return 0


def cmd_mastering_profile(args: argparse.Namespace) -> int:
    from . import mastering_audio_profile

    samples, sr = _load_audio(args.file)
    params = _parse_kv_params(args.params) if args.params else {}
    result_json = mastering_audio_profile(samples, sample_rate=sr, params=params)
    print(_strict_json_dumps(_json_keys_to_snake_case(json.loads(result_json))))
    return 0


def _mix_assistant_options(params: dict[str, float]) -> dict[str, Any]:
    """Map ``suggest-mix --params`` keys onto ``suggest_mix_scene`` keywords.

    The accepted set is the assistant's own option table, so either spelling the
    C ABI reads is accepted and nothing here restates the list. Two conversions
    are this boundary's: an option whose config field is a bool is converted
    rather than handed a number, and an integral value for an integer option is
    narrowed, since ``--params`` parses every value as a float.
    """
    from .mixing_assistant import _INTEGER_PARAMS, _PARAM_KEYS

    options: dict[str, Any] = {}
    for key, value in params.items():
        name = _json_key_to_snake_case(key)
        if name not in _PARAM_KEYS:
            raise ValueError(f"unknown suggest-mix param: {key}")
        if name.startswith("enable_"):
            options[name] = value != 0.0
        elif name in _INTEGER_PARAMS and value.is_integer():
            options[name] = int(value)
        else:
            options[name] = value
    return options


def _mix_assistant_tracks(entries: list[str], sample_rate: int) -> list[Any]:
    """Load each ``[ID=]WAV`` entry as one mono track at ``sample_rate``."""
    from .mixing_assistant import MixTrackInput

    tracks: list[Any] = []
    for entry in entries:
        track_id, separator, path = entry.partition("=")
        if not separator:
            # Bare path: the file's own name is the id the scene addresses it by.
            path = entry
            track_id = os.path.splitext(os.path.basename(path))[0]
        if not track_id:
            raise ValueError(f"--input track id must not be empty: {entry}")
        if not path:
            raise ValueError(f"--input requires a file path: {entry}")
        samples, track_rate = _load_audio(path)
        if track_rate != sample_rate:
            samples = _resample(samples, track_rate, sample_rate)
        tracks.append(MixTrackInput(track_id=track_id, left=samples, name=track_id))
    return tracks


def _resolve_tempo_bpm(raw: str, entries: list[str]) -> float:
    """Read ``--tempo-bpm``, detecting it from the first track when asked to.

    ``auto`` measures the first ``--input`` rather than the whole set: a tempo is
    a property of the song, so every track shares one, and detecting it once on
    the track the caller listed first keeps which file was measured visible in
    the command instead of hidden in an averaging rule.
    """
    from . import detect_bpm

    if raw.strip().lower() != "auto":
        return float(raw)
    _, _, path = entries[0].partition("=")
    samples, sample_rate = _load_audio(path or entries[0])
    return float(detect_bpm(samples, sample_rate=sample_rate))


def cmd_suggest_mix(args: argparse.Namespace) -> int:
    from . import suggest_mix_scene

    if not args.input:
        raise ValueError("suggest-mix requires at least one --input")
    options = _mix_assistant_options(_parse_kv_params(args.params) if args.params else {})
    if args.tempo_bpm:
        # The dedicated option and `--params tempoBpm=` reach the same field, so
        # naming both is a contradiction rather than a precedence question.
        if "tempo_bpm" in options:
            raise ValueError("--tempo-bpm and --params tempoBpm= set the same value")
        options["tempo_bpm"] = _resolve_tempo_bpm(args.tempo_bpm, args.input)
    tracks = _mix_assistant_tracks(args.input, args.sample_rate)
    document = suggest_mix_scene(tracks, sample_rate=args.sample_rate, **options)
    if args.scene_out:
        # Written from the document already in hand rather than through
        # suggest_mix_scene_json, which would re-run an STFT per track and every
        # pairwise pass to reach the same scene. That the two agree is pinned by
        # a test rather than assumed here.
        with open(args.scene_out, "w", encoding="utf-8") as fh:
            fh.write(_strict_json_dumps(document.get("scene", {})) + "\n")
    print(_strict_json_dumps(document))
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


def _mix_strip_channels(
    entries: list[str], strip_ids: list[str], sample_rate: int
) -> tuple[list[list[float]], int]:
    """Resolve ``--input`` entries onto the scene's strips, in strip order.

    Two spellings normalize here so the rest of the command sees one shape.
    Addressed by id — either an explicit ``ID=WAV`` or a bare path whose base
    name names a strip — a scene may carry strips no input feeds, which is what
    an assistant-suggested scene always looks like: its effect returns are fed
    by sends rather than by a file, and requiring a silent WAV for each of them
    made the suggestion unrenderable without one. Addressed positionally, the
    historical form, every strip takes the input at its own index; it is kept
    because a built-in preset's strip ids are fixed vocabulary that a file on
    disk has no reason to match.

    A strip no entry names is fed silence rather than dropped: it may still
    carry an insert whose tail belongs in the mix.

    Returns the per-strip buffers and their shared length. Inputs shorter than
    the longest are padded rather than the set being truncated to the shortest,
    which would delete a part that only enters late in the song.
    """
    resolved: dict[int, list[float]] = {}
    positional: list[list[float]] = []
    addressed = False
    index_of = {strip_id: index for index, strip_id in enumerate(strip_ids)}

    for entry in entries:
        track_id, separator, path = entry.partition("=")
        if not separator:
            path = entry
            track_id = os.path.splitext(os.path.basename(path))[0]
        elif not track_id:
            raise ValueError(f"--input strip id must not be empty: {entry}")
        if not path:
            raise ValueError(f"--input requires a file path: {entry}")

        samples, in_sr = _load_audio(path)
        if in_sr != sample_rate:
            samples = _resample(samples, in_sr, sample_rate)
        buffer = list(samples)

        target = index_of.get(track_id)
        if target is None:
            if separator:
                # An explicit id is an assertion about the scene, so a miss is
                # the caller's mistake rather than a reason to fall back.
                raise ValueError(
                    f"--input names strip {track_id!r}, which the scene does not have "
                    f"(strips: {', '.join(strip_ids)})"
                )
            positional.append(buffer)
            continue
        if target in resolved:
            raise ValueError(f"--input names strip {track_id!r} more than once")
        addressed = True
        resolved[target] = buffer

    if addressed and positional:
        raise ValueError(
            "--input entries must either all name a strip or all be positional; "
            f"{', '.join(sorted(strip_ids))} are the scene's strips"
        )
    if positional and len(positional) != len(strip_ids):
        raise ValueError(
            f"scene has {len(strip_ids)} strips but {len(positional)} inputs were given; "
            "name them as --input ID=WAV to feed only some of them"
        )
    for index, buffer in enumerate(positional):
        resolved[index] = buffer

    length = max((len(buffer) for buffer in resolved.values()), default=0)
    channels = [resolved.get(index, []) for index in range(len(strip_ids))]
    return [buffer + [0.0] * (length - len(buffer)) for buffer in channels], length


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
            # Each input WAV feeds one strip (mono inputs are duplicated to both
            # channels). Inputs that were captured at a different sample rate are
            # resampled to the mixer rate so a 44.1 kHz stem is not played back
            # fast at the 48 kHz default.
            strip_ids = [
                str(strip.get("id", "")) for strip in json.loads(scene_json).get("strips", [])
            ]
            # The ids are read from the scene text while the buffers are handed
            # to the compiled mixer, so a disagreement between the two would
            # misalign every strip silently rather than fail.
            if len(strip_ids) != strip_count:
                raise ValueError(
                    f"scene text declares {len(strip_ids)} strips but the mixer compiled "
                    f"{strip_count}"
                )
            channels, length = _mix_strip_channels(args.input, strip_ids, args.sample_rate)
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
