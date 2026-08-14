"""Focused parser and inventory coverage for the Python CLI contract."""

from __future__ import annotations

import json
from types import SimpleNamespace

import pytest


def _parser():
    from libsonare import cli

    return cli._build_parser()


def _option(command: dict[str, object], name: str) -> dict[str, object]:
    options = command["options"]
    assert isinstance(options, list)
    return next(option for option in options if option["name"] == name)


def test_dump_cli_contract_uses_argparse_actions_and_schema_v2(monkeypatch, capsys) -> None:
    from libsonare import cli

    parser = _parser()
    chroma = cli._inventory_subparsers(parser)["chroma"]
    n_fft = next(action for action in chroma._actions if action.dest == "n_fft")
    n_fft.default = 4096
    monkeypatch.setattr(cli, "_build_parser", lambda: parser)
    monkeypatch.setattr(cli.sys, "argv", ["sonare", "--dump-cli-contract"])

    cli.main()

    payload = json.loads(capsys.readouterr().out)
    assert payload["schema_version"] == 2
    chroma_record = next(command for command in payload["commands"] if command["path"] == "chroma")
    assert _option(chroma_record, "n-fft")["default"] == 4096
    for command in payload["commands"]:
        for option in command["options"]:
            assert set(option) == {"name", "type", "default", "aliases", "repeatable", "required"}


def test_inventory_preserves_required_repeatable_and_none_defaults() -> None:
    from libsonare._cli_inventory import build_cli_contract

    commands = {record["path"]: record for record in build_cli_contract(_parser())["commands"]}

    key_n_fft = _option(commands["key"], "n-fft")
    assert key_n_fft["default"] == 4096
    bounce_rate = _option(commands["project.bounce"], "sample-rate")
    assert bounce_rate["default"] is None
    assert bounce_rate["required"] is False

    project_input = _option(commands["project.validate"], "in")
    assert project_input["required"] is True
    assert project_input["default"] is None
    project_output = _option(commands["project.validate"], "output")
    assert project_output["type"] == "path"
    assert project_output["default"] is None
    assert project_output["aliases"] == ["o"]
    assert project_output["required"] is False

    overrides = _option(commands["voice-preset-validate"], "set")
    assert overrides["repeatable"] is True
    assert overrides["default"] == []


def test_inventory_exposes_canonical_aliases_and_handler_options() -> None:
    from libsonare._cli_inventory import build_cli_contract

    commands = {record["path"]: record for record in build_cli_contract(_parser())["commands"]}

    assert _option(commands["key"], "use-hpss")["aliases"] == ["hpss"]
    assert _option(commands["chords"], "smoothing-window")["default"] == 2.0
    assert _option(commands["chords"], "no-beat-sync")["default"] is False
    assert _option(commands["dynamics"], "window-sec")["default"] == 0.4
    assert _option(commands["rhythm"], "start-bpm")["default"] == 120.0
    assert _option(commands["acoustic"], "n-bands")["default"] == 6
    room_bands = _option(commands["estimate-room"], "n-octave-bands")
    assert room_bands["aliases"] == ["n-bands"]
    assert room_bands["default"] is None
    assert _option(commands["mastering-processor"], "processor")["required"] is True
    assert _option(commands["mastering-processor"], "processor")["default"] is None
    assert _option(commands["mastering-processor"], "bits")["default"] == 16
    assert _option(commands["mastering-processor"], "stereo")["default"] is False
    for option_name in ("analysis", "reference"):
        assert _option(commands["mastering-pair-analyze"], option_name)["required"] is True
        assert _option(commands["mastering-pair-analyze"], option_name)["default"] is None


def test_handler_level_requirements_remain_outside_argparse() -> None:
    parser = _parser()

    # These commands historically report a missing destination/value from the
    # handler (invalid-parameter), so parser construction must remain lenient.
    for argv in (
        ["pitch-shift", "input.wav"],
        ["time-stretch", "input.wav"],
        ["hpss", "input.wav"],
    ):
        parser.parse_args(argv)

    with pytest.raises(SystemExit) as exc_info:
        parser.parse_args(["resample", "input.wav"])
    assert exc_info.value.code == 2


