"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import os
from collections.abc import Sequence
from typing import TYPE_CHECKING, Any, cast

from ._cli_common import (
    _absent_if_none,
    _add_unreconstructed_runs,
    _atomic_write_bytes,
    _load_channels_or_downmix,
    _measured_pairs,
    _OutputFit,
    _params_entries,
    _params_grammar_refusal,
    _params_keys,
    _parse_kv_params,
    _print_unreconstructed_runs,
    _read_audio_bounded,
    _strict_json_dumps,
    _write_mastering_output,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain, _token_of
from ._cli_options import (
    SharedParsers,
    _add_wav_bits_argument,
    _c_int,
    _ContractArgumentParser,
    _finite_float,
    _library_catalog,
    _NumericTextError,
    _option_supplied,
    _read_number_text,
    _wav_bits,
)
from ._cli_repair import (
    cmd_repair as cmd_repair,
)
from ._cli_repair import (
    register_repair_parsers,
)
from ._errors import SonareError, _malformed_output
from ._mastering_offline import _chain_config_paths, _check_chain_document, _nest_chain_config
from ._narrowing import _FLOAT32_MAX
from ._result_records import _result_to_json_dict
from ._runtime import _mapping_keys

if TYPE_CHECKING:
    from .analyzer import MasteringPreset, SoloProcessor
    from .types import StreamingPlatform


def _mastering_report_payload(report: Any, gain_db: float = 0.0) -> dict[str, object]:
    """Serialize the shared chain report without depending on dataclass internals.

    ``gain_db`` is the output fit's gain: the report describes the file as written,
    so every level in ``after`` and every band delta moves by exactly that, and the
    loudness range does not.
    """

    if report is None:
        raise _malformed_output("loaded libsonare did not return a mastering report")
    before = report.before
    after = report.after
    return {
        "before": _loudness_summary_payload(before),
        "after": _loudness_summary_payload(after, gain_db),
        "applied_gain_db": report.applied_gain_db,
        "max_gain_reduction_db": report.max_gain_reduction_db,
        "loudness_target_limited": report.loudness_target_limited,
        "loudness_gain_db": _absent_if_none(report.loudness_gain_db),
        **_measured_pairs(
            [
                None if delta is None else float(delta) + gain_db
                for delta in report.band_energy_delta_db
            ],
            "band_energy_delta_db",
            "band_energy_delta_band",
        ),
    }


def _loudness_summary_payload(summary: Any, gain_db: float = 0.0) -> dict[str, object]:
    payload: dict[str, object] = {}
    for key in (
        "integrated_lufs",
        "max_momentary_lufs",
        "max_short_term_lufs",
        "true_peak_dbtp",
        "loudness_range_lu",
    ):
        value = getattr(summary, key)
        if value is not None and key != "loudness_range_lu":
            value = float(value) + gain_db
        payload[key] = _absent_if_none(value)
    return payload


def _write_mastering_report(path: str, report: Any, gain_db: float = 0.0) -> None:
    """Write the report through the shared artifact writer.

    Routing it there rather than through a bare ``open`` gives the report the
    same atomic replace and the same write-failure exit class as every other
    artifact the CLI produces.
    """
    payload = _strict_json_dumps(_mastering_report_payload(report, gain_db)) + "\n"
    _atomic_write_bytes(path, payload.encode("utf-8"))


# Each named assistant option and the flat-param key it sets; the same table
# the native CLI's assistant_config_from_cli() reads.
_ASSISTANT_OPTIONS = (
    ("targetLufs", "target-lufs"),
    ("ceilingDbtp", "ceiling-dbtp"),
    ("enableRepair", "enable-repair"),
    ("preferStreamingSafe", "no-streaming-safe"),
    ("speechMonoAmount", "speech-mono-amount"),
    ("targetPlatform", "target-platform"),
    ("preset", "preset"),
)
_NAMED_ASSISTANT_KEYS = ("targetPlatform", "preset")
_ASSISTANT_PARAM_KEYS = tuple(key for key, _ in _ASSISTANT_OPTIONS)


def _assistant_params_from_args(
    args: argparse.Namespace, params_raw: str
) -> dict[str, float | int | bool | str]:
    """Build the assistant's flat params from the command line.

    ``mastering --assistant`` and ``mastering-suggest`` declare the same named
    options, and ``mastering-suggest`` also takes them as ``--params``. Only what
    the caller named is sent: a supplied loudness key is what marks it explicit
    for a delivery target, and the library holds every other default.
    ``targetPlatform`` and ``preset`` take a name, which the library resolves; a
    setting named by an option and by ``--params`` is a contradiction rather
    than a precedence question.
    """
    params: dict[str, float | int | bool | str] = {}
    # The argv preflight has checked each entry before the read.
    for item in _params_entries(params_raw):
        key, raw = item.split("=", 1)
        if key in _NAMED_ASSISTANT_KEYS:
            params[key] = raw
        else:
            params[key] = _read_number_text(raw)
    if _option_supplied(args, "target-lufs"):
        params["targetLufs"] = float(args.target_lufs)
    if _option_supplied(args, "ceiling-dbtp"):
        params["ceilingDbtp"] = float(args.ceiling_dbtp)
    if getattr(args, "enable_repair", False):
        params["enableRepair"] = True
    if getattr(args, "no_streaming_safe", False):
        params["preferStreamingSafe"] = False
    if _option_supplied(args, "speech-mono-amount"):
        params["speechMonoAmount"] = float(args.speech_mono_amount)
    if _option_supplied(args, "target-platform"):
        params["targetPlatform"] = args.target_platform
    if getattr(args, "preset", ""):
        params["preset"] = args.preset
    return params


def _chain_config_document(raw: str) -> dict[str, Any]:
    """Read a chain config document, from JSON text or a file path, as flat parameter paths.

    The library reads the document, so a refused one is an invalid format, as on the native CLI.
    """
    from . import mastering_chain_config_from_json

    if os.path.isfile(raw):
        with open(raw, encoding="utf-8") as fh:
            raw = fh.read()
    return _chain_config_paths(mastering_chain_config_from_json(raw))


def _mastering_chain_payload(
    result: Any,
    fit: _OutputFit,
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
        "output_lufs": result.output_lufs + fit.gain_db,
        "applied_gain_db": result.applied_gain_db,
        "output": output,
    }
    if output:
        payload["output_gain_db"] = fit.gain_db
    _add_unreconstructed_runs(payload, result.declip_unreconstructed_runs)
    if preset:
        payload["preset"] = preset
    payload["stages"] = list(getattr(result, "stages", []))
    if explanation:
        payload["explanation"] = explanation
    if include_report_latency:
        payload["latency_samples"] = 0
    return payload


# Every ``eq`` option that selects part of the band --params would otherwise
# specify in full, in the native validator's order.
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
    "ratio",
    "range-db",
    "attack-ms",
    "release-ms",
    "detector-delay-ms",
    "phase-mode",
    "resolution",
    "auto-gain",
    "gain-scale",
    "output-gain-db",
    "output-pan",
    "auto-threshold",
    "sidechain-freq-hz",
    "sidechain-q",
)


