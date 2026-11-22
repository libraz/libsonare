"""The system-info command and the commands that take no input file."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from typing import Any, cast

from ._cli_common import _strict_json_dumps
from ._cli_dsp import _save_result
from ._cli_inventory import _cli_domain
from ._cli_options import (
    SharedParsers,
    _c_int,
    _ContractArgumentParser,
    _effective,
    _finite_float,
    _given,
    _NumericTextError,
    _read_number_text,
)
from ._runtime import _C_INT_MAX

# The sample rates the generators accept: those the library's own decoder reads back.
_MIN_GENERATOR_SAMPLE_RATE = 8000
_MAX_GENERATOR_SAMPLE_RATE = 384000

# The longest offline buffer the library renders, in samples.
_MAX_OFFLINE_AUDIO_SAMPLES = 500_000_000

# The `--norm-type` indexes, each mapped to the library's `norm` spelling.
_NORM_TYPES = ("max", "l1", "l2", "power")

# Commands whose `--values` hold integers (frame indices).
_INTEGER_VALUE_COMMANDS = frozenset({"fix-frames"})
_C_INT_LIMIT = _C_INT_MAX + 1


def _count_text(value: Any) -> str:
    return "unknown" if value is None else str(value)


def _gigabytes_text(value: Any) -> str:
    return "unknown" if value is None else f"{value:.1f} GB"


def cmd_system_info(args: argparse.Namespace) -> int:
    from . import system_info

    info = system_info()
    if args.json:
        print(_strict_json_dumps(info))
        return 0
    cpu = info.get("cpu", {})
    memory = info.get("memory", {})
    parallel = info.get("parallel", {})
    print("System Information")
    print(
        f"  CPU Cores: {_count_text(cpu.get('logical_cores'))} logical, "
        f"{_count_text(cpu.get('physical_cores'))} physical"
    )
    print(
        f"  Memory: {_gigabytes_text(memory.get('total_gb'))} total, "
        f"{_gigabytes_text(memory.get('available_gb'))} available"
    )
    print()
    print("Parallel Configuration")
    print(f"  Parallel Enabled: {'yes' if parallel.get('enabled') else 'no'}")
    print(f"  Workers: {_count_text(parallel.get('workers'))}")
    print(f"  Strategy: {_count_text(parallel.get('strategy'))}")
    return 0


def register_utilities_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the system-info command and the no-input commands."""
    _register_generators(sub, shared)
    _register_conversions(sub, shared)
    _register_arrays(sub, shared)
    sub.add_parser(
        "system-info",
        parents=[shared.stdout_options],
        help="Show system and parallel configuration",
    )


def _read_number_list(text: str) -> list[float] | None:
    """Read a comma-separated list of finite numbers, or None when any item is not one."""
    numbers: list[float] = []
    for item in text.split(","):
        # An empty entry is skipped, so "1,,2" reads as two numbers.
        if item == "":
            continue
        try:
            numbers.append(_read_number_text(item))
        except _NumericTextError:
            return None
    return numbers or None


def _list_refusal(option: str) -> str:
    return f"argument --{option}: must be a comma-separated list of finite numbers"


def _utility_argument_refusal(args: argparse.Namespace) -> str | None:
    """The usage refusal for a no-input command's list grammar or option relation, else None."""
    command = args.command
    for option in ("values", "times"):
        text = getattr(args, option, None)
        if text is None:
            continue
        numbers = _read_number_list(text)
        if numbers is None:
            return _list_refusal(option)
        if command in _INTEGER_VALUE_COMMANDS and any(
            value != int(value) or abs(value) >= _C_INT_LIMIT for value in numbers
        ):
            return _list_refusal(option)
        if command == "pcen" and len(numbers) != args.n_bins * args.n_frames:
            return "argument --values: must hold --n-bins times --n-frames values"
    if command == "chirp" and args.exponential and args.fmin_hz <= 0:
        return "--fmin-hz must be greater than 0 for an exponential sweep"
    return None