@pytest.mark.parametrize(
    "argv",
    [
        ["mastering-processor", "input.wav"],
        ["mastering-pair-analyze", "input.wav", "--analysis", "loudness"],
        ["mastering-pair-analyze", "input.wav", "--reference", "reference.wav"],
    ],
)
@pytest.mark.parametrize(
    ("legacy", "expected_code"),
    [(False, 2), (True, 1)],
    ids=["normal", "legacy"],
)
def test_mastering_required_options_are_parser_errors(
    monkeypatch: pytest.MonkeyPatch,
    argv: list[str],
    legacy: bool,
    expected_code: int,
) -> None:
    if legacy:
        monkeypatch.setenv("SONARE_LEGACY_EXIT", "1")
    else:
        monkeypatch.delenv("SONARE_LEGACY_EXIT", raising=False)

    with pytest.raises(SystemExit) as exc_info:
        _parser().parse_args(argv)
    assert exc_info.value.code == expected_code


def test_output_actions_are_scoped_to_artifact_commands() -> None:
    from libsonare._cli_inventory import build_cli_contract

    commands = {record["path"]: record for record in build_cli_contract(_parser())["commands"]}
    for path in ("version", "analyze", "chroma", "rhythm", "mastering-processors"):
        assert "output" not in {option["name"] for option in commands[path]["options"]}
    for path in ("hpss", "normalize", "resample", "mastering-processor", "project.validate"):
        assert _option(commands[path], "output")["type"] == "path"


def test_trim_silence_selector_defaults_are_dynamic() -> None:
    from libsonare._cli_inventory import build_cli_contract

    commands = {record["path"]: record for record in build_cli_contract(_parser())["commands"]}
    assert _option(commands["trim-silence"], "threshold-db")["default"] is None

    parser = _parser()
    default_args = parser.parse_args(["trim-silence", "input.wav"])
    assert default_args.threshold_db == -60.0

    top_args = parser.parse_args(["trim-silence", "input.wav", "--top-db", "24"])
    assert top_args.threshold_db is None
    assert top_args.top_db == 24.0

    threshold_args = parser.parse_args(["trim-silence", "input.wav", "--threshold-db", "-45"])
    assert threshold_args.threshold_db == -45.0
    assert threshold_args.top_db is None


@pytest.mark.parametrize(
    "argv",
    [
        ["version", "--output", "ignored"],
        ["chroma", "input.wav", "-o", "ignored"],
        ["voice-preset-validate", "--output", "ignored"],
        ["analyze", "input.wav", "--output", "ignored"],
        ["voice-presets", "-o", "ignored"],
        ["spectral", "input.wav", "--output", "ignored"],
        ["project", "--output", "ignored", "compile", "--in", "project.json"],
    ],
)
@pytest.mark.parametrize(
    ("legacy", "expected_code"),
    [(False, 2), (True, 1)],
    ids=["normal", "legacy"],
)
def test_stdout_only_output_is_parser_failure_without_dispatch(
    monkeypatch: pytest.MonkeyPatch,
    argv: list[str],
    legacy: bool,
    expected_code: int,
) -> None:
    from libsonare import cli

    if legacy:
        monkeypatch.setenv("SONARE_LEGACY_EXIT", "1")
    else:
        monkeypatch.delenv("SONARE_LEGACY_EXIT", raising=False)

    called = False

    def handler(_args: object) -> int:
        nonlocal called
        called = True
        return 0

    monkeypatch.setattr(cli, "cmd_version", handler)
    monkeypatch.setattr(cli.sys, "argv", ["sonare", *argv])
    with pytest.raises(SystemExit) as exc_info:
        cli.main()

    assert exc_info.value.code == expected_code
    assert called is False


@pytest.mark.parametrize(
    "extra",
    [
        ["--chroma-highpass", "nan"],
        ["--chroma-highpass", "inf"],
        ["--chroma-highpass", "-1"],
        ["--with-seventh=true"],
    ],
)
def test_analyze_options_have_strict_parser_validation(
    monkeypatch: pytest.MonkeyPatch, extra: list[str]
) -> None:
    monkeypatch.delenv("SONARE_LEGACY_EXIT", raising=False)
    with pytest.raises(SystemExit) as exc_info:
        _parser().parse_args(["analyze", "input.wav", *extra])
    assert exc_info.value.code == 2


