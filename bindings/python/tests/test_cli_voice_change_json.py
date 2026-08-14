"""Focused JSON contract tests for the Python voice-change CLI."""

from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]


def _write_tone(path: Path, *, sample_rate: int, length: int) -> None:
    frames = bytearray()
    for index in range(length):
        sample = 0.2 * math.sin(2.0 * math.pi * 220.0 * index / sample_rate)
        frames += struct.pack("<h", int(round(sample * 32767.0)))
    with wave.open(str(path), "wb") as rendered:
        rendered.setnchannels(1)
        rendered.setsampwidth(2)
        rendered.setframerate(sample_rate)
        rendered.writeframes(bytes(frames))


def _run_cli(*args: str, legacy: bool = False) -> subprocess.CompletedProcess[str]:
    environment = dict(os.environ)
    environment.pop("SONARE_LEGACY_EXIT", None)
    if legacy:
        environment["SONARE_LEGACY_EXIT"] = "1"
    source = ROOT / "bindings" / "python" / "src"
    environment["PYTHONPATH"] = str(source) + os.pathsep + environment.get("PYTHONPATH", "")
    if "SONARE_LIB_PATH" not in environment:
        for name in ("libsonare.dylib", "libsonare.so"):
            candidate = ROOT / "build-python-shared" / "lib" / name
            if candidate.exists():
                environment["SONARE_LIB_PATH"] = str(candidate)
                break
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )


