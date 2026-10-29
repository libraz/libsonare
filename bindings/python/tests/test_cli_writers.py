"""Bounded and failure-atomic CLI file-boundary tests."""

from __future__ import annotations

import struct
import tracemalloc
import wave
from pathlib import Path
from typing import Any

import pytest

_NATIVE_PROJECT_MIDI_LIMIT = 64 * 1024 * 1024


def _assert_only_old_output(directory: Path, output: Path) -> None:
    assert output.read_bytes() == b"old artifact"
    assert list(directory.iterdir()) == [output]


@pytest.mark.parametrize(
    ("size", "accepted"),
    [
        (_NATIVE_PROJECT_MIDI_LIMIT - 1, True),
        (_NATIVE_PROJECT_MIDI_LIMIT, True),
        (_NATIVE_PROJECT_MIDI_LIMIT + 1, False),
    ],
)
def test_native_project_midi_sparse_file_limit(tmp_path, size, accepted) -> None:
    from libsonare import cli

    source = tmp_path / "sparse-input.bin"
    with source.open("wb") as fh:
        fh.truncate(size)

    if accepted:
        assert len(cli._read_bounded(str(source), _NATIVE_PROJECT_MIDI_LIMIT)) == size
    else:
        with pytest.raises(ValueError, match="67108864 byte limit"):
            cli._read_bounded(str(source), _NATIVE_PROJECT_MIDI_LIMIT)


def _artifact_writers(source: Path, destination: Path) -> dict[str, Any]:
    """One call per kind of user-named artifact the CLI writes."""
    from libsonare import cli

    return {
        "wav": lambda: cli._write_wav(str(destination), [0.25] * 16, 22_050),
        "bytes": lambda: cli._atomic_write_bytes(str(destination), b"payload"),
        "project": lambda: cli.cmd_project(
            cli._build_parser().parse_args(["project", "new", "-o", str(destination)])
        ),
        "mastering-report": lambda: cli.cmd_mastering(
            cli._build_parser().parse_args(
                ["mastering", str(source), "--report", str(destination), "--json"]
            )
        ),
    }


@pytest.mark.parametrize("artifact", ["wav", "bytes", "project", "mastering-report"])
def test_every_artifact_writer_classifies_a_failed_write(tmp_path, capsys, artifact) -> None:
    """No writer's OSError reaches the generic error code.

    The destination is an existing directory, the commonest instance of a
    refused write: nothing rejects it until the atomic replace, and before this
    two of the four writers reported it as an unknown internal failure.
    """
    from libsonare import cli
    from libsonare._cli_common import EXIT_ENCODE_FAILED
    from libsonare._runtime import SonareError

    source = tmp_path / "tone.wav"
    cli._write_wav(str(source), [0.25, -0.25] * 512, 22_050)
    destination = tmp_path / "destination"
    destination.mkdir()

    with pytest.raises(SonareError) as raised:
        _artifact_writers(source, destination)[artifact]()
    assert cli._exit_code_for(raised.value) == EXIT_ENCODE_FAILED
    # The destination the caller named, not the scratch file the errno mentions.
    assert str(destination) in str(raised.value)
    capsys.readouterr()


def test_bounded_reader_classifies_a_path_it_cannot_read(tmp_path) -> None:
    """A directory reads as invalid format, the class the native CLI reports.

    A missing path keeps the file-not-found class, so the two refusals stay
    distinguishable.
    """
    from libsonare import cli
    from libsonare._cli_common import EXIT_FILE_NOT_FOUND, EXIT_INVALID_FORMAT
    from libsonare._runtime import SonareError

    directory = tmp_path / "not-a-file"
    directory.mkdir()
    with pytest.raises(SonareError) as raised:
        cli._read_bounded(str(directory), _NATIVE_PROJECT_MIDI_LIMIT)
    assert cli._exit_code_for(raised.value) == EXIT_INVALID_FORMAT

    with pytest.raises(FileNotFoundError) as missing:
        cli._read_bounded(str(tmp_path / "absent.json"), _NATIVE_PROJECT_MIDI_LIMIT)
    assert cli._exit_code_for(missing.value) == EXIT_FILE_NOT_FOUND


