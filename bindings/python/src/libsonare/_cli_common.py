"""Command-line interface for libsonare."""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
import re
import sys
from collections.abc import Callable, Iterable, Iterator, Sequence
from typing import Any, cast, get_args

import numpy as np

from ._cli_output import (
    _CHANNEL_MASKS as _CHANNEL_MASKS,
)
from ._cli_output import (
    _EXTENSIBLE_HEADER_SIZE as _EXTENSIBLE_HEADER_SIZE,
)
from ._cli_output import (
    _MASK64 as _MASK64,
)
from ._cli_output import (
    _MAX_WAV_DATA_BYTES as _MAX_WAV_DATA_BYTES,
)
from ._cli_output import (
    _PCM_DITHER_SEED as _PCM_DITHER_SEED,
)
from ._cli_output import (
    _PCM_HEADER_SIZE as _PCM_HEADER_SIZE,
)
from ._cli_output import (
    _PCM_SUBFORMAT_GUID as _PCM_SUBFORMAT_GUID,
)
from ._cli_output import (
    _SPLITMIX64_GAMMA as _SPLITMIX64_GAMMA,
)
from ._cli_output import (
    _SPLITMIX64_MUL1 as _SPLITMIX64_MUL1,
)
from ._cli_output import (
    _SPLITMIX64_MUL2 as _SPLITMIX64_MUL2,
)
from ._cli_output import (
    _TPDF_UNIT as _TPDF_UNIT,
)
from ._cli_output import (
    _WAV_CHUNK_FRAMES as _WAV_CHUNK_FRAMES,
)
from ._cli_output import (
    _WAVE_FORMAT_EXTENSIBLE as _WAVE_FORMAT_EXTENSIBLE,
)
from ._cli_output import (
    _WAVE_FORMAT_PCM as _WAVE_FORMAT_PCM,
)
from ._cli_output import (
    _atomic_riff_writer as _atomic_riff_writer,
)
from ._cli_output import (
    _atomic_wav_writer as _atomic_wav_writer,
)
from ._cli_output import (
    _atomic_wav_writer_multichannel as _atomic_wav_writer_multichannel,
)
from ._cli_output import (
    _atomic_write_bytes as _atomic_write_bytes,
)
from ._cli_output import (
    _channel_dither_seed as _channel_dither_seed,
)
from ._cli_output import (
    _check_wav_request as _check_wav_request,
)
from ._cli_output import (
    _on_pcm_grid as _on_pcm_grid,
)
from ._cli_output import (
    _OutputFit as _OutputFit,
)
from ._cli_output import (
    _padded_chunks as _padded_chunks,
)
from ._cli_output import (
    _pcm as _pcm,
)
from ._cli_output import (
    _pcm16 as _pcm16,
)
from ._cli_output import (
    _pcm24 as _pcm24,
)
from ._cli_output import (
    _PcmQuantizer as _PcmQuantizer,
)
from ._cli_output import (
    _quantize_codes as _quantize_codes,
)
from ._cli_output import (
    _quantize_sample as _quantize_sample,
)
from ._cli_output import (
    _RiffWavWriter as _RiffWavWriter,
)
from ._cli_output import (
    _splitmix64_at as _splitmix64_at,
)
from ._cli_output import (
    _splitmix64_at_array as _splitmix64_at_array,
)
from ._cli_output import (
    _to_float32 as _to_float32,
)
from ._cli_output import (
    _tpdf_lsb as _tpdf_lsb,
)
from ._cli_output import (
    _tpdf_lsb_array as _tpdf_lsb_array,
)
from ._cli_output import (
    _write_channel_output as _write_channel_output,
)
from ._cli_output import (
    _write_frame_major_wav as _write_frame_major_wav,
)
from ._cli_output import (
    _write_project_bounce_wav as _write_project_bounce_wav,
)
from ._cli_output import (
    _write_wav as _write_wav,
)
from ._cli_output import (
    _write_wav_mono_frames as _write_wav_mono_frames,
)
from ._cli_output import (
    _write_wav_stereo as _write_wav_stereo,
)
from ._cli_output import (
    _write_wav_stereo_frames as _write_wav_stereo_frames,
)
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
    SONARE_ERROR_STALE_RESULT,
    resolved_library_path,
)
from ._field_spelling import parameter_bag_subject
from ._runtime import SonareError, _cli_refusal_subjects
from .types import KeyMode, KeyProfile, PitchClass, SectionType

