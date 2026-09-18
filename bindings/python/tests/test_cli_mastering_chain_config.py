"""Chain-config hand-off between the two command-line front-ends.

``mastering-suggest --config-out`` writes the core's own chain-config form and
``--chain-config`` reads it back, so a suggestion made on either front-end is
applicable on either. The Python half of that round trip runs here against the
real library; the native half is pinned by the spellings its sources declare,
because a Python test cannot rebuild the native binary.
"""

from __future__ import annotations

import json
import math
import struct
import wave
from pathlib import Path

import pytest

_SAMPLE_RATE = 22_050
_DURATION_SEC = 0.5

_NATIVE_REGISTRY = Path(__file__).resolve().parents[3] / "tools" / "cli" / "sonare_cli_registry.cpp"
_NATIVE_HANDLER = (
    Path(__file__).resolve().parents[3] / "tools" / "cli" / "sonare_cli_mastering_mixing.cpp"
)


def _planes(channels: int) -> list[list[float]]:
    """Half a second of tonal material with a distinct signal per channel."""
    frames = int(_SAMPLE_RATE * _DURATION_SEC)
    left = [
        0.4 * math.sin(2 * math.pi * 220.0 * i / _SAMPLE_RATE)
        + 0.15 * math.sin(2 * math.pi * 1750.0 * i / _SAMPLE_RATE)
        for i in range(frames)
    ]
    if channels == 1:
        return [left]
    right = [
        0.3 * math.sin(2 * math.pi * 331.0 * i / _SAMPLE_RATE)
        + 0.1 * math.sin(2 * math.pi * 1750.0 * i / _SAMPLE_RATE)
        for i in range(frames)
    ]
    return [left, right]


def _write_source(path: Path, channels: int = 1) -> list[list[float]]:
    planes = _planes(channels)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(_SAMPLE_RATE)
        interleaved = [
            int(max(-1.0, min(1.0, plane[frame])) * 32767)
            for frame in range(len(planes[0]))
            for plane in planes
        ]
        handle.writeframes(b"".join(struct.pack("<h", value) for value in interleaved))
    return planes


# The dispatch table lives inside the entry point, so the handler is named here
# rather than reached through it -- the same way the other CLI tests invoke one.
_HANDLERS = {
    "master": "cmd_master",
    "mastering": "cmd_mastering",
    "mastering-suggest": "cmd_mastering_suggest",
}


def _run(argv: list[str]) -> int:
    from libsonare import _cli_mastering, cli

    parsed = cli._build_parser().parse_args(argv)
    return int(getattr(_cli_mastering, _HANDLERS[argv[0]])(parsed))


def _suggest_config(source: Path, destination: Path, capsys) -> dict[str, object]:
    """Run ``mastering-suggest --config-out`` and return the file it wrote."""
    assert _run(["mastering-suggest", str(source), "--config-out", str(destination)]) == 0
    printed = json.loads(capsys.readouterr().out)
    written = json.loads(destination.read_text(encoding="utf-8"))
    # The document beside the file, from the same run: a --config-out that wrote
    # anything else would be writing a second suggestion nobody asked for.
    assert written == printed["chain_config"]
    return written


def test_config_out_writes_the_chain_config_the_core_reports(tmp_path, capsys) -> None:
    """The written params are the core's own suggestion, key for key.

    The referent is the library entry point, never a table of expected keys: a
    fixed one turns the day an assistant default moves into "the hand-off broke"
    instead of "the suggestion changed".
    """
    import libsonare

    source = tmp_path / "input.wav"
    planes = _write_source(source)
    written = _suggest_config(source, tmp_path / "chain.json", capsys)

    expected = libsonare.mastering_assistant_suggest_chain(planes[0], _SAMPLE_RATE)
    assert expected, "the assistant reported no chain, so nothing was compared"
    assert written["params"] == expected
    assert isinstance(written["version"], int)


