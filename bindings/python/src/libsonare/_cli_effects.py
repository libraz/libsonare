"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import sys
from typing import TYPE_CHECKING, Any, Literal, cast

import numpy as np

if TYPE_CHECKING:
    from .types import VoicePresetId

from ._cli_common import (
    EXIT_INVALID_PARAMETER,
    SONARE_ERROR_INVALID_PARAMETER,
    SonareError,
    _apply_voice_sets,
    _audio_array,
    _catalog_token,
    _cli_voice_set_subjects,
    _DuplicateJsonKeyError,
    _emit_effect_result,
    _find_cli_token,
    _fmt_optional,
    _load_channels_or_downmix,
    _load_voice_preset_pack,
    _mean_abs,
    _note_output_fit,
    _OutputFit,
    _resample,
    _strict_json_dumps,
    _voice_set_refusal,
    _write_wav,
)
from ._cli_common import (
    _load_audio_from_facade as _load_audio,
)
from ._cli_inventory import _cli_domain
from ._cli_note_edit import (
    _DEFAULT_UNMATCHED_POLICY as _DEFAULT_UNMATCHED_POLICY,
)
from ._cli_note_edit import (
    _POLYPHONIC_EDIT_FIELDS as _POLYPHONIC_EDIT_FIELDS,
)
from ._cli_note_edit import (
    _UNMATCHED_POLICY_NAMES as _UNMATCHED_POLICY_NAMES,
)
from ._cli_note_edit import (
    _parse_polyphonic_edit_bool as _parse_polyphonic_edit_bool,
)
from ._cli_note_edit import (
    cmd_note_move as cmd_note_move,
)
from ._cli_note_edit import (
    cmd_note_stretch as cmd_note_stretch,
)
from ._cli_note_edit import (
    cmd_pitch_correct as cmd_pitch_correct,
)
from ._cli_note_edit import (
    cmd_pitch_correct_timevarying as cmd_pitch_correct_timevarying,
)
from ._cli_note_edit import (
    cmd_polyphonic_notes as cmd_polyphonic_notes,
)
from ._cli_note_edit import (
    cmd_polyphonic_render as cmd_polyphonic_render,
)
from ._cli_note_edit import (
    cmd_scale_quantize as cmd_scale_quantize,
)
from ._cli_note_edit import (
    cmd_tune_to_midi as cmd_tune_to_midi,
)
from ._cli_note_edit import (
    cmd_vocal_notes as cmd_vocal_notes,
)
from ._cli_note_edit import (
    register_note_edit_parsers,
)
from ._cli_options import (
    SharedParsers,
    _c_int,
    _ContractArgumentParser,
    _effective,
    _finite_float,
    _given,
)
from ._cli_utilities import _MAX_GENERATOR_SAMPLE_RATE, _MIN_GENERATOR_SAMPLE_RATE
from ._effects_level import _DEFAULT_NORMALIZE_TARGET_DB
from ._runtime import _get_lib, _render_as_given


def _hpss_refusal(args: argparse.Namespace) -> str | None:
    modes = ("harmonic_only", "percussive_only", "with_residual")
    if sum(bool(getattr(args, name, False)) for name in modes) > 1:
        return (
            "hpss output modes are mutually exclusive: choose one of --harmonic-only, "
            "--percussive-only, or --with-residual"
        )
    return None


def _normalize_refusal(args: argparse.Namespace) -> str | None:
    # A peak target above full scale is out of reach for any input: the writer fits the
    # peak back to 0 dBFS. An RMS target depends on the crest, so it is fitted and reported.
    target_db = getattr(args, "target_db", None)
    if str(getattr(args, "mode", "peak")).lower() != "peak" or target_db is None or target_db <= 0:
        return None
    return (
        "argument --target-db: must be at most 0 for a peak target: "
        "the output format cannot hold a level above full scale"
    )


def _trim_silence_refusal(args: argparse.Namespace) -> str | None:
    if (
        getattr(args, "threshold_db", None) is not None
        and getattr(args, "top_db", None) is not None
    ):
        return "--threshold-db and --top-db are mutually exclusive"
    return None


def _builtin_voice_preset_refusal(value: str) -> str | None:
    from . import realtime_voice_changer_preset_names

    names = realtime_voice_changer_preset_names()
    if _find_cli_token(value, names) is not None:
        return None
    return "argument --preset: must be one of " + ", ".join(names)