PITCH_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MODE_NAMES = ["major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"]

# Frames per call wherever the CLI itself drives a processor block by block. A host choice
# stated here once; the output of every such loop was measured identical across chunk sizes.
_CLI_HOST_CHUNK_FRAMES = 4096

# CLI exit codes. Failures map to codes aligned with the C-ABI SonareError so
# scripts can distinguish usage / missing-file / decode / processing errors.
# argparse keeps its native exit 2 for usage errors.
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
# Exit codes and C-ABI error codes are separate numbering spaces: error 11 is
# StaleResult while exit 11 is a cancellation, so StaleResult exits 14.
EXIT_STALE_RESULT = 14

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
    SONARE_ERROR_STALE_RESULT: EXIT_STALE_RESULT,
}


# A keyed value that is not there: the writer leaves its key out, the way a NaN is.
_ABSENT = object()


def _left_out(value: object) -> bool:
    return value is _ABSENT or (isinstance(value, float) and math.isnan(value))


def _sanitize_json_value(value: object) -> object:
    """Apply the level convention the native writer holds.

    A NaN (not measured) is never written: a keyed one drops its key and one in an
    array is refused, since an array holding one goes through ``_measured_pairs``.
    An infinity is written as null, the end its field's schema declares.
    """
    if _left_out(value):
        raise ValueError("a JSON array cannot carry a not-measured value")
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {
            key: _sanitize_json_value(item) for key, item in value.items() if not _left_out(item)
        }
    if isinstance(value, (list, tuple)):
        return [_sanitize_json_value(item) for item in value]
    return value


def _absent_if_none(value: object) -> object:
    """A value the library returned as None leaves its key out of the document."""
    return _ABSENT if value is None else value


def _measured_pairs(values: Any, values_key: str, index_key: str) -> dict[str, list[Any]]:
    """The measured entries of ``values`` beside their ascending indices, as two keyed lists."""
    kept = [
        (index, float(value))
        for index, value in enumerate(values)
        if value is not None and not math.isnan(float(value))
    ]
    return {
        values_key: [value for _, value in kept],
        index_key: [index for index, _ in kept],
    }


def _fmt_optional(value: float | None, spec: str, unit: str = "") -> str:
    """Format a measured value for text output, or "-" when nothing was measured."""
    return "-" if value is None else f"{format(value, spec)}{unit}"


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


_BOLD = "\033[1m"
_GREEN = "\033[32m"
_YELLOW = "\033[33m"
_BLUE = "\033[34m"
_MAGENTA = "\033[35m"
_CYAN = "\033[36m"
_RESET = "\033[0m"


def _styled(text: str, *codes: str) -> str:
    """Wrap text in ANSI codes when the terminal takes them."""
    if not _color_enabled():
        return text
    return "".join(codes) + text + _RESET


#: Longest clipped run, in samples, a declip stage reconstructs; longer runs are left as they were.
_DECLIP_MAX_GAP_SAMPLES = 512


def _add_unreconstructed_runs(payload: dict[str, object], runs: int | None) -> None:
    """Add ``unreconstructed_runs`` to a JSON payload when a declip stage ran."""
    if runs is not None:
        payload["unreconstructed_runs"] = runs


def _print_unreconstructed_runs(runs: int | None, label_width: int) -> None:
    """Print the runs a declip stage left clipped; nothing when no declip stage ran."""
    if runs is not None:
        label = "Left clipped:".ljust(label_width)
        print(f"    {label}{runs} runs longer than {_DECLIP_MAX_GAP_SAMPLES} samples")


def _note_output_fit(fit: _OutputFit) -> None:
    """Say on stderr that ``fit`` lowered the output; silent when it did not."""
    if fit.gain == 1.0:
        return
    print(
        _styled(
            f"Output lowered by {-fit.gain_db:.2f} dB so its peak fits full scale "
            "instead of clipping",
            _YELLOW,
        ),
        file=sys.stderr,
    )


def _write_mastering_output(
    output: str,
    channels: Sequence[Sequence[float] | np.ndarray],
    sample_rate: int,
    bits: int,
) -> _OutputFit:
    """Write a mastering-family result to ``output`` under its own fit, and note the fit.

    Returns the identity fit, with nothing written, when no output path was given.
    """
    fit = _OutputFit()
    if output:
        for channel in channels:
            fit.include(channel)
        _write_channel_output(output, channels, sample_rate, bits, fit=fit)
        _note_output_fit(fit)
    return fit


