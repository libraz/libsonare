"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import math
import os
from typing import TYPE_CHECKING, Any, cast

from ._cli_common import (
    _atomic_write_bytes,
    _float_sequence,
    _json_keys_to_snake_case,
    _load_channels_or_downmix,
    _load_json_object,
    _parse_json_config,
    _parse_json_list,
    _parse_kv_params,
    _strict_json_dumps,
    _write_channel_output,
    _write_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_options import SharedParsers, _add_wav_bits_argument, _finite_float

if TYPE_CHECKING:
    from .analyzer import MasteringPreset, SoloProcessor
    from .types import MasteringChainResult, MasteringChainStereoResult


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
    planes, sr = _load_channels_or_downmix(args.file)
    samples = planes[0]
    stereo = len(planes) == 2
    report_path = getattr(args, "report", "") or ""
    preset = getattr(args, "preset", None) or ""
    config_raw = getattr(args, "config", None) or ""
    assistant = bool(getattr(args, "assistant", False))
    enable_repair = bool(getattr(args, "enable_repair", False))
    explain = bool(getattr(args, "explain", False))
    params_raw = getattr(args, "params", "") or ""
    bits = _wav_bits(args)

    # Named by the option's canonical spelling, which is --chain-config; `mode`
    # below stays the reported payload value.
    selectors = [
        name
        for name, selected in (
            ("preset", bool(preset)),
            ("chain-config", bool(config_raw)),
            ("assistant", assistant),
        )
        if selected
    ]
    if len(selectors) > 1:
        raise ValueError(
            "--preset, --chain-config (--config), and --assistant are mutually exclusive"
        )
    if params_raw and not selectors:
        raise ValueError("--params requires --preset, --chain-config, or --assistant")
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

    def _run_chain(config: dict[str, Any]) -> Any:
        """Run one chain config over however many channels the source has."""
        from . import mastering_chain, mastering_chain_stereo

        if stereo:
            return mastering_chain_stereo(planes[0], planes[1], sample_rate=sr, config=config)
        return mastering_chain(samples, sample_rate=sr, config=config)

    result: Any
    mode = "loudness"
    explanation: list[str] = []
    if preset:
        from . import master_audio, master_audio_stereo

        if stereo:
            result = master_audio_stereo(
                planes[0],
                planes[1],
                sample_rate=sr,
                preset_name=cast("MasteringPreset", preset),
                overrides=params or None,
            )
        else:
            result = master_audio(
                samples,
                sample_rate=sr,
                preset_name=cast("MasteringPreset", preset),
                overrides=params or None,
            )
        mode = "preset"
    elif config_raw:
        config = _chain_params_config(_mastering_config(config_raw))
        config.update(params)
        result = _run_chain(config)
        mode = "config"
    elif assistant:
        from . import mastering_assistant_suggest, mastering_assistant_suggest_stereo

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
        # The assistant reads the material to decide the chain, so it is handed
        # the same channels the chain will run over rather than a fold of them.
        suggestion = json.loads(
            mastering_assistant_suggest_stereo(
                planes[0], planes[1], sample_rate=sr, params=suggestion_params
            )
            if stereo
            else mastering_assistant_suggest(samples, sample_rate=sr, params=suggestion_params)
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
        result = _run_chain(config)
        explanation_value = suggestion.get("explanation", [])
        if not isinstance(explanation_value, list) or not all(
            isinstance(item, str) for item in explanation_value
        ):
            raise ValueError("mastering assistant returned invalid explanation data")
        explanation = list(explanation_value)
        mode = "assistant"
    elif report_path or stereo:
        # The standalone loudness facade has no stereo form, so a stereo source
        # takes the loudness-only chain the report path already uses. That is
        # the same operation rather than a substitute: over one mono input the
        # two produce bit-identical samples and identical input/output LUFS and
        # applied gain.
        result = _run_chain(
            {
                "loudness": {
                    "enabled": True,
                    "targetLufs": getattr(args, "target_lufs", -14.0),
                    "ceilingDb": getattr(args, "ceiling_db", -1.0),
                    "truePeakOversample": getattr(args, "true_peak_oversample", 4),
                }
            }
        )
    else:
        from .audio import Audio

        result = Audio.from_buffer(samples, sr).mastering(
            target_lufs=getattr(args, "target_lufs", -14.0),
            ceiling_db=getattr(args, "ceiling_db", -1.0),
            true_peak_oversample=getattr(args, "true_peak_oversample", 4),
        )

    rendered = [result.left, result.right] if stereo else [result.samples]
    output = getattr(args, "output", "") or ""
    if output:
        _write_channel_output(output, rendered, result.sample_rate, bits)
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

    planes, sr = _load_channels_or_downmix(args.file)
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw) if params_raw else {}
    bits = _wav_bits(args)
    processor = getattr(args, "processor", None) or ""
    _reject_unknown_processor_params(processor, params)
    processor_name = cast("SoloProcessor", processor)
    stereo_only = {
        entry["id"] for entry in mastering_processor_catalog() if entry.get("stereoOnly", False)
    }
    # The file's own channel count and the library's own stereo-only set decide
    # this, with nothing in between. A two-channel source takes the stereo entry
    # point, which accepts every processor: whether a given one links its decision
    # across the pair or runs per channel is already settled per processor inside
    # the library, and downmixing first would take that choice away. A stereo-only
    # processor routes there from any source because the mono entry rejects it.
    use_stereo = len(planes) == 2 or processor in stereo_only
    result: Any
    channels: list[list[float]]
    if use_stereo:
        left, right = (planes[0], planes[1]) if len(planes) == 2 else (planes[0], planes[0])
        stereo = mastering_process_stereo(
            processor_name, left, right, sample_rate=sr, params=params
        )
        # Written as a pair rather than folded back: these processors exist to act
        # on or create a difference between the channels, and a downmix discards
        # exactly what they produced.
        channels = [list(stereo.left), list(stereo.right)]
        result = argparse.Namespace(
            sample_rate=stereo.sample_rate,
            input_lufs=stereo.input_lufs,
            output_lufs=stereo.output_lufs,
            applied_gain_db=stereo.applied_gain_db,
            latency_samples=stereo.latency_samples,
        )
    else:
        result = mastering_process(processor_name, planes[0], sample_rate=sr, params=params)
        channels = [list(result.samples)]

    output = getattr(args, "output", "") or ""
    if output:
        _write_channel_output(output, channels, result.sample_rate, bits)

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
    from . import mastering_chain, mastering_chain_stereo

    planes, sr = _load_channels_or_downmix(args.file)
    config = _parse_json_config(args.config, args.config_file)
    if args.params:
        config.update(_parse_kv_params(args.params))
    # Declared across the branches rather than inferred from the first one: the
    # two results share the metrics this function goes on to read, and differ
    # only in which buffers they carry.
    result: MasteringChainStereoResult | MasteringChainResult
    if len(planes) == 2:
        result = mastering_chain_stereo(planes[0], planes[1], sample_rate=sr, config=config)
        rendered = [result.left, result.right]
    else:
        result = mastering_chain(planes[0], sample_rate=sr, config=config)
        rendered = [result.samples]
    report_path = getattr(args, "report", "")

    if args.output:
        _write_channel_output(args.output, rendered, result.sample_rate)
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
    from . import (
        master_audio,
        master_audio_stereo,
        mastering_assistant_suggest_chain,
        mastering_assistant_suggest_chain_stereo,
        mastering_chain,
        mastering_chain_stereo,
    )

    planes, sr = _load_channels_or_downmix(args.file)
    stereo = len(planes) == 2
    overrides = _parse_json_config(args.config, args.config_file)
    if args.params:
        overrides.update(_parse_kv_params(args.params))
    chain_config_path = getattr(args, "chain_config", None) or ""
    assistant = bool(getattr(args, "assistant", False))
    # --preset carries a default, so which selectors the caller chose is decided
    # by what was spelled rather than by the value that reached the namespace.
    # The same three-way exclusion the native `mastering` handler enforces;
    # --config / --config-file / --params stay overrides on top of whichever one
    # built the chain, as --params is on the native side.
    selectors = [
        name
        for name, selected in (
            ("preset", _option_supplied(args, "preset")),
            ("chain-config", bool(chain_config_path)),
            ("assistant", assistant),
        )
        if selected
    ]
    if len(selectors) > 1:
        raise ValueError("--preset, --chain-config, and --assistant are mutually exclusive")

    def _run_chain(
        config: dict[str, Any],
    ) -> tuple[MasteringChainStereoResult | MasteringChainResult, list[Any]]:
        """Run one complete chain config over however many channels the source has.

        Returns the channels to write beside the result, because which attribute
        carries them is what the two chain entry points differ in.
        """
        if stereo:
            pair = mastering_chain_stereo(planes[0], planes[1], sample_rate=sr, config=config)
            return pair, [pair.left, pair.right]
        mono = mastering_chain(planes[0], sample_rate=sr, config=config)
        return mono, [mono.samples]

    result: MasteringChainStereoResult | MasteringChainResult
    rendered: list[Any]
    mode = "preset"
    if chain_config_path:
        # A complete chain, so it runs as the chain rather than as overrides over
        # a preset the caller never named -- the file states every stage.
        config = _chain_params_config(_load_json_object(chain_config_path))
        config.update(overrides)
        result, rendered = _run_chain(config)
        # `mode` names how the chain was chosen, not the option that carried it,
        # so it is the same token the native CLI reports for a chain read from a
        # file. The option's canonical spelling only shows up in diagnostics.
        mode = "config"
    elif assistant:
        # The assistant reads the material to decide the chain, so it is handed
        # the same channels the chain will run over rather than a fold of them.
        suggested: dict[str, Any] = dict(
            mastering_assistant_suggest_chain_stereo(planes[0], planes[1], sample_rate=sr)
            if stereo
            else mastering_assistant_suggest_chain(planes[0], sample_rate=sr)
        )
        suggested.update(overrides)
        result, rendered = _run_chain(suggested)
        mode = "assistant"
    elif stereo:
        stereo_result = master_audio_stereo(
            planes[0], planes[1], sample_rate=sr, preset_name=args.preset, overrides=overrides
        )
        result, rendered = stereo_result, [stereo_result.left, stereo_result.right]
    else:
        mono_result = master_audio(
            planes[0], sample_rate=sr, preset_name=args.preset, overrides=overrides
        )
        result, rendered = mono_result, [mono_result.samples]
    report_path = getattr(args, "report", "")

    if args.output:
        _write_channel_output(args.output, rendered, result.sample_rate)
    if report_path:
        _write_mastering_report(report_path, result.report)

    if args.json:
        payload: dict[str, object] = {}
        # A preset name only describes the preset route; the other two chose
        # every stage themselves, so they name the route instead of reporting a
        # preset that took no part in the render.
        if mode == "preset":
            payload["preset"] = args.preset
        else:
            payload["mode"] = mode
        payload.update(
            {
                "input_lufs": round(result.input_lufs, 4),
                "output_lufs": round(result.output_lufs, 4),
                "applied_gain_db": round(result.applied_gain_db, 4),
                "sample_rate": result.sample_rate,
                "stages": result.stages,
            }
        )
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print(f"  Master preset: {args.preset}" if mode == "preset" else f"  Master {mode}:")
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
    from . import mastering_repair_declip, mastering_repair_declip_stereo

    planes, sr = _load_channels_or_downmix(args.file)
    knobs = {
        "clip_threshold": args.clip_threshold,
        "lpc_order": args.lpc_order,
        "iterations": args.iterations,
        "lpc_blend": args.lpc_blend,
    }
    if len(planes) == 2:
        # The stereo entry repairs the union of both channels' clipped runs, so
        # a plateau one channel alone reaches is still reconstructed against the
        # other's unclipped extent. Declipping the channels separately cannot
        # see that and is what the mono loader used to force.
        stereo = mastering_repair_declip_stereo(planes[0], planes[1], sample_rate=sr, **knobs)
        rendered = [_float_sequence(stereo.left), _float_sequence(stereo.right)]
    else:
        rendered = [_float_sequence(mastering_repair_declip(planes[0], sample_rate=sr, **knobs))]
    repaired = rendered[0]

    if args.output:
        _write_channel_output(args.output, rendered, sr)

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


def _fit_repair_output_to_full_scale(samples: Any) -> tuple[Any, float]:
    """Scale ``samples`` so their peak lands at full scale.

    Returns the samples to write and the applied gain in dB (``0.0`` when the
    peak already fit and nothing was touched).

    Declipping reconstructs the peaks a clipper cut off, so its output routinely
    exceeds full scale -- and this command deliberately runs no limiter. The
    integer writer clamps, which pins exactly the samples the repair just
    rebuilt back onto the ceiling they were rescued from, undoing the stage that
    was asked for. One gain for the whole file keeps the reconstructed waveform
    intact; a per-sample fit would be the clipper again.
    """
    # A non-finite sample would make every comparison false and leave the peak
    # at 0, so the scale is skipped rather than turned into a NaN gain.
    peak = max((abs(float(v)) for v in samples if math.isfinite(v)), default=0.0)
    if peak <= 1.0:
        return samples, 0.0
    gain = 1.0 / peak
    return [float(v) * gain for v in samples], 20.0 * math.log10(gain)


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
    samples, output_gain_db = _fit_repair_output_to_full_scale(result.samples)
    _write_wav(output, samples, result.sample_rate, bits)

    if getattr(args, "json", False):
        payload: dict[str, object] = {"mode": mode}
        if preset:
            payload["preset"] = preset
        payload["stages"] = stages
        if explain:
            payload["explanation"] = explanation
        payload["output"] = output
        # Always emitted, 0 when the peak already fit: a key that appeared only
        # on the files it acted on would make its absence mean both "did not
        # clip" and "this build does not report it".
        payload["output_gain_db"] = output_gain_db
        payload["defects"] = defects
        print(_strict_json_dumps(payload))
    else:
        print(f"  Repair ({mode}):")
        if preset:
            print(f"    Preset:  {preset}")
        print(f"    Stages:  {', '.join(stages) if stages else '(none)'}")
        if output_gain_db != 0.0:
            print(f"    Gain:    {output_gain_db} dB (the repair rebuilt peaks past full scale)")
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
    # The suggested chain is fed back to the library verbatim
    # (`mastering --chain-config`), so the names under it belong to the chain
    # param schema rather than to CLI stdout, and re-keying them would make the
    # document the library rejects. Everything beside it is measurement output
    # and follows the snake_case rule.
    chain_config = suggestion.pop("chainConfig", None)
    config_out = getattr(args, "config_out", "") or ""
    if config_out:
        if not isinstance(chain_config, dict):
            raise ValueError("mastering assistant returned no chain config to write")
        # Written from the document already in hand rather than through the
        # chain-only entry point, which would re-measure the file to reach the
        # same config, and written through the shared artifact writer so a failed
        # write carries the class every other CLI artifact's does.
        _atomic_write_bytes(config_out, (_strict_json_dumps(chain_config) + "\n").encode("utf-8"))
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


def register_mastering_parsers(sub: argparse._SubParsersAction, shared: SharedParsers) -> None:
    """Register the mastering, repair and processor commands."""
    common = shared.common
    stdout_options = shared.stdout_options

    # Mastering commands
    mastering_p = sub.add_parser(
        "mastering", parents=[common], help="Loudness-normalize with a true-peak ceiling"
    )
    mastering_p.add_argument("--preset", default="")
    # "--chain-config" names a complete chain config in the core's own
    # ``{params, version}`` form. It is the canonical spelling because the bare
    # ``--config`` carries a second meaning on this CLI (preset overrides on
    # ``master``); that original spelling stays accepted here as an alias, and
    # the handler's destination keeps its own name.
    mastering_p.add_argument("--chain-config", "--config", dest="config", default=None)
    mastering_p.add_argument("--target-lufs", type=_finite_float, default=-14.0)
    mastering_p.add_argument("--ceiling-db", type=_finite_float, default=-1.0)
    mastering_p.add_argument("--params", default="")
    _add_wav_bits_argument(mastering_p)
    mastering_p.add_argument(
        "--true-peak-oversample", type=int, choices=(1, 2, 4, 8, 16), default=4
    )
    mastering_p.add_argument("--report", default=None, help="Write a mastering report JSON file")
    mastering_p.add_argument("--assistant", action="store_true")
    mastering_p.add_argument("--enable-repair", action="store_true")
    mastering_p.add_argument("--explain", action="store_true")
    # The remaining assistant controls. The accepted delivery-target names are
    # the rows of the table in src/mastering/assistant/platform_targets.h, which
    # the native CLI reads directly; the cross-surface option-domain comparison
    # is what keeps this restatement pinned to it. ``prefer_streaming_safe``
    # defaults to true in the library, so the reachable control is the one that
    # turns it off. ``--speech-mono-amount`` declares no domain because the
    # suggester clamps it to [0, 1] rather than refusing an outside value.
    mastering_p.add_argument(
        "--target-platform",
        default="streaming",
        choices=(
            "streaming",
            "youtube",
            "broadcast",
            "podcast",
            "audiobook",
            "cinema",
            "club",
            "cd",
        ),
        help="Delivery target the assistant masters for (default: streaming)",
    )
    mastering_p.add_argument(
        "--no-streaming-safe",
        action="store_true",
        help="Let the assistant suggest treatments it withholds for streaming delivery",
    )
    mastering_p.add_argument(
        "--speech-mono-amount",
        type=_finite_float,
        default=1.0,
        help="How far speech-like material is collapsed toward mono (0-1; default: 1)",
    )
    mproc_p = sub.add_parser(
        "mastering-processor", parents=[common], help="Apply a named mastering processor"
    )
    mproc_p.add_argument("--processor", required=True, help="Processor name")
    mproc_p.add_argument("--params", default="", help="Params as k=v,k=v (floats)")
    _add_wav_bits_argument(mproc_p)
    eq_p = sub.add_parser("eq", parents=[common], help="Apply the unified equalizer")
    eq_p.add_argument("--params", default="", help="Params as k=v,k=v (overrides band shortcuts)")
    # Each of these selects an enumerator by index. cmd_eq refuses an index
    # outside the enumeration after parsing, which is why the published domain
    # records the invalid-parameter class rather than the usage one; without the
    # refusal the underlying switch answers an unknown index with its first
    # enumerator, so a typo applies a different filter and exits 0.
    _cli_domain(
        eq_p.add_argument(
            "--type",
            type=int,
            default=0,
            help=(
                "Band type enum: 0 peak, 1 low shelf, 2 high shelf, 3 low pass, "
                "4 high pass, 5 band pass, 6 notch, 7 tilt, 8 flat tilt"
            ),
        ),
        choices=range(_EQ_ENUM_BOUNDS["type"] + 1),
        reject_exit="invalid_parameter",
    )
    eq_p.add_argument("--frequency-hz", type=_finite_float, default=1000.0)
    eq_p.add_argument("--gain-db", type=_finite_float, default=0.0)
    eq_p.add_argument("--q", type=_finite_float, default=1.0)
    _cli_domain(
        eq_p.add_argument("--coeff-mode", type=int, default=0, help="0 RBJ, 1 Vicanek"),
        choices=range(_EQ_ENUM_BOUNDS["coeff_mode"] + 1),
        reject_exit="invalid_parameter",
    )
    eq_p.add_argument("--slope-db-oct", type=int, default=12)
    _cli_domain(
        eq_p.add_argument(
            "--placement", type=int, default=0, help="0 stereo, 1 left, 2 right, 3 mid, 4 side"
        ),
        choices=range(_EQ_ENUM_BOUNDS["placement"] + 1),
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        eq_p.add_argument(
            "--phase-mode",
            type=int,
            default=1,
            help="0 inherit, 1 zero latency, 2 natural, 3 linear",
        ),
        choices=range(_EQ_ENUM_BOUNDS["phase_mode"] + 1),
        reject_exit="invalid_parameter",
    )
    _cli_domain(
        eq_p.add_argument(
            "--resolution",
            type=int,
            default=0,
            help="0 custom/default, 1 low, 2 medium, 3 high, 4 very high, 5 maximum",
        ),
        choices=range(_EQ_ENUM_BOUNDS["resolution"] + 1),
        reject_exit="invalid_parameter",
    )
    eq_p.add_argument("--auto-gain", action="store_true")
    eq_p.add_argument("--gain-scale", type=_finite_float, default=1.0)
    eq_p.add_argument("--output-gain-db", type=_finite_float, default=0.0)
    eq_p.add_argument("--output-pan", type=_finite_float, default=0.0)
    eq_p.add_argument("--proportional-q", action="store_true")
    eq_p.add_argument("--dynamic", action="store_true")
    eq_p.add_argument("--threshold-db", type=_finite_float, default=-24.0)
    eq_p.add_argument("--auto-threshold", action="store_true")
    eq_p.add_argument("--ratio", type=_finite_float, default=2.0)
    eq_p.add_argument("--range-db", type=_finite_float, default=-6.0)
    eq_p.add_argument("--attack-ms", type=_finite_float, default=5.0)
    eq_p.add_argument("--release-ms", type=_finite_float, default=50.0)
    # "--lookahead-ms" is the flag's former (misleading) spelling; still
    # accepted, both writing to the same destination, so a stored script
    # keeps working.
    eq_p.add_argument(
        "--detector-delay-ms",
        "--lookahead-ms",
        dest="lookahead_ms",
        type=_finite_float,
        default=0.0,
    )
    eq_p.add_argument("--sidechain-freq-hz", type=_finite_float, default=-1.0)
    eq_p.add_argument("--sidechain-q", type=_finite_float, default=1.0)
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
    mpp_p = sub.add_parser(
        "mastering-pair-processor",
        parents=[common],
        help="Apply a two-input mastering processor",
    )
    mpp_p.add_argument("--processor", required=True, help="Pair processor name")
    mpp_p.add_argument("--reference", required=True, help="Reference audio file")
    mpp_p.add_argument("--params", default="")
    _add_wav_bits_argument(mpp_p)
    msa_p = sub.add_parser(
        "mastering-stereo-analyze",
        parents=[stdout_options],
        help="Run a stereo mastering analysis (always JSON output)",
    )
    msa_p.add_argument("--reference", required=True, help="Right-channel audio file")
    msa_p.add_argument("--analysis", required=True, help="Analysis name")
    msa_p.add_argument("--params", default="")
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
    # A whole chain rather than a base to override: the file is a complete chain
    # config in the core's own ``{params, version}`` form, the one
    # ``mastering-suggest --config-out`` writes and the native CLI reads. It
    # replaces the preset instead of layering on it, which is why it is a third
    # spelling rather than a meaning added to --config / --config-file.
    master_p.add_argument(
        "--chain-config", default=None, help="Complete chain config JSON file (replaces --preset)"
    )
    master_p.add_argument(
        "--assistant",
        action="store_true",
        help="Master with the chain the assistant suggests for this file",
    )
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
    repair_p = sub.add_parser(
        "repair",
        parents=[common],
        help=(
            "Measure and repair defects only "
            "(declip/declick/decrackle/dehum/denoise/dereverb, in that order)"
        ),
    )
    repair_p.add_argument(
        "--preset", default="", help="Repair preset name (omitted: measure and choose)"
    )
    repair_p.add_argument(
        "--params",
        default="",
        help="Repair config overrides as repair.<stage>.<field>=value,...",
    )
    repair_p.add_argument(
        "--detect",
        action="store_true",
        help="Measure and report only; no processing, --output not required",
    )
    repair_p.add_argument(
        "--explain", action="store_true", help="Say why each repair stage was chosen"
    )
    _add_wav_bits_argument(repair_p)
    declip_p = sub.add_parser("declip", parents=[common], help="Repair clipped audio")
    declip_p.add_argument("--clip-threshold", type=_finite_float, default=0.98)
    declip_p.add_argument("--lpc-order", type=int, default=36)
    declip_p.add_argument("--iterations", type=int, default=2)
    declip_p.add_argument("--lpc-blend", type=_finite_float, default=0.65)
    sub.add_parser(
        "mastering-presets", parents=[stdout_options], help="List mastering preset names"
    )
    msuggest_p = sub.add_parser(
        "mastering-suggest", parents=[stdout_options], help="Suggest a mastering chain as JSON"
    )
    msuggest_p.add_argument("--params", default="", help="Assistant params as k=v,k=v")
    msuggest_p.add_argument(
        "--config-out",
        default="",
        help="Write the suggested chain config where --chain-config reads it back",
    )
    mprofile_p = sub.add_parser(
        "mastering-profile",
        parents=[stdout_options],
        help="Analyze a mastering audio profile as JSON",
    )
    mprofile_p.add_argument("--params", default="", help="Profile params as k=v,k=v")
