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
    # targetLufs / ceilingDb are absent because neither was named on the command
    # line: supplying a key marks the field explicit for the assistant, and a
    # delivery target only fills in what the caller left alone, so passing the
    # default through unconditionally suppressed every platform target's
    # loudness. The remaining three carry the assistant controls the CLI now
    # reaches -- delivery target, streaming-safe repair, speech mono amount --
    # at their defaults.
    assert calls["assistant_params"] == {
        "enableRepair": True,
        "targetPlatform": "streaming",
        "preferStreamingSafe": True,
        "speechMonoAmount": 1.0,
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
    """Omitting ``--preset`` still resolves to the advertised default.

    Driven through the parser rather than a hand-built namespace: ``--preset``
    declares its default in argparse, so ``preset=None`` is a state the CLI
    cannot produce, and asserting on it pinned a handler-side fallback that was
    unreachable. What the contract actually promises is this end-to-end path.
    """
    import libsonare
    from libsonare import _cli_mastering, cli

    calls: list[str] = []
    monkeypatch.setattr(
        libsonare,
        "mixing_scene_preset_json",
        lambda name: calls.append(name) or "{}",
    )
    args = cli._build_parser().parse_args(["mixing-preset"])
    assert _cli_mastering.cmd_mixing_preset(args) == 0
    assert calls == ["vocalReverbSend"]
    assert capsys.readouterr().out == "{}\n"


def test_mixing_preset_default_matches_the_native_cli() -> None:
    """Both CLIs advertise the same default, from their own declarations.

    The two surfaces declare it separately, so a change to one is invisible to
    the other; this is the only place the pair is compared.
    """
    from pathlib import Path

    from libsonare import cli

    action = next(
        action
        for action in cli._build_parser()
        ._subparsers._group_actions[0]  # noqa: SLF001
        .choices["mixing-preset"]
        ._actions
        if action.dest == "preset"
    )
    assert action.default == "vocalReverbSend"

    native = Path(__file__).resolve().parents[3] / "tools" / "cli_support.cpp"
    text = native.read_text(encoding="utf-8")
    assert 'string_value("preset", "vocalReverbSend")' in text


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


def test_mastering_cli_reports_a_blocked_loudness_target(capsys, monkeypatch) -> None:
    """The default JSON payload carries the flag, and it agrees with the API.

    Peak-normalized material asked for -6 LUFS under a -3 dBTP ceiling cannot
    reach the target, and the CLI is the only machine-readable way to learn that
    without switching to the ``--report`` code path.
    """
    import math

    import libsonare
    from libsonare import _cli_mastering, cli

    sample_rate = 48_000
    samples = [
        math.sin(2.0 * math.pi * 440.0 * index / sample_rate) for index in range(sample_rate)
    ]
    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: (samples, sample_rate))

    args = cli._build_parser().parse_args(
        [
            "mastering",
            "input.wav",
            "--target-lufs",
            "-6",
            "--ceiling-db",
            "-3",
            "--json",
        ]
    )
    assert _cli_mastering.cmd_mastering(args) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["loudness_target_limited"] is True

    # The same input through the named-processor entry point must agree: these
    # are two views of one computation, not two independent estimates.
    processed = libsonare.mastering_process(
        "maximizer.loudnessOptimize",
        samples,
        sample_rate=sample_rate,
        params={"targetLufs": -6.0, "ceilingDb": -3.0},
    )
    assert processed.loudness_target_limited is True


def test_target_platform_moves_the_loudness_the_assistant_masters_to(capsys, monkeypatch) -> None:
    """The delivery target decides the loudness, and it is reachable from the CLI.

    Read from the rendered output loudness rather than from a flag round-trip:
    with the target unreachable, someone mastering for EBU R128 broadcast
    silently got the -14 LUFS streaming convention -- a 9 dB error under exit 0.
    """
    import math

    from libsonare import _cli_mastering, cli

    sample_rate = 22_050
    samples = [
        0.5 * math.sin(2.0 * math.pi * 440.0 * index / sample_rate) for index in range(sample_rate)
    ]
    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: (samples, sample_rate))

    def master_to(platform: str | None) -> float:
        argv = ["mastering", "input.wav", "--assistant", "--json"]
        if platform is not None:
            argv += ["--target-platform", platform]
        args = cli._build_parser().parse_args(argv)
        assert _cli_mastering.cmd_mastering(args) == 0
        return json.loads(capsys.readouterr().out)["output_lufs"]

    streaming = master_to(None)
    assert -14.5 < streaming < -13.5
    # Named explicitly, the default target lands in the same place as the
    # omitted one.
    assert master_to("streaming") == pytest.approx(streaming)
    assert -23.5 < master_to("broadcast") < -22.5
    assert -16.5 < master_to("podcast") < -15.5
    assert -9.5 < master_to("club") < -8.5