def _chain_params_refusal(args: argparse.Namespace) -> str | None:
    """The refusal of ``mastering --params`` read as a chain document, or None."""
    raw = getattr(args, "params", "") or ""
    if (refusal := _params_grammar_refusal(raw)) is not None:
        return refusal
    if not raw:
        return None
    try:
        _check_chain_document(_parse_kv_params(raw, names=True))
    except SonareError as exc:
        # The core's own words, without the binding's "[code] " prefix.
        return str(exc).partition("] ")[2]
    except ValueError as exc:
        return str(exc)
    return None


def _processor_params_refusal(args: argparse.Namespace) -> str | None:
    """The refusal of ``mastering-processor --params`` decided from argv, or None."""
    raw = getattr(args, "params", "") or ""
    if (refusal := _params_grammar_refusal(raw)) is not None:
        return refusal
    try:
        _reject_unknown_processor_params(args.processor, _params_keys(raw))
    except ValueError as exc:
        return str(exc)
    return None


def _eq_params_refusal(args: argparse.Namespace) -> str | None:
    """The refusal of ``eq --params`` decided from argv, or None."""
    if not _option_supplied(args, "params"):
        return None
    if (refusal := _params_grammar_refusal(args.params or "")) is not None:
        return refusal
    # --params fully specifies the EQ, so a shortcut beside it is refused rather than dropped.
    for name in _EQ_SHORTCUT_NAMES:
        if _option_supplied(args, name):
            return f"--{name} cannot be combined with --params"
    try:
        _reject_unknown_processor_params("eq.equalizer", _params_keys(args.params or ""))
    except ValueError as exc:
        return str(exc)
    return None


def _assistant_params_refusal(args: argparse.Namespace) -> str | None:
    """The refusal of ``mastering-suggest --params`` decided from argv, or None."""
    from . import capability_catalog, mastering_platform_names

    raw = getattr(args, "params", "") or ""
    if (refusal := _params_grammar_refusal(raw)) is not None:
        return refusal
    for item in _params_entries(raw):
        key, value = item.split("=", 1)
        if key not in _ASSISTANT_PARAM_KEYS:
            return f"unknown key {key} (expected {', '.join(sorted(_ASSISTANT_PARAM_KEYS))})"
        for named_key, option in _ASSISTANT_OPTIONS:
            if key == named_key and _option_supplied(args, option):
                return f"--{option} and --params {key}= set the same value"
        if key == "preset":
            tokens = [
                entry.name
                for entry in capability_catalog().mastering_presets
                if entry.kind == "mastering"
            ]
        elif key == "targetPlatform":
            tokens = mastering_platform_names()
        else:
            try:
                _read_number_text(value)
            except _NumericTextError:
                return f"invalid numeric value for parameter '{key}'"
            continue
        if value not in tokens:
            return f"{key} must be one of {', '.join(tokens)}"
    return None