def _voice_change_refusal(args: argparse.Namespace) -> str | None:
    preset = getattr(args, "preset", None)
    preset_json = getattr(args, "preset_json", None) or None
    preset_pack = getattr(args, "preset_pack", None) or None
    selected = preset is not None or preset_json is not None
    if preset is not None and preset_json is not None:
        return (
            "voice-change preset selectors are mutually exclusive: choose one of --preset, "
            "--preset-json, or --preset-pack"
        )
    if preset_pack and preset_json:
        return "--preset-pack and --preset-json are mutually exclusive"
    # A pack names the file, --preset the entry inside it; reported ahead of the rules below.
    if preset_pack and preset is None:
        return "--preset-pack requires --preset to select an entry"
    # The default mode is a no-op, so only an explicit absolute request conflicts with a preset.
    if selected and (
        getattr(args, "pitch_semitones", None) is not None
        or getattr(args, "formant_factor", None) is not None
        or (getattr(args, "formant_mode", None) or "relative") != "relative"
    ):
        return (
            "--pitch-semitones/--formant-factor/--formant-mode cannot be combined with a "
            "realtime preset"
        )
    assignments = getattr(args, "set", None) or []
    if assignments and not selected:
        return "--set requires --preset, --preset-json, or --preset-pack"
    if preset is not None and not preset_pack:
        refusal = _builtin_voice_preset_refusal(preset)
        if refusal is not None:
            return refusal
    refusal = _voice_set_refusal(assignments)
    if refusal is not None:
        return refusal
    if assignments and preset is not None and not preset_json and not preset_pack:
        from . import (
            realtime_voice_changer_preset_json,
            realtime_voice_changer_preset_names,
        )
        from ._effects_voice import _validate_voice_config_for_cli

        builtin = _catalog_token(preset, realtime_voice_changer_preset_names())
        base = json.loads(realtime_voice_changer_preset_json(builtin))
        writes: list[tuple[tuple[str, ...], tuple[str, ...], str, int]] = []
        config = _apply_voice_sets(base, assignments, provenance=writes)
        if isinstance(config, dict):
            try:
                with _cli_voice_set_subjects(writes, config):
                    _validate_voice_config_for_cli(config)
            except SonareError as error:
                if error.code == SONARE_ERROR_INVALID_PARAMETER:
                    return str(error)
                raise
    return None


def _voice_preset_refusal(args: argparse.Namespace) -> str | None:
    preset = getattr(args, "preset", None)
    return None if preset is None else _builtin_voice_preset_refusal(preset)


def _voice_preset_validate_refusal(args: argparse.Namespace) -> str | None:
    return _voice_set_refusal(getattr(args, "set", None))


def cmd_hpss(args: argparse.Namespace) -> int:
    from . import hpss

    modes = [
        name
        for name in ("harmonic_only", "percussive_only", "with_residual")
        if bool(getattr(args, name, False))
    ]

    # A setting the caller left out is not passed, so the library chooses it.
    separation = {
        "hard_mask": bool(getattr(args, "hard_mask", False)),
        **_given(args, "kernel_harmonic", "kernel_percussive", "n_fft", "hop_length"),
    }

    samples, sr = _load_audio(args.file)

    if modes == ["with_residual"]:
        separated = hpss(
            samples,
            sample_rate=sr,
            with_residual=True,
            **separation,
        )
        harmonic = _audio_array(separated.harmonic)
        percussive = _audio_array(separated.percussive)
        residual = _audio_array(separated.residual)
        output_sr = separated.sample_rate
    else:
        result = hpss(
            samples,
            sample_rate=sr,
            **separation,
        )
        harmonic = _audio_array(result.harmonic)
        percussive = _audio_array(result.percussive)
        residual = _audio_array([])
        output_sr = int(result.sample_rate)

    # The components sum to the input, so the files one run writes share one gain,
    # and the levels reported are those of the files as written.
    fit = _OutputFit()
    if modes == ["harmonic_only"]:
        fit.include(harmonic)
    elif modes == ["percussive_only"]:
        fit.include(percussive)
    else:
        fit.include(harmonic).include(percussive)
        if modes == ["with_residual"]:
            fit.include(residual)
    h_energy = _mean_abs(harmonic) * fit.gain
    p_energy = _mean_abs(percussive) * fit.gain

    harmonic_path = ""
    percussive_path = ""
    residual_path = ""
    if args.output:
        base = args.output[:-4] if args.output.lower().endswith(".wav") else args.output
        if modes == ["harmonic_only"]:
            harmonic_path = f"{base}.wav"
            _write_wav(harmonic_path, harmonic, output_sr, fit=fit)
        elif modes == ["percussive_only"]:
            percussive_path = f"{base}.wav"
            _write_wav(percussive_path, percussive, output_sr, fit=fit)
        else:
            harmonic_path = f"{base}_harmonic.wav"
            percussive_path = f"{base}_percussive.wav"
            _write_wav(harmonic_path, harmonic, output_sr, fit=fit)
            _write_wav(percussive_path, percussive, output_sr, fit=fit)
            if modes == ["with_residual"]:
                residual_path = f"{base}_residual.wav"
                _write_wav(residual_path, residual, output_sr, fit=fit)
        _note_output_fit(fit)

    if args.json:
        # Each component's mean absolute amplitude is reported only when that component is part
        # of the requested output, so a single-component run publishes the same
        # keys on both CLIs -- the native one never separates the other half.
        payload: dict[str, object] = {
            "length": len(percussive) if modes == ["percussive_only"] else len(harmonic),
            "output_gain_db": fit.gain_db,
            "sample_rate": output_sr,
        }
        if modes != ["percussive_only"]:
            payload["harmonic_mean_abs"] = h_energy
        if modes != ["harmonic_only"]:
            payload["percussive_mean_abs"] = p_energy
        if residual.size:
            payload["residual_mean_abs"] = _mean_abs(residual) * fit.gain
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
        if residual.size:
            print(f"  Residual energy:   {_mean_abs(residual) * fit.gain:.6f}")
        paths = [path for path in (harmonic_path, percussive_path, residual_path) if path]
        if paths:
            print(f"  Wrote: {', '.join(paths)}")
    return 0