def test_voice_change_simple_json_has_sample_preserving_common_schema(tmp_path: Path) -> None:
    sample_rate = 22_050
    input_length = 2_205
    source = tmp_path / "input.wav"
    output = tmp_path / "simple.wav"
    _write_tone(source, sample_rate=sample_rate, length=input_length)

    result = _run_cli(
        "voice-change",
        str(source),
        "--output",
        str(output),
        "--pitch-semitones",
        "5",
        "--formant-factor",
        "1.1",
        "--json",
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert set(payload) == {
        "output",
        "length",
        "duration",
        "sample_rate",
        "latency_samples",
        "pitch_semitones",
        "formant_factor",
    }
    assert payload["output"] == str(output)
    assert payload["length"] == input_length
    assert payload["sample_rate"] == sample_rate
    assert payload["duration"] == input_length / sample_rate
    assert payload["latency_samples"] == 0
    assert payload["pitch_semitones"] == 5.0
    assert payload["formant_factor"] == 1.1
    with wave.open(str(output), "rb") as rendered:
        assert rendered.getnframes() == input_length
        assert rendered.getframerate() == sample_rate


def test_voice_change_preset_json_reports_resolved_chain_latency(tmp_path: Path) -> None:
    sample_rate = 48_000
    input_length = 2_400
    source = tmp_path / "input.wav"
    output = tmp_path / "preset.wav"
    _write_tone(source, sample_rate=sample_rate, length=input_length)

    result = _run_cli(
        "voice-change",
        str(source),
        "--output",
        str(output),
        "--preset",
        "bright-idol",
        "--json",
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert set(payload) == {
        "output",
        "length",
        "duration",
        "sample_rate",
        "latency_samples",
        "preset",
    }
    assert payload["output"] == str(output)
    assert payload["length"] == input_length
    assert payload["duration"] == input_length / sample_rate
    assert payload["sample_rate"] == sample_rate
    assert payload["latency_samples"] > 0
    assert payload["preset"] == "bright-idol"
    with wave.open(str(output), "rb") as rendered:
        assert rendered.getnframes() == input_length
        assert rendered.getframerate() == sample_rate

    overridden_output = tmp_path / "preset-overridden.wav"
    overridden = _run_cli(
        "voice-change",
        str(source),
        "--output",
        str(overridden_output),
        "--preset",
        "bright-idol",
        "--set",
        "dsp.retune.grainSize=64",
        "--json",
    )
    assert overridden.returncode == 0, overridden.stderr
    overridden_payload = json.loads(overridden.stdout)
    assert overridden_payload["latency_samples"] > 0
    assert overridden_payload["latency_samples"] != payload["latency_samples"]


def test_voice_change_custom_preset_json_omits_inferred_preset_id(tmp_path: Path) -> None:
    sample_rate = 48_000
    input_length = 2_400
    source = tmp_path / "input.wav"
    baseline_output = tmp_path / "baseline.wav"
    output = tmp_path / "custom.wav"
    preset_path = tmp_path / "custom-preset.json"
    _write_tone(source, sample_rate=sample_rate, length=input_length)

    baseline = _run_cli(
        "voice-change",
        str(source),
        "--output",
        str(baseline_output),
        "--preset",
        "bright-idol",
        "--json",
    )
    assert baseline.returncode == 0, baseline.stderr
    baseline_payload = json.loads(baseline.stdout)

    generated = _run_cli("voice-preset", "--preset", "bright-idol")
    assert generated.returncode == 0, generated.stderr
    preset = json.loads(generated.stdout)
    preset["id"] = "custom-grain"
    preset["name"] = "Custom Grain"
    preset["category"] = "custom"
    preset["dsp"]["retune"]["grainSize"] = 64
    preset_path.write_text(json.dumps(preset), encoding="utf-8")

    result = _run_cli(
        "voice-change",
        str(source),
        "--output",
        str(output),
        "--preset-json",
        str(preset_path),
        "--json",
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert set(payload) == {
        "output",
        "length",
        "duration",
        "sample_rate",
        "latency_samples",
    }
    assert payload["length"] == input_length
    assert payload["duration"] == input_length / sample_rate
    assert payload["sample_rate"] == sample_rate
    assert payload["latency_samples"] > 0
    assert payload["latency_samples"] != baseline_payload["latency_samples"]
    assert "preset" not in payload


def test_voice_change_preset_pack_without_id_omits_default_preset(tmp_path: Path) -> None:
    sample_rate = 22_050
    input_length = 2_205
    source = tmp_path / "input.wav"
    output = tmp_path / "pack.wav"
    _write_tone(source, sample_rate=sample_rate, length=input_length)

    result = _run_cli(
        "voice-change",
        str(source),
        "--output",
        str(output),
        "--preset-pack",
        str(ROOT / "schemas" / "realtime-voice-changer-presets.example.json"),
        "--json",
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert set(payload) == {
        "output",
        "length",
        "duration",
        "sample_rate",
        "latency_samples",
    }
    assert payload["length"] == input_length
    assert payload["duration"] == input_length / sample_rate
    assert payload["sample_rate"] == sample_rate
    assert payload["latency_samples"] > 0
    assert "preset" not in payload


@pytest.mark.parametrize("legacy, expected_code", [(False, 3), (True, 1)])
def test_voice_preset_validate_malformed_set_is_preprocessing_error(
    tmp_path: Path, legacy: bool, expected_code: int
) -> None:
    preset = _run_cli("voice-preset", "--preset", "neutral-monitor")
    assert preset.returncode == 0, preset.stderr
    preset_path = tmp_path / "preset.json"
    preset_path.write_text(preset.stdout, encoding="utf-8")

    result = _run_cli(
        "voice-preset-validate",
        str(preset_path),
        "--set",
        "malformed-assignment",
        "--json",
        legacy=legacy,
    )

    assert result.returncode == expected_code
    assert result.stdout == ""
    assert result.stderr
    assert "ok" not in result.stderr


@pytest.mark.parametrize("legacy, expected_code", [(False, 4), (True, 1)])
def test_voice_preset_validate_missing_input_is_file_error(
    legacy: bool, expected_code: int
) -> None:
    result = _run_cli("voice-preset-validate", "--json", legacy=legacy)

    assert result.returncode == expected_code
    assert result.stdout == ""
    assert result.stderr
    assert "ok" not in result.stderr


@pytest.mark.parametrize("legacy, expected_code", [(False, 4), (True, 1)])
def test_voice_preset_validate_missing_file_is_file_error(
    tmp_path: Path, legacy: bool, expected_code: int
) -> None:
    missing = tmp_path / "missing-preset.json"
    result = _run_cli("voice-preset-validate", str(missing), "--json", legacy=legacy)

    assert result.returncode == expected_code
    assert result.stdout == ""
    assert result.stderr
    assert "ok" not in result.stderr


def test_synthesize_rir_json_reports_sample_rate(tmp_path: Path) -> None:
    output = tmp_path / "rir.wav"
    result = _run_cli(
        "synthesize-rir",
        "--sample-rate",
        "44100",
        "--max-seconds",
        "0.05",
        "--output",
        str(output),
        "--json",
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert payload["sample_rate"] == 44_100
    with wave.open(str(output), "rb") as rendered:
        assert rendered.getframerate() == 44_100
