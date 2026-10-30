"""PCM quantization and atomic WAV/byte writers for the command line."""

from __future__ import annotations

import math
import os
import struct
import tempfile
from collections.abc import Iterable, Iterator, Sequence
from contextlib import AbstractContextManager, contextmanager, suppress
from typing import Any, cast

import numpy as np

from ._errors import SonareValueError
from ._ffi import (
    SONARE_ERROR_ENCODE_FAILED,
)
from ._runtime import SonareError


@contextmanager
def _encode_failed_on_oserror(path: str) -> Iterator[None]:
    """Re-raise an ``OSError`` from producing ``path`` as an ENCODE_FAILED error."""
    try:
        yield
    except OSError as exc:
        raise SonareError(SONARE_ERROR_ENCODE_FAILED, f"cannot write {path}: {exc}") from exc


def _to_float32(value: float) -> float:
    """Round a Python float to the nearest ``float`` the C++ writer would hold.

    The native quantizer takes a 32-bit sample and scales it in 32-bit
    arithmetic, so a mirror computing in Python's float64 lands on a different
    integer for some inputs even though both round the same way. A magnitude no
    32-bit float can hold becomes an infinity, as the narrowing conversion does,
    rather than an error.
    """
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

# UINT32_MAX minus the WAVE_FORMAT_EXTENSIBLE header's bytes after the RIFF size
# field (68 - 8) and the byte that closes an odd data chunk -- one RIFF chunk
# cannot declare a size past 2**32 - 1, and both header shapes share that bound.
_MAX_WAV_DATA_BYTES = 0xFFFFFFFF - 61


