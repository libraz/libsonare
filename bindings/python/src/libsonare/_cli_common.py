"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile
from collections.abc import Iterator, Sequence
from contextlib import contextmanager, suppress
from typing import Any, cast

from ._ffi import (
    SONARE_ERROR_CANCELLED,
    SONARE_ERROR_DECODE_FAILED,
    SONARE_ERROR_FILE_NOT_FOUND,
    SONARE_ERROR_INVALID_FORMAT,
    SONARE_ERROR_INVALID_PARAMETER,
    SONARE_ERROR_INVALID_STATE,
    SONARE_ERROR_NOT_SUPPORTED,
    SONARE_ERROR_OUT_OF_MEMORY,
    resolved_library_path,
)
from ._runtime import SonareError
from .types import KeyProfile, Mode, PitchClass

PITCH_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MODE_NAMES = ["major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"]

# CLI exit codes. Failures map to codes aligned with the C-ABI SonareError so
# scripts can distinguish usage / missing-file / decode / processing errors.
# argparse keeps its native exit 2 for usage errors. Set SONARE_LEGACY_EXIT=1 to
# fold every failure back to 1 for scripts that hardcode the old contract.
#
# NOTE: for an undecodable input, whether the CLI reports 5 (INVALID_FORMAT)
# or 6 (DECODE_FAILED) depends on whether the native library was built with
# FFmpeg support, not on the input itself. A build without FFmpeg reports 5
# for input that an FFmpeg build reports as 6. Scripts should treat {5, 6}
# as a single "bad/undecodable input" category rather than branching on one
# specific code.
EXIT_SUCCESS = 0
EXIT_USAGE = 2
EXIT_INVALID_PARAMETER = 3
EXIT_FILE_NOT_FOUND = 4
EXIT_INVALID_FORMAT = 5
EXIT_DECODE_FAILED = 6
EXIT_OUT_OF_MEMORY = 7
EXIT_NOT_SUPPORTED = 8
EXIT_INVALID_STATE = 9
EXIT_ERROR = 10
EXIT_CANCELLED = 11

_SONARE_CODE_TO_EXIT = {
    SONARE_ERROR_INVALID_PARAMETER: EXIT_INVALID_PARAMETER,
    SONARE_ERROR_FILE_NOT_FOUND: EXIT_FILE_NOT_FOUND,
    SONARE_ERROR_INVALID_FORMAT: EXIT_INVALID_FORMAT,
    SONARE_ERROR_DECODE_FAILED: EXIT_DECODE_FAILED,
    SONARE_ERROR_OUT_OF_MEMORY: EXIT_OUT_OF_MEMORY,
    SONARE_ERROR_NOT_SUPPORTED: EXIT_NOT_SUPPORTED,
    SONARE_ERROR_INVALID_STATE: EXIT_INVALID_STATE,
    SONARE_ERROR_CANCELLED: EXIT_CANCELLED,
}


def _sanitize_json_value(value: object) -> object:
    """Recursively replace non-finite floats with JSON ``null`` values."""
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {key: _sanitize_json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_sanitize_json_value(item) for item in value]
    return value


def _strict_json_dumps(value: object, **kwargs: Any) -> str:
    """Serialize a CLI payload as standards-compliant JSON."""
    return json.dumps(_sanitize_json_value(value), allow_nan=False, **kwargs)


def _color_enabled() -> bool:
    """Whether human-readable CLI output may include ANSI color sequences."""
    return "NO_COLOR" not in os.environ and sys.stdout.isatty() and sys.stderr.isatty()