def test_a_written_chain_config_is_read_back_under_both_spellings(tmp_path, capsys) -> None:
    """``master --chain-config``, ``mastering --chain-config`` and the original
    ``mastering --config`` render one chain-config file identically."""
    source = tmp_path / "input.wav"
    _write_source(source)
    config = tmp_path / "chain.json"
    _suggest_config(source, config, capsys)

    rendered: dict[str, bytes] = {}
    for label, argv in (
        ("master.chain-config", ["master", str(source), "--chain-config", str(config)]),
        ("mastering.chain-config", ["mastering", str(source), "--chain-config", str(config)]),
        ("mastering.config", ["mastering", str(source), "--config", str(config)]),
    ):
        output = tmp_path / f"{label}.wav"
        assert _run([*argv, "--output", str(output), "--json"]) == 0
        capsys.readouterr()
        rendered[label] = output.read_bytes()

    assert len(set(rendered.values())) == 1, {
        label: len(payload) for label, payload in rendered.items()
    }

    # The file is read rather than stood in for: one changed field moves the
    # render, so a reader that quietly mastered with its own defaults instead
    # could not pass this.
    document = json.loads(config.read_text(encoding="utf-8"))
    document["params"]["loudness.targetLufs"] = -24.0
    retargeted = tmp_path / "retargeted.json"
    retargeted.write_text(json.dumps(document), encoding="utf-8")
    output = tmp_path / "retargeted.wav"
    assert (
        _run(
            [
                "master",
                str(source),
                "--chain-config",
                str(retargeted),
                "--output",
                str(output),
                "--json",
            ]
        )
        == 0
    )
    capsys.readouterr()
    assert output.read_bytes() != rendered["master.chain-config"]


def test_applying_a_chain_config_needs_no_base_preset(tmp_path, capsys) -> None:
    """A complete chain config decides every stage on its own.

    The same params laid over two different presets and over no preset at all
    render the same bytes, which is what makes ``--chain-config`` a chain rather
    than an overlay whose result depends on what it landed on.
    """
    source = tmp_path / "input.wav"
    _write_source(source)
    config = tmp_path / "chain.json"
    written = _suggest_config(source, config, capsys)

    flat = tmp_path / "flat.json"
    flat.write_text(json.dumps(written["params"]), encoding="utf-8")

    rendered: dict[str, bytes] = {}
    for label, argv in (
        ("chain-config", ["master", str(source), "--chain-config", str(config)]),
        ("overrides-on-default", ["master", str(source), "--config-file", str(flat)]),
        (
            "overrides-on-speech",
            ["master", str(source), "--preset", "speech", "--config-file", str(flat)],
        ),
    ):
        output = tmp_path / f"{label}.wav"
        assert _run([*argv, "--output", str(output), "--json"]) == 0
        capsys.readouterr()
        rendered[label] = output.read_bytes()

    assert len(set(rendered.values())) == 1, {
        label: len(payload) for label, payload in rendered.items()
    }