def cmd_doctor(args: argparse.Namespace) -> int:
    """Print a concise diagnostic report for the loaded libsonare build."""
    from ._analysis_music import _capabilities_document

    descriptor = _capabilities_document()
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


# The librosa keyword for the rate, as a word of a refusal; the CLI has no such option.
_LIBROSA_RATE_WORD = re.compile(r"(?<![\w.-])sr(?![\w(])")

_MEL_FLAG_SUBJECTS = {
    subject: f"--{edge}-hz"
    for edge in ("fmin", "fmax")
    for subject in (edge, f"{edge}_hz", f"options.{edge}_hz")
}


def _mel_flag_refusals() -> contextlib.AbstractContextManager[None]:
    return _cli_refusal_subjects(_MEL_FLAG_SUBJECTS, bare=True)


def _error_message(exc: BaseException) -> str:
    """The one-line text reported for ``exc``.

    The rate comes from the input file rather than an ``sr`` option, so a refused
    argument names it ``sample_rate``, as the native CLI does.
    """
    if isinstance(exc, KeyboardInterrupt):
        return "cancelled"
    message = str(getattr(exc, "_cli_error_message", str(exc)))
    if isinstance(exc, SonareValueError):
        message = _LIBROSA_RATE_WORD.sub("sample_rate", message)
    return message


def _exit_code_for(exc: BaseException) -> int:
    """Map an exception to a CLI exit code aligned with the C-ABI error codes."""
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
        return int(Audio.file_num_channels(path))
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
    _warn_downmix(_source_channel_count(path))
    return _decode_mono(path)


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


def _warn_downmix(channels: int) -> None:
    """Warn on stderr that a multi-channel source is downmixed to mono."""
    if channels > 1:
        print(
            f"warning: {channels}-channel input is downmixed to mono by this CLI command; "
            "use the stereo library API for channel-preserving processing",
            file=sys.stderr,
        )


def _decode_mono(path: str) -> tuple[list[float], int]:
    """Decode a file to a mono buffer without any downmix warning."""
    from .audio import Audio

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
    except IsADirectoryError as exc:
        # The native path loaders refuse a directory as a read failure.
        raise SonareError(SONARE_ERROR_DECODE_FAILED, f"Failed to read file: {path}") from exc
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
        samples, sample_rate = _decode_mono(path)
        return [samples], sample_rate

    from .audio import decode_channels

    result = decode_channels(_read_audio_bounded(path))
    decoded, sample_rate = result.channels, result.sample_rate
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
    _warn_downmix(channels)
    samples, sample_rate = _decode_mono(path)
    return [samples], sample_rate


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
    fit: _OutputFit | None = None,
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

    A render's required ``-o`` is refused at parse time (``_REQUIRED_OPTIONS``);
    ``trim-silence`` doubles as analysis, so its destination stays optional.

    The channels are written under one ``fit`` (their own peak unless the caller
    measured it first, as ``normalize`` does to report the level as written), and
    ``output_gain_db`` is published with the output.
    """
    if fit is None:
        fit = _OutputFit()
        for channel in channels:
            fit.include(channel)
    if args.output:
        _write_channel_output(args.output, channels, sr, fit=fit)
        _note_output_fit(fit)

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
            payload["output_gain_db"] = fit.gain_db
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
    if all(math.isfinite(v) for v in vals):
        mean = statistics.mean(vals)
        std = statistics.pstdev(vals) if len(vals) > 1 else 0.0
    else:
        # statistics refuses an infinity; float arithmetic gives the native writer's
        # result: -inf for a mean over silent frames (null), NaN for its spread (absent).
        mean = sum(vals) / len(vals)
        std = math.nan
    stats = {"mean": mean, "std": std, "min": min(vals), "max": max(vals)}
    if with_count:
        return {"count": len(vals), **stats}
    return stats


_PARAMS_GRAMMAR_REFUSAL = "argument --params: must be a comma-separated list of key=value entries"


def _cli_key_subject(flag: str, key: str) -> str:
    """Name a key exactly as the caller supplied it to a CLI flag."""
    return f"{flag} {key}"


def _cli_key_refusal(flag: str, key: str, detail: str) -> str:
    """Render one CLI refusal whose cause belongs to a key in a flag's value."""
    return f"{_cli_key_subject(flag, key)} {detail}"


