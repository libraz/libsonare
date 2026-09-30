"""Command-line interface for libsonare: the repair commands."""

from __future__ import annotations

import argparse
import json
import math
from typing import TYPE_CHECKING, Any, cast

from ._cli_common import (
    _chain_params_config,
    _float_sequence,
    _load_channels_or_downmix,
    _strict_json_dumps,
    _write_channel_output,
    _write_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_options import (
    SharedParsers,
    _add_wav_bits_argument,
    _ContractArgumentParser,
    _finite_float,
    _option_supplied,
    _wav_bits,
)

if TYPE_CHECKING:
    from .analyzer import MasteringPreset


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
# explain non-repair choices (base preset, EQ, dynamics) that `repair` never
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


def register_repair_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the repair commands."""
    common = shared.common

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
