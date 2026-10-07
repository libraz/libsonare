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

import numpy as np

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
EXIT_ENCODE_FAILED = 12
# Python-only: the native CLI links its library statically and cannot skew.
EXIT_ABI_MISMATCH = 13

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
    """Whether SONARE_LEGACY_EXIT requests the old all-failures-are-1 behaviour."""
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
    from .audio import Audio

    channels = _source_channel_count(path)
    if channels > 1:
        print(
            f"warning: {channels}-channel input is downmixed to mono by this CLI command; "
            "use the stereo library API for channel-preserving processing",
            file=sys.stderr,
        )
    with Audio.from_file(path) as audio:
        return audio.data, audio.sample_rate


def _load_audio_channels(path: str) -> tuple[list[list[float]], int]:
    """Load every source channel of a file, in source order.

    Returns one buffer per channel rather than a downmix, for the commands that
    write audio back out and would otherwise deliver a mono file for a stereo
    input. A mono source takes the single-decode path, so nothing pays for this
    that does not have channels to keep.

    Raises:
        SonareError: As :meth:`Audio.from_file` does.
    """
    from .audio import Audio

    channels = _source_channel_count(path)
    if channels <= 1:
        samples, sample_rate = _load_audio(path)
        return [samples], sample_rate

    planes: list[list[float]] = []
    sample_rate = 0
    for index in range(channels):
        with Audio.from_file_channel(path, index) as audio:
            planes.append(audio.data)
            sample_rate = audio.sample_rate
    return planes, sample_rate


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


def _quantize_sample(sample: float, full_scale: float, minimum: int, maximum: int) -> int:
    """Scale a float sample to an integer PCM code.

    Reproduces ``float_to_pcm16`` / ``float_to_pcm24`` (audio_io.cpp) step for
    step: narrow to 32-bit, write a non-finite sample as digital silence, clamp
    to ``[-1, 1]``, scale in 32-bit by ``2**(bits - 1)`` (the grid the decoder
    divides by), round half away from zero rather than to even, then clamp the
    code to the container. The guard precedes the clamp
    because a comparison against a non-finite value is false: a clamp alone lets
    NaN and +Inf through as positive full scale, a peak the encoder never
    produced. Narrowing and the rounding rule are both load-bearing -- either
    one alone still disagrees with the native writer on some samples.

    The core threads a substitution count out of ``save_wav``; neither CLI asks
    for it, so both substitute without reporting.
    """
    narrowed = _to_float32(sample)
    if not math.isfinite(narrowed):
        return 0
    clamped = max(-1.0, min(1.0, narrowed))
    scaled = _to_float32(clamped * full_scale)
    rounded = math.floor(scaled + 0.5) if scaled >= 0.0 else math.ceil(scaled - 0.5)
    return max(minimum, min(maximum, int(rounded)))


def _pcm16(sample: float) -> bytes:
    """Clamp a float to ``[-1.0, 1.0]`` and pack it as little-endian 16-bit PCM.

    Shared by every WAV writer so the clamp-and-scale contract stays identical.
    A non-finite sample is written as digital silence; see ``_quantize_sample``.
    """
    import struct

    return struct.pack("<h", _quantize_sample(sample, 32768.0, -32768, 32767))


def _pcm24(sample: float) -> bytes:
    """Clamp a float and pack it as little-endian 24-bit PCM."""
    value = _quantize_sample(sample, 8388608.0, -8388608, 8388607)
    return value.to_bytes(3, byteorder="little", signed=True)


def _pcm(sample: float, bits_per_sample: int) -> bytes:
    if bits_per_sample == 16:
        return _pcm16(sample)
    if bits_per_sample == 24:
        return _pcm24(sample)
    raise ValueError("WAV bits must be 16 or 24")


