"""Command-line interface for libsonare: the repair commands."""

from __future__ import annotations

import argparse
import math
from typing import TYPE_CHECKING, Any, cast

import numpy as np

from ._cli_common import (
    _audio_array,
    _load_channels_or_downmix,
    _strict_json_dumps,
    _write_channel_output,
    _write_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _token_of
from ._cli_options import (
    SharedParsers,
    _add_wav_bits_argument,
    _c_int,
    _ContractArgumentParser,
    _finite_float,
    _library_catalog,
    _option_supplied,
    _wav_bits,
)
from ._mastering_offline import _chain_config_paths, _nest_chain_config
from ._result_records import _result_to_json_dict

if TYPE_CHECKING:
    from .analyzer import MasteringPreset
    from .types import AudioDefects


def cmd_declip(args: argparse.Namespace) -> int:
    from . import mastering_repair_declip, mastering_repair_declip_stereo

    planes, sr = _load_channels_or_downmix(args.file)
    knobs: dict[str, Any] = {
        "lpc_order": args.lpc_order,
        "iterations": args.iterations,
        "lpc_blend": args.lpc_blend,
    }
    if args.clip_threshold_db is not None:
        knobs["clip_threshold_db"] = args.clip_threshold_db
    if len(planes) == 2:
        # The stereo entry repairs the union of both channels' clipped runs, so
        # a plateau one channel alone reaches is still reconstructed against the
        # other's unclipped extent. Declipping the channels separately cannot
        # see that and is what the mono loader used to force.
        stereo = mastering_repair_declip_stereo(planes[0], planes[1], sample_rate=sr, **knobs)
        rendered = [_audio_array(stereo.left), _audio_array(stereo.right)]
    else:
        mono = mastering_repair_declip(planes[0], sample_rate=sr, **knobs)
        rendered = [_audio_array(mono.samples)]
    repaired = rendered[0]

    if args.output:
        _write_channel_output(args.output, rendered, sr)

    if args.json:
        payload: dict[str, object] = {
            "sample_rate": sr,
            "samples": len(repaired),
            "lpc_order": args.lpc_order,
            "iterations": args.iterations,
            "lpc_blend": args.lpc_blend,
        }
        if args.clip_threshold_db is not None:
            payload["clip_threshold_db"] = args.clip_threshold_db
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
    buffer = np.asarray(samples, dtype=np.float64)
    peak = float(np.max(np.abs(buffer[np.isfinite(buffer)]), initial=0.0))
    if peak <= 1.0:
        return samples, 0.0
    gain = 1.0 / peak
    return buffer * gain, 20.0 * math.log10(gain)


# Each detector group of the profile's ``defects`` object, with the wire key
# and the snake_case key the native CLI's ``--json`` emits for it.
def _repair_detection_report(defects: AudioDefects | None) -> dict[str, dict[str, object]] | None:
    """Render the profile's ``defects`` in the snake_case shape the native CLI's
    ``--json`` emits, group for group; None when no detector ran.

    A large ``click.rejected`` beside a zero ``click.count`` means the detector
    could not decide, and a ``hum.fundamental_prominence`` (a linear energy
    ratio) of 1.0 means no stationary peak was found.
    """
    if defects is None:
        return None
    return cast("dict[str, dict[str, object]]", _result_to_json_dict(defects))


def _print_repair_detection_report(report: dict[str, dict[str, object]] | None) -> None:
    print("  Repair detection:")
    if report is None:
        print("    Measured:     no (nothing looked at this recording)")
        return
    click, clip, crackle = report["click"], report["clip"], report["crackle"]
    noise, hum, reverb = report["noise"], report["hum"], report["reverb"]
    print("    Measured:     yes")
    click_count = cast("int", click["count"])
    click_rejected = cast("int", click["rejected"])
    print(
        f"    Clicks:       {click_count} found, {click_rejected} rejected as undecidable, "
        f"longest run {click['longest_run_samples']} samples, "
        f"{cast('float', click['per_second']):.2f}/s"
    )
    if click_count == 0 and click_rejected > 0:
        print(
            "    Note:         click detector rejected candidates as undecidable "
            "and confirmed none -- detector uncertainty, not necessarily a clean signal"
        )
    print(
        f"    Crackle:      {crackle['sample_count']} samples "
        f"({cast('float', crackle['sample_fraction']) * 100:.3f}% of input), "
        f"{cast('float', crackle['per_second']):.2f}/s"
    )
    print(
        f"    Clipping:     {clip['clipped_samples']} samples in "
        f"{clip['run_count']} run(s) "
        f"({cast('float', clip['clipped_ratio']) * 100:.3f}% of input), "
        f"longest run {clip['longest_run_samples']} samples"
    )
    if cast("int", clip["flat_run_count"]) > 0:
        print(
            f"    Flat tops:    {clip['flat_run_count']} run(s) pinned at "
            f"{cast('float', clip['flat_level']):.3f} "
            f"({clip['flat_sample_count']} samples, longest "
            f"{clip['longest_flat_run_samples']}) -- these survive a later gain "
            "change, so they are clipping rather than a peak that reaches the ceiling"
        )
    else:
        print(
            "    Flat tops:    none (an unclipped peak reaches the ceiling too; resampling "
            "or a lossy codec erases a real one, so this is not proof of no clipping)"
        )
    band_floor = cast("list[float]", noise["band_floor_dbfs"])
    loudest_band = max(range(len(band_floor)), key=band_floor.__getitem__)
    print(
        f"    Noise floor:  {cast('float', noise['floor_dbfs']):.1f} dBFS, "
        f"loudest band {band_floor[loudest_band]:.1f} dBFS (band {loudest_band})"
    )
    if cast("float", hum["fundamental_prominence"]) > 1.0:
        harmonic_dbfs = cast("list[float]", hum["harmonic_dbfs"])
        print(
            f"    Hum:          {cast('float', hum['fundamental_hz']):.1f} Hz, "
            f"{cast('float', hum['fundamental_prominence']):.1f}x the rest of the search, "
            f"{hum['harmonics']} harmonic(s), "
            f"fundamental {harmonic_dbfs[0]:.1f} dBFS, "
            f"loudest harmonic {max(harmonic_dbfs):.1f} dBFS"
        )
    else:
        print("    Hum:          search found no candidate peak (could not decide)")
    print(
        f"    Late decay:   {cast('float', reverb['late_decay_ratio_db']):.1f} dB "
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

    # Every mode reports the detectors, so they always run here -- never only
    # when it happens to be convenient for one mode.
    profile = mastering_audio_profile(samples, sample_rate=sr, params={"detectDefects": True})
    # The profile carries no `defects` when no detector ran; so does this report.
    defects = _repair_detection_report(profile.defects)

    if detect_only:
        if getattr(args, "json", False):
            detect_payload: dict[str, object] = {"mode": "detect"}
            if defects is not None:
                detect_payload["defects"] = defects
            print(_strict_json_dumps(detect_payload))
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
            preset=cast("MasteringPreset", preset),
            overrides=_nest_chain_config(overrides),
        )
        mode = "preset"
    else:
        suggestion = mastering_assistant_suggest(
            samples, sample_rate=sr, params={"enableRepair": True}
        )
        chain_config = _chain_config_paths(suggestion.chain_config)
        repair_config = {
            key: value for key, value in chain_config.items() if key.startswith("repair.")
        }
        repair_config.update(params)
        result = master_audio(
            samples, sample_rate=sr, preset=None, overrides=_nest_chain_config(repair_config)
        )
        mode = "assistant"
        explanation = _repair_explanation(suggestion.explanation)

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
        if defects is not None:
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
        "--preset",
        type=_token_of(_library_catalog("mastering_preset_names")),
        default="",
        help="Repair preset name (omitted: measure and choose)",
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
    declip_p.add_argument(
        "--clip-threshold-db",
        type=_finite_float,
        default=None,
        help="Level in dBFS at or above which a sample counts as clipped "
        "(default: the library's, 0.98 of full scale, about -0.18 dBFS)",
    )
    declip_p.add_argument("--lpc-order", type=_c_int, default=36)
    declip_p.add_argument("--iterations", type=_c_int, default=2)
    declip_p.add_argument("--lpc-blend", type=_finite_float, default=0.65)