def _values(args: argparse.Namespace) -> list[float]:
    # The command validator has already refused anything but a well-formed list.
    return cast(list[float], _read_number_list(args.values))


def _item_text(value: float) -> str:
    return str(value) if isinstance(value, int) else format(value, "g")


def _print_list(args: argparse.Namespace, items: Sequence[float], payload: Any = None) -> int:
    """Print a result as JSON (the list, or @p payload), else as comma-separated values."""
    if args.json:
        print(_strict_json_dumps(list(items) if payload is None else payload))
    else:
        print(",".join(_item_text(value) for value in items))
    return 0


def cmd_tone(args: argparse.Namespace) -> int:
    from . import tone

    given = _given(
        args, sr="sample_rate", duration="duration_sec", phi="phase", amplitude="amplitude"
    )
    samples = tone(args.frequency_hz, **given)
    sample_rate = _effective(args, tone, "sr", "sample_rate")
    return _save_result(
        args, samples, sample_rate, {"frequency_hz": args.frequency_hz, "sample_rate": sample_rate}
    )


def cmd_chirp(args: argparse.Namespace) -> int:
    from . import chirp

    given = _given(args, sr="sample_rate", duration="duration_sec")
    samples = chirp(args.fmin_hz, args.fmax_hz, linear=not args.exponential, **given)
    sample_rate = _effective(args, chirp, "sr", "sample_rate")
    return _save_result(
        args,
        samples,
        sample_rate,
        {
            "fmin_hz": args.fmin_hz,
            "fmax_hz": args.fmax_hz,
            "sample_rate": sample_rate,
            "linear": not args.exponential,
        },
    )


def cmd_clicks(args: argparse.Namespace) -> int:
    from . import clicks

    times = cast(list[float], _read_number_list(args.times))
    given = _given(
        args,
        sr="sample_rate",
        length="length",
        click_freq="frequency_hz",
        click_duration="click_duration_sec",
    )
    samples = clicks(times, **given)
    sample_rate = _effective(args, clicks, "sr", "sample_rate")
    return _save_result(
        args, samples, sample_rate, {"n_clicks": len(times), "sample_rate": sample_rate}
    )


def cmd_frames_to_samples(args: argparse.Namespace) -> int:
    from . import frames_to_samples

    samples = frames_to_samples(args.frames, **_given(args, hop_length="hop_length", n_fft="n_fft"))
    print(_strict_json_dumps({"samples": samples}) if args.json else samples)
    return 0


def cmd_samples_to_frames(args: argparse.Namespace) -> int:
    from . import samples_to_frames

    frames = samples_to_frames(args.samples, **_given(args, hop_length="hop_length", n_fft="n_fft"))
    print(_strict_json_dumps({"frames": frames}) if args.json else frames)
    return 0


def cmd_power_to_db(args: argparse.Namespace) -> int:
    from . import power_to_db

    return _print_list(
        args, power_to_db(_values(args), **_given(args, "ref", "amin", "top_db")).tolist()
    )


def cmd_amplitude_to_db(args: argparse.Namespace) -> int:
    from . import amplitude_to_db

    return _print_list(
        args, amplitude_to_db(_values(args), **_given(args, "ref", "amin", "top_db")).tolist()
    )


def cmd_db_to_power(args: argparse.Namespace) -> int:
    from . import db_to_power

    return _print_list(args, db_to_power(_values(args), **_given(args, "ref")).tolist())


def cmd_db_to_amplitude(args: argparse.Namespace) -> int:
    from . import db_to_amplitude

    return _print_list(args, db_to_amplitude(_values(args), **_given(args, "ref")).tolist())


def cmd_frame_signal(args: argparse.Namespace) -> int:
    from . import frame_signal

    frames = frame_signal(_values(args), args.frame_length, args.hop_length)
    # The library returns (frame_length, n_frames); the payload lists frame after frame.
    flat = frames.T.reshape(-1).tolist()
    return _print_list(args, flat, {"n_frames": frames.shape[1], "frames": flat})