def cmd_decompose_stems(args: argparse.Namespace) -> int:
    from . import decompose_stems

    samples, sr = _load_audio(args.file)
    result = decompose_stems(
        samples,
        sample_rate=sr,
        **_given(
            args, "n_components", "n_fft", "hop_length", "n_iter", "beta", "init", "mask_power"
        ),
    )
    components = [_audio_array(component) for component in result.components]
    output_sr = result.sample_rate

    # One file per component, numbered from 1, in the base-name shape `hpss`
    # uses for its two: the destination names the set, not a single file.
    base = args.output[:-4] if args.output.lower().endswith(".wav") else args.output
    paths = [f"{base}_component{index}.wav" for index in range(1, len(components) + 1)]
    # The components sum to the input, so they share one gain and keep summing to it.
    fit = _OutputFit()
    for component in components:
        fit.include(component)
    for path, component in zip(paths, components, strict=True):
        _write_wav(path, component, output_sr, fit=fit)
    _note_output_fit(fit)

    energies = [_mean_abs(component) * fit.gain for component in components]
    if args.json:
        print(
            _strict_json_dumps(
                {
                    "count": len(components),
                    "output_gain_db": fit.gain_db,
                    "length": len(components[0]) if components else 0,
                    "sample_rate": output_sr,
                    "energies": energies,
                    "components": paths,
                }
            )
        )
    else:
        print(f"  Stems: {len(components)} components")
        for index, (path, energy) in enumerate(zip(paths, energies, strict=True), start=1):
            print(f"    {index:2d}. energy {energy:.6f}  {path}")
        if paths:
            print(f"  Wrote: {', '.join(paths)}")
    return 0


def cmd_pitch_shift(args: argparse.Namespace) -> int:
    from . import pitch_shift

    samples, sr = _load_audio(args.file)
    result = pitch_shift(
        samples,
        sample_rate=sr,
        pitch_semitones=args.pitch_semitones,
        **_given(args, "n_fft", "hop_length"),
    )

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={"pitch_semitones": args.pitch_semitones},
        label=f"Pitch shift ({args.pitch_semitones:+.2f} semitones)",
    )


def cmd_time_stretch(args: argparse.Namespace) -> int:
    from . import time_stretch

    samples, sr = _load_audio(args.file)
    result = time_stretch(
        samples,
        sample_rate=sr,
        speed_ratio=args.rate,
        **_given(args, "n_fft", "hop_length"),
    )

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra={"rate": args.rate},
        label=f"Time stretch (rate {args.rate:.4f})",
    )