# Highest accepted index of every ``eq`` option that selects an enumerator, keyed
# by its argparse destination. The enumerations are EqBandType, BiquadCoeffMode,
# StereoPlacement and PhaseMode in mastering/eq/eq_band.h plus
# LinearPhaseEqConfig::Resolution; the switches that map an index answer an
# unknown one with their first enumerator, so an unchecked `--type 999` would
# apply a peak filter and exit 0. The native CLI declares the same bounds in its
# registry and the cross-surface option-domain comparison pins the two together.
_EQ_ENUM_BOUNDS = {
    "type": 9,
    "coeff_mode": 1,
    "placement": 4,
    "phase_mode": 3,
    "resolution": 5,
}


def _reject_unknown_processor_params(processor: str, keys: Sequence[str]) -> None:
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
    unknown = [key for key in keys if key not in known]
    if unknown:
        joined = ", ".join(unknown)
        raise ValueError(f"unknown --params key for {processor}: {joined}")


def cmd_mastering(args: argparse.Namespace) -> int:
    planes, sr = _load_channels_or_downmix(args.file)
    samples = planes[0]
    stereo = len(planes) == 2
    report_path = getattr(args, "report", "") or ""
    preset = getattr(args, "preset", None) or ""
    config_raw = getattr(args, "config", None) or ""
    assistant = bool(getattr(args, "assistant", False))
    explain = bool(getattr(args, "explain", False))
    params_raw = getattr(args, "params", "") or ""
    bits = _wav_bits(args)

    # The selector and the params document were settled before the input was read.
    params = _parse_kv_params(params_raw, names=True) if params_raw else {}

    def _run_chain(config: dict[str, Any]) -> Any:
        """Run one chain config over however many channels the source has."""
        from . import master_audio, master_audio_stereo

        if stereo:
            return master_audio_stereo(
                planes[0],
                planes[1],
                sample_rate=sr,
                preset=None,
                overrides=_nest_chain_config(config),
            )
        return master_audio(
            samples, sample_rate=sr, preset=None, overrides=_nest_chain_config(config)
        )

    result: Any
    mode = "chainConfig"
    explanation: list[str] = []
    if preset and not assistant:
        from . import master_audio, master_audio_stereo

        if stereo:
            result = master_audio_stereo(
                planes[0],
                planes[1],
                sample_rate=sr,
                preset=cast("MasteringPreset", preset),
                overrides=_nest_chain_config(params) if params else None,
            )
        else:
            result = master_audio(
                samples,
                sample_rate=sr,
                preset=cast("MasteringPreset", preset),
                overrides=_nest_chain_config(params) if params else None,
            )
        mode = "preset"
    elif config_raw:
        config = _chain_config_document(config_raw)
        config.update(params)
        result = _run_chain(config)
        mode = "chainConfig"
    elif assistant:
        from . import mastering_assistant_suggest, mastering_assistant_suggest_stereo

        # --params here overrides the suggested chain, so none of it reaches the
        # assistant's own config.
        suggestion_params = _assistant_params_from_args(args, "")
        # The assistant reads the material to decide the chain, so it is handed
        # the same channels the chain will run over rather than a fold of them.
        suggestion = (
            mastering_assistant_suggest_stereo(
                planes[0], planes[1], sample_rate=sr, params=suggestion_params
            )
            if stereo
            else mastering_assistant_suggest(samples, sample_rate=sr, params=suggestion_params)
        )
        config = _chain_config_paths(suggestion.chain_config)
        config.update(params)
        result = _run_chain(config)
        explanation = list(suggestion.explanation)
        mode = "assistant"
    rendered = [result.left, result.right] if stereo else [result.samples]
    output = getattr(args, "output", "") or ""
    fit = _write_mastering_output(output, rendered, result.sample_rate, bits)
    if report_path:
        _write_mastering_report(report_path, result.report, fit.gain_db)

    if getattr(args, "json", False):
        payload = _mastering_chain_payload(
            result,
            fit,
            mode=mode,
            output=output,
            # preset selects the assistant's starting config too (see
            # assistant_config_from_cli), but the payload's preset key is
            # reserved for the preset ROUTE, matching the native CLI:
            # tools/cli/sonare_cli_mastering.cpp only assigns
            # preset_name on the plain --preset route, so its JSON never
            # carries the key for --assistant --preset.
            preset=preset if mode == "preset" else "",
            explanation=explanation if explain else None,
            include_report_latency=bool(report_path),
        )
        print(_strict_json_dumps(payload))
    else:
        print(f"  Mastering {mode}:")
        # See the JSON payload's own preset= comment above: preset also seeds
        # the assistant's starting config, but the printed line is reserved
        # for the preset ROUTE, matching the native CLI (preset_name stays
        # empty off that route there too).
        if preset and mode == "preset":
            print(f"    Preset:       {preset}")
        stages = list(getattr(result, "stages", []))
        print(f"    Stages:       {', '.join(stages) if stages else '(none)'}")
        print(f"    Input LUFS:  {result.input_lufs:.2f}")
        print(f"    Output LUFS: {result.output_lufs + fit.gain_db:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        _print_unreconstructed_runs(result.declip_unreconstructed_runs, 14)
        if output:
            print(f"    Wrote: {output}")
    return 0


def cmd_mastering_normalize_loudness(args: argparse.Namespace) -> int:
    from . import master_audio, master_audio_stereo

    planes, sr = _load_channels_or_downmix(args.file)
    samples = planes[0]
    stereo = len(planes) == 2
    report_path = getattr(args, "report", "") or ""
    bits = _wav_bits(args)

    def _run_chain(config: dict[str, Any]) -> Any:
        """Run one chain config over however many channels the source has."""
        if stereo:
            return master_audio_stereo(
                planes[0],
                planes[1],
                sample_rate=sr,
                preset=None,
                overrides=_nest_chain_config(config),
            )
        return master_audio(
            samples, sample_rate=sr, preset=None, overrides=_nest_chain_config(config)
        )

    result: Any
    if report_path or stereo:
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
                    "ceilingDbtp": getattr(args, "ceiling_dbtp", -1.0),
                    "truePeakOversample": getattr(args, "true_peak_oversample", 4),
                }
            }
        )
    else:
        from .audio import Audio

        result = Audio.from_buffer(samples, sr).mastering(
            target_lufs=getattr(args, "target_lufs", -14.0),
            ceiling_dbtp=getattr(args, "ceiling_dbtp", -1.0),
            true_peak_oversample=getattr(args, "true_peak_oversample", 4),
        )

    rendered = [result.left, result.right] if stereo else [result.samples]
    output = getattr(args, "output", "") or ""
    fit = _write_mastering_output(output, rendered, result.sample_rate, bits)
    if report_path:
        _write_mastering_report(report_path, result.report, fit.gain_db)

    if getattr(args, "json", False):
        payload = {
            "input_lufs": result.input_lufs,
            "output_lufs": result.output_lufs + fit.gain_db,
            "applied_gain_db": result.applied_gain_db,
            "target_lufs": getattr(args, "target_lufs", -14.0),
            "ceiling_dbtp": getattr(args, "ceiling_dbtp", -1.0),
            "true_peak_oversample": getattr(args, "true_peak_oversample", 4),
            "latency_samples": getattr(result, "latency_samples", 0),
            "loudness_target_limited": bool(result.loudness_target_limited),
            "sample_rate": result.sample_rate,
            "output": output,
        }
        if output:
            payload["output_gain_db"] = fit.gain_db
        print(_strict_json_dumps(payload))
    else:
        print("  Mastering:")
        print(f"    Input LUFS:  {result.input_lufs:.2f}")
        print(f"    Output LUFS: {result.output_lufs + fit.gain_db:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        if output:
            print(f"    Wrote: {output}")
    return 0


def cmd_mastering_processor(args: argparse.Namespace) -> int:
    from . import (
        mastering_apply_processor,
        mastering_apply_processor_stereo,
        mastering_processor_catalog,
    )

    planes, sr = _load_channels_or_downmix(args.file)
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw, names=True) if params_raw else {}
    bits = _wav_bits(args)
    processor = getattr(args, "processor", None) or ""
    processor_name = cast("SoloProcessor", processor)
    stereo_only = {entry.id for entry in mastering_processor_catalog() if entry.stereo_only}
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
        stereo = mastering_apply_processor_stereo(
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
            declip_unreconstructed_runs=stereo.declip_unreconstructed_runs,
        )
    else:
        result = mastering_apply_processor(processor_name, planes[0], sample_rate=sr, params=params)
        channels = [list(result.samples)]

    output = getattr(args, "output", "") or ""
    fit = _write_mastering_output(output, channels, result.sample_rate, bits)

    if getattr(args, "json", False):
        payload = {
            "processor": processor,
            "input_lufs": result.input_lufs,
            "output_lufs": result.output_lufs + fit.gain_db,
            "applied_gain_db": result.applied_gain_db,
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
            "output": output,
            "stereo": use_stereo,
        }
        if output:
            payload["output_gain_db"] = fit.gain_db
        _add_unreconstructed_runs(payload, result.declip_unreconstructed_runs)
        print(_strict_json_dumps(payload))
    else:
        print(f"  Mastering processor: {processor}")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs + fit.gain_db:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        _print_unreconstructed_runs(result.declip_unreconstructed_runs, 14)
        if output:
            print(f"    Wrote: {output}")
    return 0