def cmd_doctor(args: argparse.Namespace) -> int:
    """Print a concise diagnostic report for the loaded libsonare build."""
    from ._analysis_music import capabilities

    descriptor = capabilities()
    payload = {
        "version": descriptor["version"],
        "abi": descriptor["abi"],
        "platform": descriptor["platform"],
        "features": descriptor["features"],
        "decode": descriptor["decode"],
        "simd": descriptor["simd"],
        "hardware_concurrency": descriptor["hardwareConcurrency"],
    }
    if args.json:
        print(_strict_json_dumps(payload))
        return EXIT_SUCCESS

    library_path = resolved_library_path()
    abi = descriptor["abi"]
    features = descriptor["features"]
    decode = descriptor["decode"]
    print(f"libsonare {descriptor['version']}")
    print(f"  Library:              {library_path}")
    print(f"  Platform:             {descriptor['platform']}")
    print(f"  ABI:                  project={abi['project']}, engine={abi['engine']}")
    print(
        "  Features:             "
        f"mastering={features['mastering']}, mixing={features['mixing']}, "
        f"fx={features['fx']}, ffmpeg={features['ffmpeg']}"
    )
    print(f"  Decode (built-in):    {', '.join(decode['builtin'])}")
    print(f"  Decode (FFmpeg):      {', '.join(decode['ffmpeg']) or 'none'}")
    print(f"  SIMD:                 {descriptor['simd']}")
    print(f"  Hardware concurrency: {payload['hardware_concurrency']}")
    return EXIT_SUCCESS


def _legacy_exit_codes() -> bool:
    """Whether SONARE_LEGACY_EXIT requests the old all-failures-are-1 behaviour."""
    return os.environ.get("SONARE_LEGACY_EXIT") == "1"


def _exit_code_for(exc: BaseException) -> int:
    """Map an exception to a CLI exit code aligned with the C-ABI error codes."""
    if _legacy_exit_codes():
        return 1
    if isinstance(exc, SonareError):
        return _SONARE_CODE_TO_EXIT.get(exc.code, EXIT_ERROR)
    if isinstance(exc, FileNotFoundError):
        return EXIT_FILE_NOT_FOUND
    if isinstance(exc, MemoryError):
        return EXIT_OUT_OF_MEMORY
    if isinstance(exc, (ValueError, json.JSONDecodeError)):
        return EXIT_INVALID_PARAMETER
    return EXIT_ERROR


# NOTE: Some C++ CLI commands (sections, melody, boundaries, cqt variants, and
# low-level math/unit converters) are not mirrored here yet. Several already
# have Python library backing; this note tracks CLI parity, not Python API
# availability.


def _load_audio(path: str) -> tuple[list[float], int]:
    """Load audio from file via the Audio class.

    ``Audio.from_file`` always returns a mono signal: stereo (and higher
    channel-count) inputs are downmixed to a single channel on load. Callers
    that render stereo output (for example ``mix``) therefore start from a mono
    source and duplicate it across channels rather than preserving the original
    channels.
    """
    from .audio import Audio

    with Audio.from_file(path) as audio:
        return audio.data, audio.sample_rate


def _load_audio_from_facade(path: str) -> tuple[list[float], int]:
    """Load through the stable facade so its historical patch point remains usable."""
    from . import cli

    return cli._load_audio(path)


def _resample(samples: list[float], source_rate: int, target_rate: int) -> list[float]:
    """Resample mono samples with the native anti-aliased resampler.

    Routes through the C-ABI ``resample`` (r8brain) so the CLI matches the C++
    CLI and ``Audio.resample()`` numerically. Falls back to linear interpolation
    only when the native shared library cannot be loaded, keeping the CLI usable
    in a library-less environment.
    """
    if source_rate <= 0 or target_rate <= 0:
        raise ValueError("sample rates must be positive")
    if source_rate == target_rate:
        return list(samples)
    if len(samples) == 0:
        return []

    from . import resample as _native_resample

    try:
        return _native_resample(list(samples), src_sr=source_rate, target_sr=target_rate)
    except OSError:
        # Shared library missing/unloadable: degrade to linear interpolation.
        return _resample_linear(samples, source_rate, target_rate)


