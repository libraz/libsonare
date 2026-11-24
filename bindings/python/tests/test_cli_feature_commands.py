"""End-to-end CLI coverage for the measurement and spectral-feature commands.

Each command is compared with the library call it wraps; the payload key sets are
the cross-front-end contract, so they are asserted exactly.
"""

from __future__ import annotations

import inspect
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


def _write(path: Path, samples: list[float]) -> Path:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SR)
        handle.writeframes(
            b"".join(
                struct.pack("<h", int(round(max(-1.0, min(1.0, s)) * 32767.0))) for s in samples
            )
        )
    return path


def _tone(amplitude: float, hz: float, phase: float = 0.0, frames: int = FRAMES) -> list[float]:
    return [amplitude * math.sin(2.0 * math.pi * hz * n / SR + phase) for n in range(frames)]


@pytest.fixture(scope="module")
def source(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return _write(tmp_path_factory.mktemp("features") / "source.wav", _tone(0.5, 220.0))


@pytest.fixture(scope="module")
def clipped(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return _write(tmp_path_factory.mktemp("features") / "clipped.wav", _tone(1.5, 220.0))


@pytest.fixture(scope="module")
def other(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return _write(tmp_path_factory.mktemp("features") / "other.wav", _tone(0.4, 220.0, 0.5))


@pytest.fixture(scope="module")
def two_part(tmp_path_factory: pytest.TempPathFactory) -> Path:
    samples = _tone(0.5, 220.0, frames=SR * 3) + _tone(0.5, 880.0, frames=SR * 3)
    return _write(tmp_path_factory.mktemp("features") / "two_part.wav", samples)


def _load(path: Path) -> tuple[Any, int]:
    from libsonare import Audio

    with Audio.from_file(str(path)) as audio:
        return audio.data, audio.sample_rate


def _json(result: subprocess.CompletedProcess[str]) -> dict[str, Any]:
    assert result.returncode == 0, result.stderr
    payload: dict[str, Any] = json.loads(result.stdout)
    return payload


def test_melody_matches_library(source: Path) -> None:
    from libsonare import analyze_melody

    payload = _json(_run_cli(["melody", str(source), "--json"]))
    samples, rate = _load(source)
    expected = analyze_melody(samples, rate)
    assert set(payload) == {
        "pitch_range_octaves",
        "mean_frequency_hz",
        "pitch_stability",
        "vibrato_rate_hz",
        "pitch_count",
    }
    assert payload["pitch_count"] == len(expected.points)
    assert payload["mean_frequency_hz"] == pytest.approx(expected.mean_frequency_hz)


def test_melody_without_voiced_frames_keeps_only_the_count(tmp_path: Path) -> None:
    silent = _write(tmp_path / "silent.wav", [0.0] * FRAMES)
    payload = _json(_run_cli(["melody", str(silent), "--json"]))
    assert set(payload) == {"pitch_count"}


def test_melody_options_reach_the_library(source: Path) -> None:
    from libsonare import analyze_melody

    payload = _json(
        _run_cli(["melody", str(source), "--hop-length", "512", "--f0-min-hz", "100", "--json"])
    )
    samples, rate = _load(source)
    expected = analyze_melody(samples, rate, hop_length=512, f0_min_hz=100.0)
    assert payload["pitch_count"] == len(expected.points)


def test_melody_human_report(source: Path) -> None:
    out = _run_cli(["melody", str(source)]).stdout
    assert "source.wav" in out
    assert "> Melody Analysis:" in out
    assert "    Pitch Points:    " in out


def test_melody_band_relation_is_a_usage_refusal(source: Path) -> None:
    result = _run_cli(["melody", str(source), "--f0-min-hz", "3000"])
    assert result.returncode == EXIT_USAGE
    assert (
        "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz 3000, --f0-max-hz 2093)"
        in result.stderr
    )


def test_melody_defaults_equal_the_pitch_tracker() -> None:
    from libsonare import analyze_melody, pitch_pyin

    melody = inspect.signature(analyze_melody).parameters
    pitch = inspect.signature(pitch_pyin).parameters
    for name in ("f0_min_hz", "f0_max_hz"):
        assert melody[name].default == pitch[name].default


def test_boundaries_matches_library(two_part: Path) -> None:
    from libsonare import detect_boundaries

    payload = _json(_run_cli(["boundaries", str(two_part), "--json"]))
    samples, rate = _load(two_part)
    expected = detect_boundaries(samples, rate).boundaries
    assert set(payload) == {"count", "boundaries"}
    assert payload["count"] == len(expected)
    for got, want in zip(payload["boundaries"], expected, strict=True):
        assert set(got) == {"time", "frame", "strength"}
        assert got["time"] == pytest.approx(want.time_sec)
        assert got["frame"] == want.frame


def test_boundaries_stream_switches_reach_the_library(two_part: Path) -> None:
    from libsonare import detect_boundaries

    payload = _json(
        _run_cli(["boundaries", str(two_part), "--no-mfcc", "--threshold", "0.2", "--json"])
    )
    samples, rate = _load(two_part)
    expected = detect_boundaries(samples, rate, use_mfcc=False, threshold=0.2).boundaries
    assert payload["count"] == len(expected)


def test_meter_matches_library(clipped: Path) -> None:
    from libsonare import (
        metering_crest_factor_db,
        metering_dc_offset,
        metering_detect_clipping,
        metering_peak_db,
        metering_rms_db,
        metering_silence_ratio,
        metering_true_peak_dbtp,
    )

    payload = _json(_run_cli(["meter", str(clipped), "--json"]))
    samples, rate = _load(clipped)
    assert list(payload) == [
        "peak_db",
        "rms_db",
        "crest_factor_db",
        "true_peak_db",
        "clipping_ratio",
        "silence_ratio",
        "dc_offset",
    ]
    assert payload["peak_db"] == pytest.approx(metering_peak_db(samples, rate))
    assert payload["rms_db"] == pytest.approx(metering_rms_db(samples, rate))
    assert payload["crest_factor_db"] == pytest.approx(metering_crest_factor_db(samples, rate))
    assert payload["true_peak_db"] == pytest.approx(metering_true_peak_dbtp(samples, rate))
    assert payload["clipping_ratio"] == pytest.approx(
        metering_detect_clipping(samples, rate).clipping_ratio
    )
    assert payload["clipping_ratio"] > 0.0
    assert payload["silence_ratio"] == pytest.approx(metering_silence_ratio(samples, rate))
    assert payload["dc_offset"] == pytest.approx(metering_dc_offset(samples, rate))


def test_meter_options_reach_the_library(source: Path) -> None:
    from libsonare import metering_true_peak_dbtp

    payload = _json(_run_cli(["meter", str(source), "--oversample", "8", "--json"]))
    samples, rate = _load(source)
    assert payload["true_peak_db"] == pytest.approx(
        metering_true_peak_dbtp(samples, rate, oversample_factor=8)
    )


def test_meter_human_report(source: Path) -> None:
    out = _run_cli(["meter", str(source)]).stdout
    assert "> Meters:" in out
    assert "    True Peak:      " in out
    assert "    DC Offset:      " in out


def test_meter_oversample_is_a_closed_set(source: Path) -> None:
    result = _run_cli(["meter", str(source), "--oversample", "3"])
    assert result.returncode == EXIT_USAGE
    assert "must be one of 1, 2, 4, 8, 16" in result.stderr


def test_clipping_matches_library(clipped: Path) -> None:
    from libsonare import metering_detect_clipping

    payload = _json(_run_cli(["clipping", str(clipped), "--min-region", "5", "--json"]))
    samples, rate = _load(clipped)
    expected = metering_detect_clipping(samples, rate, min_region_samples=5)
    assert set(payload) == {"clipped_samples", "clipping_ratio", "max_clipped_peak", "regions"}
    assert payload["clipped_samples"] == expected.clipped_samples
    assert len(payload["regions"]) == len(expected.regions)
    assert set(payload["regions"][0]) == {"start_sample", "end_sample", "length", "peak"}
    assert payload["regions"][0]["start_sample"] == expected.regions[0].start_sample


def test_clipping_human_report_caps_the_region_list(clipped: Path) -> None:
    out = _run_cli(["clipping", str(clipped)]).stdout
    assert "Clipping Detection:" in out
    assert "more regions)" in out


def test_clipping_min_region_domain_is_a_usage_refusal(source: Path) -> None:
    result = _run_cli(["clipping", str(source), "--min-region", "-1"])
    assert result.returncode == EXIT_USAGE
    assert "--min-region" in result.stderr


def test_clipping_threshold_db_reaches_the_library(clipped: Path) -> None:
    from libsonare import metering_detect_clipping

    payload = _json(
        _run_cli(["clipping", str(clipped), "--threshold-db", "-14", "--min-region", "0", "--json"])
    )
    samples, rate = _load(clipped)
    expected = metering_detect_clipping(samples, rate, threshold_db=-14.0, min_region_samples=0)
    assert payload["clipped_samples"] == expected.clipped_samples
    assert payload["clipped_samples"] > metering_detect_clipping(samples, rate).clipped_samples
    assert len(payload["regions"]) == len(expected.regions)


def test_meter_clip_threshold_db_reaches_the_library(clipped: Path) -> None:
    from libsonare import metering_detect_clipping

    payload = _json(_run_cli(["meter", str(clipped), "--clip-threshold-db", "-14", "--json"]))
    samples, rate = _load(clipped)
    expected = metering_detect_clipping(samples, rate, threshold_db=-14.0).clipping_ratio
    assert payload["clipping_ratio"] == pytest.approx(expected)
    assert expected > metering_detect_clipping(samples, rate).clipping_ratio


@pytest.mark.parametrize(
    ("command", "flag"), [("meter", "--clip-threshold-db"), ("clipping", "--threshold-db")]
)
def test_clipping_level_above_full_scale_is_a_usage_refusal(
    source: Path, command: str, flag: str
) -> None:
    result = _run_cli([command, str(source), flag, "0.5"])
    assert result.returncode == EXIT_USAGE
    assert flag in result.stderr


def test_dynamic_range_matches_library(source: Path) -> None:
    from libsonare import metering_dynamic_range

    payload = _json(_run_cli(["dynamic-range", str(source), "--window-sec", "0.2", "--json"]))
    samples, rate = _load(source)
    expected = metering_dynamic_range(samples, rate, window_sec=0.2)
    assert set(payload) == {
        "dynamic_range_db",
        "low_percentile_db",
        "high_percentile_db",
        "window_rms_db",
    }
    assert payload["dynamic_range_db"] == pytest.approx(expected.dynamic_range_db)
    assert payload["window_rms_db"] == pytest.approx(expected.window_rms_db.tolist())


@pytest.mark.parametrize(
    ("argv", "message"),
    [
        (
            ["--low-percentile", "0.9", "--high-percentile", "0.1"],
            "--high-percentile must be at least --low-percentile "
            "(--low-percentile 0.9, --high-percentile 0.1)",
        ),
        (
            ["--low-percentile", "0.99"],
            "--high-percentile must be at least --low-percentile "
            "(--low-percentile 0.99, --high-percentile 0.95)",
        ),
    ],
)
def test_dynamic_range_refuses_inverted_percentiles(
    source: Path, argv: list[str], message: str
) -> None:
    result = _run_cli(["dynamic-range", str(source), *argv])
    assert result.returncode == EXIT_USAGE
    assert message in result.stderr


def test_dynamic_range_accepts_equal_percentiles(source: Path) -> None:
    payload = _json(
        _run_cli(
            [
                "dynamic-range",
                str(source),
                "--low-percentile",
                "0.5",
                "--high-percentile",
                "0.5",
                "--json",
            ]
        )
    )
    assert payload["dynamic_range_db"] == 0.0


def test_dynamic_range_domains_are_usage_refusals(source: Path) -> None:
    for argv in (["--window-sec", "-1"], ["--hop-sec", "0"], ["--high-percentile", "2"]):
        assert _run_cli(["dynamic-range", str(source), *argv]).returncode == EXIT_USAGE


def test_stereo_matches_library(source: Path, other: Path) -> None:
    from libsonare import metering_stereo_correlation, metering_stereo_width

    payload = _json(_run_cli(["stereo", str(source), "--reference", str(other), "--json"]))
    left, rate = _load(source)
    right, _ = _load(other)
    assert set(payload) == {"correlation", "stereo_width"}
    assert payload["correlation"] == pytest.approx(metering_stereo_correlation(left, right, rate))
    assert payload["stereo_width"] == pytest.approx(metering_stereo_width(left, right, rate))


def test_phase_matches_library(source: Path, other: Path) -> None:
    from libsonare import metering_phase_scope

    payload = _json(_run_cli(["phase", str(source), "--reference", str(other), "--json"]))
    left, rate = _load(source)
    right, _ = _load(other)
    expected = metering_phase_scope(left, right, rate)
    assert set(payload) == {"correlation", "average_abs_angle_rad", "max_radius"}
    assert payload["correlation"] == pytest.approx(expected.correlation)
    assert payload["max_radius"] == pytest.approx(expected.max_radius)


def test_stereo_and_phase_leave_unmeasured_values_out_over_silence(tmp_path: Path) -> None:
    silence = _write(tmp_path / "silence.wav", [0.0] * FRAMES)
    argv = [str(silence), "--reference", str(silence)]
    assert _json(_run_cli(["stereo", *argv, "--json"])) == {}
    assert _json(_run_cli(["phase", *argv, "--json"])) == {"max_radius": 0.0}
    text = _run_cli(["phase", *argv])
    assert text.returncode == 0, text.stderr
    assert "Correlation:        -" in text.stdout
    assert "Avg Abs Angle:      -" in text.stdout
    assert "Correlation:  -" in _run_cli(["stereo", *argv]).stdout


def test_tempo_rhythm_timbre_and_pitch_leave_unmeasured_values_out_over_silence(
    tmp_path: Path,
) -> None:
    silence = str(_write(tmp_path / "silence.wav", [0.0] * FRAMES))
    assert _json(_run_cli(["bpm", silence, "--json"])) == {}
    rhythm = _json(_run_cli(["rhythm", silence, "--json"]))
    for key in ("bpm", "syncopation", "pattern_regularity", "tempo_stability"):
        assert key not in rhythm, key
    assert _json(_run_cli(["timbre", silence, "--json"])).keys() == {
        "spectral_centroid",
        "spectral_flatness",
        "spectral_rolloff",
    }
    analysis = _json(_run_cli(["analyze", silence, "--json"]))
    assert "bpm" not in analysis
    assert analysis["timbre"] == {}
    assert analysis["rhythm"] == {"groove_type": "straight"}
    melody = _json(_run_cli(["melody", silence, "--json"]))
    assert melody.keys() == {"pitch_count"}
    pitch = _json(_run_cli(["pitch", silence, "--json"]))
    assert "median_f0" not in pitch and "mean_f0" not in pitch
    assert "BPM: -" in _run_cli(["bpm", silence]).stdout
    rhythm_text = _run_cli(["rhythm", silence]).stdout
    assert "Syncopation:        -" in rhythm_text
    assert "Tempo stability:    -" in rhythm_text
    assert "Brightness: -" in _run_cli(["timbre", silence]).stdout
    assert "Median F0: -" in _run_cli(["pitch", silence]).stdout


@pytest.mark.parametrize("command", ["stereo", "phase"])
def test_reference_is_needed_after_the_input_is_read(command: str, source: Path) -> None:
    result = _run_cli([command, str(source)])
    assert result.returncode == EXIT_INVALID_PARAMETER
    assert "--reference is required" in result.stderr


@pytest.mark.parametrize("command", ["stereo", "phase"])
def test_reference_must_match_the_input(command: str, source: Path, tmp_path: Path) -> None:
    short = _write(tmp_path / "short.wav", _tone(0.4, 220.0, frames=FRAMES // 2))
    result = _run_cli([command, str(source), "--reference", str(short)])
    assert result.returncode == EXIT_INVALID_PARAMETER
    assert "reference length must match input length" in result.stderr


def test_tonnetz_matches_library(source: Path) -> None:
    from libsonare import chroma, tonnetz

    payload = _json(_run_cli(["tonnetz", str(source), "--json"]))
    samples, rate = _load(source)
    values = tonnetz(chroma(samples, sample_rate=rate).features)
    assert set(payload) == {"n_dims", "n_frames", "stats"}
    assert (payload["n_dims"], payload["n_frames"]) == values.shape
    assert set(payload["stats"]) == {"mean", "std", "min", "max"}
    assert payload["stats"]["max"] == pytest.approx(float(values.max()))


def test_fourier_tempogram_matches_library(source: Path) -> None:
    from libsonare._conversions import fourier_tempogram, onset_strength

    payload = _json(_run_cli(["fourier-tempogram", str(source), "--win-length", "128", "--json"]))
    samples, rate = _load(source)
    matrix = fourier_tempogram(onset_strength(samples, sr=rate), sr=rate, win_length=128)
    assert set(payload) == {"win_length", "n_bins", "n_frames", "stats"}
    assert payload["win_length"] == 128
    assert (payload["n_bins"], payload["n_frames"]) == matrix.shape
    assert payload["stats"]["mean"] == pytest.approx(float(matrix.mean()), rel=1e-4)


def test_tempogram_ratio_matches_library(source: Path) -> None:
    from libsonare._conversions import onset_strength, tempogram, tempogram_ratio

    payload = _json(_run_cli(["tempogram-ratio", str(source), "--json"]))
    samples, rate = _load(source)
    matrix = tempogram(onset_strength(samples, sr=rate), sr=rate)
    expected = tempogram_ratio(matrix, sr=rate, factors=[0.5, 1.0, 2.0, 3.0, 4.0]).tolist()
    assert set(payload) == {"win_length", "ratios"}
    assert payload["win_length"] == 384
    assert [r["factor"] for r in payload["ratios"]] == [0.5, 1.0, 2.0, 3.0, 4.0]
    assert [r["value"] for r in payload["ratios"]] == pytest.approx(expected)


def test_cqt_matches_library(source: Path) -> None:
    from libsonare import cqt

    payload = _json(
        _run_cli(["cqt", str(source), "--n-bins", "48", "--hop-length", "256", "--json"])
    )
    samples, rate = _load(source)
    result = cqt(samples, sr=rate, n_bins=48, hop_length=256)
    assert set(payload) == {"n_bins", "n_frames", "duration", "fmin_hz", "bins_per_octave"}
    assert (payload["n_bins"], payload["n_frames"]) == result.magnitude.shape
    assert payload["duration"] == pytest.approx(result.magnitude.shape[1] * 256 / rate)
    assert payload["fmin_hz"] == pytest.approx(32.70319566257483)
    assert payload["bins_per_octave"] == 12


def test_cqt_human_report(source: Path) -> None:
    out = _run_cli(["cqt", str(source)]).stdout
    assert "Constant-Q Transform:" in out
    assert "(7 octaves)" in out


def test_cqt_nyquist_relation_is_a_library_refusal(source: Path) -> None:
    result = _run_cli(["cqt", str(source), "--fmin-hz", "20000", "--json"])
    assert result.returncode == EXIT_INVALID_PARAMETER
    assert "must be less than sample_rate / 2" in result.stderr


def test_a_refused_rate_is_named_sample_rate_not_the_librosa_keyword() -> None:
    from libsonare import SonareError, SonareValueError
    from libsonare._cli_common import _error_message

    assert _error_message(SonareValueError("cqt: x must be less than sr / 2")) == (
        "cqt: x must be less than sample_rate / 2"
    )
    assert _error_message(SonareValueError("mel: sr must be an integer")) == (
        "mel: sample_rate must be an integer"
    )
    assert _error_message(SonareValueError("srate, --sr, a.sr and sr() stay")) == (
        "srate, --sr, a.sr and sr() stay"
    )
    coded = SonareError(9, "sr is untouched outside a refusal")
    assert _error_message(coded) == str(coded)


def test_vqt_matches_library(source: Path) -> None:
    from libsonare import vqt

    payload = _json(
        _run_cli(["vqt", str(source), "--n-bins", "48", "--filter-scale", "0.5", "--json"])
    )
    samples, rate = _load(source)
    result = vqt(samples, sr=rate, n_bins=48, filter_scale=0.5)
    assert set(payload) == {
        "n_bins",
        "n_frames",
        "duration",
        "fmin_hz",
        "bins_per_octave",
        "gamma",
    }
    assert (payload["n_bins"], payload["n_frames"]) == result.magnitude.shape
    assert payload["gamma"] == pytest.approx(result.gamma)
    assert payload["gamma"] > 0.0


def test_vqt_gamma_reaches_the_library(source: Path) -> None:
    payload = _json(_run_cli(["vqt", str(source), "--gamma", "3.5", "--json"]))
    assert payload["gamma"] == pytest.approx(3.5)


def test_vqt_negative_gamma_is_a_usage_refusal(source: Path) -> None:
    result = _run_cli(["vqt", str(source), "--gamma", "-1"])
    assert result.returncode == EXIT_USAGE
    assert "--gamma" in result.stderr


def test_vqt_human_report(source: Path) -> None:
    out = _run_cli(["vqt", str(source)]).stdout
    assert "Variable-Q Transform:" in out
    assert "  Gamma:           " in out


def test_cqt_hop_domain_is_a_usage_refusal(source: Path) -> None:
    result = _run_cli(["cqt", str(source), "--hop-length", "0"])
    assert result.returncode == EXIT_USAGE
    assert "must be an integer in [1, 65536]" in result.stderr


@pytest.mark.parametrize(
    "command",
    [
        "melody",
        "boundaries",
        "meter",
        "clipping",
        "dynamic-range",
        "stereo",
        "phase",
        "tonnetz",
        "fourier-tempogram",
        "tempogram-ratio",
        "cqt",
    ],
)
def test_usage_refusal_precedes_the_input_read(command: str, tmp_path: Path) -> None:
    result = _run_cli([command, str(tmp_path / "missing.wav"), "--bogus"])
    assert result.returncode == EXIT_USAGE