def cmd_eq(args: argparse.Namespace) -> int:
    from . import mastering_apply_processor, mastering_apply_processor_stereo

    planes, sr = _load_channels_or_downmix(args.file)
    params_raw = getattr(args, "params", "") or ""
    bits = _wav_bits(args)
    if params_raw:
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
            "band0.detectorDelayMs": float(args.detector_delay_ms),
            "band0.sidechainFreqHz": float(getattr(args, "sidechain_freq_hz", -1.0)),
            "band0.sidechainQ": float(getattr(args, "sidechain_q", 1.0)),
            "phaseMode": float(getattr(args, "phase_mode", 1)),
            "resolution": float(getattr(args, "resolution", 0)),
            "autoGain": 1.0 if getattr(args, "auto_gain", False) else 0.0,
            "gainScale": float(getattr(args, "gain_scale", 1.0)),
            "outputGainDb": float(getattr(args, "output_gain_db", 0.0)),
            "outputPan": float(getattr(args, "output_pan", 0.0)),
        }
    # A two-channel source is equalized as a pair, as mastering-processor does,
    # so --placement and --output-pan act on the image the file carries.
    use_stereo = len(planes) == 2
    result: Any
    channels: list[list[float]]
    if use_stereo:
        stereo = mastering_apply_processor_stereo(
            "eq.equalizer", planes[0], planes[1], sample_rate=sr, params=params
        )
        channels = [list(stereo.left), list(stereo.right)]
        result = stereo
    else:
        result = mastering_apply_processor("eq.equalizer", planes[0], sample_rate=sr, params=params)
        channels = [list(result.samples)]

    output = getattr(args, "output", "") or ""
    fit = _write_mastering_output(output, channels, result.sample_rate, bits)

    if getattr(args, "json", False):
        payload = {
            "processor": "eq.equalizer",
            "input_lufs": result.input_lufs,
            "output_lufs": result.output_lufs + fit.gain_db,
            "applied_gain_db": result.applied_gain_db,
            "latency_samples": result.latency_samples,
            "sample_rate": result.sample_rate,
            "output": output,
            "stereo": use_stereo,
        }
        if output:
            payload["output_gain_db"] = fit.gain_db
        print(_strict_json_dumps(payload))
    else:
        print("  Equalizer")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs + fit.gain_db:.2f}")
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
    from . import mastering_analyze_pair
    from .audio import decode_channels

    source = decode_channels(_read_audio_bounded(args.file))
    reference = decode_channels(_read_audio_bounded(args.reference))
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw) if params_raw else {}
    result = mastering_analyze_pair(
        args.analysis,
        source.channels,
        reference.channels,
        sample_rate=source.sample_rate,
        reference_sample_rate=reference.sample_rate,
        params=params or None,
    )
    print(_strict_json_dumps(_result_to_json_dict(result)))
    return 0