def _resample_linear(samples: list[float], source_rate: int, target_rate: int) -> list[float]:
    """Resample mono samples with linear interpolation.

    Fallback path used only when the native resampler cannot be loaded;
    ``_resample`` is the normal entry point.
    """
    if source_rate <= 0 or target_rate <= 0:
        raise ValueError("sample rates must be positive")
    if source_rate == target_rate:
        return list(samples)
    if len(samples) == 0:
        return []

    output_count = max(1, int(round(len(samples) * target_rate / source_rate)))
    if output_count == 1 or len(samples) == 1:
        return [samples[0]] * output_count

    ratio = source_rate / target_rate
    last_index = len(samples) - 1
    output: list[float] = []
    for i in range(output_count):
        position = min(i * ratio, float(last_index))
        index = int(position)
        fraction = position - index
        if index >= last_index:
            output.append(samples[last_index])
        else:
            output.append(samples[index] + (samples[index + 1] - samples[index]) * fraction)
    return output


def _pcm16(sample: float) -> bytes:
    """Clamp a float to ``[-1.0, 1.0]`` and pack it as little-endian 16-bit PCM.

    Shared by every WAV writer so the clamp-and-scale contract stays identical.
    """
    import struct

    clamped = -1.0 if sample < -1.0 else (1.0 if sample > 1.0 else sample)
    return struct.pack("<h", int(round(clamped * 32767.0)))


def _pcm24(sample: float) -> bytes:
    """Clamp a float and pack it as little-endian 24-bit PCM."""
    clamped = -1.0 if sample < -1.0 else (1.0 if sample > 1.0 else sample)
    value = int(round(clamped * 8388607.0))
    return value.to_bytes(3, byteorder="little", signed=True)


def _pcm(sample: float, bits_per_sample: int) -> bytes:
    if bits_per_sample == 16:
        return _pcm16(sample)
    if bits_per_sample == 24:
        return _pcm24(sample)
    raise ValueError("WAV bits must be 16 or 24")


_WAV_CHUNK_FRAMES = 8192


