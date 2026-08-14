"""Source-channel probing and vectorized ``info`` CLI contracts."""

from __future__ import annotations

import json
import math
import wave
from types import SimpleNamespace

import numpy as np
import pytest

from libsonare import Audio, _cli_analysis
from libsonare import audio as audio_module
from libsonare._runtime import SonareError

from ._helpers import LIB_AVAILABLE


def _source_probe_available() -> bool:
    if not LIB_AVAILABLE:
        return False
    try:
        from libsonare._runtime import _get_lib

        return hasattr(_get_lib(), "sonare_audio_file_channel_count")
    except Exception:
        return False


SOURCE_PROBE_AVAILABLE = _source_probe_available()


def _write_wav(path, *, channels: int, frames: int = 32, sample_rate: int = 22050) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(b"\x00\x00" * channels * frames)


@pytest.mark.skipif(not SOURCE_PROBE_AVAILABLE, reason="native source-channel probe unavailable")
def test_audio_file_channel_count_probes_source_without_changing_mono_audio(tmp_path) -> None:
    source = tmp_path / "stereo.wav"
    _write_wav(source, channels=2)

    assert Audio.file_channel_count(str(source)) == 2
    with Audio.from_file(str(source)) as audio:
        assert audio.data.ndim == 1
        assert audio.length == 32


@pytest.mark.skipif(not SOURCE_PROBE_AVAILABLE, reason="native source-channel probe unavailable")
def test_audio_file_channel_count_propagates_probe_failure(tmp_path) -> None:
    missing = tmp_path / "missing.wav"
    with pytest.raises(SonareError):
        Audio.file_channel_count(str(missing))


def test_audio_file_channel_count_reports_missing_optional_symbol(monkeypatch) -> None:
    monkeypatch.setattr(audio_module, "_get_lib", lambda: SimpleNamespace())
    with pytest.raises(RuntimeError, match="sonare_audio_file_channel_count"):
        Audio.file_channel_count("input.wav")


def test_info_json_uses_vectorized_float64_reductions_and_source_channels(monkeypatch, capsys):
    class NonIteratingArray(np.ndarray):
        def __iter__(self):
            raise AssertionError("info must use NumPy reductions, not Python iteration")

    samples = np.asarray([0.5, -0.25, 0.0], dtype=np.float32).view(NonIteratingArray)
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: (samples, 10))
    monkeypatch.setattr(Audio, "file_channel_count", lambda path: 2)

    args = SimpleNamespace(file="stereo.mp3", json=True)
    assert _cli_analysis.cmd_info(args) == 0
    payload = json.loads(capsys.readouterr().out)

    assert set(payload) == {
        "path",
        "duration",
        "sample_rate",
        "channels",
        "samples",
        "peak_db",
        "rms_db",
    }
    assert payload["channels"] == 2
    assert payload["samples"] == 3
    assert payload["duration"] == pytest.approx(0.3)
    assert payload["peak_db"] == pytest.approx(20.0 * math.log10(0.5))
    expected_rms = math.sqrt((0.5**2 + 0.25**2) / 3.0)
    assert payload["rms_db"] == pytest.approx(20.0 * math.log10(expected_rms))


def test_info_rejects_successful_zero_channel_probe(monkeypatch):
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: (np.zeros(1), 10))
    monkeypatch.setattr(Audio, "file_channel_count", lambda path: 0)

    with pytest.raises(RuntimeError, match="invalid audio channel count"):
        _cli_analysis.cmd_info(SimpleNamespace(file="broken.wav", json=True))


def test_info_preserves_minus_200_db_silence_floor(monkeypatch, capsys):
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: (np.zeros(4), 10))
    monkeypatch.setattr(Audio, "file_channel_count", lambda path: 1)

    assert _cli_analysis.cmd_info(SimpleNamespace(file="silence.wav", json=True)) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["peak_db"] == -200.0
    assert payload["rms_db"] == -200.0


def test_info_keeps_legacy_wav_fallback_when_probe_symbol_is_missing(monkeypatch, tmp_path, capsys):
    source = tmp_path / "mono.wav"
    _write_wav(source, channels=1)
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: (np.zeros(2), 10))

    def missing_probe(path):
        raise RuntimeError("loaded libsonare does not expose sonare_audio_file_channel_count")

    monkeypatch.setattr(Audio, "file_channel_count", missing_probe)

    args = SimpleNamespace(file=str(source), json=True)
    assert _cli_analysis.cmd_info(args) == 0
    assert json.loads(capsys.readouterr().out)["channels"] == 1