@pytest.mark.parametrize("channels", [1, 2])
def test_master_assistant_renders_the_chain_it_suggests(tmp_path, capsys, channels) -> None:
    """``master --assistant`` equals applying the suggestion it wrote out.

    Run over both channel counts because the two take different library entry
    points, and a stereo source must come back as a pair rather than a fold.
    """
    source = tmp_path / "input.wav"
    _write_source(source, channels)
    config = tmp_path / "chain.json"
    _suggest_config(source, config, capsys)

    direct = tmp_path / "assistant.wav"
    assert _run(["master", str(source), "--assistant", "--output", str(direct), "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["mode"] == "assistant"

    applied = tmp_path / "applied.wav"
    assert (
        _run(
            [
                "master",
                str(source),
                "--chain-config",
                str(config),
                "--output",
                str(applied),
                "--json",
            ]
        )
        == 0
    )
    capsys.readouterr()

    assert direct.read_bytes() == applied.read_bytes()
    with wave.open(str(direct), "rb") as handle:
        assert handle.getnchannels() == channels


@pytest.mark.parametrize(
    ("command", "extra"),
    [
        ("master", ["--preset", "speech", "--chain-config", "chain.json"]),
        ("master", ["--assistant", "--chain-config", "chain.json"]),
        ("master", ["--preset", "speech", "--assistant"]),
        ("mastering", ["--preset", "speech", "--chain-config", "chain.json"]),
        ("mastering", ["--assistant", "--chain-config", "chain.json"]),
    ],
)
def test_chain_config_conflicts_are_refused_by_name(tmp_path, command, extra) -> None:
    source = tmp_path / "input.wav"
    _write_source(source)
    config = tmp_path / "chain.json"
    config.write_text(json.dumps({"version": 1, "params": {"loudness.enabled": True}}), "utf-8")
    argv = [
        command,
        str(source),
        *(str(config) if item == "chain.json" else item for item in extra),
    ]

    with pytest.raises(ValueError) as caught:
        _run(argv)
    message = str(caught.value)
    assert "mutually exclusive" in message
    for option in ("--preset", "--chain-config", "--assistant"):
        assert option in message


def test_the_reported_mode_is_the_token_the_native_cli_reports(tmp_path, capsys) -> None:
    """``mode`` names how the chain was chosen, not the option that carried it.

    Both front-ends report ``config`` for a chain read from a file, so the token
    is unaffected by the option's canonical spelling. The native value ships in a
    release asset, which is what makes this the side that matches.
    """
    source = tmp_path / "input.wav"
    _write_source(source)
    config = tmp_path / "chain.json"
    _suggest_config(source, config, capsys)

    output = tmp_path / "out.wav"
    assert (
        _run(
            [
                "master",
                str(source),
                "--chain-config",
                str(config),
                "--output",
                str(output),
                "--json",
            ]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["mode"] == "config"
    assert 'mode = "config";' in _NATIVE_HANDLER.read_text(encoding="utf-8")


@pytest.mark.parametrize("spelling", ["--chain-config", "--config"])
def test_a_loudness_flag_beside_a_chain_config_names_the_canonical_option(
    tmp_path, spelling
) -> None:
    """The refusal names ``--chain-config`` whichever spelling was used.

    Under either one the caller set the same option, so a diagnostic naming the
    legacy alias would send them looking for a second option.
    """
    source = tmp_path / "input.wav"
    _write_source(source)
    config = tmp_path / "chain.json"
    config.write_text(json.dumps({"version": 1, "params": {"loudness.enabled": True}}), "utf-8")

    with pytest.raises(ValueError) as caught:
        _run(["mastering", str(source), spelling, str(config), "--target-lufs", "-12"])
    assert str(caught.value) == "--target-lufs cannot be combined with --chain-config"


def test_mastering_chain_config_is_one_option_under_the_canonical_name() -> None:
    """On ``mastering`` the two spellings are one option, published as one record.

    ``chain-config`` is the published name and ``config`` its alias, not the other
    way round: the inventory is what a consumer reads, and the ambiguous spelling
    must not be the one it sees as primary. A second action would read as an
    option the native CLI does not have.
    """
    from libsonare import cli
    from libsonare._cli_inventory import _inventory_option

    mastering = (
        cli._build_parser()._subparsers._group_actions[0].choices["mastering"]  # noqa: SLF001
    )
    actions = [action for action in mastering._actions if "--chain-config" in action.option_strings]
    assert len(actions) == 1
    assert actions[0].dest == "config"
    assert "--config" in actions[0].option_strings

    record = _inventory_option(actions[0])
    assert record is not None
    assert record["name"] == "chain-config"
    assert record["aliases"] == ["config"]
    assert record["type"] == "path"


def test_native_front_end_declares_the_same_two_spellings() -> None:
    """The native CLI's own declarations, since a Python run cannot exercise them."""
    registry = _NATIVE_REGISTRY.read_text(encoding="utf-8")
    assert 'path_value("chain-config", false, false, true, {"config"})' in registry
    assert 'string_value("config-out", "")' in registry

    handler = _NATIVE_HANDLER.read_text(encoding="utf-8")
    # Written through the chain serializer on the config in hand, which is the
    # one form `--chain-config` parses on either front-end.
    assert "mastering::api::chain_config_to_json(suggestion.config)" in handler
    assert (
        '"--preset, --chain-config (--config), and --assistant are mutually exclusive"' in handler
    )
    # The chain-config selector is read and named under its canonical spelling,
    # matching the Python handler's refusals above.
    assert 'args.has("chain-config")' in handler
    assert 'args.get_string("chain-config")' in handler
    assert 'mode == "config" ? "chain-config" : mode' in handler