def test_analyze_options_forward_existing_api_keywords(monkeypatch, capsys) -> None:
    import libsonare
    from libsonare import _cli_analysis, cli

    parser = _parser()
    defaults = parser.parse_args(["analyze", "input.wav"])
    assert defaults.with_seventh is False
    assert defaults.no_hpss is False
    assert defaults.chroma_highpass == 80.0

    captured: dict[str, object] = {}

    def fake_analyze(samples: object, **kwargs: object) -> SimpleNamespace:
        captured.update(kwargs)
        return SimpleNamespace(
            bpm=120.0,
            bpm_confidence=0.5,
            key=SimpleNamespace(
                root=SimpleNamespace(value=0),
                mode=SimpleNamespace(value=0),
                confidence=0.5,
            ),
            time_signature=SimpleNamespace(numerator=4, denominator=4, confidence=0.5),
            beats=[],
            chords=[],
            sections=[],
            timbre=None,
            dynamics=None,
            rhythm=None,
            form="A",
        )

    monkeypatch.setattr(libsonare, "analyze", fake_analyze)
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))

    args = parser.parse_args(
        [
            "analyze",
            "--with-seventh",
            "--no-hpss",
            "--chroma-highpass",
            "123.5",
            "--json",
            "input.wav",
        ]
    )
    assert cli.cmd_analyze(args) == 0
    json.loads(capsys.readouterr().out)
    assert captured == {
        "sample_rate": 22050,
        "use_triads_only": False,
        "use_hpss": False,
        "chroma_highpass_hz": 123.5,
    }


def test_missing_command_is_usage_error_with_empty_stdout(monkeypatch, capsys) -> None:
    from libsonare import cli

    monkeypatch.delenv("SONARE_LEGACY_EXIT", raising=False)
    monkeypatch.setattr(cli.sys, "argv", ["sonare"])
    with pytest.raises(SystemExit) as exc_info:
        cli.main()
    captured = capsys.readouterr()
    assert exc_info.value.code == 2
    assert captured.out == ""
    assert "usage:" in captured.err
    assert "error:" in captured.err


def test_project_validate_strict_writes_canonical_artifact(tmp_path) -> None:
    from libsonare import cli

    source = tmp_path / "warning.json"
    output = tmp_path / "canonical.json"
    source.write_text(
        json.dumps(
            {
                "version": 1,
                "sample_rate": 48000,
                "tracks": [
                    {
                        "id": 1,
                        "name": "audio",
                        "kind": 0,
                        "channel_strip_ref": "",
                        "output_target": "",
                        "midi_destination_id": 0,
                        "automation_lanes": [],
                    }
                ],
                "clips": [
                    {
                        "id": 1,
                        "track_id": 1,
                        "source_id": 99,
                        "start_ppq": 0,
                        "length_ppq": 1,
                        "source_offset_ppq": 0,
                        "gain": 1,
                        "fade_in": {"length_ppq": 0, "curve": 0},
                        "fade_out": {"length_ppq": 0, "curve": 0},
                        "loop_mode": 0,
                        "loop_length_ppq": 0,
                        "warp_ref_id": 0,
                        "warp_mode": 0,
                    }
                ],
            }
        ),
        encoding="utf-8",
    )

    parser = cli._build_parser()
    before = parser.parse_args(
        [
            "project",
            "--output",
            str(output),
            "validate",
            "--in",
            str(source),
            "--strict",
            "--json",
        ]
    )
    assert before.output == str(output)
    args = parser.parse_args(
        [
            "project",
            "validate",
            "--in",
            str(source),
            "--strict",
            "--output",
            str(output),
            "--json",
        ]
    )
    assert cli.cmd_project(args) == 9
    assert output.exists()
    assert isinstance(json.loads(output.read_text(encoding="utf-8"))["clips"], list)