class _TemporaryFileProxy:
    def __init__(self, raw: Any, *, fail_write: bool, fail_close: bool) -> None:
        self._raw = raw
        self._fail_write = fail_write
        self._fail_close = fail_close

    def __enter__(self) -> _TemporaryFileProxy:
        return self

    def __exit__(self, exc_type, exc, traceback) -> bool:
        self._raw.close()
        if self._fail_close:
            raise OSError("injected close failure")
        return False

    def write(self, data: bytes) -> int:
        if self._fail_write:
            raise OSError("injected write failure")
        return self._raw.write(data)

    def __getattr__(self, name: str) -> Any:
        return getattr(self._raw, name)


@pytest.mark.parametrize("stage", ["write", "close", "replace"])
def test_atomic_byte_writer_preserves_old_output_at_every_failure_stage(
    monkeypatch, tmp_path, stage
) -> None:
    import libsonare._cli_common as implementation
    from libsonare import cli
    from libsonare._cli_common import EXIT_ENCODE_FAILED
    from libsonare._runtime import SonareError

    output = tmp_path / "result.bin"
    output.write_bytes(b"old artifact")
    original_temporary_file = implementation.tempfile.NamedTemporaryFile

    if stage in {"write", "close"}:

        def temporary_file(*args, **kwargs):
            raw = original_temporary_file(*args, **kwargs)
            return _TemporaryFileProxy(
                raw, fail_write=stage == "write", fail_close=stage == "close"
            )

        monkeypatch.setattr(implementation.tempfile, "NamedTemporaryFile", temporary_file)
    else:
        monkeypatch.setattr(
            implementation.os,
            "replace",
            lambda _source, _target: (_ for _ in ()).throw(OSError("injected replace failure")),
        )

    # The byte writer carries the same contract as the WAV writer: every stage
    # is a stage of producing the output file, so all three report the encode
    # class instead of the generic error code a bare OSError landed on, and the
    # message names the destination the caller asked for rather than the scratch
    # file the errno mentions.
    with pytest.raises(SonareError, match=f"injected {stage} failure") as raised:
        cli._atomic_write_bytes(str(output), b"new artifact")
    assert cli._exit_code_for(raised.value) == EXIT_ENCODE_FAILED
    assert str(output) in str(raised.value)

    _assert_only_old_output(tmp_path, output)


class _WaveCloseProxy:
    def __init__(self, wav: Any) -> None:
        self._wav = wav

    def close(self) -> None:
        self._wav.close()
        raise OSError("injected close failure")

    def __getattr__(self, name: str) -> Any:
        return getattr(self._wav, name)


@pytest.mark.parametrize("stage", ["write", "close", "replace"])
def test_atomic_wav_writer_preserves_old_output_at_every_failure_stage(
    monkeypatch, tmp_path, stage
) -> None:
    import libsonare._cli_common as implementation
    from libsonare import cli
    from libsonare._cli_common import EXIT_ENCODE_FAILED
    from libsonare._runtime import SonareError

    output = tmp_path / "result.wav"
    output.write_bytes(b"old artifact")

    if stage == "write":
        monkeypatch.setattr(
            implementation,
            "_write_wav_mono_frames",
            lambda *_args: (_ for _ in ()).throw(OSError("injected write failure")),
        )
    elif stage == "close":
        original_wave_open = wave.open

        def failing_close_wave(*args, **kwargs):
            return _WaveCloseProxy(original_wave_open(*args, **kwargs))

        monkeypatch.setattr(wave, "open", failing_close_wave)
    else:
        monkeypatch.setattr(
            implementation.os,
            "replace",
            lambda _source, _target: (_ for _ in ()).throw(OSError("injected replace failure")),
        )

    # Every stage here is a stage of producing the output file, so all three
    # report the encode class rather than the generic error code the bare OSError
    # used to land on. The native CLI reports the same class for the same
    # condition, which is what lets a script branch on it whichever CLI it calls.
    with pytest.raises(SonareError, match=f"injected {stage} failure") as raised:
        cli._write_wav(str(output), [0.25] * 10, 48000)
    assert cli._exit_code_for(raised.value) == EXIT_ENCODE_FAILED

    _assert_only_old_output(tmp_path, output)


def _read_codes(path: Path, bits_per_sample: int) -> tuple[int, ...]:
    """Every PCM code in a WAV, signed, at either sample width."""
    width = bits_per_sample // 8
    with wave.open(str(path), "rb") as wav:
        assert wav.getsampwidth() == width
        frames = wav.readframes(wav.getnframes())
    return tuple(
        int.from_bytes(frames[offset : offset + width], "little", signed=True)
        for offset in range(0, len(frames), width)
    )