def cmd_normalize(args: argparse.Namespace) -> int:
    from . import normalize, normalize_stereo

    planes, sr = _load_channels_or_downmix(args.file)
    mode = getattr(args, "mode", "peak")
    checked_mode = cast(Literal["peak", "rms"], mode)
    given = getattr(args, "target_db", None)
    # The payload reports the level the library normalizes to, the mode's own when none is given.
    target_db = _DEFAULT_NORMALIZE_TARGET_DB[checked_mode] if given is None else given

    # One gain for the pair, measured across both channels. Normalizing each
    # channel to the target on its own would drive the quieter one up by a
    # different amount and collapse the image toward the centre -- for a source
    # 12 dB apart that is a factor of four on one side.
    if len(planes) == 2:
        stereo = normalize_stereo(
            planes[0], planes[1], sample_rate=sr, mode=checked_mode, target_db=given
        )
        channels = [stereo.left, stereo.right]
    else:
        mono = normalize(planes[0], sample_rate=sr, mode=checked_mode, target_db=given)
        channels = [mono]

    # The library's result is unclipped: a level past full scale reaches the
    # writer's fit, which lowers the whole file instead of flattening the peaks.
    fit = _OutputFit()
    for channel in channels:
        fit.include(channel)
    return _emit_effect_result(
        args,
        channels,
        sr,
        # The level of the file as written: the target, less whatever the fit took off.
        extra={
            "mode": mode,
            "target_db": target_db,
            "output_db": float(np.float32(target_db) + np.float32(fit.gain_db)),
        },
        label=f"Normalize ({mode}, target {target_db:.2f} dB)",
        fit=fit,
    )


def cmd_trim_silence(args: argparse.Namespace) -> int:
    from . import trim, trim_silence

    samples, sr = _load_audio(args.file)
    threshold_db = getattr(args, "threshold_db", None)
    top_db = getattr(args, "top_db", None)
    geometry = _given(args, frame_length="n_fft", hop_length="hop_length")
    facade: Any
    extra: dict[str, Any]
    if top_db is not None:
        result = trim_silence(samples, top_db=top_db, **geometry).audio.tolist()
        facade = trim_silence
        extra = {"top_db": top_db}
        label = f"Trim silence (top {top_db:.1f} dB)"
    else:
        result = trim(samples, sample_rate=sr, **_given(args, "threshold_db"), **geometry)
        facade = trim
        threshold_db = _effective(args, trim, "threshold_db")
        extra = {"threshold_db": threshold_db}
        label = f"Trim silence (threshold {threshold_db:.1f} dB)"
    # The payload reports the geometry the library ran with, its own where none is given.
    extra["n_fft"] = _effective(args, facade, "frame_length", "n_fft")
    extra["hop_length"] = _effective(args, facade, "hop_length")

    return _emit_effect_result(
        args,
        [result],
        sr,
        extra=extra,
        label=label,
        # trim-silence doubles as analysis (it reports the trimmed length), so an
        # output file is optional here, matching the native CLI.
    )


def _split_silence_takes(args: argparse.Namespace) -> tuple[list[np.ndarray], int]:
    """Load the takes ``split-silence`` measures together, in command-line order."""
    first, *others = args.file
    samples, sr = _load_audio(first)
    takes = [samples]
    for path in others:
        take, take_sr = _load_audio(path)
        # Frame indices from two rates are not comparable, so a mismatch would
        # report intervals in units the caller cannot map back onto either take.
        if take_sr != sr:
            raise ValueError(
                f"take sample rate differs: {path} is {take_sr} Hz, the first take is {sr} Hz"
            )
        takes.append(take)
    return takes, sr


def _split_silence_slice(take: np.ndarray, start: int, end: int) -> np.ndarray:
    """One interval of one take, padded where the take ended before it.

    A take shorter than the interval is silent past its end -- the rule the union
    was built on -- so the slice is padded rather than shortened and every take's
    file for one interval carries the same sample count.
    """
    window = take[start:end]
    if len(window) < end - start:
        window = np.concatenate([window, np.zeros(end - start - len(window), dtype=window.dtype)])
    return window


