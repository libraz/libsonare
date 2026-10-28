"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile
from collections.abc import Iterable, Iterator, Sequence
from contextlib import contextmanager, suppress
from typing import Any, cast

import numpy as np

from ._errors import SonareValueError
from ._ffi import (
    SONARE_ERROR_ABI_MISMATCH,
    SONARE_ERROR_CANCELLED,
    SONARE_ERROR_DECODE_FAILED,
    SONARE_ERROR_ENCODE_FAILED,
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
# fold every failure back to 1 for scripts that hardcode the all-failures-are-1 contract.
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
EXIT_ENCODE_FAILED = 12
# Python-only: the native CLI links its library statically and cannot skew.
EXIT_ABI_MISMATCH = 13

# Mirrors kAnalysisSampleRate and kMaxStftNFft: analysis windows are held as
# samples at this rate and rescaled to the input rate.
_ANALYSIS_SAMPLE_RATE = 22050
_MAX_ANALYSIS_WINDOW = 524288


def _window_length_at_rate(samples_at_analysis_rate: int, sample_rate: int) -> int:
    """Rescale a window length in samples at 22050 Hz to ``sample_rate``, rounded, at least 2.

    The native CLI's ``window_length_at_rate`` is the reference: a window held as
    a duration, not as a sample count of whatever rate the file happens to have.
    """
    scaled = samples_at_analysis_rate * sample_rate / _ANALYSIS_SAMPLE_RATE
    if scaled > _MAX_ANALYSIS_WINDOW:
        raise SonareValueError("analysis window exceeds the maximum FFT length at this sample rate")
    return max(2, math.floor(scaled + 0.5))


# Keep the path loader's native file ceiling and read granularity when a
# channel-preserving command takes the buffer decode route below. The native
# reader uses these same values for regular files and streaming inputs.
_MAX_AUDIO_FILE_BYTES = 500 * 1024 * 1024
_AUDIO_READ_CHUNK_BYTES = 64 * 1024

_SONARE_CODE_TO_EXIT = {
    SONARE_ERROR_INVALID_PARAMETER: EXIT_INVALID_PARAMETER,
    SONARE_ERROR_FILE_NOT_FOUND: EXIT_FILE_NOT_FOUND,
    SONARE_ERROR_INVALID_FORMAT: EXIT_INVALID_FORMAT,
    SONARE_ERROR_DECODE_FAILED: EXIT_DECODE_FAILED,
    SONARE_ERROR_OUT_OF_MEMORY: EXIT_OUT_OF_MEMORY,
    SONARE_ERROR_NOT_SUPPORTED: EXIT_NOT_SUPPORTED,
    SONARE_ERROR_INVALID_STATE: EXIT_INVALID_STATE,
    SONARE_ERROR_CANCELLED: EXIT_CANCELLED,
    SONARE_ERROR_ENCODE_FAILED: EXIT_ENCODE_FAILED,
    SONARE_ERROR_ABI_MISMATCH: EXIT_ABI_MISMATCH,
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
    """Whether SONARE_LEGACY_EXIT requests the all-failures-are-1 behaviour."""
    return os.environ.get("SONARE_LEGACY_EXIT") == "1"


def _exit_code_for(exc: BaseException) -> int:
    """Map an exception to a CLI exit code aligned with the C-ABI error codes."""
    if _legacy_exit_codes():
        return 1
    if isinstance(exc, SonareError):
        return _SONARE_CODE_TO_EXIT.get(exc.code, EXIT_ERROR)
    # Ctrl-C is the one cancellation this surface can originate, and it carries
    # the same code the core reports for a cancelled operation.
    if isinstance(exc, KeyboardInterrupt):
        return EXIT_CANCELLED
    if isinstance(exc, FileNotFoundError):
        return EXIT_FILE_NOT_FOUND
    if isinstance(exc, MemoryError):
        return EXIT_OUT_OF_MEMORY
    if isinstance(exc, (ValueError, json.JSONDecodeError)):
        return EXIT_INVALID_PARAMETER
    return EXIT_ERROR


# Which commands this CLI carries and which it deliberately does not is recorded
# per command in tests/conformance/cli_contract_v2.json, where a one-sided one
# states its reason and a checker compares the two front-ends. A hand-kept list
# here would be a second answer to the same question, and the one nothing reads.


def _source_channel_count(path: str) -> int:
    """Probe a file's source channel count, or 0 when it cannot be read.

    Advisory, exactly as the native CLI treats it: the successful decoder stays
    authoritative, so a probe failure on a container an optional external
    decoder handles must not fail a command that decodes it perfectly well.
    """
    from .audio import Audio

    try:
        return int(Audio.file_channel_count(path))
    except Exception:
        return 0


def _load_audio(path: str) -> tuple[list[float], int]:
    """Load audio from file via the Audio class, downmixed to one channel.

    ``Audio.from_file`` always returns a mono signal: stereo (and higher
    channel-count) inputs are downmixed to a single channel on load. A caller
    that must preserve the source channels uses :func:`_load_audio_channels`
    instead; this one warns when it drops any, so a command that is mono by
    nature still says what it did to the input.

    The warning is the native CLI's, in its wording: both front-ends downmix on
    the same commands, and a warning on only one of them is a difference a user
    would read as a difference in what was processed.
    """
    channels = _source_channel_count(path)
    if channels > 1:
        print(
            f"warning: {channels}-channel input is downmixed to mono by this CLI command; "
            "use the stereo library API for channel-preserving processing",
            file=sys.stderr,
        )
    return _load_audio_known_channels(path, channels, emit_warning=False)


def _load_audio_channels(path: str) -> tuple[list[list[float]], int]:
    """Load every source channel of a file, in source order.

    Returns one buffer per channel rather than a downmix, for the commands that
    write audio back out and would otherwise deliver a mono file for a stereo
    input. A mono source takes the single-decode path, so nothing pays for this
    that does not have channels to keep.

    Raises:
        SonareError: As :meth:`Audio.from_file` does.
    """
    channels = _source_channel_count(path)
    return _load_audio_channels_known(path, channels)


def _load_audio_known_channels(
    path: str, channels: int, *, emit_warning: bool = False
) -> tuple[list[float], int]:
    """Load mono audio given the source channel count the caller already probed.

    ``channels`` is used only for the downmix warning, so the probe runs once per
    command rather than once per loader.
    """
    from .audio import Audio

    if emit_warning and channels > 1:
        print(
            f"warning: {channels}-channel input is downmixed to mono by this CLI command; "
            "use the stereo library API for channel-preserving processing",
            file=sys.stderr,
        )
    with Audio.from_file(path) as audio:
        return audio.data, audio.sample_rate


def _read_audio_bounded(path: str) -> bytes:
    """Read an audio path in bounded chunks with the native 500 MiB ceiling.

    The path probe remains advisory and has already happened before this helper
    is called. A second path read is therefore deliberate: it is the one input
    handed to :func:`decode_channels`, and its chunked ceiling also applies to
    FIFOs and other streams whose size is unknown before reading.
    """
    try:
        file_size = os.stat(path).st_size
    except OSError:
        # Opening below gives the same file-not-found/read classification as the
        # native path loader for a path whose metadata cannot be inspected.
        file_size = 0
    if file_size > _MAX_AUDIO_FILE_BYTES:
        raise SonareValueError(
            f"File too large: {file_size} bytes (max: {_MAX_AUDIO_FILE_BYTES} bytes)"
        )

    try:
        stream = open(path, "rb")  # noqa: SIM115 - classify the open before entering context
    except IsADirectoryError:
        # On POSIX, Python reports a directory at open time while the native
        # ifstream path can open it and hand an empty stream to the decoder.
        # Preserve that decoder-specific classification instead of guessing an
        # open or read error here.
        return b""
    except OSError as exc:
        # Native read_file() maps every failure to open the path, including
        # PermissionError and other open-time OSError subclasses, to
        # FileNotFound/Cannot open file. Only errors after this point are read
        # failures and retain DecodeFailed/Failed to read file.
        raise SonareError(SONARE_ERROR_FILE_NOT_FOUND, f"Cannot open file: {path}") from exc

    data = bytearray()
    try:
        with stream:
            while True:
                try:
                    chunk = stream.read(_AUDIO_READ_CHUNK_BYTES)
                except OSError as exc:
                    raise SonareError(
                        SONARE_ERROR_DECODE_FAILED, f"Failed to read file: {path}"
                    ) from exc
                if not chunk:
                    break
                if len(chunk) > _MAX_AUDIO_FILE_BYTES - len(data):
                    raise SonareValueError(f"File too large: exceeds {_MAX_AUDIO_FILE_BYTES} bytes")
                data.extend(chunk)
    except SonareError:
        raise
    except OSError as exc:
        # A stream read or close can fail outside the direct read() call. It is
        # still a post-open failure and therefore has native read semantics.
        raise SonareError(SONARE_ERROR_DECODE_FAILED, f"Failed to read file: {path}") from exc
    return bytes(data)


def _load_audio_channels_known(path: str, channels: int) -> tuple[list[list[float]], int]:
    """Load channel planes after a probe, decoding a multi-channel source once."""
    if channels <= 1:
        samples, sample_rate = _load_audio_known_channels(path, channels, emit_warning=False)
        return [samples], sample_rate

    from .audio import decode_channels

    decoded, sample_rate = decode_channels(_read_audio_bounded(path))
    # Each row is a contiguous NumPy view into ``decoded``. Keeping the parent
    # array alive through the row views makes the planes independently mutable
    # without aliasing one another or copying every channel a second time.
    rows = [decoded[index, :] for index in range(int(decoded.shape[0]))]
    return cast(list[list[float]], rows), sample_rate


def _load_audio_from_facade(path: str) -> tuple[list[float], int]:
    """Load mono audio through ``cli._load_audio``, the facade the command modules share."""
    from . import cli

    return cli._load_audio(path)


def _resample(
    samples: Sequence[float] | np.ndarray, source_rate: int, target_rate: int
) -> np.ndarray:
    """Resample mono samples with the native anti-aliased resampler.

    Routes through the C-ABI ``resample`` (r8brain) so the CLI matches the C++
    CLI and ``Audio.resample()`` numerically. Falls back to linear interpolation
    only when the native shared library cannot be loaded, keeping the CLI usable
    in a library-less environment.
    """
    if source_rate <= 0 or target_rate <= 0:
        raise ValueError("sample rates must be positive")
    buffer = _audio_array(samples)
    if source_rate == target_rate:
        return buffer.copy()
    if buffer.size == 0:
        return buffer

    from ._conversions import _resample_float32

    try:
        return _resample_float32(buffer, source_rate, target_rate)
    except OSError:
        # Shared library missing/unloadable: degrade to linear interpolation.
        # Interpolated in double, as the list-based fallback always was.
        return _audio_array(_resample_linear(buffer.astype(np.float64), source_rate, target_rate))


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


def _to_float32(value: float) -> float:
    """Round a Python float to the nearest ``float`` the C++ writer would hold.

    The native quantizer takes a 32-bit sample and scales it in 32-bit
    arithmetic, so a mirror computing in Python's float64 lands on a different
    integer for some inputs even though both round the same way. A magnitude no
    32-bit float can hold becomes an infinity, as the narrowing conversion does,
    rather than an error.
    """
    import struct

    # struct.unpack is typed as tuple[Any, ...], so the element is narrowed here
    # rather than left to leak an untyped value out of a float-returning helper.
    try:
        return float(struct.unpack("<f", struct.pack("<f", value))[0])
    except OverflowError:
        return math.inf if value > 0.0 else -math.inf


# TPDF dither recipe of the native WAV writer (src/core/audio_io.cpp: kPcmDitherSeed,
# pcm_tpdf_lsb, quantize_pcm, on_pcm_grid) over SplitMix64 (src/util/splitmix64.h).
_MASK64 = (1 << 64) - 1
_PCM_DITHER_SEED = 0x50434D4449544852
_SPLITMIX64_GAMMA = 0x9E3779B97F4A7C15
_SPLITMIX64_MUL1 = 0xBF58476D1CE4E5B9
_SPLITMIX64_MUL2 = 0x94D049BB133111EB
_TPDF_UNIT = 1.0 / 16777216.0


def _splitmix64_at(seed: int, index: int) -> int:
    """Draw ``index`` of the SplitMix64 stream seeded with ``seed``."""
    z = (seed + (index + 1) * _SPLITMIX64_GAMMA) & _MASK64
    z = ((z ^ (z >> 30)) * _SPLITMIX64_MUL1) & _MASK64
    z = ((z ^ (z >> 27)) * _SPLITMIX64_MUL2) & _MASK64
    return z ^ (z >> 31)


def _channel_dither_seed(channel: int) -> int:
    return _PCM_DITHER_SEED ^ ((channel * _SPLITMIX64_GAMMA) & _MASK64)


def _tpdf_lsb(channel: int, frame: int) -> float:
    """TPDF dither in LSBs for one sample; exact in float32, like ``pcm_tpdf_lsb``."""
    seed = _channel_dither_seed(channel)
    a = (_splitmix64_at(seed, 2 * frame) >> 40) * _TPDF_UNIT
    b = (_splitmix64_at(seed, 2 * frame + 1) >> 40) * _TPDF_UNIT
    return a - b


def _splitmix64_at_array(seed: np.ndarray, index: np.ndarray) -> np.ndarray:
    """Vectorized :func:`_splitmix64_at`; uint64 arithmetic wraps modulo 2**64."""
    u64 = np.uint64
    z = seed + (index + u64(1)) * u64(_SPLITMIX64_GAMMA)
    z = (z ^ (z >> u64(30))) * u64(_SPLITMIX64_MUL1)
    z = (z ^ (z >> u64(27))) * u64(_SPLITMIX64_MUL2)
    return z ^ (z >> u64(31))


def _tpdf_lsb_array(count: int, channels: int, first_frame: int) -> np.ndarray:
    """Vectorized :func:`_tpdf_lsb` for ``count`` interleaved samples as float32."""
    u64 = np.uint64
    position = np.arange(count, dtype=np.uint64)
    frame = position // u64(channels) + u64(first_frame)
    seed = u64(_PCM_DITHER_SEED) ^ ((position % u64(channels)) * u64(_SPLITMIX64_GAMMA))
    unit = np.float32(_TPDF_UNIT)
    a = (_splitmix64_at_array(seed, frame * u64(2)) >> u64(40)).astype(np.float32) * unit
    b = (_splitmix64_at_array(seed, frame * u64(2) + u64(1)) >> u64(40)).astype(np.float32) * unit
    return np.asarray(a - b, dtype=np.float32)


def _quantize_sample(
    sample: float, full_scale: float, minimum: int, maximum: int, tpdf: float = 0.0
) -> int:
    """Scale a float sample to an integer PCM code.

    Reproduces ``quantize_pcm`` (audio_io.cpp) step for step: narrow to 32-bit,
    write a non-finite sample as digital silence, clamp to ``[-1, 1]``, scale in
    32-bit by ``2**(bits - 1)`` (the grid the decoder divides by), add ``tpdf``
    LSBs of dither in 32-bit, round half away from zero rather than to even, then
    clamp the code to the container. An exact zero stays code 0, undithered.
    The guard precedes the clamp
    because a comparison against a non-finite value is false: a clamp alone lets
    NaN and +Inf through as positive full scale, a peak the encoder never
    produced. Narrowing and the rounding rule are both load-bearing -- either
    one alone still disagrees with the native writer on some samples.

    The core threads a substitution count out of ``save_wav``; neither CLI asks
    for it, so both substitute without reporting.
    """
    narrowed = _to_float32(sample)
    if not math.isfinite(narrowed) or narrowed == 0.0:
        return 0
    clamped = max(-1.0, min(1.0, narrowed))
    scaled = _to_float32(_to_float32(clamped * full_scale) + tpdf)
    rounded = math.floor(scaled + 0.5) if scaled >= 0.0 else math.ceil(scaled - 0.5)
    return max(minimum, min(maximum, int(rounded)))


def _pcm16(sample: float, tpdf: float = 0.0) -> bytes:
    """Clamp a float to ``[-1.0, 1.0]`` and pack it as little-endian 16-bit PCM.

    Shared by every WAV writer so the clamp-and-scale contract stays identical.
    A non-finite sample is written as digital silence; see ``_quantize_sample``.
    """
    import struct

    return struct.pack("<h", _quantize_sample(sample, 32768.0, -32768, 32767, tpdf))


def _pcm24(sample: float, tpdf: float = 0.0) -> bytes:
    """Clamp a float and pack it as little-endian 24-bit PCM."""
    value = _quantize_sample(sample, 8388608.0, -8388608, 8388607, tpdf)
    return value.to_bytes(3, byteorder="little", signed=True)


def _pcm(sample: float, bits_per_sample: int, tpdf: float = 0.0) -> bytes:
    if bits_per_sample == 16:
        return _pcm16(sample, tpdf)
    if bits_per_sample == 24:
        return _pcm24(sample, tpdf)
    raise ValueError("WAV bits must be 16 or 24")


def _quantize_codes(
    samples: np.ndarray,
    full_scale: float,
    minimum: int,
    maximum: int,
    tpdf: np.ndarray | None = None,
) -> np.ndarray:
    """Vectorized :func:`_quantize_sample`, one PCM code per input sample.

    Reproduces the scalar function step for step and in the same precision at
    each step (float32 narrow, double-precision scale, float32 re-narrow, float32
    dither add, then integer round and clamp): a shortcut at any one of those
    steps has disagreed with the native writer on some sample before (see
    ``_quantize_sample``'s own docstring). ``tests/test_cli_writers.py``
    checks this against the scalar path over a large random-plus-edge-case
    sample set, not just the handful of codes the WAV-writer tests pin.
    """
    # A magnitude no float32 can hold overflows to +/-inf on this narrowing
    # cast (matching _to_float32's OverflowError catch below), which numpy
    # warns about even though the resulting inf is exactly what the isfinite
    # guard two lines down exists to substitute -- not a sign anything here
    # went wrong.
    with np.errstate(over="ignore"):
        narrowed = np.asarray(samples, dtype=np.float32)
    finite = np.isfinite(narrowed)
    # Substituted before the pipeline runs, not after: a NaN/inf surviving into
    # the round-and-cast below is what the scalar function's isfinite guard
    # exists to prevent reaching in the first place (an unguarded clamp would
    # answer +/-full_scale, a peak the encoder never produced), and it is also
    # what int64 casts a RuntimeWarning about for no benefit here, since the
    # position is discarded either way.
    safe = np.where(finite, narrowed, 0.0).astype(np.float32)
    clamped = np.clip(safe, -1.0, 1.0)
    scaled32 = (clamped.astype(np.float64) * full_scale).astype(np.float32)
    if tpdf is not None:
        scaled32 = scaled32 + np.asarray(tpdf, dtype=np.float32)
    scaled = scaled32.astype(np.float64)
    rounded = np.where(scaled >= 0.0, np.floor(scaled + 0.5), np.ceil(scaled - 0.5))
    codes = np.clip(rounded, minimum, maximum).astype(np.int64)
    # An exact zero stays digital silence, undithered, as in quantize_pcm.
    return np.where(finite & (narrowed != 0.0), codes, 0)


def _on_pcm_grid(samples: Any, bits_per_sample: int) -> bool:
    """True when every finite sample already sits on the PCM grid (``on_pcm_grid``)."""
    scale = 32768.0 if bits_per_sample == 16 else 8388608.0
    with np.errstate(over="ignore"):
        narrowed = np.asarray(samples, dtype=np.float32)
    finite = narrowed[np.isfinite(narrowed)]
    scaled = np.clip(finite, -1.0, 1.0).astype(np.float64) * scale
    return bool(np.all(scaled == np.floor(scaled)))


class _PcmQuantizer:
    """Quantizes and packs one WAV stream's interleaved samples, dither included.

    The native writer dithers a stream with TPDF noise unless every finite sample
    is already on the PCM grid (audio_io.cpp ``pack_pcm_bytes``). The grid
    decision covers the whole stream, so it is made once, before the first chunk
    is written (:meth:`for_stream`); the noise is a pure function of (channel,
    frame), so each chunk carries its absolute first frame.
    """

    def __init__(self, bits_per_sample: int, channels: int = 1, dither: bool = False) -> None:
        if bits_per_sample not in (16, 24):
            raise ValueError("WAV bits must be 16 or 24")
        self.bits_per_sample = bits_per_sample
        self.channels = channels
        self.dither = dither
        self._scale, self._minimum, self._maximum = (
            (32768.0, -32768, 32767) if bits_per_sample == 16 else (8388608.0, -8388608, 8388607)
        )

    @classmethod
    def for_stream(
        cls, bits_per_sample: int, channels: int, chunks: Iterable[Any]
    ) -> _PcmQuantizer:
        """Decide dithering from every sample of the stream about to be written."""
        if bits_per_sample not in (16, 24):
            raise ValueError("WAV bits must be 16 or 24")
        on_grid = all(_on_pcm_grid(chunk, bits_per_sample) for chunk in chunks)
        return cls(bits_per_sample, channels, dither=not on_grid)

    def codes(self, samples: Any, first_frame: int = 0) -> np.ndarray:
        """PCM codes for interleaved ``samples`` whose first frame is ``first_frame``."""
        flat = np.asarray(samples)
        tpdf = (
            _tpdf_lsb_array(flat.size, self.channels, first_frame).reshape(flat.shape)
            if self.dither
            else None
        )
        return _quantize_codes(flat, self._scale, self._minimum, self._maximum, tpdf)

    def pack(self, samples: Any, first_frame: int = 0) -> bytes:
        """Vectorized packed little-endian bytes for a chunk of interleaved samples.

        16-bit output is a straight int16 pack. 24-bit has no native numpy
        integer width, so the codes are widened to int32, packed little-endian,
        and every 4th byte (the sign-extension byte) is dropped -- the same low 3
        bytes ``int.to_bytes(3, "little", signed=True)`` would have produced.
        """
        codes = self.codes(samples, first_frame)
        if self.bits_per_sample == 16:
            return bytes(codes.astype("<i2").tobytes())
        widened = codes.astype("<i4").view(np.uint8).reshape(-1, 4)
        return bytes(widened[:, :3].tobytes())

    def pack_scalar(self, sample: float, channel: int, frame: int) -> bytes:
        """Scalar sibling of :meth:`pack` for one sample."""
        tpdf = _tpdf_lsb(channel, frame) if self.dither else 0.0
        return _pcm(sample, self.bits_per_sample, tpdf)


_WAV_CHUNK_FRAMES = 8192

# UINT32_MAX minus the WAVE_FORMAT_EXTENSIBLE header's extra bytes over the
# plain-PCM header (68 - 8), matching src/core/audio_io.cpp's
# check_riff_size_fits exactly -- one RIFF chunk cannot declare a size past
# 2**32 - 1, and both writers share that bound regardless of which header they
# end up writing.
_MAX_WAV_DATA_BYTES = 0xFFFFFFFF - 60


def _check_wav_frame_count(path: str, frames: int, channels: int, bits_per_sample: int) -> None:
    """Refuse an empty or RIFF-size-exceeding WAV write before any bytes move.

    Mirrors the native writer's two checks (save_wav / save_wav_multichannel's
    ``n_frames > 0`` and ``check_riff_size_fits``) so both CLIs reach the same
    verdict, with the same error class, before either touches the filesystem.
    """
    if frames <= 0:
        raise SonareValueError(f"No frames to save: {path}")
    block_align = channels * (bits_per_sample // 8)
    if block_align <= 0 or frames > _MAX_WAV_DATA_BYTES // block_align:
        raise SonareValueError(
            f"WAV data exceeds RIFF 32-bit size limit; use a chunked container: {path}"
        )


@contextmanager
def _atomic_wav_writer(
    path: str, channels: int, sample_rate: int, bits_per_sample: int = 16, *, frames: int
) -> Iterator[Any]:
    """Yield a WAV writer whose completed file atomically replaces ``path``.

    ``frames`` is the total frame count the caller intends to write, checked
    up front by @ref _check_wav_frame_count -- before the temp file even
    exists, so a refused request touches neither it nor ``path``.
    """
    import wave

    if bits_per_sample not in (16, 24):
        raise ValueError("WAV bits must be 16 or 24")
    _check_wav_frame_count(path, frames, channels, bits_per_sample)
    target = os.path.abspath(path)
    directory = os.path.dirname(target)
    # Creating the scratch file, writing it, and replacing the destination with
    # it are all stages of producing the output, so an OSError from any of them
    # carries one class. The commonest instance is a `-o` that resolves to a
    # directory, which nothing rejects until the atomic replace; escaping as a
    # bare OSError it landed on the generic error code, while the native CLI
    # reports the same condition as ErrorCode::EncodeFailed. The yielded block is
    # covered too: the only file it touches is this one, so an OSError raised
    # there is a failed write to the output rather than an unrelated caller
    # error, which is why the conversion is by exception type and not by scope.
    try:
        raw = tempfile.NamedTemporaryFile(  # noqa: SIM115 - lifetime spans the yielded writer
            mode="w+b",
            prefix=f".{os.path.basename(target)}.",
            suffix=".tmp",
            dir=directory,
            delete=False,
        )
    except OSError as exc:
        raise SonareError(SONARE_ERROR_ENCODE_FAILED, f"cannot write {path}: {exc}") from exc
    temporary = raw.name
    wav = None
    try:
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
        except OSError as exc:
            raise SonareError(SONARE_ERROR_ENCODE_FAILED, f"cannot write {path}: {exc}") from exc
    except BaseException:
        if wav is not None:
            with suppress(Exception):
                wav.close()
        with suppress(Exception):
            raw.close()
        with suppress(FileNotFoundError):
            os.unlink(temporary)
        raise


# WAVE_FORMAT_EXTENSIBLE, and the KSDATAFORMAT_SUBTYPE_PCM subformat GUID
# {00000001-0000-0010-8000-00AA00389B71} stored as little-endian Data1/2/3
# followed by the 8 Data4 bytes verbatim -- the same layout the native CLI's
# `save_wav_multichannel` writes.
_WAVE_FORMAT_EXTENSIBLE = 0xFFFE
_PCM_SUBFORMAT_GUID = bytes(
    [0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71]
)
# dwChannelMask by channel count, matching src/core/channel_layout.h's
# wave_channel_mask table (mono carries no meaningful mask).
_CHANNEL_MASKS = {1: 0x0, 2: 0x3, 6: 0x3F, 8: 0x63F}
# RIFF/WAVE(12) + fmt chunk id/size/body(8 + 40) + data chunk id/size(8).
_EXTENSIBLE_HEADER_SIZE = 68


class _ExtensibleWavWriter:
    """Streams raw PCM into an open WAVE_FORMAT_EXTENSIBLE file; header patched on close.

    Exposes the same ``writeframesraw`` surface stdlib ``wave.Wave_write``
    does, so a caller already chunking through ``_atomic_wav_writer`` needs no
    second code path for a >2-channel target.
    """

    def __init__(self, handle: Any, channels: int, sample_rate: int, bits_per_sample: int) -> None:
        self._handle = handle
        self._channels = channels
        self._sample_rate = sample_rate
        self._bits_per_sample = bits_per_sample
        self._data_size = 0

    def writeframesraw(self, data: bytes | bytearray) -> None:
        self._handle.write(bytes(data))
        self._data_size += len(data)

    def write_header(self) -> None:
        """Patch the RIFF/fmt/data header now that ``_data_size`` is known."""
        import struct

        block_align = self._channels * (self._bits_per_sample // 8)
        byte_rate = self._sample_rate * block_align
        fmt_size = 40
        riff_size = 4 + (8 + fmt_size) + (8 + self._data_size)
        channel_mask = _CHANNEL_MASKS.get(self._channels, 0)
        header = bytearray()
        header += b"RIFF" + struct.pack("<I", riff_size) + b"WAVE"
        header += b"fmt " + struct.pack("<I", fmt_size)
        header += struct.pack(
            "<HHIIHHHHI",
            _WAVE_FORMAT_EXTENSIBLE,
            self._channels,
            self._sample_rate,
            byte_rate,
            block_align,
            self._bits_per_sample,
            22,  # cbSize
            self._bits_per_sample,  # wValidBitsPerSample
            channel_mask,
        )
        header += _PCM_SUBFORMAT_GUID
        header += b"data" + struct.pack("<I", self._data_size)
        assert len(header) == _EXTENSIBLE_HEADER_SIZE
        self._handle.seek(0)
        self._handle.write(bytes(header))


@contextmanager
def _atomic_wav_writer_multichannel(
    path: str, channels: int, sample_rate: int, bits_per_sample: int = 16, *, frames: int
) -> Iterator[_ExtensibleWavWriter]:
    """Yield a WAVE_FORMAT_EXTENSIBLE writer whose completed file atomically replaces ``path``.

    Stdlib ``wave`` always emits plain WAVE_FORMAT_PCM, which carries no
    channel mask a 5.1/7.1 player needs to place its planes correctly, so a
    width beyond stereo is hand-written here to match the native CLI's own
    ``save_wav_multichannel`` layout byte for byte. Mono and stereo stay on
    ``_atomic_wav_writer``, which already writes the right (plain-PCM) format
    for those widths.

    ``frames`` is checked up front the same way ``_atomic_wav_writer`` checks
    it; see @ref _check_wav_frame_count.
    """
    if bits_per_sample not in (16, 24):
        raise ValueError("WAV bits must be 16 or 24")
    _check_wav_frame_count(path, frames, channels, bits_per_sample)
    target = os.path.abspath(path)
    directory = os.path.dirname(target)
    try:
        raw = tempfile.NamedTemporaryFile(  # noqa: SIM115 - lifetime spans the yielded writer
            mode="w+b",
            prefix=f".{os.path.basename(target)}.",
            suffix=".tmp",
            dir=directory,
            delete=False,
        )
    except OSError as exc:
        raise SonareError(SONARE_ERROR_ENCODE_FAILED, f"cannot write {path}: {exc}") from exc
    temporary = raw.name
    writer = _ExtensibleWavWriter(raw, channels, sample_rate, bits_per_sample)
    try:
        try:
            raw.seek(_EXTENSIBLE_HEADER_SIZE)
            yield writer
            writer.write_header()
            raw.flush()
            os.fsync(raw.fileno())
            raw.close()
            os.replace(temporary, target)
        except OSError as exc:
            raise SonareError(SONARE_ERROR_ENCODE_FAILED, f"cannot write {path}: {exc}") from exc
    except BaseException:
        with suppress(Exception):
            raw.close()
        with suppress(FileNotFoundError):
            os.unlink(temporary)
        raise


def _write_wav_mono_frames(
    wav: Any, samples: Sequence[float], quantizer: _PcmQuantizer, first_frame: int = 0
) -> None:
    """Append one bounded mono PCM chunk starting at absolute frame ``first_frame``."""
    wav.writeframesraw(quantizer.pack(np.asarray(samples, dtype=np.float64), first_frame))


def _write_wav_stereo_frames(
    wav: Any,
    left: Sequence[float],
    right: Sequence[float],
    quantizer: _PcmQuantizer,
    first_frame: int = 0,
) -> None:
    """Append one bounded stereo PCM chunk starting at absolute frame ``first_frame``."""
    count = min(len(left), len(right))
    # Interleaved the same way _interleave_planes does (frame-major), so one
    # pack call quantizes and packs both channels' samples together.
    interleaved = np.empty(count * 2, dtype=np.float64)
    interleaved[0::2] = np.asarray(left[:count], dtype=np.float64)
    interleaved[1::2] = np.asarray(right[:count], dtype=np.float64)
    wav.writeframesraw(quantizer.pack(interleaved, first_frame))


def _write_wav(
    path: str, samples: Sequence[float] | np.ndarray, sample_rate: int, bits_per_sample: int = 16
) -> None:
    """Write mono 16- or 24-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled to the selected PCM range;
    a non-finite sample becomes digital silence. Input off the PCM grid is TPDF
    dithered exactly as the native writer does (``pack_pcm_bytes`` in
    src/core/audio_io.cpp).
    """
    with _atomic_wav_writer(path, 1, sample_rate, bits_per_sample, frames=len(samples)) as wav:
        starts = range(0, len(samples), _WAV_CHUNK_FRAMES)
        quantizer = _PcmQuantizer.for_stream(
            bits_per_sample, 1, (samples[o : o + _WAV_CHUNK_FRAMES] for o in starts)
        )
        for offset in starts:
            _write_wav_mono_frames(
                wav, samples[offset : offset + _WAV_CHUNK_FRAMES], quantizer, offset
            )


def _write_wav_stereo(
    path: str,
    left: Sequence[float] | np.ndarray,
    right: Sequence[float] | np.ndarray,
    sample_rate: int,
    bits_per_sample: int = 16,
) -> None:
    """Write a stereo 16- or 24-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled to the selected PCM range;
    a non-finite sample becomes digital silence. Input off the PCM grid is TPDF
    dithered exactly as the native writer does (``pack_pcm_bytes`` in
    src/core/audio_io.cpp).
    """
    count = min(len(left), len(right))
    with _atomic_wav_writer(path, 2, sample_rate, bits_per_sample, frames=count) as wav:
        starts = range(0, count, _WAV_CHUNK_FRAMES)

        def planes() -> Iterator[Any]:
            for offset in starts:
                end = min(offset + _WAV_CHUNK_FRAMES, count)
                yield left[offset:end]
                yield right[offset:end]

        quantizer = _PcmQuantizer.for_stream(bits_per_sample, 2, planes())
        for offset in starts:
            end = min(offset + _WAV_CHUNK_FRAMES, count)
            _write_wav_stereo_frames(wav, left[offset:end], right[offset:end], quantizer, offset)


def _load_channels_or_downmix(path: str) -> tuple[list[list[float]], int]:
    """Load a file as a stereo pair where it is one, and as mono otherwise.

    The library's offline operations come in a mono and a stereo form and
    nothing wider, so a two-channel source is the only one that can be carried
    through whole. Anything else goes through the downmixing loader, which says
    so: silently keeping channel 0 of a surround file would deliver a quarter of
    the record under the name of the whole.

    The channel count is probed once and handed to the decode, so the file is not
    probed again for the downmix warning.
    """
    channels = _source_channel_count(path)
    if channels == 2:
        return _load_audio_channels_known(path, channels)
    samples, sample_rate = _load_audio_known_channels(path, channels, emit_warning=True)
    return [samples], sample_rate


def _write_channel_output(
    path: str,
    channels: Sequence[Sequence[float] | np.ndarray],
    sample_rate: int,
    bits_per_sample: int = 16,
) -> None:
    """Write however many channels an operation produced."""
    if len(channels) == 2:
        _write_wav_stereo(path, channels[0], channels[1], sample_rate, bits_per_sample)
    else:
        _write_wav(path, channels[0], sample_rate, bits_per_sample)


def _write_frame_major_wav(
    path: str, audio: object, sample_rate: int, bits_per_sample: int = 16
) -> tuple[int, int]:
    """Write frame-major PCM (row i holds frame i's per-channel values) to WAV.

    Channel width comes from ``audio`` itself: ``shape[1]`` for an ndarray, or
    the widest row otherwise. A width beyond stereo goes through
    ``_atomic_wav_writer_multichannel`` (WAVE_FORMAT_EXTENSIBLE, matching the
    native CLI's own writer); mono and stereo stay on the plain-PCM
    ``_atomic_wav_writer``. Returns (frames, channels written).
    """
    frames = len(cast(Any, audio))
    shape = cast(tuple[int, ...], getattr(audio, "shape", ()))
    if len(shape) >= 2:
        channels = max(1, int(shape[1]))
    else:
        channels = 1
        for row in cast(Any, audio):
            if isinstance(row, Sequence) and not isinstance(row, (str, bytes, bytearray)):
                channels = max(channels, len(row))

    writer_cm = (
        _atomic_wav_writer_multichannel(path, channels, sample_rate, bits_per_sample, frames=frames)
        if channels > 2
        else _atomic_wav_writer(path, channels, sample_rate, bits_per_sample, frames=frames)
    )
    with writer_cm as wav:
        # The fast path: `audio` is already a (frames, channels) ndarray with
        # no ragged/padded rows to fold in -- true for every real caller
        # (Project.bounce, PlaybackRenderer's reshaped render). Quantizing and
        # packing a whole chunk through numpy replaces one Python function call
        # and one bytearray.extend per SAMPLE with one call per CHUNK, which is
        # the difference between this finishing and running out of memory or
        # time on a movie-length multichannel file.
        if isinstance(audio, np.ndarray) and audio.ndim == 2 and audio.shape[1] == channels:
            starts = range(0, frames, _WAV_CHUNK_FRAMES)
            quantizer = _PcmQuantizer.for_stream(
                bits_per_sample, channels, (audio[o : o + _WAV_CHUNK_FRAMES] for o in starts)
            )
            for offset in starts:
                chunk = audio[offset : offset + _WAV_CHUNK_FRAMES]
                wav.writeframesraw(quantizer.pack(chunk.astype(np.float64).ravel(), offset))
            return frames, channels
        # The general duck-typed fallback: possibly-ragged rows, padded with
        # silence past a short row -- the shape a fast-path ndarray never has.
        quantizer = _PcmQuantizer.for_stream(
            bits_per_sample, channels, (values for _, values in _padded_chunks(audio, channels))
        )
        for first_frame, values in _padded_chunks(audio, channels):
            pcm = bytearray()
            for position, value in enumerate(values):
                pcm.extend(
                    quantizer.pack_scalar(
                        value, position % channels, first_frame + position // channels
                    )
                )
            wav.writeframesraw(pcm)
    return frames, channels


def _padded_chunks(audio: object, channels: int) -> Iterator[tuple[int, list[float]]]:
    """Yield (first frame, interleaved values) per chunk, short rows padded with silence."""
    values: list[float] = []
    first_frame = 0
    for frame, row in enumerate(cast(Any, audio), start=1):
        if hasattr(row, "__iter__") and not isinstance(row, (str, bytes, bytearray)):
            row_values = [float(sample) for sample in row]
        else:
            row_values = [float(row)]
        for channel in range(channels):
            values.append(row_values[channel] if channel < len(row_values) else 0.0)
        if frame - first_frame == _WAV_CHUNK_FRAMES:
            yield first_frame, values
            values = []
            first_frame = frame
    if values:
        yield first_frame, values


def _write_project_bounce_wav(path: str, audio: object, sample_rate: int) -> tuple[int, int]:
    """Write a Project.bounce ndarray to WAV and return (frames, written channels)."""
    return _write_frame_major_wav(path, audio, sample_rate)


def _atomic_write_bytes(path: str, data: bytes) -> None:
    """Atomically replace ``path`` with ``data`` after a successful flush."""
    target = os.path.abspath(path)
    directory = os.path.dirname(target)
    temporary = ""
    try:
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
        except OSError as exc:
            # Same classification the WAV writer applies: every stage here is
            # part of producing the output, and the raw errno names the scratch
            # file rather than the destination the caller asked for.
            raise SonareError(SONARE_ERROR_ENCODE_FAILED, f"cannot write {path}: {exc}") from exc
    except BaseException:
        if temporary:
            with suppress(FileNotFoundError):
                os.unlink(temporary)
        raise


# The largest project or MIDI document this CLI reads. It lives beside the
# bounded reader rather than in one command module, because both the project
# commands and the reference-melody read go through it.
_MAX_PROJECT_OR_MIDI_BYTES = 64 * 1024 * 1024


def _read_bounded(path: str, max_bytes: int) -> bytes:
    """Read at most ``max_bytes`` and reject oversized files before facade copies."""
    if max_bytes < 0:
        raise ValueError("max_bytes must be non-negative")
    # A missing path keeps the FileNotFoundError the file-not-found exit maps.
    # Anything else -- a directory, an unreadable file -- is a path that exists
    # but holds nothing this reader can accept, which is the class the native
    # CLI reports for it after its own read returns no parseable bytes.
    try:
        size = os.stat(path).st_size
        if size > max_bytes:
            raise ValueError(f"input file exceeds {max_bytes} byte limit")
        with open(path, "rb") as fh:
            data = fh.read(max_bytes + 1)
    except FileNotFoundError:
        raise
    except OSError as exc:
        raise SonareError(SONARE_ERROR_INVALID_FORMAT, f"cannot read {path}: {exc}") from exc
    if len(data) > max_bytes:
        raise ValueError(f"input file exceeds {max_bytes} byte limit")
    return data


def _emit_effect_result(
    args: argparse.Namespace,
    channels: Sequence[Sequence[float] | np.ndarray],
    sr: int,
    *,
    extra: dict[str, object] | None = None,
    label: str,
    requires_output: bool = True,
) -> int:
    """Write the optional output WAV and print an offline-effect result.

    Shared by the offline-effect subcommands. ``channels`` is one buffer for a
    mono result and two for a stereo pair, the same shape ``_write_chain_output``
    takes: a command that carries a stereo source through has a pair to write,
    and giving it its own emitter would put two spellings on one payload. The
    JSON payload keeps the key order ``length, sample_rate, duration,
    <extra...>, output`` and the human-readable form prints ``<label>: <n>
    samples`` followed by an optional ``Wrote:`` line.

    ``length`` is per channel, not the frame count times the channel count, so a
    stereo result reports the same figure its mono downmix would. The payload
    carries no channel count for the same reason the native CLI's does not:
    these keys are a cross-front-end contract, and the channel layout is a
    property of the written file.

    ``length`` and ``duration`` are the same quantity in two units and both are
    published: neither converts to the other without the sample rate, and the
    native CLI's callers read one while these read the other.

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
        _write_channel_output(args.output, channels, sr)

    length = len(channels[0])
    if args.json:
        payload: dict[str, object] = {
            "length": length,
            "sample_rate": sr,
            "duration": length / sr if sr > 0 else 0.0,
        }
        if extra:
            payload.update(extra)
        if args.output:
            payload["output"] = args.output
        print(_strict_json_dumps(payload))
    else:
        print(f"  {label}: {length} samples")
        if args.output:
            print(f"    Wrote: {args.output}")
    return 0


def _array_stats(vals: list[float], *, with_count: bool = True) -> dict[str, float | int]:
    """Summary statistics for a numeric array (avoids dumping huge arrays).

    Values are published at full precision: the native CLI serializes floats at
    round-trip precision, so rounding here for display made the same statistic
    differ between the two CLIs.
    """
    import statistics

    if not vals:
        stats: dict[str, float | int] = {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
        if with_count:
            return {"count": 0, **stats}
        return stats
    stats = {
        "mean": statistics.mean(vals),
        "std": statistics.pstdev(vals) if len(vals) > 1 else 0.0,
        "min": min(vals),
        "max": max(vals),
    }
    if with_count:
        return {"count": len(vals), **stats}
    return stats


def _parse_kv_params(value: str, *, names: bool = False) -> dict[str, Any]:
    """Parse a ``k=v,k=v`` string into a dict of floats.

    With ``names`` a value that is not a number is kept as text, for the library to
    resolve as the ``choices`` name of an enum-valued key (``noiseEstimator=mcra``).
    """
    params: dict[str, Any] = {}
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise ValueError(f"invalid param (expected key=value): {item}")
        key, raw = item.split("=", 1)
        text = raw.strip()
        try:
            params[key.strip()] = float(text)
        except ValueError:
            if not names:
                raise
            params[key.strip()] = text
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


def _chain_params_config(config: dict[str, Any]) -> dict[str, Any]:
    """Unwrap the native ``{version, params}`` chain-config representation."""
    params = config.get("params")
    if isinstance(params, dict):
        return dict(params)
    return dict(config)


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


def _audio_array(value: object) -> np.ndarray:
    """Convert a whole-buffer result to float32 in one bulk pass, without boxing samples."""
    return np.asarray(value, dtype=np.float32)


def _mean_abs(values: Sequence[float] | np.ndarray) -> float:
    """Mean absolute amplitude, accumulated in double; 0.0 for an empty buffer."""
    if len(values) == 0:
        return 0.0
    return float(np.mean(np.abs(np.asarray(values)), dtype=np.float64))


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
    # One --set occurrence is one PATH=VALUE assignment. The value reaches the
    # JSON parser byte for byte, so an object, an array, or a string that
    # carries its own commas survives; splitting the occurrences on any
    # separator would tear those apart with no escape available to the caller.
    for assignment in assignments:
        if not assignment:
            continue
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


def _parse_meter_candidates(value: str) -> list[int]:
    """Parse a comma-separated meter numerator list.

    Only the shape is checked here — an entry that is not an integer never
    reaches a range rule. Which numerators are acceptable, and how many, is the
    core's to enforce, so the same list is rejected for the same reason whether
    it came from a CLI or from a facade call.
    """
    parts = [item.strip() for item in value.split(",") if item.strip()]
    numerators: list[int] = []
    for part in parts:
        try:
            numerators.append(int(part))
        except ValueError as exc:
            raise ValueError(f"invalid meter numerator: {part}") from exc
    return numerators


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
