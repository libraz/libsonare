"""Focused option-routing coverage for mastering and mixing CLI handlers."""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path
from types import SimpleNamespace

import pytest


def _result(*, stages: list[str] | None = None, report: object | None = None) -> SimpleNamespace:
    return SimpleNamespace(
        samples=[0.1, -0.1],
        sample_rate=48_000,
        input_lufs=-18.0,
        output_lufs=-14.0,
        applied_gain_db=4.0,
        latency_samples=12,
        stages=stages or ["loudness"],
        report=report,
    )


def test_mastering_preset_params_and_bits_reach_api_and_writer(
    monkeypatch, capsys, tmp_path
) -> None:
    import libsonare
    from libsonare import _cli_mastering, cli

    calls: dict[str, object] = {}
    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 44_100))

    def master_audio(*args, **kwargs):
        calls["preset"] = kwargs["preset_name"]
        calls["overrides"] = kwargs["overrides"]
        return _result(stages=["eq.tilt", "loudness"])

    monkeypatch.setattr(libsonare, "master_audio", master_audio)
    monkeypatch.setattr(
        _cli_mastering,
        "_write_wav",
        lambda path, samples, sample_rate, bits: calls.update(writer=(path, sample_rate, bits)),
    )

    output = tmp_path / "mastered.wav"
    args = cli._build_parser().parse_args(
        [
            "mastering",
            "input.wav",
            "--preset",
            "pop",
            "--params",
            "loudness.targetLufs=-13",
            "--bits",
            "24",
            "--output",
            str(output),
            "--json",
        ]
    )
    assert _cli_mastering.cmd_mastering(args) == 0

    assert calls["preset"] == "pop"
    assert calls["overrides"] == {"loudness.targetLufs": -13.0}
    assert calls["writer"] == (str(output), 48_000, 24)
    payload = json.loads(capsys.readouterr().out)
    assert payload["mode"] == "preset"
    assert payload["preset"] == "pop"


def test_mastering_config_unwraps_native_wrapper_and_forwards_params(monkeypatch, tmp_path) -> None:
    import libsonare
    from libsonare import _cli_mastering, cli

    calls: dict[str, object] = {}
    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 44_100))

    def mastering_chain(*args, **kwargs):
        calls["config"] = kwargs["config"]
        return _result(stages=["loudness"])

    monkeypatch.setattr(libsonare, "mastering_chain", mastering_chain)
    config_path = tmp_path / "chain.json"
    config_path.write_text(
        json.dumps({"version": 1, "params": {"loudness.targetLufs": -15.0}}),
        encoding="utf-8",
    )
    args = cli._build_parser().parse_args(
        [
            "mastering",
            "input.wav",
            "--config",
            str(config_path),
            "--params",
            "loudness.ceilingDb=-0.5",
        ]
    )

    assert _cli_mastering.cmd_mastering(args) == 0
    assert calls["config"] == {
        "loudness.targetLufs": -15.0,
        "loudness.ceilingDb": -0.5,
    }


def test_mastering_assistant_enable_repair_explain_reaches_suggestion_and_chain(
    monkeypatch, capsys
) -> None:
    import libsonare
    from libsonare import _cli_mastering, cli

    calls: dict[str, object] = {}
    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 48_000))

    def suggest(*args, **kwargs):
        calls["assistant_params"] = kwargs["params"]
        return json.dumps(
            {
                "chainConfig": {
                    "version": 1,
                    "params": {"loudness.targetLufs": -14.0, "repair.declick.enabled": 1},
                },
                "explanation": ["repair enabled"],
            }
        )

    def mastering_chain(*args, **kwargs):
        calls["config"] = kwargs["config"]
        return _result(stages=["repair.declick", "loudness"])

    monkeypatch.setattr(libsonare, "mastering_assistant_suggest", suggest)
    monkeypatch.setattr(libsonare, "mastering_chain", mastering_chain)
    args = cli._build_parser().parse_args(
        [
            "mastering",
            "input.wav",
            "--assistant",
            "--enable-repair",
            "--explain",
            "--true-peak-oversample",
            "8",
            "--json",
        ]
    )

    assert _cli_mastering.cmd_mastering(args) == 0
    assert calls["assistant_params"] == {
        "targetLufs": -14.0,
        "ceilingDb": -1.0,
        "enableRepair": True,
    }
    assert calls["config"] == {
        "loudness.targetLufs": -14.0,
        "repair.declick.enabled": 1,
        "loudness.truePeakOversample": 8.0,
    }
    assert json.loads(capsys.readouterr().out)["explanation"] == ["repair enabled"]


def test_mastering_semantic_selector_conflicts_are_invalid_parameters(monkeypatch) -> None:
    from libsonare import _cli_mastering

    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 48_000))
    args = argparse.Namespace(
        file="input.wav",
        preset="pop",
        config="{}",
        assistant=False,
        params="",
        bits=16,
        enable_repair=False,
        explain=False,
        report=None,
    )
    with pytest.raises(ValueError, match="mutually exclusive"):
        _cli_mastering.cmd_mastering(args)


