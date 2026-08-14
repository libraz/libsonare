"""Regression coverage for Python CLI JSON schemas shared with native CLI."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from types import SimpleNamespace


def _json_output(capsys) -> dict[str, object]:
    return json.loads(capsys.readouterr().out)


def test_rhythm_json_uses_native_fields_and_unrounded_numbers(monkeypatch, capsys) -> None:
    """Rhythm JSON includes meter confidence and the complete interval summary."""
    import libsonare
    from libsonare import _cli_advanced

    monkeypatch.setattr(_cli_advanced, "_load_audio", lambda path: ([0.0], 22050))
    monkeypatch.setattr(
        libsonare,
        "analyze_rhythm",
        lambda samples, sample_rate: SimpleNamespace(
            bpm=123.456789,
            time_signature=SimpleNamespace(numerator=7, denominator=8, confidence=0.87654321),
            groove_type="shuffle",
            syncopation=0.123456789,
            pattern_regularity=0.234567891,
            tempo_stability=0.345678912,
            beat_intervals=[0.1, 0.2, 0.4],
        ),
    )

    assert _cli_advanced.cmd_rhythm(argparse.Namespace(file="input.wav", json=True)) == 0
    payload = _json_output(capsys)

    assert set(payload) == {
        "bpm",
        "time_signature",
        "groove_type",
        "syncopation",
        "pattern_regularity",
        "tempo_stability",
        "beat_intervals",
    }
    assert payload["bpm"] == 123.456789
    assert payload["time_signature"] == {
        "numerator": 7,
        "denominator": 8,
        "confidence": 0.87654321,
    }
    assert payload["syncopation"] == 0.123456789
    stats = payload["beat_intervals"]
    assert isinstance(stats, dict)
    assert stats["count"] == 3
    assert math.isclose(stats["mean"], statistics.mean([0.1, 0.2, 0.4]))
    assert math.isclose(stats["std"], statistics.pstdev([0.1, 0.2, 0.4]))
    assert stats["min"] == 0.1
    assert stats["max"] == 0.4


def test_pitch_json_reports_voicing_and_serializes_nonfinite_as_null(monkeypatch, capsys) -> None:
    """Pitch JSON mirrors native voicing counts and strict JSON null behavior."""
    import libsonare
    from libsonare import _cli_analysis

    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))
    monkeypatch.setattr(
        libsonare,
        "pitch_pyin",
        lambda samples, sample_rate: SimpleNamespace(
            n_frames=0,
            voiced_flag=[],
            median_f0=math.nan,
            mean_f0=math.inf,
        ),
    )

    args = argparse.Namespace(file="input.wav", algorithm="pyin", json=True)
    assert _cli_analysis.cmd_pitch(args) == 0
    payload = _json_output(capsys)

    assert set(payload) == {
        "algorithm",
        "n_frames",
        "voiced_count",
        "voiced_ratio",
        "median_f0",
        "mean_f0",
    }
    assert payload["algorithm"] == "pyin"
    assert payload["n_frames"] == 0
    assert payload["voiced_count"] == 0
    assert payload["voiced_ratio"] is None
    assert payload["median_f0"] is None
    assert payload["mean_f0"] is None


def _mastering_result() -> SimpleNamespace:
    return SimpleNamespace(
        samples=[0.1, -0.1],
        sample_rate=48000,
        input_lufs=-18.123456,
        output_lufs=-14.654321,
        applied_gain_db=3.469135,
        latency_samples=17,
    )


def test_eq_json_has_canonical_output_and_sample_rate(monkeypatch, capsys) -> None:
    """EQ JSON always carries an output path, even when no file was requested."""
    import libsonare
    from libsonare import _cli_mastering, cli

    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 44100))
    monkeypatch.setattr(libsonare, "mastering_process", lambda *args, **kwargs: _mastering_result())

    args = cli._build_parser().parse_args(["eq", "input.wav", "--json"])
    assert _cli_mastering.cmd_eq(args) == 0
    payload = _json_output(capsys)

    assert set(payload) == {
        "processor",
        "input_lufs",
        "output_lufs",
        "applied_gain_db",
        "latency_samples",
        "sample_rate",
        "output",
    }
    assert payload["processor"] == "eq.equalizer"
    assert payload["sample_rate"] == 48000
    assert payload["output"] == ""
    assert payload["input_lufs"] == -18.123456


def test_mastering_processor_json_always_reports_stereo_mode(monkeypatch, capsys) -> None:
    """Named processor JSON is EQ-shaped and includes an explicit stereo flag."""
    import libsonare
    from libsonare import _cli_mastering, cli

    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 44100))
    monkeypatch.setattr(libsonare, "mastering_processor_catalog", lambda: [])
    monkeypatch.setattr(libsonare, "mastering_process", lambda *args, **kwargs: _mastering_result())

    args = cli._build_parser().parse_args(
        ["mastering-processor", "--processor", "dynamics.compressor", "input.wav", "--json"]
    )
    assert _cli_mastering.cmd_mastering_processor(args) == 0
    payload = _json_output(capsys)

    assert set(payload) == {
        "processor",
        "input_lufs",
        "output_lufs",
        "applied_gain_db",
        "latency_samples",
        "sample_rate",
        "output",
        "stereo",
    }
    assert payload["processor"] == "dynamics.compressor"
    assert payload["stereo"] is False
    assert payload["output"] == ""


def test_mastering_processor_json_marks_stereo_only_processor(monkeypatch, capsys) -> None:
    """The auto-routed stereo path sets the same always-present flag to true."""
    import libsonare
    from libsonare import _cli_mastering, cli

    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 44100))
    monkeypatch.setattr(
        libsonare,
        "mastering_processor_catalog",
        lambda: [{"id": "stereo.imager", "stereoOnly": True}],
    )
    monkeypatch.setattr(
        libsonare,
        "mastering_process_stereo",
        lambda *args, **kwargs: SimpleNamespace(
            left=[0.1],
            right=[0.1],
            sample_rate=48000,
            input_lufs=-18.123456,
            output_lufs=-14.654321,
            applied_gain_db=3.469135,
            latency_samples=17,
        ),
    )

    args = cli._build_parser().parse_args(
        ["mastering-processor", "--processor", "stereo.imager", "input.wav", "--json"]
    )
    assert _cli_mastering.cmd_mastering_processor(args) == 0
    payload = _json_output(capsys)

    assert payload["stereo"] is True
    assert payload["sample_rate"] == 48000
    assert payload["output"] == ""