@contextmanager
def _atomic_wav_writer(
    path: str, channels: int, sample_rate: int, bits_per_sample: int = 16
) -> Iterator[Any]:
    """Yield a WAV writer whose completed file atomically replaces ``path``."""
    import wave

    if bits_per_sample not in (16, 24):
        raise ValueError("WAV bits must be 16 or 24")
    target = os.path.abspath(path)
    directory = os.path.dirname(target)
    raw = tempfile.NamedTemporaryFile(  # noqa: SIM115 - lifetime spans the yielded writer
        mode="w+b",
        prefix=f".{os.path.basename(target)}.",
        suffix=".tmp",
        dir=directory,
        delete=False,
    )
    temporary = raw.name
    wav = None
    try:
        wav = wave.open(raw, "wb")  # noqa: SIM115 - closed before the atomic replace
        wav.setnchannels(channels)
        wav.setsampwidth(bits_per_sample // 8)
        wav.setframerate(int(sample_rate))
        yield wav
        wav.close()
        raw.flush()
        os.fsync(raw.fileno())
        raw.close()
        os.replace(temporary, target)
    except BaseException:
        if wav is not None:
            with suppress(Exception):
                wav.close()
        with suppress(Exception):
            raw.close()
        with suppress(FileNotFoundError):
            os.unlink(temporary)
        raise


def _write_wav_mono_frames(wav: Any, samples: Sequence[float], bits_per_sample: int = 16) -> None:
    """Append one bounded mono PCM chunk to an open WAV writer."""
    frames = bytearray()
    for sample in samples:
        frames.extend(_pcm(float(sample), bits_per_sample))
    wav.writeframesraw(frames)


def _write_wav_stereo_frames(
    wav: Any,
    left: Sequence[float],
    right: Sequence[float],
    bits_per_sample: int = 16,
) -> None:
    """Append one bounded stereo PCM chunk to an open WAV writer."""
    count = min(len(left), len(right))
    frames = bytearray()
    for index in range(count):
        frames.extend(_pcm(float(left[index]), bits_per_sample))
        frames.extend(_pcm(float(right[index]), bits_per_sample))
    wav.writeframesraw(frames)


def _write_wav(
    path: str, samples: list[float], sample_rate: int, bits_per_sample: int = 16
) -> None:
    """Write mono 16- or 24-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled to the selected PCM range.
    """
    with _atomic_wav_writer(path, 1, sample_rate, bits_per_sample) as wav:
        for offset in range(0, len(samples), _WAV_CHUNK_FRAMES):
            chunk = samples[offset : offset + _WAV_CHUNK_FRAMES]
            if bits_per_sample == 16:
                # Keep the historical two-argument call shape for callers that
                # instrument this helper; 24-bit output opts into the width.
                _write_wav_mono_frames(wav, chunk)
            else:
                _write_wav_mono_frames(wav, chunk, bits_per_sample)


def _write_wav_stereo(path: str, left: list[float], right: list[float], sample_rate: int) -> None:
    """Write a stereo 16-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled by 32767.
    """
    count = min(len(left), len(right))
    with _atomic_wav_writer(path, 2, sample_rate) as wav:
        for offset in range(0, count, _WAV_CHUNK_FRAMES):
            end = min(offset + _WAV_CHUNK_FRAMES, count)
            _write_wav_stereo_frames(wav, left[offset:end], right[offset:end])


def _write_project_bounce_wav(path: str, audio: object, sample_rate: int) -> tuple[int, int]:
    """Write a Project.bounce ndarray to WAV and return (frames, written channels)."""
    frames = len(cast(Any, audio))
    shape = cast(tuple[int, ...], getattr(audio, "shape", ()))
    if len(shape) >= 2:
        channels = max(1, int(shape[1]))
    else:
        channels = 1
        for row in cast(Any, audio):
            if isinstance(row, Sequence) and not isinstance(row, (str, bytes, bytearray)):
                channels = max(channels, len(row))

    with _atomic_wav_writer(path, channels, sample_rate) as wav:
        pcm = bytearray()
        chunk_frames = 0
        for row in cast(Any, audio):
            if hasattr(row, "__iter__") and not isinstance(row, (str, bytes, bytearray)):
                values = row
            else:
                values = (row,)
            row_values = [float(sample) for sample in values]
            for channel in range(channels):
                pcm.extend(_pcm16(row_values[channel] if channel < len(row_values) else 0.0))
            chunk_frames += 1
            if chunk_frames == _WAV_CHUNK_FRAMES:
                wav.writeframesraw(pcm)
                pcm.clear()
                chunk_frames = 0
        if pcm:
            wav.writeframesraw(pcm)
    return frames, channels


def _atomic_write_bytes(path: str, data: bytes) -> None:
    """Atomically replace ``path`` with ``data`` after a successful flush."""
    target = os.path.abspath(path)
    directory = os.path.dirname(target)
    temporary = ""
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{os.path.basename(target)}.",
            suffix=".tmp",
            dir=directory,
            delete=False,
        ) as fh:
            temporary = fh.name
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(temporary, target)
    except BaseException:
        if temporary:
            with suppress(FileNotFoundError):
                os.unlink(temporary)
        raise


def _read_bounded(path: str, max_bytes: int) -> bytes:
    """Read at most ``max_bytes`` and reject oversized files before facade copies."""
    if max_bytes < 0:
        raise ValueError("max_bytes must be non-negative")
    size = os.stat(path).st_size
    if size > max_bytes:
        raise ValueError(f"input file exceeds {max_bytes} byte limit")
    with open(path, "rb") as fh:
        data = fh.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise ValueError(f"input file exceeds {max_bytes} byte limit")
    return data


