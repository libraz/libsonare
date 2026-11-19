"""Command-line interface for libsonare: the repair commands."""

from __future__ import annotations

import argparse
import math
from typing import TYPE_CHECKING, Any, cast

import numpy as np

from ._cli_common import (
    _load_channels_or_downmix,
    _strict_json_dumps,
    _write_channel_output,
)
from ._cli_inventory import _token_of
from ._cli_options import (
    SharedParsers,
    _add_wav_bits_argument,
    _ContractArgumentParser,
    _library_catalog,
    _option_supplied,
    _wav_bits,
)
from ._mastering_offline import _chain_config_paths, _nest_chain_config
from ._result_records import _result_to_json_dict

if TYPE_CHECKING:
    from .analyzer import MasteringPreset
    from .types import AudioDefects


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


# The stage vocabulary ``repair --stages`` accepts; the chain, not this tuple,
# decides the order they run in.
_REPAIR_STAGE_NAMES: tuple[str, ...] = (
    "declip",
    "declick",
    "decrackle",
    "dehum",
    "denoise",
    "dereverb",
)


def _parse_repair_stages(raw: str) -> list[str]:
    """Parse ``repair --stages`` into lower-cased stage names.

    The set is closed; an empty entry or a repeated stage is refused rather than
    ignored.
    """
    stages: list[str] = []
    for entry in (part.strip().lower() for part in raw.split(",")):
        if not entry:
            raise ValueError("--stages has an empty entry")
        if entry not in _REPAIR_STAGE_NAMES:
            raise ValueError(
                f"--stages: unknown stage '{entry}' (expected {', '.join(_REPAIR_STAGE_NAMES)})"
            )
        if entry in stages:
            raise ValueError(f"--stages names {entry} twice")
        stages.append(entry)
    return stages


def _reject_unlisted_stage_params(params: dict[str, float | str], stages: list[str]) -> None:
    """Refuse a ``--params`` key reaching a stage ``--stages`` leaves out."""
    for key in params:
        stage = key[len("repair.") :].split(".", 1)[0]
        if stage in _REPAIR_STAGE_NAMES and stage not in stages:
            raise ValueError(f"{key} names a stage --stages does not run")


def _parse_repair_params(raw: str) -> dict[str, float | str]:
    """Parse ``repair --params`` into the chain's flat ``repair.*`` dict.

    A number stays a number and anything else stays text, for the library's chain
    document reader to judge: it resolves a choice-valued key's name, refuses a
    number there, and refuses a fractional value for an integer field, naming the
    key. This parser deliberately carries no second list of either kind of field.
    """
    params: dict[str, float | str] = {}
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise ValueError(f"invalid param (expected key=value): {item}")
        key, value = (part.strip() for part in item.split("=", 1))
        if not key.startswith("repair."):
            raise ValueError(f"--params key must be a repair.* field: {key}")
        try:
            params[key] = float(value)
        except ValueError:
            params[key] = value
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