def cmd_mastering_pair_processor(args: argparse.Namespace) -> int:
    from . import mastering_apply_pair_processor

    source, sr = _load_audio(args.file)
    reference, ref_sr = _load_audio(args.reference)
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw) if params_raw else {}
    result = mastering_apply_pair_processor(
        args.processor,
        source,
        reference,
        sample_rate=sr,
        reference_sample_rate=ref_sr,
        params=params or None,
    )
    output = getattr(args, "output", "") or ""
    fit = _write_mastering_output(output, [result.samples], result.sample_rate, _wav_bits(args))
    if args.json:
        payload = {
            "processor": args.processor,
            "input_lufs": result.input_lufs,
            "output_lufs": result.output_lufs + fit.gain_db,
            "applied_gain_db": result.applied_gain_db,
            "latency_samples": result.latency_samples,
            "output": output,
        }
        if output:
            payload["output_gain_db"] = fit.gain_db
        _add_unreconstructed_runs(payload, result.declip_unreconstructed_runs)
        print(_strict_json_dumps(payload))
    else:
        print(f"  Mastering pair processor: {args.processor}")
        print(f"    Input LUFS:   {result.input_lufs:.2f}")
        print(f"    Output LUFS:  {result.output_lufs + fit.gain_db:.2f}")
        print(f"    Applied gain: {result.applied_gain_db:.2f} dB")
        print(f"    Latency:      {result.latency_samples} samples")
        _print_unreconstructed_runs(result.declip_unreconstructed_runs, 14)
        if output:
            print(f"    Wrote: {output}")
    return 0