def test_assistant_controls_are_refused_without_assistant(monkeypatch) -> None:
    """Options that only reach an AssistantConfig field are refused, not dropped."""
    from libsonare import _cli_mastering, cli

    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0], 48_000))
    for option in (
        ["--target-platform", "broadcast"],
        ["--no-streaming-safe"],
        ["--speech-mono-amount", "0.5"],
    ):
        args = cli._build_parser().parse_args(["mastering", "input.wav", *option])
        with pytest.raises(ValueError, match="requires --assistant"):
            _cli_mastering.cmd_mastering(args)


def test_unknown_target_platform_is_rejected_against_the_delivery_table() -> None:
    """The accepted names are the delivery-target table's rows, not a free string."""
    from libsonare import cli

    with pytest.raises(SystemExit) as raised:
        cli._build_parser().parse_args(
            ["mastering", "input.wav", "--assistant", "--target-platform", "bogus"]
        )
    assert raised.value.code == 2


def test_no_streaming_safe_reaches_the_suggester(capsys, monkeypatch) -> None:
    """prefer_streaming_safe defaults to true, so the reachable control turns it off."""
    import math

    from libsonare import _cli_mastering, cli

    sample_rate = 22_050
    samples = [
        0.5 * math.sin(2.0 * math.pi * 440.0 * index / sample_rate) for index in range(sample_rate)
    ]
    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: (samples, sample_rate))

    def explanation(*extra: str) -> list[str]:
        args = cli._build_parser().parse_args(
            [
                "mastering",
                "input.wav",
                "--assistant",
                "--enable-repair",
                "--explain",
                "--json",
                *extra,
            ]
        )
        assert _cli_mastering.cmd_mastering(args) == 0
        return json.loads(capsys.readouterr().out)["explanation"]

    assert any("streaming-safe repair enabled" in line for line in explanation())
    open_lines = explanation("--no-streaming-safe")
    assert not any("streaming-safe repair enabled" in line for line in open_lines)
    assert any("repair stages enabled" in line for line in open_lines)


@pytest.mark.parametrize(
    ("option", "first_rejected", "last_accepted"),
    [
        ("type", 9, 8),
        ("coeff-mode", 2, 1),
        ("placement", 5, 4),
        ("phase-mode", 4, 3),
        ("resolution", 6, 5),
    ],
)
def test_eq_refuses_an_enumerator_index_outside_its_enumeration(
    monkeypatch, option, first_rejected, last_accepted
) -> None:
    """An index past the enumeration maps to the first enumerator without this.

    The switches behind these options answer an unrecognized index with their
    first enumerator, so a mistyped `--type 999` applied a peak filter and exited
    0 with a normal JSON payload.
    """
    from libsonare import _cli_mastering, cli

    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0] * 2048, 48_000))
    for value in (first_rejected, -1):
        args = cli._build_parser().parse_args(["eq", "input.wav", f"--{option}={value}", "--json"])
        with pytest.raises(ValueError, match=f"invalid value for --{option}"):
            _cli_mastering.cmd_eq(args)

    # The last enumerator is inside the domain, so it must get past validation
    # rather than be refused by an off-by-one bound.
    args = cli._build_parser().parse_args(
        ["eq", "input.wav", f"--{option}", str(last_accepted), "--json"]
    )
    _cli_mastering._check_eq_enum_options(args)


def test_eq_names_and_refuses_a_params_key_the_processor_does_not_read(monkeypatch) -> None:
    """A supplied key no config builder probes took no effect at all."""
    from libsonare import _cli_mastering, cli

    monkeypatch.setattr(_cli_mastering, "_load_audio", lambda path: ([0.0] * 2048, 48_000))
    args = cli._build_parser().parse_args(
        ["eq", "input.wav", "--params", "band0.bogusKey=42", "--json"]
    )
    with pytest.raises(ValueError, match="unknown --params key for eq.equalizer"):
        _cli_mastering.cmd_eq(args)

    # A key the processor does read still runs.
    good = cli._build_parser().parse_args(
        ["eq", "input.wav", "--params", "band0.gainDb=3", "--json"]
    )
    assert _cli_mastering.cmd_eq(good) == 0