def _cli_expected_keys(expected: Iterable[str]) -> str:
    """Format expected key names like the shared C schema renderer."""
    collapsed = {re.sub(r"(?<=[A-Za-z])\d+(?=\.)", "{i}", key) for key in expected}
    return ", ".join(sorted(collapsed))


def _cli_unknown_key(flag: str, key: str, expected: Iterable[str]) -> str:
    """Render an unknown-key refusal with the one sorted vocabulary used by both CLIs."""
    return f"unknown key {_cli_key_subject(flag, key)} (expected {_cli_expected_keys(expected)})"


@contextlib.contextmanager
def _cli_key_subjects(
    flag: str,
    keys: Iterable[str],
    *,
    roots: tuple[str, ...] = ("params", "config", "overrides", "document"),
    extra: dict[str, str] | None = None,
    bag_entry: str | None = None,
) -> Iterator[None]:
    """Respell structured native subjects for keys supplied through one CLI flag."""
    mapping = dict(extra or {})
    for key in keys:
        spelling = _cli_key_subject(flag, key)
        canonical = (
            _json_key_to_snake_case(key)
            if bag_entry is None
            else parameter_bag_subject(bag_entry, key)
        )
        mapping[key] = spelling
        for root in roots:
            mapping[f"{root}.{key}"] = spelling
        # Several surface spellings can share one C subject. Prefer a literal
        # C-style key when both it and an alias appear in the same bag.
        if canonical == key or canonical not in mapping:
            mapping[canonical] = spelling
            for root in roots:
                mapping[f"{root}.{canonical}"] = spelling
    with _cli_refusal_subjects(mapping):
        yield


def _params_entries(value: str) -> list[str]:
    """Split ``--params`` text into its entries, as the native CLI's line reader does.

    A trailing comma ends the list and an empty text holds no entry; any other empty
    entry is kept, so the grammar check refuses it.
    """
    entries = value.split(",")
    return entries[:-1] if entries[-1] == "" else entries


def _numeric_params_refusal(value: str) -> str | None:
    """The first numeric-only bag refusal, or None when every entry parses."""
    try:
        _parse_kv_params(value)
    except ValueError as error:
        return str(error)
    return None


def _params_grammar_refusal(value: str) -> str | None:
    """The refusal for a ``--params`` entry that is not ``key=value``, or None."""
    for entry in _params_entries(value):
        key, separator, raw = entry.partition("=")
        if not separator or not key or not raw:
            return _PARAMS_GRAMMAR_REFUSAL
    return None


def _params_keys(value: str) -> list[str]:
    """The key of each entry of a ``--params`` text that already passed the grammar check."""
    return [entry.partition("=")[0] for entry in _params_entries(value)]


def _parse_kv_params(value: str, *, names: bool = False) -> dict[str, Any]:
    """Parse a ``k=v,k=v`` string into a dict of floats.

    With ``names`` a value that is not a number is kept as text, for the library to
    resolve as the ``choices`` name of an enum-valued key (``noiseEstimator=mcra``).
    """
    from ._cli_options import _NumericTextError, _read_number_text

    params: dict[str, Any] = {}
    if (refusal := _params_grammar_refusal(value)) is not None:
        raise ValueError(refusal)
    for item in _params_entries(value):
        key, text = (part.strip() for part in item.split("=", 1))
        try:
            params[key] = _read_number_text(text, float32=False)
        except _NumericTextError:
            if not names:
                raise ValueError(
                    _cli_key_refusal("--params", key, "must be a finite number")
                ) from None
            params[key] = text
    return params


class _JsonObjectPairs:
    """Retain ordered object members until duplicate-key paths have been checked."""

    def __init__(self, pairs: list[tuple[str, Any]]) -> None:
        self.pairs = pairs


class _DuplicateJsonKeyError(SonareError):
    """A duplicate document key with the native JSON path and requested error code."""

    def __init__(self, path: str, code: int) -> None:
        self.text = f"duplicate key {path}"
        self._cli_error_message = self.text
        super().__init__(code, self.text)


