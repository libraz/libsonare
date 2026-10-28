"""Focused probes for the Python CLI's decode reuse and patch seams."""

from __future__ import annotations

import errno
import wave
from types import SimpleNamespace

import numpy as np
import pytest

from libsonare import _cli_common as common
from libsonare import audio, cli
from libsonare._errors import SonareError, SonareValueError


class _FakeAudio:
    def __init__(self, samples: list[float], sample_rate: int) -> None:
        self.data = np.asarray(samples, dtype=np.float32)
        self.sample_rate = sample_rate

    def __enter__(self) -> _FakeAudio:
        return self

    def __exit__(self, *_args: object) -> None:
        return None


@pytest.mark.parametrize("channel_count", [2, 6, 8])
def test_known_channel_decode_is_one_ordered_shared_decode(
    monkeypatch, tmp_path, channel_count: int
) -> None:
    """Known N-channel input decodes once and exposes source-ordered row views."""
    path = tmp_path / "input.audio"
    path.write_bytes(b"encoded audio")
    decoded = np.arange(channel_count * 4, dtype=np.float32).reshape(channel_count, 4)
    decode_calls: list[bytes] = []

    monkeypatch.setattr(common, "_source_channel_count", lambda _path: channel_count)

    def decode(data: bytes) -> tuple[np.ndarray, int]:
        decode_calls.append(data)
        return decoded, 48_000

    monkeypatch.setattr(audio, "decode_channels", decode)
    monkeypatch.setattr(
        audio.Audio,
        "from_file_channel",
        classmethod(lambda _cls, _path, _index: pytest.fail("channel-by-channel decode")),
    )

    planes, sample_rate = common._load_audio_channels(str(path))

    assert sample_rate == 48_000
    assert len(decode_calls) == 1
    assert decode_calls[0] == b"encoded audio"
    assert len(planes) == channel_count
    assert [plane.tolist() for plane in planes] == decoded.tolist()
    assert all(plane.flags.c_contiguous for plane in planes)
    assert all(np.shares_memory(plane, decoded) for plane in planes)
    planes[0][0] = -99.0
    assert planes[1][0] == decoded[1, 0]


def test_mono_and_unknown_channel_paths_keep_audio_from_file(monkeypatch, capsys, tmp_path) -> None:
    """Mono and an advisory probe failure use the mono decoder path."""
    path = str(tmp_path / "input.wav")
    from_file_calls: list[str] = []

    monkeypatch.setattr(audio.Audio, "file_channel_count", classmethod(lambda _cls, _path: 1))

    def from_file(_cls, file_path: str) -> _FakeAudio:
        from_file_calls.append(file_path)
        return _FakeAudio([0.25, -0.5], 44_100)

    monkeypatch.setattr(audio.Audio, "from_file", classmethod(from_file))
    samples, sample_rate = common._load_audio(path)
    assert samples.tolist() == [0.25, -0.5]
    assert sample_rate == 44_100
    assert from_file_calls == [path]
    assert capsys.readouterr().err == ""

    probe_calls: list[str] = []

    def failed_probe(_cls, file_path: str) -> int:
        probe_calls.append(file_path)
        raise RuntimeError("optional decoder probe failed")

    monkeypatch.setattr(audio.Audio, "file_channel_count", classmethod(failed_probe))
    from_file_calls.clear()
    samples, sample_rate = common._load_audio(path)
    assert samples.tolist() == [0.25, -0.5]
    assert sample_rate == 44_100
    assert probe_calls == [path]
    assert from_file_calls == [path]
    assert capsys.readouterr().err == ""


def test_downmix_known_nonstereo_reuses_probe_and_keeps_warning(
    monkeypatch, capsys, tmp_path
) -> None:
    path = tmp_path / "surround.wav"
    path.write_bytes(b"encoded")
    from_file_calls: list[str] = []
    monkeypatch.setattr(common, "_source_channel_count", lambda _path: 6)

    def from_file(_cls, file_path: str) -> _FakeAudio:
        from_file_calls.append(file_path)
        return _FakeAudio([0.1, 0.2], 48_000)

    monkeypatch.setattr(audio.Audio, "from_file", classmethod(from_file))
    planes, sample_rate = common._load_channels_or_downmix(str(path))

    assert planes[0].tolist() == pytest.approx([0.1, 0.2])
    assert sample_rate == 48_000
    assert from_file_calls == [str(path)]
    assert capsys.readouterr().err.splitlines() == [
        "warning: 6-channel input is downmixed to mono by this CLI command; "
        "use the stereo library API for channel-preserving processing"
    ]


def test_wrapping_the_facade_loader_does_not_recurse(monkeypatch, tmp_path) -> None:
    """A spy that calls the real loader runs it once; no loader dispatches on patches."""
    path = str(tmp_path / "input.wav")
    monkeypatch.setattr(common, "_source_channel_count", lambda _path: 1)
    monkeypatch.setattr(
        audio.Audio,
        "from_file",
        classmethod(lambda _cls, _path: _FakeAudio([0.5], 22_050)),
    )
    real_loader = cli._load_audio
    calls: list[str] = []

    def spy(file_path: str) -> tuple[list[float], int]:
        calls.append(file_path)
        return real_loader(file_path)

    monkeypatch.setattr(cli, "_load_audio", spy)
    samples, sample_rate = cli._load_audio(path)
    assert samples.tolist() == [0.5]
    assert sample_rate == 22_050
    assert calls == [path]

    planes, sample_rate = common._load_channels_or_downmix(path)
    assert planes[0].tolist() == [0.5]
    assert calls == [path]


