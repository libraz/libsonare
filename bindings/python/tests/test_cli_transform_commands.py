"""End-to-end CLI coverage for the emphasis filters and the mel / MFCC inversions.

Each command is compared with the library call it wraps; the payload key sets are the
cross-front-end contract, so each is asserted exactly.
"""

from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path
from typing import Any

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 22050
FRAMES = SR // 2
EXIT_USAGE = 2
EXIT_INVALID_PARAMETER = 3
QUANTUM = 2e-4


def _run_cli(args: list[str]) -> subprocess.CompletedProcess[str]:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        capture_output=True,
        text=True,
        env=env,
    )


def _read_wav(path: Path) -> tuple[list[float], int]:
    with wave.open(str(path), "rb") as handle:
        rate = handle.getframerate()
        raw = handle.readframes(handle.getnframes())
    return [code / 32768.0 for code in struct.unpack(f"<{len(raw) // 2}h", raw)], rate


@pytest.fixture(scope="module")
def source(tmp_path_factory: pytest.TempPathFactory) -> Path:
    path = tmp_path_factory.mktemp("transform") / "source.wav"
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SR)
        samples = [0.4 * math.sin(2.0 * math.pi * 440.0 * n / SR) for n in range(FRAMES)]
        handle.writeframes(b"".join(struct.pack("<h", int(round(s * 32767.0))) for s in samples))
    return path


def _json(result: subprocess.CompletedProcess[str]) -> dict[str, Any]:
    assert result.returncode == 0, result.stderr
    payload: dict[str, Any] = json.loads(result.stdout)
    return payload


def _assert_close(written: list[float], expected: Any) -> None:
    assert len(written) == len(expected)
    # The WAV holds 16-bit samples, so the library's output is clipped to full scale.
    expected = [max(-1.0, min(1.0, float(b))) for b in expected]
    assert max(abs(a - b) for a, b in zip(written, expected, strict=True)) < QUANTUM


@pytest.mark.parametrize("command", ["preemphasis", "deemphasis"])
def test_emphasis_matches_library(command: str, source: Path, tmp_path: Path) -> None:
    import libsonare

    out = tmp_path / f"{command}.wav"
    payload = _json(_run_cli([command, str(source), "-o", str(out), "--coef", "0.9", "--json"]))
    assert set(payload) == {"output", "coef", "samples"}
    assert payload["coef"] == 0.9
    input_samples, rate = _read_wav(source)
    written, out_rate = _read_wav(out)
    assert out_rate == rate
    assert payload["samples"] == len(written)
    _assert_close(written, getattr(libsonare, command)(input_samples, coef=0.9))


@pytest.mark.parametrize("command", ["preemphasis", "deemphasis"])
def test_emphasis_reports_library_default_coef(command: str, source: Path, tmp_path: Path) -> None:
    out = tmp_path / f"{command}-default.wav"
    payload = _json(_run_cli([command, str(source), "-o", str(out), "--json"]))
    assert payload["coef"] == pytest.approx(0.97)


def test_mel_to_audio_matches_library(source: Path, tmp_path: Path) -> None:
    from libsonare import mel_spectrogram, mel_to_audio

    out = tmp_path / "mel.wav"
    payload = _json(
        _run_cli(
            ["mel-to-audio", str(source), "-o", str(out), "--n-iter", "4", "--n-mels", "64"]
            + ["--json"]
        )
    )
    assert set(payload) == {"output", "n_mels", "n_frames", "n_iter", "duration"}
    input_samples, rate = _read_wav(source)
    mel = mel_spectrogram(input_samples, rate, n_mels=64)
    expected = mel_to_audio(mel.power, sr=rate, hop_length=512, n_iter=4)
    written, out_rate = _read_wav(out)
    assert out_rate == rate
    assert payload["n_mels"] == 64
    assert payload["n_frames"] == mel.power.shape[1]
    assert payload["n_iter"] == 4
    assert payload["duration"] == pytest.approx(len(expected) / rate)
    _assert_close(written, expected)


def test_mfcc_to_audio_matches_library(source: Path, tmp_path: Path) -> None:
    from libsonare import mfcc, mfcc_to_audio

    out = tmp_path / "mfcc.wav"
    payload = _json(
        _run_cli(
            ["mfcc-to-audio", str(source), "-o", str(out), "--n-mfcc", "13", "--n-iter", "4"]
            + ["--n-mels", "64", "--fmax-hz", "8000", "--json"]
        )
    )
    assert set(payload) == {"output", "n_mfcc", "n_frames", "n_iter", "duration"}
    input_samples, rate = _read_wav(source)
    coefficients = mfcc(input_samples, rate, n_mels=64, n_mfcc=13, fmax=8000.0).coefficients
    expected = mfcc_to_audio(
        coefficients, n_mels=64, sr=rate, hop_length=512, n_iter=4, fmax=8000.0
    )
    written, _ = _read_wav(out)
    assert payload["n_mfcc"] == 13
    assert payload["n_frames"] == coefficients.shape[1]
    assert payload["duration"] == pytest.approx(len(expected) / rate)
    _assert_close(written, expected)


@pytest.mark.parametrize("command", ["preemphasis", "deemphasis", "mel-to-audio", "mfcc-to-audio"])
def test_missing_output_is_a_usage_refusal(command: str, source: Path) -> None:
    result = _run_cli([command, str(source)])
    assert result.returncode == EXIT_USAGE
    assert "Missing required option '--output'" in result.stderr


@pytest.mark.parametrize(
    ("extra", "needle"),
    [
        (["--fmin-hz", "-1"], "--fmin-hz"),
        (["--fmax-hz", "0"], "--fmax-hz"),
        (["--n-iter", "abc"], "--n-iter"),
        (["--n-fft", "1.5"], "--n-fft"),
    ],
)
def test_inverse_option_refusals(
    extra: list[str], needle: str, source: Path, tmp_path: Path
) -> None:
    result = _run_cli(["mel-to-audio", str(source), "-o", str(tmp_path / "x.wav"), *extra])
    assert result.returncode == EXIT_USAGE
    assert needle in result.stderr
    assert not (tmp_path / "x.wav").exists()


def test_emphasis_refuses_non_numeric_coef(source: Path, tmp_path: Path) -> None:
    result = _run_cli(["preemphasis", str(source), "-o", str(tmp_path / "x.wav"), "--coef", "x"])
    assert result.returncode == EXIT_USAGE
    assert "--coef" in result.stderr


def test_library_refusal_exits_3(source: Path, tmp_path: Path) -> None:
    out = tmp_path / "x.wav"
    result = _run_cli(["mel-to-audio", str(source), "-o", str(out), "--n-fft", "0"])
    assert result.returncode == EXIT_INVALID_PARAMETER
    assert not out.exists()
