"""Bounded and failure-atomic CLI file-boundary tests."""

from __future__ import annotations

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
            lambda _wav, _samples: (_ for _ in ()).throw(OSError("injected write failure")),
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


def test_wav_writer_saturates_non_finite_samples_like_the_core(tmp_path) -> None:
    """NaN and infinities saturate to full scale instead of raising.

    The core's WAV writer clamps with ``std::max(-1, std::min(1, x))``, where
    every comparison against NaN is false, so NaN and +Inf land on +1.0 and
    -Inf on -1.0. A writer that lets NaN reach ``int(round(...))`` raises
    instead, turning a hostile sample into a failed export.
    """
    import struct

    from libsonare import cli

    samples = [float("nan"), float("inf"), float("-inf"), 0.5, 0.0]
    output = tmp_path / "non_finite.wav"
    cli._write_wav(str(output), samples, 48000)

    with wave.open(str(output), "rb") as wav:
        assert wav.getnframes() == len(samples)
        frames = wav.readframes(wav.getnframes())
    written = struct.unpack("<" + "h" * len(samples), frames)
    assert written == (32767, 32767, -32767, 16384, 0)


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