def cmd_split_silence(args: argparse.Namespace) -> int:
    from . import split_silence_common_with_report

    top_db = _effective(args, split_silence_common_with_report, "top_db")

    takes, sr = _split_silence_takes(args)
    # Always asked for, only sometimes printed: one call site rather than two
    # that could come to refuse differently.
    split = split_silence_common_with_report(
        takes, **_given(args, "top_db", frame_length="n_fft", hop_length="hop_length")
    )
    intervals = split.intervals.tolist()
    report = split.report

    write_takes = getattr(args, "write_takes", None)
    fit = _OutputFit()
    if write_takes:
        # The slices are parts of takes meant to be compared, so they share one gain.
        slices = [
            [_split_silence_slice(take, start, end) for start, end in intervals] for take in takes
        ]
        for take_slices in slices:
            for window in take_slices:
                fit.include(window)
        for take_index, take_slices in enumerate(slices, start=1):
            for interval_index, window in enumerate(take_slices, start=1):
                _write_wav(
                    f"{write_takes}{take_index:02d}_{interval_index:03d}.wav", window, sr, fit=fit
                )
        _note_output_fit(fit)

    with_report = bool(getattr(args, "report", False))
    interval_rows = [{"start_sample": start, "end_sample": end} for start, end in intervals]
    if args.json:
        # One object whatever the flags; the gain and the report are present when their flag is.
        document: dict[str, object] = {"intervals": interval_rows}
        if write_takes:
            document["output_gain_db"] = fit.gain_db
        if with_report:
            document["report"] = {
                "silence_ceiling_db": report.silence_ceiling_db,
                "max_signal_intervals": report.max_signal_intervals,
                "min_signal_intervals": report.min_signal_intervals,
            }
        print(_strict_json_dumps(document))
    else:
        print(f"Non-silent intervals: {len(intervals)}")
        for start, end in intervals:
            print(f"  {start} - {end}")
        if with_report:
            # Read against the --top-db in use, which is the whole rule: a ceiling
            # under it means the threshold was too loose for the quiet these takes
            # have, and one at or over it means the quiet is there and they do not
            # share it.
            print(
                f"  silence ceiling {_fmt_optional(report.silence_ceiling_db, '.2f', ' dB')} "
                f"at --top-db {float(top_db):.2f}"
            )
            print(
                f"  intervals per signal: {report.min_signal_intervals}.."
                f"{report.max_signal_intervals}"
            )
        if write_takes:
            print(f"Wrote {len(takes) * len(intervals)} take files with prefix {write_takes}")
    return 0