def _emit_effect_result(
    args: argparse.Namespace,
    result: list[float],
    sr: int,
    *,
    extra: dict[str, object] | None = None,
    label: str,
    requires_output: bool = True,
) -> int:
    """Write the optional output WAV and print an offline-effect result.

    Shared by the offline-effect subcommands whose result is a mono buffer plus
    an optional ``extra`` payload block. The JSON payload keeps the key order
    ``length, sample_rate, <extra...>, output`` and the human-readable form
    prints ``<label>: <n> samples`` followed by an optional ``Wrote:`` line,
    matching each command's historical output exactly.

    A command that renders audio requires an output destination (``requires_output``),
    so running it without ``-o`` is a parameter error (exit ``EXIT_INVALID_PARAMETER``)
    rather than silently discarding the render, matching the native CLI. Commands
    that double as analysis (e.g. ``trim-silence`` reporting the trimmed length)
    pass ``requires_output=False`` to keep the destination optional.
    """
    if requires_output and not args.output:
        print(f"Error: {label} requires an output file (-o/--output)", file=sys.stderr)
        return 1 if _legacy_exit_codes() else EXIT_INVALID_PARAMETER
    if args.output:
        _write_wav(args.output, result, sr)

    if args.json:
        payload: dict[str, object] = {"length": len(result), "sample_rate": sr}
        if extra:
            payload.update(extra)
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print(f"  {label}: {len(result)} samples")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def _array_stats(
    vals: list[float], *, digits: int = 6, with_count: bool = True
) -> dict[str, float | int]:
    """Summary statistics for a numeric array (avoids dumping huge arrays)."""
    import statistics

    if not vals:
        stats: dict[str, float | int] = {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
        if with_count:
            return {"count": 0, **stats}
        return stats
    stats = {
        "mean": round(statistics.mean(vals), digits),
        "std": round(statistics.pstdev(vals), digits) if len(vals) > 1 else 0.0,
        "min": round(min(vals), digits),
        "max": round(max(vals), digits),
    }
    if with_count:
        return {"count": len(vals), **stats}
    return stats


def _parse_kv_params(value: str) -> dict[str, float]:
    """Parse a ``k=v,k=v`` string into a dict of floats."""
    params: dict[str, float] = {}
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise ValueError(f"invalid param (expected key=value): {item}")
        key, raw = item.split("=", 1)
        params[key.strip()] = float(raw.strip())
    return params


def _load_json_object(path: str) -> dict[str, Any]:
    with open(path, encoding="utf-8") as fh:
        loaded = json.load(fh)
    if not isinstance(loaded, dict):
        raise ValueError("JSON config must be an object")
    return loaded


def _parse_json_config(raw: str, path: str) -> dict[str, Any]:
    if path:
        return _load_json_object(path)
    if not raw:
        return {}
    loaded = json.loads(raw)
    if not isinstance(loaded, dict):
        raise ValueError("--config must be a JSON object")
    return loaded


def _parse_json_list(raw: str, path: str) -> list[dict[str, Any]]:
    if path:
        with open(path, encoding="utf-8") as fh:
            loaded = json.load(fh)
    elif raw:
        loaded = json.loads(raw)
    else:
        return []
    if not isinstance(loaded, list) or not all(isinstance(item, dict) for item in loaded):
        raise ValueError("platforms must be a JSON array of objects")
    return cast(list[dict[str, Any]], loaded)


def _float_sequence(value: object) -> list[float]:
    if hasattr(value, "tolist"):
        value = cast(Any, value).tolist()
    return [float(sample) for sample in cast(Any, value)]


def _load_voice_preset_pack(path: str, preset_id: str) -> dict[str, Any]:
    with open(path, encoding="utf-8") as fh:
        pack = json.load(fh)
    presets = pack.get("presets")
    if not isinstance(presets, list):
        raise ValueError("preset pack must contain a presets array")
    matches = [
        preset for preset in presets if isinstance(preset, dict) and preset.get("id") == preset_id
    ]
    if len(matches) > 1:
        raise ValueError(f"duplicate preset id in preset pack: {preset_id}")
    if not matches:
        raise ValueError(f"preset not found in preset pack: {preset_id}")
    return cast(dict[str, Any], matches[0])


def _parse_voice_set_value(raw: str) -> object:
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return raw


def _set_nested_value(root: dict[str, Any], path: str, value: object) -> None:
    parts = [part for part in path.split(".") if part]
    if not parts:
        raise ValueError("empty --set path")
    cursor = root
    for part in parts[:-1]:
        child = cursor.get(part)
        if not isinstance(child, dict):
            child = {}
            cursor[part] = child
        cursor = child
    cursor[parts[-1]] = value


def _apply_voice_sets(
    preset: str | dict[str, Any], assignments: list[str] | None
) -> str | dict[str, Any]:
    if not assignments:
        return preset
    root = cast(
        dict[str, Any],
        json.loads(preset) if isinstance(preset, str) else json.loads(json.dumps(preset)),
    )
    for group in assignments:
        for assignment in [item for item in group.split(",") if item]:
            if "=" not in assignment:
                raise ValueError(f"invalid --set assignment: {assignment}")
            path, raw = assignment.split("=", 1)
            value = _parse_voice_set_value(raw)
            _set_nested_value(root, path, value)
    return root


def _format_time(seconds: float) -> str:
    """Format seconds as mm:ss."""
    mm = int(seconds) // 60
    ss = int(seconds) % 60
    return f"{mm}:{ss:02d}"


def _parse_pitch_class(value: str) -> PitchClass:
    names = {
        "C": PitchClass.C,
        "C#": PitchClass.CS,
        "DB": PitchClass.CS,
        "D": PitchClass.D,
        "D#": PitchClass.DS,
        "EB": PitchClass.DS,
        "E": PitchClass.E,
        "F": PitchClass.F,
        "F#": PitchClass.FS,
        "GB": PitchClass.FS,
        "G": PitchClass.G,
        "G#": PitchClass.GS,
        "AB": PitchClass.GS,
        "A": PitchClass.A,
        "A#": PitchClass.AS,
        "BB": PitchClass.AS,
        "B": PitchClass.B,
    }
    key = value.upper()
    if key not in names:
        raise ValueError(f"invalid pitch class: {value}")
    return names[key]


def _parse_mode(value: str) -> Mode:
    key = value.lower()
    if key in ("major", "maj"):
        return Mode.MAJOR
    if key in ("minor", "min", "m"):
        return Mode.MINOR
    if key == "dorian":
        return Mode.DORIAN
    if key == "phrygian":
        return Mode.PHRYGIAN
    if key == "lydian":
        return Mode.LYDIAN
    if key == "mixolydian":
        return Mode.MIXOLYDIAN
    if key == "locrian":
        return Mode.LOCRIAN
    raise ValueError(f"invalid mode: {value}")


def _parse_modes(value: str) -> list[Mode]:
    key = value.lower()
    if key in ("major-minor", "majmin", "diatonic"):
        return [Mode.MAJOR, Mode.MINOR]
    if key in ("all", "modal"):
        return [
            Mode.MAJOR,
            Mode.MINOR,
            Mode.DORIAN,
            Mode.PHRYGIAN,
            Mode.LYDIAN,
            Mode.MIXOLYDIAN,
            Mode.LOCRIAN,
        ]
    return [_parse_mode(item.strip()) for item in value.split(",") if item.strip()]


def _parse_key_profile(value: str) -> KeyProfile:
    names = {
        "ks": KeyProfile.KRUMHANSL_SCHMUCKLER,
        "krumhansl": KeyProfile.KRUMHANSL_SCHMUCKLER,
        "krumhansl-schmuckler": KeyProfile.KRUMHANSL_SCHMUCKLER,
        "temperley": KeyProfile.TEMPERLEY,
        "shaath": KeyProfile.SHAATH,
        "keyfinder": KeyProfile.SHAATH,
        "faraldo-edmt": KeyProfile.FARALDO_EDMT,
        "edmt": KeyProfile.FARALDO_EDMT,
        "faraldo-edma": KeyProfile.FARALDO_EDMA,
        "edma": KeyProfile.FARALDO_EDMA,
        "faraldo-edmm": KeyProfile.FARALDO_EDMM,
        "edmm": KeyProfile.FARALDO_EDMM,
        "bellman-budge": KeyProfile.BELLMAN_BUDGE,
        "bellman": KeyProfile.BELLMAN_BUDGE,
        "budge": KeyProfile.BELLMAN_BUDGE,
    }
    key = value.lower()
    if key not in names:
        raise ValueError(f"invalid key profile: {value}")
    return names[key]