def cmd_pad_center(args: argparse.Namespace) -> int:
    from . import pad_center

    result = pad_center(_values(args), args.size, **_given(args, "pad_value"))
    return _print_list(args, result.tolist())


def cmd_fix_length(args: argparse.Namespace) -> int:
    from . import fix_length

    result = fix_length(_values(args), args.size, **_given(args, "pad_value"))
    return _print_list(args, result.tolist())


def cmd_fix_frames(args: argparse.Namespace) -> int:
    from . import fix_frames

    frames = [int(value) for value in _values(args)]
    result = fix_frames(frames, **_given(args, "x_min", "x_max"), pad=not args.no_pad)
    return _print_list(args, result.tolist())


def cmd_peak_pick(args: argparse.Namespace) -> int:
    from . import peak_pick

    result = peak_pick(
        _values(args),
        args.pre_max,
        args.post_max,
        args.pre_avg,
        args.post_avg,
        args.delta,
        args.wait,
    )
    return _print_list(args, result.tolist())


def cmd_vector_normalize(args: argparse.Namespace) -> int:
    from . import vector_normalize

    given: dict[str, Any] = _given(args, "threshold")
    if args.norm_type is not None:
        given["norm"] = _NORM_TYPES[args.norm_type]
    return _print_list(args, vector_normalize(_values(args), **given).tolist())


def cmd_pcen(args: argparse.Namespace) -> int:
    from . import pcen

    values = _values(args)
    spectrogram = [
        values[row * args.n_frames : (row + 1) * args.n_frames] for row in range(args.n_bins)
    ]
    given = _given(
        args,
        sr="sample_rate",
        hop_length="hop_length",
        time_constant="time_constant",
        gain="gain",
        bias="bias",
        power="power",
        eps="eps",
    )
    result = pcen(spectrogram, **given)
    return _print_list(args, result.reshape(-1).tolist())


def cmd_mastering_stereo_analyses(args: argparse.Namespace) -> int:
    from . import mastering_stereo_analysis_names

    names = mastering_stereo_analysis_names()
    if args.json:
        print(_strict_json_dumps({"analyses": names}))
    else:
        for name in names:
            print(name)
    return 0


def _stated(text: str, default: str) -> str:
    return f"{text} (default: {default})"


def _sample_rate_option(parser: argparse.ArgumentParser, text: str = "Sample rate in Hz") -> None:
    _cli_domain(
        parser.add_argument(
            "--sample-rate", type=_c_int, default=None, help=_stated(text, "22050")
        ),
        minimum=_MIN_GENERATOR_SAMPLE_RATE,
        maximum=_MAX_GENERATOR_SAMPLE_RATE,
    )