def _quantize_codes(
    samples: np.ndarray, full_scale: float, minimum: int, maximum: int
) -> np.ndarray:
    """Vectorized :func:`_quantize_sample`, one PCM code per input sample.

    Reproduces the scalar function step for step and in the same precision at
    each step (float32 narrow, double-precision scale, float32 re-narrow, then
    integer round and clamp): a shortcut at any one of those steps has
    disagreed with the native writer on some sample before (see
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
    scaled = (clamped.astype(np.float64) * full_scale).astype(np.float32).astype(np.float64)
    rounded = np.where(scaled >= 0.0, np.floor(scaled + 0.5), np.ceil(scaled - 0.5))
    codes = np.clip(rounded, minimum, maximum).astype(np.int64)
    return np.where(finite, codes, 0)


def _pcm_bytes(samples: np.ndarray, bits_per_sample: int) -> bytes:
    """Vectorized sibling of :func:`_pcm`: one packed byte string for a whole chunk.

    16-bit output is a straight little-endian int16 pack. 24-bit has no native
    numpy integer width, so the codes are widened to int32, packed little-endian,
    and every 4th byte (the sign-extension byte a value in [-2**23, 2**23 - 1]
    always repeats in a 4-byte two's-complement spelling) is dropped -- the same
    low 3 bytes ``int.to_bytes(3, "little", signed=True)`` would have produced.
    """
    if bits_per_sample == 16:
        codes = _quantize_codes(samples, 32768.0, -32768, 32767)
        return bytes(codes.astype("<i2").tobytes())
    if bits_per_sample == 24:
        codes = _quantize_codes(samples, 8388608.0, -8388608, 8388607)
        widened = codes.astype("<i4").view(np.uint8).reshape(-1, 4)
        return bytes(widened[:, :3].tobytes())
    raise ValueError("WAV bits must be 16 or 24")


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
        raise SonareError(SONARE_ERROR_INVALID_PARAMETER, f"No frames to save: {path}")
    block_align = channels * (bits_per_sample // 8)
    if block_align <= 0 or frames > _MAX_WAV_DATA_BYTES // block_align:
        raise SonareError(
            SONARE_ERROR_INVALID_PARAMETER,
            f"WAV data exceeds RIFF 32-bit size limit; use a chunked container: {path}",
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


def _write_wav_mono_frames(wav: Any, samples: Sequence[float], bits_per_sample: int = 16) -> None:
    """Append one bounded mono PCM chunk to an open WAV writer."""
    wav.writeframesraw(_pcm_bytes(np.asarray(samples, dtype=np.float64), bits_per_sample))


def _write_wav_stereo_frames(
    wav: Any,
    left: Sequence[float],
    right: Sequence[float],
    bits_per_sample: int = 16,
) -> None:
    """Append one bounded stereo PCM chunk to an open WAV writer."""
    count = min(len(left), len(right))
    # Interleaved the same way _interleave_planes does (frame-major), so one
    # _pcm_bytes call quantizes and packs both channels' samples together.
    interleaved = np.empty(count * 2, dtype=np.float64)
    interleaved[0::2] = np.asarray(left[:count], dtype=np.float64)
    interleaved[1::2] = np.asarray(right[:count], dtype=np.float64)
    wav.writeframesraw(_pcm_bytes(interleaved, bits_per_sample))


def _write_wav(
    path: str, samples: list[float], sample_rate: int, bits_per_sample: int = 16
) -> None:
    """Write mono 16- or 24-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled to the selected PCM range;
    a non-finite sample becomes digital silence.
    """
    with _atomic_wav_writer(path, 1, sample_rate, bits_per_sample, frames=len(samples)) as wav:
        for offset in range(0, len(samples), _WAV_CHUNK_FRAMES):
            chunk = samples[offset : offset + _WAV_CHUNK_FRAMES]
            if bits_per_sample == 16:
                # Keep the historical two-argument call shape for callers that
                # instrument this helper; 24-bit output opts into the width.
                _write_wav_mono_frames(wav, chunk)
            else:
                _write_wav_mono_frames(wav, chunk, bits_per_sample)


def _write_wav_stereo(
    path: str,
    left: list[float],
    right: list[float],
    sample_rate: int,
    bits_per_sample: int = 16,
) -> None:
    """Write a stereo 16- or 24-bit PCM WAV using only the Python standard library.

    Floats are clamped to ``[-1.0, 1.0]`` and scaled to the selected PCM range;
    a non-finite sample becomes digital silence.
    """
    count = min(len(left), len(right))
    with _atomic_wav_writer(path, 2, sample_rate, bits_per_sample, frames=count) as wav:
        for offset in range(0, count, _WAV_CHUNK_FRAMES):
            end = min(offset + _WAV_CHUNK_FRAMES, count)
            if bits_per_sample == 16:
                # Keep the historical three-argument call shape for callers that
                # instrument this helper; 24-bit output opts into the width.
                _write_wav_stereo_frames(wav, left[offset:end], right[offset:end])
            else:
                _write_wav_stereo_frames(wav, left[offset:end], right[offset:end], bits_per_sample)


def _load_channels_or_downmix(path: str) -> tuple[list[list[float]], int]:
    """Load a file as a stereo pair where it is one, and as mono otherwise.

    The library's offline operations come in a mono and a stereo form and
    nothing wider, so a two-channel source is the only one that can be carried
    through whole. Anything else goes through the downmixing loader, which says
    so: silently keeping channel 0 of a surround file would deliver a quarter of
    the record under the name of the whole.

    The mono path goes through the facade loader rather than the direct one, so
    the documented ``cli._load_audio`` patch point still intercepts it. Calling
    the module-local loader here would work identically in production and stop
    the seam working, which is the shape of difference a test suite finds and a
    reader does not.
    """
    if _source_channel_count(path) == 2:
        return _load_audio_channels(path)
    samples, sample_rate = _load_audio_from_facade(path)
    return [samples], sample_rate


def _write_channel_output(
    path: str, channels: list[list[float]], sample_rate: int, bits_per_sample: int = 16
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
            for offset in range(0, frames, _WAV_CHUNK_FRAMES):
                chunk = audio[offset : offset + _WAV_CHUNK_FRAMES]
                wav.writeframesraw(_pcm_bytes(chunk.astype(np.float64).ravel(), bits_per_sample))
            return frames, channels
        # The general duck-typed fallback: possibly-ragged rows, padded with
        # silence past a short row -- the shape a fast-path ndarray never has.
        pcm = bytearray()
        chunk_frames = 0
        for row in cast(Any, audio):
            if hasattr(row, "__iter__") and not isinstance(row, (str, bytes, bytearray)):
                values = row
            else:
                values = (row,)
            row_values = [float(sample) for sample in values]
            for channel in range(channels):
                value = row_values[channel] if channel < len(row_values) else 0.0
                pcm.extend(_pcm(value, bits_per_sample))
            chunk_frames += 1
            if chunk_frames == _WAV_CHUNK_FRAMES:
                wav.writeframesraw(pcm)
                pcm.clear()
                chunk_frames = 0
        if pcm:
            wav.writeframesraw(pcm)
    return frames, channels


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
    channels: list[list[float]],
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
    samples`` followed by an optional ``Wrote:`` line, matching each command's
    historical output exactly.

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