def _json_key_path(path: tuple[tuple[bool, str], ...], key: str) -> str:
    rendered = ""
    for is_index, segment in path:
        if is_index:
            rendered += f"[{segment}]"
        else:
            if rendered:
                rendered += "."
            rendered += _escape_json_key(segment)
    if rendered:
        rendered += "."
    return rendered + _escape_json_key(key)


def _escape_json_key(value: str) -> str:
    return "".join(
        f"\\x{ord(char):02x}" if ord(char) < 0x20 or ord(char) == 0x7F else char for char in value
    )


def _materialize_json_pairs(
    value: object,
    path: tuple[tuple[bool, str], ...] = (),
    *,
    code: int = SONARE_ERROR_INVALID_FORMAT,
) -> Any:
    if isinstance(value, _JsonObjectPairs):
        result: dict[str, Any] = {}
        seen: set[str] = set()
        for key, child in value.pairs:
            if key in seen:
                raise _DuplicateJsonKeyError(_json_key_path(path, key), code)
            seen.add(key)
            result[key] = _materialize_json_pairs(child, (*path, (False, key)), code=code)
        return result
    if isinstance(value, list):
        return [
            _materialize_json_pairs(child, (*path, (True, str(index))), code=code)
            for index, child in enumerate(value)
        ]
    return value


def _cli_load_json_document(
    text: str,
    *,
    code: int = SONARE_ERROR_INVALID_FORMAT,
    parse_constant: Callable[[str], Any] | None = None,
) -> Any:
    """Parse a CLI JSON document without discarding duplicate members."""
    value = json.loads(
        text,
        object_pairs_hook=_JsonObjectPairs,
        parse_constant=parse_constant,
    )
    return _materialize_json_pairs(value, code=code)


def _load_json_object(path: str) -> dict[str, Any]:
    with open(path, encoding="utf-8") as fh:
        loaded = _cli_load_json_document(fh.read())
    if not isinstance(loaded, dict):
        raise ValueError("JSON config must be an object")
    return loaded


def _audio_array(value: object) -> np.ndarray:
    """Convert a whole-buffer result to float32 in one bulk pass, without boxing samples."""
    return np.asarray(value, dtype=np.float32)


def _mean_abs(values: Sequence[float] | np.ndarray) -> float:
    """Mean absolute amplitude, accumulated in double; 0.0 for an empty buffer."""
    if len(values) == 0:
        return 0.0
    return float(np.mean(np.abs(np.asarray(values)), dtype=np.float64))


def _cli_token(library_token: str) -> str:
    """Spell a lowerCamel library token as a CLI value: kebab-case, digit runs as words."""
    out: list[str] = []
    previous_digit = False
    for i, ch in enumerate(library_token):
        digit = ch.isdigit() or ch == "."
        if i > 0 and (ch.isupper() or digit != previous_digit):
            out.append("-")
        out.append(ch.lower())
        previous_digit = digit
    return "".join(out)


def _find_cli_token(value: str, tokens: Iterable[str]) -> str | None:
    """Return the token among ``tokens`` that ``value`` names in any letter case, or None.

    Every closed-set token flag is read through this, so the library below always
    receives its own exact spelling whatever case the caller typed.
    """
    folded = value.lower()
    for token in tokens:
        if token.lower() == folded:
            return token
    return None


def _catalog_token(value: str, library_tokens: Iterable[str]) -> str:
    """Return the library id ``value`` names in any letter case, else ``value`` for the library."""
    found = _find_cli_token(value, library_tokens)
    return value if found is None else found


def _library_token(flag: str, value: str, library_tokens: Sequence[str]) -> str:
    """Resolve a kebab-case flag value, in any case, to the library token it spells."""
    spellings = {_cli_token(token): token for token in library_tokens}
    found = _find_cli_token(value, spellings)
    if found is not None:
        return spellings[found]
    accepted = ", ".join(spellings)
    raise ValueError(f"{flag}: unknown value {value!r}; expected one of: {accepted}")


def _load_voice_preset_pack(path: str, preset_id: str) -> dict[str, Any]:
    with open(path, encoding="utf-8") as fh:
        pack = _cli_load_json_document(fh.read())
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
    def finite_float(text: str) -> float:
        value = float(text)
        if not math.isfinite(value):
            raise ValueError("number must be finite")
        return value

    def finite_int(text: str) -> int:
        value = int(text)
        try:
            finite = math.isfinite(float(value))
        except OverflowError:
            finite = False
        if not finite:
            raise ValueError("number must be finite")
        return value

    def refuse_non_json_constant(token: str) -> None:
        raise ValueError(token)

    try:
        return json.loads(
            raw,
            parse_constant=refuse_non_json_constant,
            parse_float=finite_float,
            parse_int=finite_int,
        )
    except (json.JSONDecodeError, ValueError):
        return raw


