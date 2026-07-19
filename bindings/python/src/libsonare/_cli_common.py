"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import math
import os
from typing import Any, cast

from ._ffi import (
    SONARE_ERROR_DECODE_FAILED,
    SONARE_ERROR_FILE_NOT_FOUND,
    SONARE_ERROR_INVALID_FORMAT,
    SONARE_ERROR_INVALID_PARAMETER,
    SONARE_ERROR_INVALID_STATE,
    SONARE_ERROR_NOT_SUPPORTED,
    SONARE_ERROR_OUT_OF_MEMORY,
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

_SONARE_CODE_TO_EXIT = {
    SONARE_ERROR_INVALID_PARAMETER: EXIT_INVALID_PARAMETER,
    SONARE_ERROR_FILE_NOT_FOUND: EXIT_FILE_NOT_FOUND,
    SONARE_ERROR_INVALID_FORMAT: EXIT_INVALID_FORMAT,
    SONARE_ERROR_DECODE_FAILED: EXIT_DECODE_FAILED,
    SONARE_ERROR_OUT_OF_MEMORY: EXIT_OUT_OF_MEMORY,
    SONARE_ERROR_NOT_SUPPORTED: EXIT_NOT_SUPPORTED,
    SONARE_ERROR_INVALID_STATE: EXIT_INVALID_STATE,
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


def _write_wav(path: str, samples: list[float], sample_rate: int) -> None:
    """Write mono 16-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled by 32767.
    """
    import wave

    frames = bytearray()
    for s in samples:
        frames += _pcm16(s)
    with wave.open(path, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(frames))


def _write_wav_stereo(path: str, left: list[float], right: list[float], sample_rate: int) -> None:
    """Write a stereo 16-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled by 32767.
    """
    import wave

    frames = bytearray()
    count = min(len(left), len(right))
    for i in range(count):
        for s in (left[i], right[i]):
            frames += _pcm16(s)
    with wave.open(path, "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(frames))


def _write_project_bounce_wav(path: str, audio: object, sample_rate: int) -> tuple[int, int]:
    """Write a Project.bounce ndarray to WAV and return (frames, written channels)."""
    import wave

    rows = getattr(audio, "tolist", lambda: audio)()
    if not isinstance(rows, list):
        rows = list(rows)
    frames = len(rows)
    channels = 1
    normalized: list[list[float]] = []
    for row in rows:
        if isinstance(row, list):
            channels = max(channels, len(row))
            normalized.append([float(sample) for sample in row])
        else:
            normalized.append([float(row)])

    pcm = bytearray()
    for row in normalized:
        for ch in range(channels):
            sample = row[ch] if ch < len(row) else 0.0
            pcm += _pcm16(sample)

    with wave.open(path, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(bytes(pcm))
    return frames, channels


def _emit_effect_result(
    args: argparse.Namespace,
    result: list[float],
    sr: int,
    *,
    extra: dict[str, object] | None = None,
    label: str,
) -> int:
    """Write the optional output WAV and print an offline-effect result.

    Shared by the offline-effect subcommands whose result is a mono buffer plus
    an optional ``extra`` payload block. The JSON payload keeps the key order
    ``length, sample_rate, <extra...>, output`` and the human-readable form
    prints ``<label>: <n> samples`` followed by an optional ``Wrote:`` line,
    matching each command's historical output exactly.
    """
    if args.output:
        _write_wav(args.output, result, sr)

    if args.json:
        payload: dict[str, object] = {"length": len(result), "sample_rate": sr}
        if extra:
            payload.update(extra)
        if args.output:
            payload["output"] = args.output
        print(json.dumps(payload))
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
        "std": round(statistics.stdev(vals), digits) if len(vals) > 1 else 0.0,
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


def _apply_voice_macro_override(root: dict[str, Any], path: str, value: object) -> None:
    # Maps the UI macro names (pitch/formant/space/intensity/output) to
    # concrete dsp.* paths so `--set macros.X=...` from the CLI is convenient.
    # CLI-only sugar; the core loader treats `dsp` as authoritative and never
    # derives dsp from macros.
    # Keep the mapping in sync with `apply_voice_macro_override` in
    # tools/sonare_cli.cpp.
    if not isinstance(value, (int, float)):
        return
    if path == "macros.pitch":
        _set_nested_value(root, "dsp.retune.semitones", value)
    elif path == "macros.formant":
        _set_nested_value(root, "dsp.formant.factor", value)
    elif path == "macros.space":
        _set_nested_value(root, "dsp.reverb.mix", value)
    elif path == "macros.intensity":
        _set_nested_value(root, "dsp.compressor.ratio", 1.0 + value * 4.0)
    elif path == "macros.output":
        _set_nested_value(root, "dsp.outputGainDb", value)


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
            _apply_voice_macro_override(root, path, value)
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
    }
    key = value.lower()
    if key not in names:
        raise ValueError(f"invalid key profile: {value}")
    return names[key]