def cmd_resample(args: argparse.Namespace) -> int:

    target_rate = args.target_sample_rate
    samples, sr = _load_audio(args.file)
    result = _resample(samples, sr, target_rate)

    fit = _OutputFit().include(result)
    if args.output:
        _write_wav(args.output, result, target_rate, fit=fit)
        _note_output_fit(fit)

    if args.json:
        payload: dict[str, object] = {
            "length": len(result),
            "source_sample_rate": sr,
            "target_sample_rate": target_rate,
        }
        if args.output:
            payload["output"] = args.output
        payload["output_gain_db"] = fit.gain_db
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
        realtime_voice_changer_preset_names,
        realtime_voice_changer_render,
        voice_change,
    )

    raw_preset_id: object = getattr(args, "preset", None)
    raw_preset_json: object = getattr(args, "preset_json", None)
    raw_preset_pack: object = getattr(args, "preset_pack", None)
    preset_id = raw_preset_id if isinstance(raw_preset_id, str) else None
    preset_json = raw_preset_json if isinstance(raw_preset_json, str) and raw_preset_json else None
    preset_pack = raw_preset_pack if isinstance(raw_preset_pack, str) and raw_preset_pack else None
    assignments = list(getattr(args, "set", None) or [])
    # The selector relations are _voice_change_refusal's, decided before the input is read.
    selectors = [value for value in (preset_id, preset_json) if value is not None]
    pitch_semitones = getattr(args, "pitch_semitones", None)
    formant_factor = getattr(args, "formant_factor", None)
    raw_formant_mode = getattr(args, "formant_mode", None) or "relative"
    formant_mode: Literal["relative", "absolute"] = (
        "absolute" if raw_formant_mode == "absolute" else "relative"
    )
    preset_source = bool(selectors or preset_pack)

    samples, sr = _load_audio(args.file)
    input_length = len(samples)
    uses_realtime_preset = bool(preset_source or assignments)
    latency_samples = 0
    if uses_realtime_preset:
        from ._effects_voice import _VoicePresetDocument

        preset: str | dict[str, Any]
        if preset_json:
            with open(preset_json, encoding="utf-8") as fh:
                document = fh.read()
            preset = _VoicePresetDocument(document)
        elif preset_pack:
            assert preset_id is not None  # a pack without --preset was refused above
            preset = _load_voice_preset_pack(preset_pack, preset_id)
        else:
            assert preset_id is not None  # --preset is the only selector left
            # A built-in preset is named by its library id in any letter case.
            builtin = _catalog_token(preset_id, realtime_voice_changer_preset_names())
            preset = (
                json.loads(realtime_voice_changer_preset_json(builtin))
                if assignments
                else builtin
            )
        # A plain string is the catalog id; a mapping or _VoicePresetDocument is a document.
        writes: list[tuple[tuple[str, ...], tuple[str, ...], str, int]] = []
        config = cast(
            "VoicePresetId | dict[str, Any]",
            _apply_voice_sets(preset, assignments, provenance=writes),
        )
        if assignments and isinstance(config, dict):
            from ._effects_voice import _validate_voice_config_for_cli

            with _cli_voice_set_subjects(writes, config):
                _validate_voice_config_for_cli(config)
        # Probe the same prepared C-ABI configuration used by the one-shot
        # realtime entry point.  Latency depends on the resolved config and
        # sample rate, so it must not be a hard-coded preset constant.
        with RealtimeVoiceChanger(sr, num_channels=1, config=config) as changer:
            latency_samples = max(int(changer.latency_samples), 0)
        result = _audio_array(realtime_voice_changer_render(samples, sample_rate=sr, config=config))
        mode_metadata: dict[str, object] = {}
        if preset_id is not None and not preset_json and not preset_pack:
            mode_metadata["preset"] = preset_id
    else:
        result = voice_change(
            samples,
            sample_rate=sr,
            pitch_semitones=pitch_semitones if pitch_semitones is not None else 0.0,
            formant_factor=formant_factor if formant_factor is not None else 1.0,
            formant_mode=formant_mode,
        )
        mode_metadata = {
            "pitch_semitones": pitch_semitones if pitch_semitones is not None else 0.0,
            "formant_factor": formant_factor if formant_factor is not None else 1.0,
            "formant_mode": formant_mode,
        }

    # Spectral pitch/formant processing can differ by one sample at a frame
    # boundary.  The CLI contract is sample-preserving for both modes; trim a
    # longer result and zero-pad a shorter one before writing/serializing.
    result = _audio_array(result)[:input_length]
    if len(result) < input_length:
        result = np.pad(result, (0, input_length - len(result)))

    return _emit_effect_result(
        args,
        [result],
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
    from . import realtime_voice_changer_preset_json, realtime_voice_changer_preset_names

    # The library returns a JSON preset regardless of --json; print it as-is.
    names = realtime_voice_changer_preset_names()
    print(realtime_voice_changer_preset_json(_catalog_token(args.preset, names)))
    return 0


def cmd_voice_preset_validate(args: argparse.Namespace) -> int:
    from . import validate_realtime_voice_changer_preset_json

    path = args.preset_json or args.file
    if not path:
        raise FileNotFoundError("voice-preset-validate requires a JSON file")
    writes: list[tuple[tuple[str, ...], tuple[str, ...], str, int]] = []
    try:
        if args.preset:
            preset = _load_voice_preset_pack(path, args.preset)
            text_or_preset = _apply_voice_sets(preset, args.set, provenance=writes)
            text = json.dumps(text_or_preset)
        else:
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            text_or_preset = _apply_voice_sets(text, args.set, provenance=writes)
            text = (
                json.dumps(text_or_preset)
                if isinstance(text_or_preset, dict)
                else text_or_preset
            )
    except _DuplicateJsonKeyError as error:
        payload = {"ok": False, "error": error.text}
        print(_strict_json_dumps(payload) if args.json else error.text)
        return EXIT_INVALID_PARAMETER

    mapped_override = False
    if args.set and isinstance(text_or_preset, dict):
        with _cli_voice_set_subjects(writes, text_or_preset) as mapper:
            result = validate_realtime_voice_changer_preset_json(text)
            if not result.get("ok"):
                rendered = _render_as_given(_get_lib(), None, None)
                mapped_override = mapper.matched
                if mapped_override and rendered is not None:
                    result = {**result, "error": rendered[0]}
    else:
        result = validate_realtime_voice_changer_preset_json(text)
    normalized = result.get("normalizedJson")
    if not result.get("ok") or not normalized:
        error = str(result.get("error", "invalid voice preset"))
        payload = {"ok": False, "error": error}
        if args.json:
            print(_strict_json_dumps(payload))
        elif mapped_override:
            print(error, file=sys.stderr)
        else:
            print(error)
        return EXIT_INVALID_PARAMETER
    payload = {"ok": True, "normalized_json": str(normalized)}
    print(_strict_json_dumps(payload) if args.json else str(normalized))
    return 0


def _add_spectral_geometry(parser: argparse.ArgumentParser) -> None:
    """Declare ``--n-fft`` and ``--hop-length`` for a spectral effect, both left to the library."""
    _cli_domain(
        parser.add_argument("--n-fft", type=_c_int, default=None, help="FFT size (default: 2048)"),
        minimum=0,
        exclusive_minimum=True,
    )
    _cli_domain(
        parser.add_argument(
            "--hop-length", type=_c_int, default=None, help="Hop length (default: n_fft/4)"
        ),
        minimum=0,
        exclusive_minimum=True,
    )


def register_effects_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the offline effect and voice-preset commands."""
    common = shared.common
    stdout_options = shared.stdout_options

    hpss_p = sub.add_parser("hpss", parents=[common], help="Harmonic-percussive separation")
    _add_spectral_geometry(hpss_p)
    hpss_p.add_argument(
        "--kernel-harmonic",
        type=_c_int,
        default=None,
        help="Harmonic median-filter kernel (default: 31)",
    )
    hpss_p.add_argument(
        "--kernel-percussive",
        type=_c_int,
        default=None,
        help="Percussive median-filter kernel (default: 31)",
    )
    hpss_p.add_argument("--harmonic-only", action="store_true")
    hpss_p.add_argument("--percussive-only", action="store_true")
    hpss_p.add_argument(
        "--with-residual",
        action="store_true",
        help="Also write what neither component claimed. Silent without --hard-mask, "
        "whose masks are the ones that leave anything over",
    )
    hpss_p.add_argument(
        "--hard-mask",
        action="store_true",
        help="Assign each cell to whichever component dominates instead of blending them",
    )

    stems_p = sub.add_parser(
        "decompose-stems",
        parents=[common],
        help="Separate into NMF components that keep the original phase",
    )
    # The hop is derived from the FFT size, so this command states its own geometry.
    _cli_domain(
        stems_p.add_argument("--n-fft", type=_c_int, default=None, help="FFT size (default: 2048)"),
        minimum=1,
    )
    _cli_domain(
        stems_p.add_argument(
            "--hop-length", type=_c_int, default=None, help="Hop length (default: n_fft/4)"
        ),
        minimum=1,
    )
    _cli_domain(
        stems_p.add_argument(
            "--n-components",
            type=_c_int,
            default=None,
            help="Number of NMF components (default: 4)",
        ),
        minimum=1,
    )
    _cli_domain(
        stems_p.add_argument(
            "--n-iter", type=_c_int, default=None, help="NMF update iterations (default: 100)"
        ),
        minimum=1,
    )
    stems_p.add_argument(
        "--beta",
        type=_finite_float,
        default=None,
        help="Beta divergence: 2 Frobenius, 1 Kullback-Leibler (default: 2.0)",
    )
    _cli_domain(
        stems_p.add_argument(
            "--init",
            default=None,
            help="NMF initialisation: random or nndsvd (default: random)",
        ),
        choices=("random", "nndsvd"),
    )
    _cli_domain(
        stems_p.add_argument(
            "--mask-power",
            type=_finite_float,
            default=None,
            help="Soft-mask exponent, >= 1; 2 is the Wiener-style power ratio (default: 1.0)",
        ),
        minimum=1.0,
    )

    register_note_edit_parsers(sub, shared)
    # Effect commands that map directly to the Python effects API. The C++ CLI
    # still exposes some low-level converters and section/melody analyses that
    # are not mirrored here; this set covers the common offline edits.
    pitch_shift_p = sub.add_parser(
        "pitch-shift", parents=[common], help="Shift pitch by a number of semitones"
    )
    pitch_shift_p.add_argument(
        "--pitch-semitones", type=_finite_float, help="Semitones to shift (positive = up)"
    )
    # The spectral backend repairs a non-positive geometry into the librosa
    # defaults instead of refusing it, so the refusal a caller gets is the
    # handler's; these two commands are where it is reachable from a command line.
    _add_spectral_geometry(pitch_shift_p)
    time_stretch_p = sub.add_parser(
        "time-stretch", parents=[common], help="Time-stretch without changing pitch"
    )
    time_stretch_p.add_argument(
        "--rate", type=_finite_float, help="Stretch factor (>1 speeds up, <1 slows down)"
    )
    _add_spectral_geometry(time_stretch_p)
    normalize_p = sub.add_parser(
        "normalize", parents=[common], help="Peak-normalize audio to a target dB level"
    )
    _cli_domain(
        normalize_p.add_argument("--mode", default="peak", help="Normalization mode: peak or rms"),
        choices=("peak", "rms"),
    )
    normalize_p.add_argument(
        "--target-db",
        type=_finite_float,
        default=None,
        help="Target level in dB (default: 0.0 (peak) or -20.0 (rms))",
    )
    trim_silence_p = sub.add_parser(
        "trim-silence",
        parents=[common],
        help="Trim leading/trailing silence",
        description=(
            "Trim leading/trailing silence. The default rule trims below an absolute "
            "-60 dBFS threshold (--threshold-db); --top-db selects the peak-relative "
            "rule instead."
        ),
    )
    trim_silence_p.add_argument(
        "--threshold-db",
        type=_finite_float,
        default=None,
        help="Silence threshold in dB (default: -60.0)",
    )
    # ``--threshold-db`` and ``--top-db`` select two handler paths. Leave the
    # alternate selector absent by default so the handler can distinguish the
    # default threshold mode from an explicit top-dB request.
    trim_silence_p.add_argument(
        "--top-db",
        type=_finite_float,
        default=None,
        help="Trim by the peak-relative rule instead, at this many dB below the peak",
    )
    trim_silence_p.add_argument(
        "--n-fft", type=_c_int, default=None, help="Analysis frame length (default: 2048)"
    )
    trim_silence_p.add_argument(
        "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
    )
    split_silence_p = sub.add_parser(
        "split-silence", parents=[stdout_options], help="List non-silent intervals"
    )
    split_silence_p.add_argument(
        "--n-fft", type=_c_int, default=None, help="Analysis frame length (default: 2048)"
    )
    split_silence_p.add_argument(
        "--hop-length", type=_c_int, default=None, help="Hop length (default: 512)"
    )
    split_silence_p.add_argument(
        "--top-db",
        type=_finite_float,
        default=None,
        help="Silence threshold below the peak in dB (default: 60.0)",
    )
    split_silence_p.add_argument(
        "--write-takes",
        metavar="PREFIX",
        help=("Write every take sliced at every interval as PREFIX{take:02d}_{interval:03d}.wav"),
    )
    split_silence_p.add_argument(
        "--report",
        action="store_true",
        help=(
            "Also say why those are the intervals: the largest --top-db at which every "
            "take still shows silence, read against the one in use. One interval covering "
            "everything answers three situations and the intervals cannot say which"
        ),
    )
    resample_p = sub.add_parser(
        "resample", parents=[common], help="Resample audio to a target sample rate"
    )
    _cli_domain(
        resample_p.add_argument(
            "--target-sample-rate",
            dest="target_sample_rate",
            type=_c_int,
            help="Target sample rate in Hz",
        ),
        minimum=_MIN_GENERATOR_SAMPLE_RATE,
        maximum=_MAX_GENERATOR_SAMPLE_RATE,
    )
    voice_change_p = sub.add_parser(
        "voice-change", parents=[common], help="Apply a voice-change effect"
    )
    voice_change_p.add_argument(
        "--pitch-semitones", type=_finite_float, help="Pitch shift in semitones"
    )
    voice_change_p.add_argument(
        "--formant-factor",
        type=_finite_float,
        help=(
            "Formant scaling factor. relative: warp applied after the pitch shift (the "
            "formants follow the pitch); absolute: formant shift relative to the input "
            "(1.0 keeps the formants)"
        ),
    )
    _cli_domain(
        voice_change_p.add_argument(
            "--formant-mode",
            default="relative",
            help="How --formant-factor relates to the pitch shift: relative or absolute",
        ),
        choices=("relative", "absolute"),
    )
    voice_change_p.add_argument(
        "--preset", default=None, help="Realtime voice changer preset id (default: none)"
    )
    voice_change_p.add_argument("--preset-json", help="Realtime voice changer preset JSON file")
    voice_change_p.add_argument(
        "--preset-pack", help="Realtime voice changer preset pack JSON file"
    )
    voice_change_p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="PATH=VALUE",
        help="Override preset JSON fields, e.g. dsp.outputGainDb=-2",
    )
    sub.add_parser(
        "voice-presets", parents=[stdout_options], help="List realtime voice changer presets"
    )
    voice_preset_p = sub.add_parser(
        "voice-preset", help="Print a realtime voice changer preset (always JSON)"
    )
    voice_preset_p.add_argument("--preset", default="neutralMonitor", help="Preset id")
    voice_preset_p.add_argument(
        "--json", action="store_true", help="No-op; a preset is always printed as JSON"
    )
    # Not a parents=[common] subcommand: it consumes a JSON preset, not audio,
    # so the analysis flags (--n-fft/--hop-length/--n-mels) do not apply and the
    # positional argument is a preset file rather than an audio file.
    voice_preset_validate_p = sub.add_parser(
        "voice-preset-validate",
        help="Validate and normalize voice preset JSON",
        description=(
            "Validate and normalize voice preset JSON. A preset the library refuses is a "
            "verdict here, not a refused input: it prints ok false and exits 3, where "
            "voice-change refuses the same document with exit 5."
        ),
    )
    voice_preset_validate_p.add_argument("--json", action="store_true", help="Output JSON")
    voice_preset_validate_p.add_argument(
        "--preset-json",
        help="Voice preset JSON file (takes precedence over the positional path)",
    )
    voice_preset_validate_p.add_argument(
        "--preset", default="", help="Preset id when validating a pack"
    )
    voice_preset_validate_p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="PATH=VALUE",
        help="Override preset JSON fields before validation",
    )
    voice_preset_validate_p.add_argument("file", nargs="?", help="Voice preset JSON file")