_VoiceSetWrite = tuple[tuple[str, ...], tuple[str, ...], str, int]


def _voice_path_prefix(parent: tuple[str, ...], child: tuple[str, ...]) -> bool:
    if len(parent) > len(child):
        return False
    for parent_part, child_part in zip(parent, child, strict=False):
        if child_part == parent_part:
            continue
        if child_part.startswith(parent_part + "[") and child_part.endswith("]"):
            continue
        return False
    return True


def _voice_path_suffix(parent: tuple[str, ...], child: tuple[str, ...]) -> str:
    suffix = ""
    for index, parent_part in enumerate(parent):
        child_part = child[index]
        if child_part == parent_part:
            continue
        if child_part.startswith(parent_part + "[") and child_part.endswith("]"):
            suffix += child_part[len(parent_part) :]
            continue
        raise ValueError("voice subject is not under its provenance path")
    if len(child) > len(parent):
        if suffix:
            suffix += "."
        suffix += ".".join(child[len(parent) :])
    return suffix


def _set_nested_value(
    root: dict[str, Any],
    path: str,
    value: object,
    *,
    provenance: list[_VoiceSetWrite] | None = None,
    order: int = 0,
) -> None:
    parts = [part for part in path.split(".") if part]
    if not parts:
        raise ValueError("empty --set path")
    cursor = root
    prefix: list[str] = []

    def record(target: tuple[str, ...], assigned: tuple[str, ...]) -> None:
        if provenance is None:
            return
        # A later write replaces this subtree, so earlier ownership beneath it
        # no longer describes the effective overlay.
        provenance[:] = [item for item in provenance if not _voice_path_prefix(target, item[0])]
        provenance.append((target, assigned, path, order))

    for part in parts[:-1]:
        prefix.append(part)
        child = cursor.get(part)
        if not isinstance(child, dict):
            cursor[part] = {}
            record(tuple(prefix), tuple(parts))
            child = cursor[part]
        cursor = child
    prefix.append(parts[-1])
    cursor[parts[-1]] = value
    record(tuple(prefix), tuple(parts))


class _VoiceSetSubjectMapper:
    """Map a structured config path to the effective --set assignment that wrote it."""

    def __init__(self, writes: list[_VoiceSetWrite], config: dict[str, Any]) -> None:
        self._writes = writes
        self._path_counts: dict[str, int] = {}
        self._count_document_paths(config)
        self.matched = False

    def _count_document_paths(self, value: object, prefix: tuple[str, ...] = ()) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                path = (*prefix, key)
                rendered = ".".join(path)
                self._path_counts[rendered] = self._path_counts.get(rendered, 0) + 1
                self._count_document_paths(child, path)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                indexed = (*prefix[:-1], prefix[-1] + f"[{index}]") if prefix else (f"[{index}]",)
                self._count_document_paths(child, indexed)

    def __call__(self, subject: str) -> str | None:
        parts = tuple(part for part in subject.split(".") if part)
        if not parts:
            return None
        rendered = ".".join(parts)
        # The C refusal path is flattened with dots. If the JSON contains both a
        # dotted literal key and the equivalent nested object, the subject alone
        # cannot identify which one failed, so preserve the document's own text.
        if self._path_counts.get(rendered, 0) > 1:
            return None

        owned: list[tuple[int, int, str]] = []
        for target, assigned, spelling, order in self._writes:
            if not _voice_path_prefix(target, parts):
                continue
            suffix = _voice_path_suffix(target, parts)
            if len(target) < len(assigned) and _voice_path_prefix(target, assigned):
                # The recorded target is an intermediate container created on the
                # way to this assignment. Keep the full concrete assignment spelling.
                subject_path = spelling
            elif suffix:
                # A supplied object owns missing or invalid descendants. The core's
                # camelCase subject supplies the child path after the assigned object.
                subject_path = spelling + ("." if not suffix.startswith("[") else "") + suffix
            else:
                subject_path = spelling
            owned.append((len(target), order, subject_path))
        if not owned:
            return None
        _, _, spelling = max(owned, key=lambda owner: (owner[0], owner[1]))
        self.matched = True
        return _cli_key_subject("--set", spelling)