def cmd_mastering_stereo_analyze(args: argparse.Namespace) -> int:
    from . import mastering_analyze_stereo

    left, sr = _load_audio(args.file)
    right, ref_sr = _load_audio(args.reference)
    if ref_sr != sr:
        raise ValueError("reference sample rate must match input sample rate")
    if len(right) != len(left):
        raise ValueError("reference length must match input length")
    params_raw = getattr(args, "params", "") or ""
    params = _parse_kv_params(params_raw) if params_raw else {}
    result = mastering_analyze_stereo(
        args.analysis, left, right, sample_rate=sr, params=params or None
    )
    print(_strict_json_dumps(_result_to_json_dict(result)))
    return 0


# The record's keys, all required.
_STREAMING_PLATFORM_KEYS = ("name", "target_lufs", "ceiling_dbtp")


def _refuse_json_constant(token: str) -> None:
    raise ValueError(token)


def _streaming_platform_number(entry: dict[str, Any], where: str, key: str) -> float:
    value = entry[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{where}.{key} must be a number")
    if not abs(float(value)) <= _FLOAT32_MAX:
        raise ValueError(f"{where}.{key} must be finite")
    return float(value)


def _parse_streaming_platforms(text: str, subject: str) -> list[dict[str, Any]]:
    """Read a platform document and report any refusal under ``subject``."""
    if not text:
        return []
    try:
        try:
            document = json.loads(text, parse_constant=_refuse_json_constant)
        except ValueError:
            document = None
        if not isinstance(document, list) or not all(isinstance(e, dict) for e in document):
            raise ValueError("must be a JSON array of platform objects")
        platforms: list[dict[str, Any]] = []
        for index, entry in enumerate(document):
            where = f"platforms[{index}]"
            _mapping_keys(entry, _STREAMING_PLATFORM_KEYS, where, required=_STREAMING_PLATFORM_KEYS)
            name = entry["name"]
            if not isinstance(name, str):
                raise ValueError(f"{where}.name must be a string")
            if not name:
                raise ValueError(f"{where}.name must not be empty")
            platforms.append(
                {
                    "name": name,
                    "target_lufs": _streaming_platform_number(entry, where, "target_lufs"),
                    "ceiling_dbtp": _streaming_platform_number(entry, where, "ceiling_dbtp"),
                }
            )
    except ValueError as exc:
        raise ValueError(f"{subject}: {exc}") from None
    return platforms


def _streaming_platforms_refusal(args: argparse.Namespace) -> str | None:
    """The refusal of a --platforms document, or None; --platforms-file wins when both are given."""
    if args.platforms_file or args.platforms is None:
        return None
    try:
        _parse_streaming_platforms(args.platforms, "argument --platforms")
    except ValueError as exc:
        return str(exc)
    return None


def cmd_mastering_streaming(args: argparse.Namespace) -> int:
    from . import mastering_streaming_preview

    samples, sr = _load_audio(args.file)
    if args.platforms_file:
        with open(args.platforms_file, encoding="utf-8") as fh:
            records = _parse_streaming_platforms(fh.read(), "--platforms-file")
    else:
        records = _parse_streaming_platforms(args.platforms or "", "argument --platforms")
    platforms = cast("list[StreamingPlatform] | None", records or None)
    result = mastering_streaming_preview(samples, sample_rate=sr, platforms=platforms)
    print(_strict_json_dumps(_result_to_json_dict(result)))
    return 0


def cmd_mastering_presets(args: argparse.Namespace) -> int:
    from . import mastering_preset_names

    names = mastering_preset_names()
    print(_strict_json_dumps({"presets": names}) if args.json else "\n".join(names))
    return 0


def cmd_mastering_suggest(args: argparse.Namespace) -> int:
    from . import mastering_assistant_suggest

    samples, sr = _load_audio(args.file)
    params = _assistant_params_from_args(args, args.params or "")
    suggestion = mastering_assistant_suggest(samples, sample_rate=sr, params=params)
    # The suggested chain is fed back to the library verbatim
    # (`mastering --chain-config`), so the names under it belong to the chain
    # param schema rather than to CLI stdout, and re-keying them would make the
    # document the library rejects. Everything beside it is measurement output
    # and follows the snake_case rule.
    chain_config = {"version": 2, "params": suggestion.chain_config}
    config_out = getattr(args, "config_out", "") or ""
    if config_out:
        # Written from the document already in hand rather than through the
        # chain-only entry point, which would re-measure the file to reach the
        # same config, and written through the shared artifact writer so a failed
        # write carries the class every other CLI artifact's does.
        _atomic_write_bytes(config_out, (_strict_json_dumps(chain_config) + "\n").encode("utf-8"))
    payload = _result_to_json_dict(suggestion)
    payload["chain_config"] = chain_config
    print(_strict_json_dumps(payload))
    return 0


def cmd_mastering_profile(args: argparse.Namespace) -> int:
    from . import mastering_audio_profile

    samples, sr = _load_audio(args.file)
    params = _parse_kv_params(args.params) if args.params else {}
    result = mastering_audio_profile(samples, sample_rate=sr, params=params)
    print(_strict_json_dumps(_result_to_json_dict(result)))
    return 0


def _add_assistant_control_arguments(parser: argparse.ArgumentParser) -> None:
    """Declare the assistant controls ``mastering`` and ``mastering-suggest`` share."""
    # The accepted delivery-target names are
    # the rows of the table in src/mastering/assistant/platform_targets.h, which
    # the native CLI reads directly; the cross-surface option-domain comparison
    # is what keeps this restatement pinned to it. ``prefer_streaming_safe``
    # defaults to true in the library, so the reachable control is the one that
    # turns it off. ``--speech-mono-amount`` declares no domain because the
    # suggester clamps it to [0, 1] rather than refusing an outside value.
    parser.add_argument(
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
        help="Delivery target the assistant masters for",
    )
    parser.add_argument(
        "--no-streaming-safe",
        action="store_true",
        help="Use whole-signal noise estimation when the assistant suggests denoise",
    )
    parser.add_argument(
        "--speech-mono-amount",
        type=_finite_float,
        default=1.0,
        help="How far speech-like material is collapsed toward mono (0-1; default: 1)",
    )


def register_mastering_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the mastering, repair and processor commands."""
    common = shared.common
    stdout_options = shared.stdout_options

    # Mastering commands
    mastering_p = sub.add_parser(
        "mastering",
        parents=[common],
        help="Master through a preset, a chain config or the assistant",
    )
    _cli_domain(
        mastering_p.add_argument(
            "--preset", type=_token_of(_library_catalog("mastering_preset_names"))
        ),
        choices=_library_catalog("mastering_preset_names"),
    )
    # "--chain-config" names a complete chain config in the core's own
    # ``{params, version}`` form.
    mastering_p.add_argument("--chain-config", dest="config", default=None)
    mastering_p.add_argument("--target-lufs", type=_finite_float, default=-14.0)
    mastering_p.add_argument("--ceiling-dbtp", type=_finite_float, default=-1.0)
    mastering_p.add_argument("--params", default="")
    _add_wav_bits_argument(mastering_p)
    mastering_p.add_argument("--report", default=None, help="Write a mastering report JSON file")
    mastering_p.add_argument("--assistant", action="store_true")
    mastering_p.add_argument("--enable-repair", action="store_true")
    mastering_p.add_argument("--explain", action="store_true")
    _add_assistant_control_arguments(mastering_p)
    loudness_p = sub.add_parser(
        "mastering-normalize-loudness",
        parents=[common],
        help="Normalise loudness under a true-peak ceiling",
    )
    loudness_p.add_argument("--target-lufs", type=_finite_float, default=-14.0)
    loudness_p.add_argument("--ceiling-dbtp", type=_finite_float, default=-1.0)
    _add_wav_bits_argument(loudness_p)
    loudness_p.add_argument(
        "--true-peak-oversample", type=_c_int, choices=(1, 2, 4, 8, 16), default=4
    )
    loudness_p.add_argument("--report", default=None, help="Write a mastering report JSON file")
    mproc_p = sub.add_parser(
        "mastering-processor", parents=[common], help="Apply a named mastering processor"
    )
    _cli_domain(
        mproc_p.add_argument(
            "--processor",
            type=_token_of(_library_catalog("mastering_processor_names")),
            help="Processor name",
        ),
        choices=_library_catalog("mastering_processor_names"),
    )
    mproc_p.add_argument("--params", default="", help="Params as k=v,k=v (floats)")
    _add_wav_bits_argument(mproc_p)
    eq_p = sub.add_parser("eq", parents=[common], help="Apply the unified equalizer")
    eq_p.add_argument("--params", default="", help="Params as k=v,k=v (overrides band shortcuts)")
    # Each of these selects an enumerator by index. Without the domain the
    # underlying switch answers an unknown index with its first enumerator, so a
    # typo applies a different filter and exits 0.
    _cli_domain(
        eq_p.add_argument(
            "--type",
            type=_c_int,
            default=0,
            help=(
                "Band type enum: 0 peak, 1 low shelf, 2 high shelf, 3 low pass, "
                "4 high pass, 5 band pass, 6 notch, 7 tilt, 8 flat tilt, 9 all pass"
            ),
        ),
        choices=range(_EQ_ENUM_BOUNDS["type"] + 1),
    )
    eq_p.add_argument("--frequency-hz", type=_finite_float, default=1000.0)
    eq_p.add_argument("--gain-db", type=_finite_float, default=0.0)
    eq_p.add_argument("--q", type=_finite_float, default=1.0)
    _cli_domain(
        eq_p.add_argument("--coeff-mode", type=_c_int, default=0, help="0 RBJ, 1 Vicanek"),
        choices=range(_EQ_ENUM_BOUNDS["coeff_mode"] + 1),
    )
    eq_p.add_argument("--slope-db-oct", type=_c_int, default=12)
    _cli_domain(
        eq_p.add_argument(
            "--placement", type=_c_int, default=0, help="0 stereo, 1 left, 2 right, 3 mid, 4 side"
        ),
        choices=range(_EQ_ENUM_BOUNDS["placement"] + 1),
    )
    _cli_domain(
        eq_p.add_argument(
            "--phase-mode",
            type=_c_int,
            default=1,
            help="0 inherit, 1 zero latency, 2 natural, 3 linear",
        ),
        choices=range(_EQ_ENUM_BOUNDS["phase_mode"] + 1),
    )
    _cli_domain(
        eq_p.add_argument(
            "--resolution",
            type=_c_int,
            default=0,
            help="0 custom/default, 1 low, 2 medium, 3 high, 4 very high, 5 maximum",
        ),
        choices=range(_EQ_ENUM_BOUNDS["resolution"] + 1),
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
    eq_p.add_argument("--detector-delay-ms", type=_finite_float, default=0.0)
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
    mpa_p.add_argument("--reference", help="Reference audio file")
    _cli_domain(
        mpa_p.add_argument(
            "--analysis",
            type=_token_of(_library_catalog("mastering_pair_analysis_names")),
            help="Analysis name",
        ),
        choices=_library_catalog("mastering_pair_analysis_names"),
    )
    mpa_p.add_argument("--params", default="")
    mpp_p = sub.add_parser(
        "mastering-pair-processor",
        parents=[common],
        help="Apply a two-input mastering processor",
    )
    _cli_domain(
        mpp_p.add_argument(
            "--processor",
            type=_token_of(_library_catalog("mastering_pair_processor_names")),
            help="Pair processor name",
        ),
        choices=_library_catalog("mastering_pair_processor_names"),
    )
    mpp_p.add_argument("--reference", help="Reference audio file")
    mpp_p.add_argument("--params", default="")
    _add_wav_bits_argument(mpp_p)
    msa_p = sub.add_parser(
        "mastering-stereo-analyze",
        parents=[stdout_options],
        help="Run a stereo mastering analysis (always JSON output)",
    )
    msa_p.add_argument("--reference", help="Right-channel audio file")
    _cli_domain(
        msa_p.add_argument(
            "--analysis",
            type=_token_of(_library_catalog("mastering_stereo_analysis_names")),
            help="Analysis name",
        ),
        choices=_library_catalog("mastering_stereo_analysis_names"),
    )
    msa_p.add_argument("--params", default="")
    mstream_p = sub.add_parser(
        "mastering-streaming",
        parents=[stdout_options],
        help="Preview streaming-platform normalization as JSON",
    )
    mstream_p.add_argument(
        "--platforms",
        default=None,
        help="Platform targets as JSON array of {name,target_lufs,ceiling_dbtp}",
    )
    mstream_p.add_argument("--platforms-file", default=None, help="Platform targets JSON file")
    register_repair_parsers(sub, shared)
    sub.add_parser(
        "mastering-presets", parents=[stdout_options], help="List mastering preset names"
    )
    msuggest_p = sub.add_parser(
        "mastering-suggest", parents=[stdout_options], help="Suggest a mastering chain as JSON"
    )
    msuggest_p.add_argument(
        "--params",
        default="",
        help="Assistant params as k=v,k=v; targetPlatform and preset take a name",
    )
    _cli_domain(
        msuggest_p.add_argument(
            "--preset",
            type=_token_of(_library_catalog("mastering_preset_names")),
            help="Mastering preset the suggestion starts from",
        ),
        choices=_library_catalog("mastering_preset_names"),
    )
    msuggest_p.add_argument("--target-lufs", type=_finite_float, default=-14.0)
    msuggest_p.add_argument("--ceiling-dbtp", type=_finite_float, default=-1.0)
    msuggest_p.add_argument("--enable-repair", action="store_true")
    _add_assistant_control_arguments(msuggest_p)
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