@pytest.mark.parametrize(
    ("bits", "full_scale", "half_code"),
    [(16, 32768, 16384), (24, 8388608, 4194304)],
)
@pytest.mark.parametrize(
    ("label", "sample"),
    [
        ("nan", float("nan")),
        ("positive-infinity", float("inf")),
        ("negative-infinity", float("-inf")),
    ],
)
def test_wav_writer_writes_a_non_finite_sample_as_silence_like_the_core(
    tmp_path, bits, full_scale, half_code, label, sample
) -> None:
    """A sample with no PCM image reaches the file as silence, as in the core.

    ``float_to_pcm16`` / ``float_to_pcm24`` (audio_io.cpp) guard on
    ``std::isfinite`` before the clamp. A clamp alone cannot stand in for that:
    every comparison against a non-finite value is false, so ``std::min``
    returns its other argument and NaN and +Inf reach the file at positive full
    scale, indistinguishable from a peak the encoder meant to produce, -Inf at
    negative full scale.
    """
    from libsonare import cli

    output = tmp_path / f"{label}-{bits}.wav"
    cli._write_wav(str(output), [sample, 0.5], 48000, bits)

    codes = _read_codes(output, bits)
    assert codes[0] == 0, (
        f"{label} was written as {codes[0]} rather than silence; "
        f"an unguarded clamp emits +/-{full_scale}"
    )
    # The guard substitutes the one sample, not the block around it.
    assert codes[1] == half_code


def test_wav_export_completes_over_a_sample_no_32_bit_float_can_hold(tmp_path) -> None:
    """One unrepresentable sample costs one sample, not the file.

    The core's guard runs on the 32-bit sample it was handed, so a Python float
    the narrowing turns into an infinity has no PCM image either and is
    substituted like any other. This is the failure with the larger blast
    radius: a substitution loses one sample, while packing that raises loses
    every frame the caller asked for. A magnitude that still fits keeps clamping
    to full scale.

    Helper robustness, and deliberately not a CLI contract case: every sample a
    command hands these writers is a core ``float`` widened to a Python float,
    so no argument reaches this branch. Its absence from the cross-surface gate
    is the measurement, not a gap in it.
    """
    from libsonare import cli

    samples = [0.25, 0.5] * 64 + [1e39, -1e39, 3.4e38, 0.5] + [0.25, 0.5] * 64
    output = tmp_path / "overflow.wav"
    cli._write_wav(str(output), samples, 48000)

    codes = _read_codes(output, 16)
    assert len(codes) == len(samples)
    assert codes[128:132] == (0, 0, 32767, 16384)
    assert set(codes[:128]) == set(codes[132:]) == {8192, 16384}


@pytest.mark.parametrize(
    ("bits", "expected"),
    [(16, (32767, -32768, 32735, 0)), (24, (8388607, -8388608, 8380220, 0))],
)
def test_wav_writer_keeps_finite_codes_at_and_below_full_scale(tmp_path, bits, expected) -> None:
    """Silencing a non-finite sample must not move any finite one.

    The inputs sit on the PCM grid, so the stream is written code for code.
    """
    from libsonare import cli

    output = tmp_path / f"finite-{bits}.wav"
    near_full_scale = expected[2] / (32768 if bits == 16 else 8388608)
    cli._write_wav(str(output), [1.0, -1.0, near_full_scale, 0.0], 48000, bits)

    assert _read_codes(output, bits) == expected


def test_stereo_and_bounce_writers_share_the_non_finite_substitution(tmp_path) -> None:
    """Both multi-channel writers reach the same quantizer as the mono one."""
    from libsonare import cli
    from libsonare._cli_common import _write_project_bounce_wav

    stereo = tmp_path / "stereo.wav"
    cli._write_wav_stereo(str(stereo), [float("nan"), 0.5], [float("-inf"), -0.5], 48000)
    assert _read_codes(stereo, 16) == (0, 0, 16384, -16384)

    bounce = tmp_path / "bounce.wav"
    frames, channels = _write_project_bounce_wav(
        str(bounce), [[float("inf"), 0.25], [0.5, float("nan")]], 48000
    )
    assert (frames, channels) == (2, 2)
    assert _read_codes(bounce, 16) == (0, 8192, 16384, 0)