def _check_wav_request(
    path: str, frames: int, channels: int, sample_rate: int, bits_per_sample: int
) -> None:
    """Refuse an empty, RIFF-size-exceeding or unrepresentable WAV write before any bytes move.

    Mirrors the native writer's checks (save_wav / save_wav_multichannel's
    ``n_frames > 0``, ``make_wav_format_fields`` and ``check_riff_size_fits``)
    so both CLIs reach the same verdict, with the same error class, before
    either touches the filesystem.
    """
    if frames <= 0:
        raise SonareValueError(f"No frames to save: {path}")
    block_align = channels * (bits_per_sample // 8)
    if block_align <= 0 or block_align > 0xFFFF:
        raise SonareValueError(f"WAV block alignment exceeds header range: {path}")
    if int(sample_rate) * block_align > 0xFFFFFFFF:
        raise SonareValueError(f"WAV byte rate exceeds header range: {path}")
    if frames > _MAX_WAV_DATA_BYTES // block_align:
        raise SonareValueError(
            f"WAV data exceeds RIFF 32-bit size limit; use a chunked container: {path}"
        )


# WAVE_FORMAT_PCM, WAVE_FORMAT_EXTENSIBLE and the KSDATAFORMAT_SUBTYPE_PCM
# subformat GUID {00000001-0000-0010-8000-00AA00389B71} stored as little-endian
# Data1/2/3 followed by the 8 Data4 bytes verbatim -- the same layout the native
# CLI's `save_wav_multichannel` writes.
_WAVE_FORMAT_PCM = 1
_WAVE_FORMAT_EXTENSIBLE = 0xFFFE
_PCM_SUBFORMAT_GUID = bytes(
    [0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71]
)
# dwChannelMask by channel count, matching src/core/channel_layout.h's
# wave_channel_mask table (mono carries no meaningful mask).
_CHANNEL_MASKS = {1: 0x0, 2: 0x3, 6: 0x3F, 8: 0x63F}
# RIFF/WAVE(12) + fmt chunk id/size/body(8 + 16 or 40) + data chunk id/size(8).
_PCM_HEADER_SIZE = 44
_EXTENSIBLE_HEADER_SIZE = 68


class _RiffWavWriter:
    """Streams raw PCM into an open WAV file; the header is patched on :meth:`finish`.

    The one RIFF writer for every width: plain WAVE_FORMAT_PCM for mono and
    stereo, WAVE_FORMAT_EXTENSIBLE (with a channel mask) beyond that, both
    byte-identical to the native CLI's writer. Exposes ``writeframesraw`` like
    stdlib ``wave.Wave_write``, which is all the callers use.
    """

    def __init__(
        self, handle: Any, channels: int, sample_rate: int, bits_per_sample: int, extensible: bool
    ) -> None:
        self._handle = handle
        self._channels = channels
        self._sample_rate = sample_rate
        self._bits_per_sample = bits_per_sample
        self._extensible = extensible
        self._data_size = 0

    @property
    def header_size(self) -> int:
        """Bytes the header occupies ahead of the first sample."""
        return _EXTENSIBLE_HEADER_SIZE if self._extensible else _PCM_HEADER_SIZE

    def writeframesraw(self, data: bytes | bytearray) -> None:
        self._handle.write(bytes(data))
        self._data_size += len(data)

    def finish(self) -> None:
        """Patch the header now that the data size is known, then close an odd data chunk."""
        block_align = self._channels * (self._bits_per_sample // 8)
        byte_rate = self._sample_rate * block_align
        fmt_size = 40 if self._extensible else 16
        # A RIFF chunk of odd size is followed by one zero byte the size counts.
        pad = self._data_size & 1
        riff_size = 4 + (8 + fmt_size) + (8 + self._data_size + pad)
        header = bytearray()
        header += b"RIFF" + struct.pack("<I", riff_size) + b"WAVE"
        header += b"fmt " + struct.pack("<I", fmt_size)
        header += struct.pack(
            "<HHIIHH",
            _WAVE_FORMAT_EXTENSIBLE if self._extensible else _WAVE_FORMAT_PCM,
            self._channels,
            self._sample_rate,
            byte_rate,
            block_align,
            self._bits_per_sample,
        )
        if self._extensible:
            header += struct.pack(
                "<HHI",
                22,  # cbSize
                self._bits_per_sample,  # wValidBitsPerSample
                _CHANNEL_MASKS.get(self._channels, 0),
            )
            header += _PCM_SUBFORMAT_GUID
        header += b"data" + struct.pack("<I", self._data_size)
        assert len(header) == self.header_size
        self._handle.seek(0)
        self._handle.write(bytes(header))
        if pad:
            self._handle.seek(0, os.SEEK_END)
            self._handle.write(b"\x00")


@contextmanager
def _atomic_riff_writer(
    path: str,
    channels: int,
    sample_rate: int,
    bits_per_sample: int,
    *,
    frames: int,
    extensible: bool,
) -> Iterator[_RiffWavWriter]:
    """Yield a WAV writer whose completed file atomically replaces ``path``.

    ``frames`` is the total frame count the caller intends to write, checked
    up front by @ref _check_wav_request -- before the temp file even
    exists, so a refused request touches neither it nor ``path``.
    """
    if bits_per_sample not in (16, 24):
        raise ValueError("WAV bits must be 16 or 24")
    _check_wav_request(path, frames, channels, sample_rate, bits_per_sample)
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
    with _encode_failed_on_oserror(path):
        raw = tempfile.NamedTemporaryFile(  # noqa: SIM115 - lifetime spans the yielded writer
            mode="w+b",
            prefix=f".{os.path.basename(target)}.",
            suffix=".tmp",
            dir=directory,
            delete=False,
        )
    temporary = raw.name
    writer = _RiffWavWriter(raw, channels, int(sample_rate), bits_per_sample, extensible)
    try:
        with _encode_failed_on_oserror(path):
            raw.seek(writer.header_size)
            yield writer
            writer.finish()
            raw.flush()
            os.fsync(raw.fileno())
            raw.close()
            os.replace(temporary, target)
    except BaseException:
        with suppress(Exception):
            raw.close()
        with suppress(FileNotFoundError):
            os.unlink(temporary)
        raise


def _atomic_wav_writer(
    path: str, channels: int, sample_rate: int, bits_per_sample: int = 16, *, frames: int
) -> AbstractContextManager[_RiffWavWriter]:
    """Plain WAVE_FORMAT_PCM writer, for the widths a plain header can describe (mono, stereo)."""
    return _atomic_riff_writer(
        path, channels, sample_rate, bits_per_sample, frames=frames, extensible=False
    )


def _atomic_wav_writer_multichannel(
    path: str, channels: int, sample_rate: int, bits_per_sample: int = 16, *, frames: int
) -> AbstractContextManager[_RiffWavWriter]:
    """WAVE_FORMAT_EXTENSIBLE writer, carrying the channel mask a 5.1/7.1 player needs."""
    return _atomic_riff_writer(
        path, channels, sample_rate, bits_per_sample, frames=frames, extensible=True
    )


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
        # Every stage is part of producing the output, and the raw errno names the
        # scratch file rather than the destination the caller asked for.
        with _encode_failed_on_oserror(path):
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
