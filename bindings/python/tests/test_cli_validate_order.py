"""CLI tests: a cross-option refusal is reached before the input is read."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from ._analyzer_helpers import _run_cli

_ROOT = Path(__file__).resolve().parents[3]
_UNREADABLE = "/nonexistent/sonare-tests/unreadable.wav"
_CHANNELS = (
    "argument --channels: must be one of 1, 2, 6, 8; leave it out for the scene master's width"
)


@pytest.mark.parametrize(
    ("argv", "message"),
    [
        (
            ["key", _UNREADABLE, "--hpss", "--hop-length", "8"],
            "--hop-length must be at least 16 with --hpss",
        ),
        (
            ["rhythm", _UNREADABLE, "--bpm-min", "250"],
            "--bpm-max must be greater than --bpm-min (--bpm-min 250, --bpm-max 200)",
        ),
        (["project", "bounce", _UNREADABLE, "--channels", "3", "-o", "{output}"], _CHANNELS),
        (["midi-render", _UNREADABLE, "--channels", "3", "-o", "{output}"], _CHANNELS),
        (["repair", _UNREADABLE], "--output is required unless --detect is given"),
        (
            ["note-move", _UNREADABLE, "--onset-sample", "0", "--onset-sec", "0", "-o", "{output}"],
            "give --onset-sample or --onset-sec, not both",
        ),
        (
            [
                "note-stretch",
                _UNREADABLE,
                "--offset-sample",
                "9",
                "--offset-sec",
                "1",
                "-o",
                "{output}",
            ],
            "give --offset-sample or --offset-sec, not both",
        ),
        (
            ["mix", "--preset", "vocalReverbSend", "--input", _UNREADABLE],
            "mix with --input requires --output",
        ),
        (
            ["mix", "--preset", "vocalReverbSend", "-o", "{output}"],
            "mix with --output requires at least one --input",
        ),
        (
            ["pitch-shift", _UNREADABLE, "-o", "{output}"],
            "Missing required option '--pitch-semitones'",
        ),
        (["time-stretch", _UNREADABLE, "-o", "{output}"], "Missing required option '--rate'"),
        (["hpss", _UNREADABLE], "Missing required option '--output'"),
        (
            ["repair", _UNREADABLE, "-o", "{output}", "--explain", "--preset", "pop"],
            "--explain cannot be combined with --preset",
        ),
        (
            ["repair", _UNREADABLE, "-o", "{output}", "--stages", "declip,reverb"],
            "--stages: unknown stage 'reverb'",
        ),
        (
            ["mastering", _UNREADABLE, "--preset", "pop", "--enable-repair"],
            "--enable-repair requires --assistant",
        ),
        (["mastering", _UNREADABLE], "mastering needs --preset or --chain-config"),
        (
            ["mastering-streaming", _UNREADABLE, "--platforms", '[{"name":""}]'],
            "argument --platforms: platforms[0].target_lufs must be given",
        ),
    ],
)
def test_cross_option_refusal_precedes_the_input_read(tmp_path, argv, message) -> None:
    output = str(tmp_path / "o.wav")
    result = _run_cli([output if token == "{output}" else token for token in argv])
    assert result.returncode == 2, result.stderr
    assert message in result.stderr
    assert "unreadable" not in result.stderr


@pytest.mark.parametrize("channels", ["1", "2", "6", "8"])
def test_a_layout_channel_count_passes_validation(tmp_path, channels) -> None:
    result = _run_cli(
        ["project", "bounce", _UNREADABLE, "--channels", channels, "-o", str(tmp_path / "o.wav")]
    )
    assert result.returncode == 4, result.stderr
    assert "--channels" not in result.stderr


@pytest.mark.parametrize("command", [["project", "bounce"], ["midi-render"]])
def test_a_render_length_past_a_c_int_passes_validation(tmp_path, command) -> None:
    """The render length is an int64 count, so a value past INT_MAX reaches the input read."""
    result = _run_cli(
        [*command, _UNREADABLE, "--frames", "3000000000", "-o", str(tmp_path / "o.wav")]
    )
    assert result.returncode == 4, result.stderr
    assert "--frames" not in result.stderr


def _domain_rows() -> list[tuple[str, str]]:
    declaration = json.loads((_ROOT / "tests/conformance/cli_option_domains.json").read_text())
    return [(row["command"], row["option"]) for row in declaration["domains"]]


@pytest.mark.parametrize(("path", "name"), _domain_rows())
def test_every_declared_domain_is_refused_on_the_command_line(path, name, capsys) -> None:
    """Each declared domain is refused at parse time, exit 2, before any input is read."""
    import sys

    from libsonare import cli
    from libsonare._cli_inventory import build_cli_contract

    for directory in ("tools", "tools/conformance"):
        if str(_ROOT / directory) not in sys.path:
            sys.path.insert(0, str(_ROOT / directory))
    from cli_contract_refusals import expected_predicate, inside_value, outside_value

    parser = cli._build_parser()
    commands = {record["path"]: record for record in build_cli_contract(parser)["commands"]}
    if path not in commands:
        pytest.skip(f"{path} is not carried by the Python CLI")
    options = commands[path]["options"]
    option = next(record for record in options if record["name"] == name)
    manifest = json.loads((_ROOT / "tests/conformance/cli_contract_v2.json").read_text())
    positionals = next(
        (record["positionals"] for record in manifest["active_paths"] if record["path"] == path),
        [],
    )
    argv = [
        *path.split("."),
        *(
            _UNREADABLE if record["type"] == "path" else "1"
            for record in positionals
            if record["required"]
        ),
        f"--{name}",
        outside_value(option["domain"], option["type"]),
    ]
    for record in options:
        if record["required"] and record["name"] != name:
            argv += [f"--{record['name']}", inside_value(record, Path("/nonexistent"))]

    with pytest.raises(SystemExit) as exc_info:
        parser.parse_args(argv)
    assert exc_info.value.code == 2
    predicate = expected_predicate(option["domain"], option["type"])
    assert f"argument --{name}: {predicate}" in capsys.readouterr().err