def test_atomic_wav_writer_refuses_an_empty_frame_count(tmp_path) -> None:
    """The native writer's ``n_frames > 0`` check (save_wav), which the Python
    WAV writers used to skip entirely."""
    import libsonare._cli_common as implementation
    from libsonare import cli
    from libsonare._cli_common import EXIT_INVALID_PARAMETER
    from libsonare._runtime import SonareError

    output = tmp_path / "empty.wav"
    with (
        pytest.raises(SonareError) as raised,
        implementation._atomic_wav_writer(str(output), 1, 48000, frames=0),
    ):
        pass
    assert cli._exit_code_for(raised.value) == EXIT_INVALID_PARAMETER
    assert not output.exists()


def test_atomic_wav_writer_refuses_a_frame_count_past_the_riff_32_bit_size_limit(tmp_path) -> None:
    """The native writer's check_riff_size_fits, mirrored for the mono/stereo path."""
    import libsonare._cli_common as implementation
    from libsonare import cli
    from libsonare._cli_common import EXIT_INVALID_PARAMETER
    from libsonare._runtime import SonareError

    output = tmp_path / "oversize.wav"
    block_align = 1 * (16 // 8)  # mono, 16-bit
    too_many_frames = (implementation._MAX_WAV_DATA_BYTES // block_align) + 1
    with (
        pytest.raises(SonareError) as raised,
        implementation._atomic_wav_writer(str(output), 1, 48000, frames=too_many_frames),
    ):
        pass
    assert cli._exit_code_for(raised.value) == EXIT_INVALID_PARAMETER
    assert not output.exists()


def test_atomic_wav_writer_accepts_the_legal_edge_of_the_riff_32_bit_size_limit(tmp_path) -> None:
    """The control: the largest frame count the limit permits is not refused."""
    import libsonare._cli_common as implementation

    output = tmp_path / "edge.wav"
    block_align = 1 * (16 // 8)  # mono, 16-bit
    max_frames = implementation._MAX_WAV_DATA_BYTES // block_align
    with implementation._atomic_wav_writer(str(output), 1, 48000, frames=max_frames):
        pass
    assert output.exists()


@pytest.mark.parametrize(
    ("writer_name", "channels", "bits"),
    [
        ("_atomic_wav_writer", 1, 16),
        ("_atomic_wav_writer", 2, 24),
        ("_atomic_wav_writer_multichannel", 6, 16),
        ("_atomic_wav_writer_multichannel", 8, 24),
    ],
)
def test_wav_writers_refuse_a_byte_rate_past_the_32_bit_header_field(
    tmp_path, writer_name: str, channels: int, bits: int
) -> None:
    """The native writer's byte-rate check, refused before a temp file exists."""
    import libsonare._cli_common as implementation
    from libsonare import cli
    from libsonare._cli_common import EXIT_INVALID_PARAMETER
    from libsonare._runtime import SonareError

    writer = getattr(implementation, writer_name)
    max_rate = 0xFFFFFFFF // (channels * (bits // 8))

    edge = tmp_path / "edge.wav"
    with writer(str(edge), channels, max_rate, bits, frames=1):
        pass
    assert edge.exists()

    output = tmp_path / "overflow.wav"
    with (
        pytest.raises(SonareError) as raised,
        writer(str(output), channels, max_rate + 1, bits, frames=1),
    ):
        pass
    assert cli._exit_code_for(raised.value) == EXIT_INVALID_PARAMETER
    assert not output.exists()
    assert list(tmp_path.iterdir()) == [edge]


def test_atomic_wav_writer_multichannel_refuses_an_empty_frame_count(tmp_path) -> None:
    """The same check on the WAVE_FORMAT_EXTENSIBLE (>2 channel) writer."""
    import libsonare._cli_common as implementation
    from libsonare import cli
    from libsonare._cli_common import EXIT_INVALID_PARAMETER
    from libsonare._runtime import SonareError

    output = tmp_path / "empty-multichannel.wav"
    with (
        pytest.raises(SonareError) as raised,
        implementation._atomic_wav_writer_multichannel(str(output), 6, 48000, frames=0),
    ):
        pass
    assert cli._exit_code_for(raised.value) == EXIT_INVALID_PARAMETER
    assert not output.exists()


def test_wav_writer_peak_memory_is_bounded_by_chunk_size(tmp_path) -> None:
    """Writer scratch memory stays nearly constant as output duration grows."""
    import libsonare._cli_common as implementation
    from libsonare import cli

    chunk = implementation._WAV_CHUNK_FRAMES
    small = [0.25] * chunk
    large = [0.25] * (chunk * 32)

    def measured_peak(path: Path, samples: list[float]) -> int:
        tracemalloc.start()
        try:
            cli._write_wav(str(path), samples, 48000)
            return tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()

    small_peak = measured_peak(tmp_path / "small.wav", small)
    large_path = tmp_path / "large.wav"
    large_peak = measured_peak(large_path, large)

    assert large_peak < small_peak * 4
    with wave.open(str(large_path), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getframerate() == 48000
        assert wav.getnframes() == len(large)
        assert wav.readframes(1) == b"\x00 "


@pytest.mark.parametrize("bits", [16, 24])
def test_vectorized_quantizer_matches_the_scalar_one_over_random_and_edge_samples(
    bits: int,
) -> None:
    """``_quantize_codes`` must answer exactly what ``_quantize_sample`` does.

    ``_pcm_bytes`` (the WAV writers' fast path) and ``_pcm16``/``_pcm24`` (the
    scalar functions the native writer's byte-exactness was pinned against,
    see ``test_wav_writer_keeps_finite_codes_at_and_below_full_scale``) must
    stay two spellings of the one quantizer, not two quantizers that happen to
    agree on the samples a fixture thought to try. numpy's random generator is
    seeded so a real disagreement reproduces the same way twice.
    """
    import numpy as np

    from libsonare._cli_common import _quantize_codes, _quantize_sample

    full_scale, minimum, maximum = (
        (32768.0, -32768, 32767)
        if bits == 16
        else (
            8388608.0,
            -8388608,
            8388607,
        )
    )
    rng = np.random.default_rng(20260928)
    half_lsb_ties = np.array([-2.5, -1.5, -0.5, 0.5, 1.5, 2.5], dtype=np.float32) / np.float32(
        full_scale
    )
    tie_neighbors = np.concatenate(
        [
            np.nextafter(half_lsb_ties, np.float32(-np.inf)),
            half_lsb_ties,
            np.nextafter(half_lsb_ties, np.float32(np.inf)),
        ]
    )
    boundary_codes = np.array(
        [minimum - 1, minimum, minimum + 1, -2, -1, 0, 1, 2, maximum - 1, maximum, maximum + 1],
        dtype=np.int32,
    )
    samples = np.concatenate(
        [
            rng.uniform(-1.5, 1.5, 20000).astype(np.float32),
            boundary_codes.astype(np.float32) / np.float32(full_scale),
            tie_neighbors,
            np.array(
                [
                    0.0,
                    -0.0,
                    1.0,
                    -1.0,
                    1.25,
                    -1.25,
                    float("nan"),
                    float("inf"),
                    float("-inf"),
                    np.finfo(np.float32).max,
                    -np.finfo(np.float32).max,
                ],
                dtype=np.float32,
            ),
        ]
    )

    vectorized = _quantize_codes(samples, full_scale, minimum, maximum)
    scalar = np.fromiter(
        (_quantize_sample(float(sample), full_scale, minimum, maximum) for sample in samples),
        dtype=vectorized.dtype,
        count=samples.size,
    )
    np.testing.assert_array_equal(vectorized, scalar)


@pytest.mark.parametrize("bits", [16, 24])
def test_vectorized_quantizer_matches_integer_grid_clamp(bits: int) -> None:
    """Check every code and both adjacent out-of-range codes against integer clamping."""
    import numpy as np

    from libsonare._cli_common import _quantize_codes

    full_scale, minimum, maximum = (
        (32768.0, -32768, 32767)
        if bits == 16
        else (
            8388608.0,
            -8388608,
            8388607,
        )
    )
    integer_codes = np.arange(-int(full_scale) - 1, int(full_scale) + 2, dtype=np.int32)
    # Integer codes in this range are exactly representable in float32, and
    # division by the power-of-two scale places each input on its PCM grid.
    samples = integer_codes.astype(np.float32) / np.float32(full_scale)
    expected = np.clip(integer_codes, minimum, maximum)

    vectorized = _quantize_codes(samples, full_scale, minimum, maximum)
    np.testing.assert_array_equal(vectorized, expected)


@pytest.mark.parametrize("bits", [16, 24])
@pytest.mark.parametrize("channels", [1, 2, 4])
def test_vectorized_quantizer_matches_the_scalar_one_with_dither(bits: int, channels: int) -> None:
    """The dithered vector path and the scalar path pack identical bytes.

    Starts at a non-zero frame so the (channel, absolute frame) noise indexing
    is exercised, and includes non-finite and out-of-range samples.
    """
    import numpy as np

    from libsonare._cli_common import _PcmQuantizer

    rng = np.random.default_rng(7)
    frames = 3000
    samples = rng.uniform(-1.2, 1.2, frames * channels)
    samples[::97] = float("nan")
    samples[5::211] = float("inf")
    quantizer = _PcmQuantizer(bits, channels, dither=True)
    first_frame = 8192 * 3 + 5
    vectorized = quantizer.pack(samples, first_frame)
    scalar = b"".join(
        quantizer.pack_scalar(float(value), i % channels, first_frame + i // channels)
        for i, value in enumerate(samples)
    )
    assert vectorized == scalar
    # Dither is a function of (channel, frame): a chunk split mid-stream agrees.
    half = (frames // 2) * channels
    split = quantizer.pack(samples[:half], first_frame) + quantizer.pack(
        samples[half:], first_frame + frames // 2
    )
    assert split == vectorized


def test_interleave_planes_matches_the_manual_frame_major_order() -> None:
    from libsonare._cli_playback import _interleave_planes

    planes = [[1.0, 2.0, 3.0], [10.0, 20.0, 30.0], [100.0, 200.0, 300.0]]
    interleaved = _interleave_planes(planes)
    assert list(interleaved) == [1.0, 10.0, 100.0, 2.0, 20.0, 200.0, 3.0, 30.0, 300.0]
    assert interleaved.dtype.name == "float32"


def test_interleave_planes_of_nothing_is_empty() -> None:
    from libsonare._cli_playback import _interleave_planes

    assert list(_interleave_planes([])) == []


def test_frame_major_wav_writer_handles_ten_seconds_of_six_channel_audio_quickly(
    tmp_path,
) -> None:
    """The acceptance floor this finding names: 10 s of 48 kHz 6-ch under 1 s.

    Not a tight microbenchmark -- CI machines vary -- but two orders of
    magnitude below the pure-Python per-sample loop this replaced, which took
    tens of seconds for the same input on a fast machine.
    """
    import time

    import numpy as np

    from libsonare._cli_common import _write_frame_major_wav

    sample_rate = 48000
    channels = 6
    frames = sample_rate * 10
    rng = np.random.default_rng(1)
    audio = rng.uniform(-0.5, 0.5, (frames, channels)).astype(np.float32)

    output = tmp_path / "six-channel.wav"
    started = time.monotonic()
    written_frames, written_channels = _write_frame_major_wav(str(output), audio, sample_rate)
    elapsed = time.monotonic() - started

    assert (written_frames, written_channels) == (frames, channels)
    assert elapsed < 1.0, f"took {elapsed:.2f}s, over the 1s floor this finding names"
    # 6 channels is WAVE_FORMAT_EXTENSIBLE (matching the native CLI's own
    # writer for anything past stereo), which stdlib `wave` cannot open --
    # read nChannels from the fmt chunk directly instead, the same way
    # test_cli_project_bounce_channels.py's own multichannel checks do.
    with open(output, "rb") as handle:
        header = handle.read(24)
    assert struct.unpack("<H", header[22:24])[0] == channels
    assert output.stat().st_size >= frames * channels * 2  # 16-bit PCM, plus header


# ---------------------------------------------------------------------------
# TPDF dither on write (recipe: src/core/audio_io.cpp pack_pcm_bytes).
# ---------------------------------------------------------------------------

_CHUNK = 8192


def _data_chunk(path: Path) -> bytes:
    """The raw ``data`` chunk payload of a WAV (plain PCM or EXTENSIBLE)."""
    blob = path.read_bytes()
    offset = 12
    while offset + 8 <= len(blob):
        tag = blob[offset : offset + 4]
        size = struct.unpack("<I", blob[offset + 4 : offset + 8])[0]
        if tag == b"data":
            return blob[offset + 8 : offset + 8 + size]
        offset += 8 + size + (size & 1)
    raise AssertionError("no data chunk")


def _reference_pcm(samples, channels: int, bits: int) -> bytes:
    """Spec-literal transcription of the native recipe, independent of the module."""
    import math

    def f32(x: float) -> float:
        return struct.unpack("<f", struct.pack("<f", x))[0]

    mask = (1 << 64) - 1
    gamma = 0x9E3779B97F4A7C15

    def at(seed: int, k: int) -> int:
        z = (seed + (k + 1) * gamma) & mask
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & mask
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
        return z ^ (z >> 31)

    scale = 32768.0 if bits == 16 else 8388608.0
    lo, hi = -int(scale), int(scale) - 1
    narrowed = [f32(float(x)) for x in samples]
    on_grid = all(
        f32(max(-1.0, min(1.0, x))) * scale == math.floor(f32(max(-1.0, min(1.0, x))) * scale)
        for x in narrowed
        if math.isfinite(x)
    )
    out = bytearray()
    for i, x in enumerate(narrowed):
        c, n = i % channels, i // channels
        if not math.isfinite(x) or x == 0.0:
            code = 0
        else:
            tpdf = 0.0
            if not on_grid:
                seed = 0x50434D4449544852 ^ ((c * gamma) & mask)
                tpdf = (at(seed, 2 * n) >> 40) * 2.0**-24 - (at(seed, 2 * n + 1) >> 40) * 2.0**-24
            v = f32(f32(max(-1.0, min(1.0, x)) * scale) + tpdf)
            code = int(math.floor(v + 0.5)) if v >= 0 else int(math.ceil(v - 0.5))
            code = max(lo, min(hi, code))
        out += code.to_bytes(bits // 8, "little", signed=True)
    return bytes(out)


def _noise(frames: int, channels: int, seed: int = 11):
    import numpy as np

    return np.random.default_rng(seed).uniform(-1.1, 1.1, (frames, channels))


@pytest.mark.parametrize("bits", [16, 24])
def test_on_grid_input_round_trips_code_for_code_without_dither(tmp_path, bits) -> None:
    import numpy as np

    from libsonare import cli
    from libsonare._cli_common import _write_frame_major_wav

    scale = 32768 if bits == 16 else 8388608
    frames = _CHUNK + 777
    codes = np.random.default_rng(3).integers(-scale, scale, frames * 2)
    values = codes / scale

    mono = tmp_path / "mono.wav"
    cli._write_wav(str(mono), values[:frames], 48000, bits)
    assert _read_codes(mono, bits) == tuple(int(c) for c in codes[:frames])

    stereo = tmp_path / "stereo.wav"
    cli._write_wav_stereo(str(stereo), values[:frames], values[frames:], 48000, bits)
    expected = np.empty(frames * 2, dtype=np.int64)
    expected[0::2], expected[1::2] = codes[:frames], codes[frames:]
    assert _read_codes(stereo, bits) == tuple(int(c) for c in expected)

    multi = tmp_path / "multi.wav"
    whole = len(codes) // 4 * 4
    _write_frame_major_wav(str(multi), values[:whole].reshape(-1, 4), 48000, bits)
    assert _data_chunk(multi) == b"".join(
        int(c).to_bytes(bits // 8, "little", signed=True) for c in codes[:whole]
    )


@pytest.mark.parametrize("bits", [16, 24])
@pytest.mark.parametrize("channels", [1, 2, 4])
def test_off_grid_input_matches_the_dither_recipe_across_chunks(tmp_path, bits, channels) -> None:
    """Writer bytes equal the spec transcription for a stream crossing chunk bounds."""
    from libsonare import cli
    from libsonare._cli_common import _write_frame_major_wav

    frames = _CHUNK * 2 + 123
    audio = _noise(frames, channels)
    output = tmp_path / "out.wav"
    if channels == 1:
        cli._write_wav(str(output), audio[:, 0], 48000, bits)
    elif channels == 2:
        cli._write_wav_stereo(str(output), audio[:, 0], audio[:, 1], 48000, bits)
    else:
        _write_frame_major_wav(str(output), audio, 48000, bits)
    assert _data_chunk(output) == _reference_pcm(audio.ravel(), channels, bits)


@pytest.mark.parametrize("bits", [16, 24])
def test_dither_decision_covers_the_whole_stream(tmp_path, bits) -> None:
    """One off-grid sample in the last chunk dithers the on-grid chunks before it."""
    import numpy as np

    from libsonare import cli

    frames = _CHUNK * 2 + 10
    scale = 32768 if bits == 16 else 8388608
    values = np.full(frames, 3 / scale)  # on the grid, and not silence, which stays undithered
    values[-1] = 0.1234567
    output = tmp_path / "late.wav"
    cli._write_wav(str(output), values, 48000, bits)
    assert _data_chunk(output) == _reference_pcm(values, 1, bits)
    assert any(code != 3 for code in _read_codes(output, bits)[:_CHUNK])


@pytest.mark.parametrize("bits", [16, 24])
def test_exact_silence_stays_undithered_in_a_dithered_stream(tmp_path, bits) -> None:
    import numpy as np

    from libsonare import cli

    values = np.zeros(_CHUNK + 100)
    values[_CHUNK:] = 0.1234567
    output = tmp_path / "silence.wav"
    cli._write_wav(str(output), values, 48000, bits)
    assert _data_chunk(output) == _reference_pcm(values, 1, bits)
    assert not any(_read_codes(output, bits)[:_CHUNK])


@pytest.mark.parametrize("bits", [16, 24])
@pytest.mark.parametrize("on_grid", [True, False])
def test_ragged_rows_agree_with_the_ndarray_fast_path(tmp_path, bits, on_grid) -> None:
    import numpy as np

    from libsonare._cli_common import _write_frame_major_wav

    frames = _CHUNK + 300
    if on_grid:
        scale = 32768 if bits == 16 else 8388608
        audio = np.random.default_rng(5).integers(-scale, scale, (frames, 3)) / scale
    else:
        audio = _noise(frames, 3, seed=9)
    ragged = [list(map(float, row)) for row in audio]
    ragged[10] = ragged[10][:1]
    ragged[_CHUNK + 5] = ragged[_CHUNK + 5][:2]
    padded = audio.copy()
    padded[10, 1:] = 0.0
    padded[_CHUNK + 5, 2:] = 0.0

    fast = tmp_path / "fast.wav"
    slow = tmp_path / "slow.wav"
    _write_frame_major_wav(str(fast), padded, 48000, bits)
    _write_frame_major_wav(str(slow), ragged, 48000, bits)
    assert _data_chunk(fast) == _data_chunk(slow)


def _native_cli() -> Path | None:
    import os

    root = Path(__file__).resolve().parents[3]
    candidates = [os.environ.get("SONARE_NATIVE_CLI"), str(root / "build/bin/sonare-cli")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    return None


def _write_pcm16_input(path: Path, channels: int, frames: int) -> None:
    import numpy as np

    rng = np.random.default_rng(channels)
    t = np.arange(frames) / 22050.0
    planes = [
        0.3 * np.sin(2 * np.pi * (110.0 * (c + 1)) * t) + 0.02 * rng.standard_normal(frames)
        for c in range(channels)
    ]
    pcm = np.clip(np.stack(planes, axis=1) * 32767.0, -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(22050)
        wav.writeframes(pcm.tobytes())


@pytest.mark.parametrize(
    ("channels", "command"),
    [
        (1, ["normalize", "--target-db", "-3"]),
        (2, ["normalize", "--target-db", "-3"]),
        (6, ["playback"]),
    ],
)
def test_python_cli_wav_data_matches_the_native_cli_byte_for_byte(
    tmp_path, channels, command
) -> None:
    """Both front ends write one rendering of the same input: dithered bytes agree.

    The native writer is not reachable in-process, so the comparison goes through
    the native CLI binary. 24-bit output has no native CLI route that avoids
    other processing; it is covered by the recipe transcription above.
    """
    import subprocess
    import sys

    native = _native_cli()
    if native is None:
        pytest.skip("native sonare-cli binary not found; set SONARE_NATIVE_CLI")
    source = tmp_path / "in.wav"
    _write_pcm16_input(source, channels, _CHUNK + 5000)
    outputs = {}
    for name, argv in (
        ("native", [str(native)]),
        ("python", [sys.executable, "-m", "libsonare.cli"]),
    ):
        out = tmp_path / f"{name}.wav"
        result = subprocess.run(
            [*argv, command[0], str(source), "-o", str(out), *command[1:]],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stderr
        outputs[name] = _data_chunk(out)
    assert len(outputs["native"]) > 0
    assert outputs["python"] == outputs["native"]