def test_mastering_processor_explicit_stereo_routes_facade_and_reports_mode(
    monkeypatch, capsys, tmp_path
) -> None:
    import libsonare
    from libsonare import _cli_mastering, cli

    calls: dict[str, object] = {}
    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 44_100))
    monkeypatch.setattr(libsonare, "mastering_processor_catalog", lambda: [])

    def stereo(*args, **kwargs):
        calls["stereo"] = (args, kwargs)
        return SimpleNamespace(
            left=[0.1],
            right=[0.2],
            sample_rate=44_100,
            input_lufs=-18.0,
            output_lufs=-17.0,
            applied_gain_db=1.0,
            latency_samples=3,
        )

    monkeypatch.setattr(libsonare, "mastering_process_stereo", stereo)
    monkeypatch.setattr(
        _cli_mastering,
        "_write_wav",
        lambda path, samples, sample_rate, bits: calls.update(writer=(sample_rate, bits)),
    )
    output = tmp_path / "processor.wav"
    args = cli._build_parser().parse_args(
        [
            "mastering-processor",
            "--processor",
            "dynamics.compressor",
            "--stereo",
            "--bits",
            "24",
            "--output",
            str(output),
            "input.wav",
            "--json",
        ]
    )

    assert _cli_mastering.cmd_mastering_processor(args) == 0
    assert calls["stereo"][0][0] == "dynamics.compressor"
    assert calls["writer"] == (44_100, 24)
    assert json.loads(capsys.readouterr().out)["stereo"] is True


def test_mastering_processor_stereo_only_catalog_auto_routes(monkeypatch, capsys) -> None:
    import libsonare
    from libsonare import _cli_mastering

    calls: list[str] = []
    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 44_100))
    monkeypatch.setattr(
        libsonare,
        "mastering_processor_catalog",
        lambda: [{"id": "stereo.imager", "stereoOnly": True}],
    )
    monkeypatch.setattr(
        libsonare,
        "mastering_process_stereo",
        lambda name, *args, **kwargs: (
            calls.append(name)
            or SimpleNamespace(
                left=[0.1],
                right=[0.1],
                sample_rate=44_100,
                input_lufs=-18.0,
                output_lufs=-17.0,
                applied_gain_db=1.0,
                latency_samples=3,
            )
        ),
    )
    args = argparse.Namespace(
        file="input.wav",
        processor="stereo.imager",
        params="",
        bits=16,
        stereo=False,
        output="",
        json=True,
    )
    assert _cli_mastering.cmd_mastering_processor(args) == 0
    assert calls == ["stereo.imager"]
    assert json.loads(capsys.readouterr().out)["stereo"] is True


def test_eq_params_and_shortcut_conflict_is_invalid_parameter(monkeypatch) -> None:
    from libsonare import _cli_mastering

    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 44_100))
    args = argparse.Namespace(
        file="input.wav",
        params="band0.gainDb=1",
        type=0,
        frequency_hz=1500.0,
        bits=16,
    )
    with pytest.raises(ValueError, match="cannot be combined"):
        _cli_mastering.cmd_eq(args)


def test_mastering_pair_analyze_forwards_params(monkeypatch) -> None:
    import libsonare
    from libsonare import _cli_mastering

    calls: dict[str, object] = {}
    monkeypatch.setattr(
        _cli_mastering,
        "_load_audio",
        lambda path: ([0.0], 44_100) if path == "source.wav" else ([0.1], 44_100),
    )

    def analyze(*args, **kwargs):
        calls["args"] = args
        calls["params"] = kwargs["params"]
        return "{}"

    monkeypatch.setattr(libsonare, "mastering_pair_analyze", analyze)
    args = argparse.Namespace(
        file="source.wav",
        reference="reference.wav",
        analysis="spectralDifference",
        params="windowMs=120",
    )
    assert _cli_mastering.cmd_mastering_pair_analyze(args) == 0
    assert calls["params"] == {"windowMs": 120.0}


def test_mixing_preset_uses_stable_default(monkeypatch, capsys) -> None:
    import libsonare
    from libsonare import _cli_mastering

    calls: list[str] = []
    monkeypatch.setattr(
        libsonare,
        "mixing_scene_preset_json",
        lambda name: calls.append(name) or "{}",
    )
    assert _cli_mastering.cmd_mixing_preset(argparse.Namespace(preset=None)) == 0
    assert calls == ["vocalReverbSend"]
    assert capsys.readouterr().out == "{}\n"


def test_python_wav_writer_emits_24_bit_header(tmp_path) -> None:
    from libsonare._cli_common import _write_wav

    output = tmp_path / "24-bit.wav"
    _write_wav(str(output), [0.0, 1.0], 48_000, 24)
    with wave.open(str(output), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getframerate() == 48_000
        assert wav.getsampwidth() == 3
        assert wav.getnframes() == 2


def test_native_handler_source_consumes_bits_and_rejects_eq_conflicts() -> None:
    source = Path(__file__).parents[3] / "tools" / "cli" / "sonare_cli_mastering_mixing.cpp"
    text = source.read_text(encoding="utf-8")
    assert 'args.get_int("bits", 16)' in text
    assert '" cannot be combined with --params"' in text
    assert 'args.has("stereo") || is_stereo_only_processor(processor)' in text