@contextlib.contextmanager
def _cli_voice_set_subjects(
    writes: list[_VoiceSetWrite],
    config: dict[str, Any],
) -> Iterator[_VoiceSetSubjectMapper]:
    """Respell only paths the effective --set overlay supplied."""
    mapper = _VoiceSetSubjectMapper(writes, config)
    with _cli_refusal_subjects(mapper):
        yield mapper


def _voice_set_refusal(assignments: Iterable[str] | None) -> str | None:
    """The refusal of the ``--set`` assignments (an empty one is skipped), or None."""
    for assignment in assignments or ():
        if not assignment:
            continue
        path, separator, _ = assignment.partition("=")
        if not separator or not path:
            return f"invalid --set assignment: {assignment}"
        if not path.strip("."):
            return "empty --set path"
    return None


def _apply_voice_sets(
    preset: str | dict[str, Any],
    assignments: list[str] | None,
    *,
    provenance: list[_VoiceSetWrite] | None = None,
) -> str | dict[str, Any]:
    if not assignments:
        return preset
    parsed: object = (
        _cli_load_json_document(preset)
        if isinstance(preset, str)
        else json.loads(json.dumps(preset))
    )
    root = parsed if isinstance(parsed, dict) else None
    # One --set occurrence is one PATH=VALUE assignment. The value reaches the
    # JSON parser byte for byte, so an object, an array, or a string that
    # carries its own commas survives; splitting the occurrences on any
    # separator would tear those apart with no escape available to the caller.
    for order, assignment in enumerate(assignments):
        if not assignment:
            continue
        # The command validator (_voice_set_refusal) has already refused a malformed one.
        path, raw = assignment.split("=", 1)
        value = _parse_voice_set_value(raw)
        if root is None:
            root = {}
        _set_nested_value(root, path, value, provenance=provenance, order=order)
    return cast(str | dict[str, Any], parsed if root is None else root)


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


_CLI_MODE_ALIASES: dict[str, KeyMode] = {
    "maj": "major",
    "min": "minor",
    "m": "minor",
}


def _section_type_label(section_type: SectionType) -> str:
    """Spell a section type as the CLI prints it (``"pre-chorus"``)."""
    return "pre-chorus" if section_type == "preChorus" else section_type


def _cli_mode_names() -> list[str]:
    """Every mode name the CLI reads, each mode followed by its aliases, as a refusal lists them."""
    return [
        spelling
        for mode in get_args(KeyMode)
        for spelling in (
            mode,
            *(alias for alias, target in _CLI_MODE_ALIASES.items() if target == mode),
        )
    ]


def _parse_mode(value: str) -> KeyMode:
    key = value.lower()
    key = _CLI_MODE_ALIASES.get(key, key)
    if key not in get_args(KeyMode):
        raise ValueError(f"invalid mode: {value}")
    return cast(KeyMode, key)


def _parse_modes(value: str) -> list[KeyMode]:
    key = value.lower()
    if key in ("major-minor", "majmin", "diatonic"):
        return ["major", "minor"]
    if key in ("all", "modal"):
        return list(get_args(KeyMode))
    modes = [_parse_mode(item) for item in value.split(",") if item]
    if not modes:
        raise ValueError("--modes must contain at least one mode")
    return modes


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
    names: dict[str, KeyProfile] = {
        "ks": "krumhanslSchmuckler",
        "krumhansl": "krumhanslSchmuckler",
        "krumhansl-schmuckler": "krumhanslSchmuckler",
        "temperley": "temperley",
        "shaath": "shaath",
        "keyfinder": "shaath",
        "faraldo-edmt": "faraldoEdmt",
        "edmt": "faraldoEdmt",
        "faraldo-edma": "faraldoEdma",
        "edma": "faraldoEdma",
        "faraldo-edmm": "faraldoEdmm",
        "edmm": "faraldoEdmm",
        "bellman-budge": "bellmanBudge",
        "bellman": "bellmanBudge",
        "budge": "bellmanBudge",
    }
    key = value.lower()
    if key not in names:
        raise ValueError(f"invalid key profile: {value}")
    return names[key]