def test_stereo_planes_from_a_real_wav_equal_the_per_channel_decode(tmp_path) -> None:
    """The in-memory stereo decode and the per-channel path loader agree sample for sample."""
    path = tmp_path / "stereo.wav"
    frames = 512
    left = np.round(8000 * np.sin(0.05 * np.arange(frames))).astype("<i2")
    right = np.round(-6000 * np.sin(0.11 * np.arange(frames))).astype("<i2")
    with wave.open(str(path), "wb") as writer:
        writer.setnchannels(2)
        writer.setsampwidth(2)
        writer.setframerate(44_100)
        writer.writeframes(np.stack([left, right], axis=1).tobytes())

    decoded, sample_rate = audio.decode_channels(path.read_bytes())
    assert sample_rate == 44_100
    assert decoded.shape == (2, frames)
    for index in range(2):
        with audio.Audio.from_file_channel(str(path), index) as plane:
            np.testing.assert_array_equal(decoded[index], plane.data)

    planes, cli_rate = common._load_audio_channels(str(path))
    assert cli_rate == 44_100
    for index in range(2):
        np.testing.assert_array_equal(planes[index], decoded[index])


def test_known_regular_oversize_preserves_native_error(monkeypatch, tmp_path) -> None:
    path = str(tmp_path / "oversize.wav")
    monkeypatch.setattr(
        common,
        "os",
        SimpleNamespace(
            stat=lambda _path: SimpleNamespace(st_size=common._MAX_AUDIO_FILE_BYTES + 1)
        ),
    )

    with pytest.raises(SonareValueError) as caught:
        common._read_audio_bounded(path)

    assert caught.value.code == common.SONARE_ERROR_INVALID_PARAMETER
    assert str(caught.value) == (
        f"File too large: {common._MAX_AUDIO_FILE_BYTES + 1} bytes "
        f"(max: {common._MAX_AUDIO_FILE_BYTES} bytes)"
    )


def test_streaming_oversize_uses_64k_chunks_and_stream_error(monkeypatch, tmp_path) -> None:
    path = str(tmp_path / "input.fifo")
    chunk_limit = 2 * common._AUDIO_READ_CHUNK_BYTES
    monkeypatch.setattr(common, "_MAX_AUDIO_FILE_BYTES", chunk_limit)
    monkeypatch.setattr(
        common, "os", SimpleNamespace(stat=lambda _path: SimpleNamespace(st_size=0))
    )

    class FakeStream:
        def __init__(self) -> None:
            self.remaining = chunk_limit + 1
            self.read_sizes: list[int] = []
            self.closed = False

        def __enter__(self) -> FakeStream:
            return self

        def __exit__(self, *_args: object) -> None:
            self.closed = True

        def read(self, size: int) -> bytes:
            self.read_sizes.append(size)
            count = min(size, self.remaining)
            self.remaining -= count
            return b"x" * count

    stream = FakeStream()
    monkeypatch.setattr("builtins.open", lambda _path, _mode: stream)

    with pytest.raises(SonareValueError) as caught:
        common._read_audio_bounded(path)

    assert caught.value.code == common.SONARE_ERROR_INVALID_PARAMETER
    assert str(caught.value) == f"File too large: exceeds {chunk_limit} bytes"
    assert stream.read_sizes == [common._AUDIO_READ_CHUNK_BYTES] * 3
    assert stream.closed


@pytest.mark.parametrize(
    "open_error",
    [
        PermissionError(errno.EACCES, "permission denied"),
        OSError(errno.EIO, "I/O error while opening"),
    ],
    ids=["permission", "open-eio"],
)
def test_open_oserrors_keep_native_open_error(monkeypatch, tmp_path, open_error: OSError) -> None:
    """Open-time OSErrors have native FileNotFound/Cannot-open semantics."""
    path = str(tmp_path / "input.wav")

    def fail_open(_path: str, _mode: str) -> object:
        raise open_error

    monkeypatch.setattr("builtins.open", fail_open)

    with pytest.raises(SonareError) as caught:
        common._read_audio_bounded(path)

    assert caught.value.code == common.SONARE_ERROR_FILE_NOT_FOUND
    assert str(caught.value) == f"[{caught.value.code}] Cannot open file: {path}"


def test_read_oserror_keeps_native_read_error(monkeypatch, tmp_path) -> None:
    """An OSError raised by an already-open stream remains DecodeFailed."""
    path = str(tmp_path / "input.wav")

    class ReadErrorStream:
        def __enter__(self) -> ReadErrorStream:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _size: int) -> bytes:
            raise OSError(errno.EIO, "I/O error while reading")

    monkeypatch.setattr("builtins.open", lambda _path, _mode: ReadErrorStream())

    with pytest.raises(SonareError) as caught:
        common._read_audio_bounded(path)

    assert caught.value.code == common.SONARE_ERROR_DECODE_FAILED
    assert str(caught.value) == f"[{caught.value.code}] Failed to read file: {path}"