def _values_option(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--values", default=None, help="Comma-separated numbers")


def _register_generators(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    common = shared.common

    tone_p = sub.add_parser("tone", parents=[common], help="Write a sine tone")
    tone_p.add_argument("--frequency-hz", type=_finite_float, help="Tone frequency in Hz")
    _sample_rate_option(tone_p)
    tone_p.add_argument(
        "--duration-sec",
        type=_finite_float,
        default=None,
        help=_stated("Duration in seconds", "1.0"),
    )
    tone_p.add_argument(
        "--phase", type=_finite_float, default=None, help=_stated("Initial phase in radians", "0.0")
    )
    tone_p.add_argument(
        "--amplitude", type=_finite_float, default=None, help=_stated("Peak amplitude", "1.0")
    )

    chirp_p = sub.add_parser("chirp", parents=[common], help="Write a frequency sweep")
    _sample_rate_option(chirp_p)
    chirp_p.add_argument(
        "--duration-sec",
        type=_finite_float,
        default=None,
        help=_stated("Duration in seconds", "1.0"),
    )
    chirp_p.add_argument(
        "--exponential", action="store_true", help="Sweep exponentially rather than linearly"
    )
    _cli_domain(
        chirp_p.add_argument(
            "--fmin-hz", type=_finite_float, default=0.0, help="Start frequency in Hz"
        ),
        minimum=0,
    )
    _cli_domain(
        chirp_p.add_argument("--fmax-hz", type=_finite_float, help="End frequency in Hz"),
        minimum=0,
        exclusive_minimum=True,
    )

    clicks_p = sub.add_parser("clicks", parents=[common], help="Write a click track")
    clicks_p.add_argument("--times", default=None, help="Comma-separated click times in seconds")
    _sample_rate_option(clicks_p)
    _cli_domain(
        clicks_p.add_argument(
            "--length",
            type=_c_int,
            default=None,
            help="Output length in samples (default: through the last click and its decay)",
        ),
        minimum=1,
        maximum=_MAX_OFFLINE_AUDIO_SAMPLES,
    )
    clicks_p.add_argument(
        "--frequency-hz",
        type=_finite_float,
        default=None,
        help=_stated("Click frequency in Hz", "1000.0"),
    )
    clicks_p.add_argument(
        "--click-duration-sec",
        type=_finite_float,
        default=None,
        help=_stated("Click length in seconds", "0.1"),
    )


def _register_conversions(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    out = shared.stdout_options

    frames_p = sub.add_parser(
        "frames-to-samples", parents=[out], help="Convert a count of frames to the other unit"
    )
    frames_p.add_argument("--frames", type=_c_int, help="Number of frames")
    _conversion_geometry(frames_p)
    samples_p = sub.add_parser(
        "samples-to-frames", parents=[out], help="Convert a count of samples to the other unit"
    )
    samples_p.add_argument("--samples", type=_c_int, help="Number of samples")
    _conversion_geometry(samples_p)

    power_db_p = sub.add_parser(
        "power-to-db", parents=[out], help="Convert power values to decibels"
    )
    _to_db_options(power_db_p, "1e-10")
    amplitude_db_p = sub.add_parser(
        "amplitude-to-db", parents=[out], help="Convert amplitude values to decibels"
    )
    _to_db_options(amplitude_db_p, "1e-05")

    db_power_p = sub.add_parser(
        "db-to-power", parents=[out], help="Convert decibels to power values"
    )
    _from_db_options(db_power_p)
    db_amplitude_p = sub.add_parser(
        "db-to-amplitude", parents=[out], help="Convert decibels to amplitude values"
    )
    _from_db_options(db_amplitude_p)


def _conversion_geometry(parser: argparse.ArgumentParser) -> None:
    _cli_domain(
        parser.add_argument(
            "--n-fft", type=_c_int, default=None, help="FFT size (default: no centring offset)"
        ),
        minimum=1,
    )
    _cli_domain(
        parser.add_argument(
            "--hop-length", type=_c_int, default=None, help=_stated("Hop length", "512")
        ),
        minimum=1,
    )


def _to_db_options(parser: argparse.ArgumentParser, amin: str) -> None:
    _values_option(parser)
    parser.add_argument("--ref", type=_finite_float, default=None, help=_stated("Reference", "1.0"))
    parser.add_argument("--amin", type=_finite_float, default=None, help=_stated("Floor", amin))
    parser.add_argument(
        "--top-db",
        type=_finite_float,
        default=None,
        help=_stated("Dynamic range below the peak in dB", "80.0"),
    )


def _from_db_options(parser: argparse.ArgumentParser) -> None:
    _values_option(parser)
    parser.add_argument("--ref", type=_finite_float, default=None, help=_stated("Reference", "1.0"))


def _sizing_options(parser: argparse.ArgumentParser) -> None:
    _values_option(parser)
    _cli_domain(parser.add_argument("--size", type=_c_int, help="Target size"), minimum=1)
    parser.add_argument(
        "--pad-value", type=_finite_float, default=None, help=_stated("Padding value", "0.0")
    )


def _register_arrays(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    out = shared.stdout_options

    frame_p = sub.add_parser("frame-signal", parents=[out], help="Slice values into frames")
    _values_option(frame_p)
    frame_p.add_argument("--frame-length", type=_c_int, help="Frame length in samples")
    frame_p.add_argument("--hop-length", type=_c_int, help="Hop length in samples")

    pad_p = sub.add_parser("pad-center", parents=[out], help="Centre-pad values to a size")
    _sizing_options(pad_p)
    fix_length_p = sub.add_parser("fix-length", parents=[out], help="Pad or trim values to a size")
    _sizing_options(fix_length_p)

    fix_p = sub.add_parser("fix-frames", parents=[out], help="Fix a frame index list")
    _values_option(fix_p)
    fix_p.add_argument("--x-min", type=_c_int, default=None, help=_stated("Lower bound", "0"))
    _cli_domain(
        fix_p.add_argument(
            "--x-max",
            type=_c_int,
            default=None,
            help="Upper bound (default: no upper bound)",
        ),
        minimum=0,
    )
    fix_p.add_argument("--no-pad", action="store_true", help="Do not add the bounds to the list")

    peak_p = sub.add_parser("peak-pick", parents=[out], help="Pick peaks from values")
    _values_option(peak_p)
    peak_p.add_argument("--pre-max", type=_c_int, help="Samples before a peak that it must exceed")
    peak_p.add_argument("--post-max", type=_c_int, help="Samples after a peak that it must exceed")
    peak_p.add_argument("--pre-avg", type=_c_int, help="Samples before a peak averaged")
    peak_p.add_argument("--post-avg", type=_c_int, help="Samples after a peak averaged")
    peak_p.add_argument("--delta", type=_finite_float, help="Margin over the average")
    peak_p.add_argument("--wait", type=_c_int, help="Samples to wait after a peak")

    norm_p = sub.add_parser("vector-normalize", parents=[out], help="Normalize values")
    _values_option(norm_p)
    _cli_domain(
        norm_p.add_argument(
            "--norm-type",
            type=_c_int,
            default=None,
            help=_stated("Norm: 0 max, 1 L1, 2 L2, 3 power", "0"),
        ),
        choices=tuple(range(len(_NORM_TYPES))),
    )
    norm_p.add_argument(
        "--threshold", type=_finite_float, default=None, help=_stated("Threshold", "0.0")
    )

    pcen_p = sub.add_parser("pcen", parents=[out], help="Per-channel energy normalization")
    _values_option(pcen_p)
    pcen_p.add_argument(
        "--sample-rate", type=_c_int, default=None, help=_stated("Sample rate in Hz", "22050")
    )
    pcen_p.add_argument(
        "--time-constant",
        type=_finite_float,
        default=None,
        help=_stated("Smoothing time constant in seconds", "0.4"),
    )
    pcen_p.add_argument(
        "--gain", type=_finite_float, default=None, help=_stated("Gain exponent", "0.98")
    )
    pcen_p.add_argument("--bias", type=_finite_float, default=None, help=_stated("Bias", "2.0"))
    pcen_p.add_argument(
        "--power", type=_finite_float, default=None, help=_stated("Compression exponent", "0.5")
    )
    pcen_p.add_argument("--eps", type=_finite_float, default=None, help=_stated("Epsilon", "1e-06"))
    _cli_domain(pcen_p.add_argument("--n-bins", type=_c_int, help="Number of rows"), minimum=1)
    _cli_domain(pcen_p.add_argument("--n-frames", type=_c_int, help="Number of columns"), minimum=1)
    pcen_p.add_argument(
        "--hop-length", type=_c_int, default=None, help=_stated("Hop length", "512")
    )

    sub.add_parser(
        "mastering-stereo-analyses",
        parents=[out],
        help="List the stereo analyses",
    )