def _fit_repair_output_to_full_scale(channels: list[Any]) -> tuple[list[Any], float]:
    """Scale ``channels`` so their joint peak lands at full scale.

    Returns the channels to write and the applied gain in dB (``0.0`` when the
    peak already fit and nothing was touched). One gain across the channels
    leaves the stereo image alone.

    Declipping reconstructs the peaks a clipper cut off, so its output routinely
    exceeds full scale -- and this command deliberately runs no limiter. The
    integer writer clamps, which pins exactly the samples the repair just
    rebuilt back onto the ceiling they were rescued from, undoing the stage that
    was asked for. One gain for the whole file keeps the reconstructed waveform
    intact; a per-sample fit would be the clipper again.
    """
    # A non-finite sample would make every comparison false and leave the peak
    # at 0, so the scale is skipped rather than turned into a NaN gain.
    buffers = [np.asarray(channel, dtype=np.float64) for channel in channels]
    peak = max(
        float(np.max(np.abs(buffer[np.isfinite(buffer)]), initial=0.0)) for buffer in buffers
    )
    if peak <= 1.0:
        return channels, 0.0
    gain = 1.0 / peak
    return [buffer * gain for buffer in buffers], 20.0 * math.log10(gain)


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
        master_audio_stereo,
        mastering_assistant_suggest,
        mastering_assistant_suggest_stereo,
        mastering_audio_profile,
        mastering_audio_profile_stereo,
    )

    detect_only = bool(getattr(args, "detect", False))
    preset = getattr(args, "preset", "") or ""
    stages_raw: str | None = args.stages if _option_supplied(args, "stages") else None
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
        for name in ("preset", "stages", "params", "bits", "output"):
            if _option_supplied(args, name):
                raise ValueError(f"--{name} cannot be combined with --detect")
    elif not output:
        raise ValueError("repair requires --output unless --detect is given")

    if stages_raw is not None and preset:
        raise ValueError("--stages cannot be combined with --preset")
    if stages_raw is not None and explain:
        raise ValueError(
            "--explain has nothing to explain under --stages: the stages are named "
            "directly, not chosen by measurement -- drop --stages to see why the "
            "assistant would pick a stage"
        )
    stage_list = _parse_repair_stages(stages_raw) if stages_raw is not None else []

    if preset and explain:
        raise ValueError(
            "--explain has nothing to explain under --preset: a preset's repair "
            "stages are named directly, not chosen by measurement -- drop --preset "
            "to see why the assistant would pick a stage"
        )

    # A two-channel input is measured and repaired as a pair, its detectors run
    # per channel and aggregated.
    planes, sr = _load_channels_or_downmix(args.file)
    stereo = len(planes) == 2

    def run_chain(overrides: dict[str, float | str], preset_name: str | None) -> Any:
        nested = _nest_chain_config(overrides)
        if stereo:
            return master_audio_stereo(
                planes[0],
                planes[1],
                sample_rate=sr,
                preset=cast("MasteringPreset | None", preset_name),
                overrides=nested,
            )
        return master_audio(
            planes[0],
            sample_rate=sr,
            preset=cast("MasteringPreset | None", preset_name),
            overrides=nested,
        )

    # Every mode reports the detectors, so they always run here -- never only
    # when it happens to be convenient for one mode.
    profile_params: dict[str, float | int | bool] = {"detectDefects": True}
    profile = (
        mastering_audio_profile_stereo(planes[0], planes[1], sample_rate=sr, params=profile_params)
        if stereo
        else mastering_audio_profile(planes[0], sample_rate=sr, params=profile_params)
    )
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
    if stages_raw is not None:
        _reject_unlisted_stage_params(params, stage_list)
    bits = _wav_bits(args)
    explanation: list[str] = []
    result: Any

    if stages_raw is not None:
        # Exactly the listed stages at the library's defaults.
        stage_config: dict[str, float | str] = {
            f"repair.{stage}.enabled": 1.0 for stage in stage_list
        }
        stage_config.update(params)
        result = run_chain(stage_config, None)
        mode = "stages"
    elif preset:
        overrides: dict[str, float | str] = dict(_NON_REPAIR_CHAIN_DISABLE_OVERRIDES)
        overrides.update(params)
        result = run_chain(overrides, preset)
        mode = "preset"
    else:
        suggestion = (
            mastering_assistant_suggest_stereo(
                planes[0], planes[1], sample_rate=sr, params={"enableRepair": True}
            )
            if stereo
            else mastering_assistant_suggest(
                planes[0], sample_rate=sr, params={"enableRepair": True}
            )
        )
        chain_config = _chain_config_paths(suggestion.chain_config)
        repair_config = {
            key: value for key, value in chain_config.items() if key.startswith("repair.")
        }
        repair_config.update(params)
        result = run_chain(repair_config, None)
        mode = "assistant"
        explanation = _repair_explanation(suggestion.explanation)

    stages = list(getattr(result, "stages", []))
    rendered, output_gain_db = _fit_repair_output_to_full_scale(
        [result.left, result.right] if stereo else [result.samples]
    )
    _write_channel_output(output, rendered, result.sample_rate, bits)

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
        "--stages",
        default="",
        help=(
            "Run exactly these repair stages at the library's defaults, as a comma "
            "list of declip,declick,decrackle,dehum,denoise,dereverb"
        ),
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
