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
import subprocess
import sys
import wave
from pathlib import Path
from typing import cast

import pytest

from libsonare._mastering_offline import _nest_chain_config

_SAMPLE_RATE = 22_050
_DURATION_SEC = 0.5

_NATIVE_REGISTRY = Path(__file__).resolve().parents[3] / "tools" / "cli" / "sonare_cli_registry.cpp"
_NATIVE_HANDLER = Path(__file__).resolve().parents[3] / "tools" / "cli" / "sonare_cli_mastering.cpp"


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


@pytest.mark.parametrize("channels", [1, 2])
def test_mastering_assistant_starts_from_a_named_preset(
    tmp_path, monkeypatch, capsys, channels
) -> None:
    """Alongside ``--assistant``, ``--preset`` is the suggestion's base."""
    import libsonare

    source = tmp_path / "input.wav"
    _write_source(source, channels)
    seen: list[dict[str, object]] = []
    for name in ("mastering_assistant_suggest", "mastering_assistant_suggest_stereo"):
        original = getattr(libsonare, name)

        def capture(*args, _original=original, **kwargs):
            seen.append(dict(kwargs.get("params") or {}))
            return _original(*args, **kwargs)

        monkeypatch.setattr(libsonare, name, capture)

    assert _run(["mastering", str(source), "--assistant", "--preset", "classical", "--json"]) == 0
    capsys.readouterr()
    assert _run(["mastering", str(source), "--assistant", "--json"]) == 0
    capsys.readouterr()
    assert len(seen) == 2
    assert seen[0].get("preset") == "classical"
    # An omitted --preset is not a choice the caller made.
    assert "preset" not in seen[1]


def test_mastering_assistant_json_omits_the_preset_key_even_with_preset_supplied(
    tmp_path, capsys
) -> None:
    """``mastering --assistant --preset P --json`` carries no ``preset`` key.

    --preset alongside --assistant selects the suggestion's starting config
    (mode stays "assistant"), not a second route -- the payload's preset key is
    reserved for mode == "preset", matching the native CLI: preset_name is only
    ever assigned on the plain --preset route in
    tools/cli/sonare_cli_mastering.cpp, so its assistant JSON never
    carries the key either. A prior bug passed preset= unconditionally into
    the payload builder, so this same invocation carried the key only on
    Python.
    """
    source = tmp_path / "input.wav"
    _write_source(source)

    assert _run(["mastering", str(source), "--assistant", "--preset", "classical", "--json"]) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["mode"] == "assistant"
    assert "preset" not in payload


# The dispatch table lives inside the entry point, so the handler is named here
# rather than reached through it -- the same way the other CLI tests invoke one.
_HANDLERS = {
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

    expected = libsonare.mastering_assistant_suggest(planes[0], _SAMPLE_RATE).chain_config
    assert expected, "the assistant reported no chain, so nothing was compared"
    assert written["params"] == expected
    assert written["version"] == 2


def test_a_written_chain_config_is_read_back(tmp_path, capsys) -> None:
    """``mastering --chain-config`` renders the file it is given, not its own defaults."""
    source = tmp_path / "input.wav"
    _write_source(source)
    config = tmp_path / "chain.json"
    _suggest_config(source, config, capsys)

    first = tmp_path / "chain-config.wav"
    assert (
        _run(["mastering", str(source), "--chain-config", str(config), "-o", str(first), "--json"])
        == 0
    )
    capsys.readouterr()

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
                "mastering",
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
    assert output.read_bytes() != first.read_bytes()


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

    def leaves(path: str, value: object) -> list[str]:
        # Each leaf as path=value: arrays by index, booleans as 1 / 0.
        if isinstance(value, dict):
            return [item for key, sub in value.items() for item in leaves(f"{path}.{key}", sub)]
        if isinstance(value, list):
            return [
                item for index, sub in enumerate(value) for item in leaves(f"{path}.{index}", sub)
            ]
        if isinstance(value, bool):
            return [f"{path}={int(value)}"]
        return [f"{path}={value}"]

    params = cast("dict[str, object]", written["params"])
    flat = ",".join(item for key, value in params.items() for item in leaves(key, value))

    rendered: dict[str, bytes] = {}
    for label, argv in (
        ("chain-config", ["mastering", str(source), "--chain-config", str(config)]),
        ("overrides-on-pop", ["mastering", str(source), "--preset", "pop", "--params", flat]),
        (
            "overrides-on-speech",
            ["mastering", str(source), "--preset", "speech", "--params", flat],
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
def test_mastering_assistant_renders_the_chain_it_suggests(tmp_path, capsys, channels) -> None:
    """``mastering --assistant`` equals applying the suggestion it wrote out.

    Run over both channel counts because the two take different library entry
    points, and a stereo source must come back as a pair rather than a fold.
    """
    source = tmp_path / "input.wav"
    _write_source(source, channels)
    config = tmp_path / "chain.json"
    _suggest_config(source, config, capsys)

    direct = tmp_path / "assistant.wav"
    assert _run(["mastering", str(source), "--assistant", "--output", str(direct), "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["mode"] == "assistant"

    applied = tmp_path / "applied.wav"
    assert (
        _run(
            [
                "mastering",
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
        ("mastering", ["--preset", "speech", "--chain-config", "chain.json"]),
        ("mastering", ["--assistant", "--chain-config", "chain.json"]),
    ],
)
def test_chain_config_conflicts_are_refused_by_name(tmp_path, capsys, command, extra) -> None:
    source = tmp_path / "input.wav"
    _write_source(source)
    config = tmp_path / "chain.json"
    config.write_text(json.dumps({"version": 2, "params": {"loudness.enabled": True}}), "utf-8")
    argv = [
        command,
        str(source),
        *(str(config) if item == "chain.json" else item for item in extra),
    ]

    # A usage error, refused before the input is read.
    with pytest.raises(SystemExit) as caught:
        _run(argv)
    assert caught.value.code == 2
    message = capsys.readouterr().err
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
                "mastering",
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
    assert json.loads(capsys.readouterr().out)["mode"] == "chainConfig"
    assert 'mode = "chainConfig";' in _NATIVE_HANDLER.read_text(encoding="utf-8")


def test_a_loudness_target_beside_a_chain_config_is_refused(tmp_path, capsys) -> None:
    """The loudness targets reach only the assistant, so a chain config refuses them."""
    source = tmp_path / "input.wav"
    _write_source(source)
    config = tmp_path / "chain.json"
    config.write_text(json.dumps({"version": 2, "params": {"loudness.enabled": True}}), "utf-8")

    with pytest.raises(SystemExit) as caught:
        _run(["mastering", str(source), "--chain-config", str(config), "--target-lufs", "-12"])
    assert caught.value.code == 2
    assert "error: --target-lufs requires --assistant" in capsys.readouterr().err


def test_mastering_chain_config_has_no_second_spelling() -> None:
    """On ``mastering`` the chain config has one spelling, published as one record."""
    from libsonare import cli
    from libsonare._cli_inventory import _inventory_option

    mastering = (
        cli._build_parser()._subparsers._group_actions[0].choices["mastering"]  # noqa: SLF001
    )
    actions = [action for action in mastering._actions if "--chain-config" in action.option_strings]
    assert len(actions) == 1
    assert actions[0].dest == "config"
    assert actions[0].option_strings == ["--chain-config"]

    record = _inventory_option(actions[0])
    assert record is not None
    assert record["name"] == "chain-config"
    assert record["aliases"] == []
    assert record["type"] == "path"


def test_native_front_end_declares_the_same_spelling() -> None:
    """The native CLI's own declarations, since a Python run cannot exercise them."""
    registry = _NATIVE_REGISTRY.read_text(encoding="utf-8")
    assert 'path_value("chain-config")' in registry
    assert 'string_value("config-out", "")' in registry
    # The selector is settled by the command's validator, before the input is read.
    assert '"--chain-config is mutually exclusive with --preset and --assistant"' in registry

    handler = _NATIVE_HANDLER.read_text(encoding="utf-8")
    # Written through the chain serializer on the config in hand, which is the
    # one form `--chain-config` parses on either front-end.
    assert "mastering::api::chain_config_to_json(suggestion.config)" in handler
    # The chain-config selector is read under its canonical spelling.
    assert 'args.has("chain-config")' in handler
    assert 'args.get_string("chain-config")' in handler


def test_mastering_params_and_chain_config_take_an_enum_name(tmp_path, capsys) -> None:
    """``--params`` and ``--chain-config`` take ``noiseEstimator`` by name; a number is refused."""
    source = tmp_path / "input.wav"
    _write_source(source)

    def run(extra: list[str]) -> dict[str, object]:
        assert _run(["mastering", str(source), *extra, "--json"]) == 0
        return json.loads(capsys.readouterr().out)

    base = ["--preset", "pop", "--params"]
    by_name = run([*base, "repair.denoise.enabled=1,repair.denoise.noiseEstimator=mcra"])
    assert by_name["mode"] == "preset"
    with pytest.raises(SystemExit) as refused:
        _run(
            [
                "mastering",
                str(source),
                *base,
                "repair.denoise.enabled=1,repair.denoise.noiseEstimator=1",
            ]
        )
    assert refused.value.code == 2
    assert (
        "params.repair.denoise.noiseEstimator must be one of quantile, mcra, imcra, spp"
        in capsys.readouterr().err
    )

    config = tmp_path / "chain.json"
    config.write_text(
        json.dumps(
            {
                "version": 2,
                "params": {
                    "repair.denoise.enabled": True,
                    "repair.denoise.noiseEstimator": "mcra",
                },
            }
        ),
        encoding="utf-8",
    )
    from_file = run(["--chain-config", str(config)])
    assert from_file["mode"] == "chainConfig"

    with pytest.raises(SystemExit) as refused:
        _run(
            [
                "mastering",
                str(source),
                "--preset",
                "pop",
                "--params",
                "repair.denoise.noiseEstimator=nope",
            ]
        )
    assert refused.value.code == 2
    assert (
        "params.repair.denoise.noiseEstimator must be one of quantile, mcra, imcra, spp"
        in capsys.readouterr().err
    )


@pytest.mark.parametrize("channels", [1, 2])
def test_chain_config_takes_a_v2_multiband_document(tmp_path, capsys, channels) -> None:
    """A v2 document's nested multiband block renders as its flattened keys do.

    The referent is ``master_audio`` with no preset and the same config flattened to ordinal
    keys, run in the same build over the same decoded source, written by the
    CLI's own writer.
    """
    import libsonare
    from libsonare._cli_common import (
        _load_channels_or_downmix,
        _write_channel_output,
    )

    prefix = "dynamics.multibandComp"
    # A v2 band is a complete compressor record; start from the library's own defaults.
    defaults = libsonare.mastering_chain_config_from_json(json.dumps({"version": 2, "params": {}}))
    compressor = "dynamics.compressor."
    band_defaults = {
        key.removeprefix(compressor): value
        for key, value in defaults.items()
        if key.startswith(compressor) and key != compressor + "enabled"
    }
    params = {
        prefix: {
            "enabled": True,
            "crossover": {
                "cutoffsHz": [200.0, 1200.0, 5000.0],
                "slope": "lr8",
                "mode": "linkwitzRiley",
                "firKernelSize": 1024,
            },
            "bands": [
                {
                    **band_defaults,
                    "thresholdDb": -40.0 + 4 * i,
                    "ratio": 2.0 + i,
                    "makeupGainDb": 0.5 * i,
                }
                for i in range(4)
            ],
        }
    }
    source = tmp_path / "input.wav"
    _write_source(source, channels)
    document = tmp_path / "v2.json"
    document.write_text(json.dumps({"version": 2, "params": params}), encoding="utf-8")

    output = tmp_path / "cli.wav"
    assert (
        _run(["mastering", str(source), "--chain-config", str(document), "--output", str(output)])
        == 0
    )
    capsys.readouterr()

    planes, rate = _load_channels_or_downmix(str(source))
    flat = {
        f"{prefix}.enabled": True,
        **{f"{prefix}.crossover.cutoffsHz.{i}": v for i, v in enumerate((200.0, 1200.0, 5000.0))},
        f"{prefix}.crossover.slope": params[prefix]["crossover"]["slope"],
        f"{prefix}.crossover.mode": "linkwitzRiley",
        f"{prefix}.crossover.firKernelSize": 1024,
        **{
            f"{prefix}.bands.{i}.{field}": value
            for i, band in enumerate(params[prefix]["bands"])
            for field, value in band.items()
        },
    }
    if channels == 2:
        result = libsonare.master_audio_stereo(
            planes[0], planes[1], rate, preset=None, overrides=_nest_chain_config(flat)
        )
        rendered = [result.left, result.right]
    else:
        mono = libsonare.master_audio(
            planes[0], rate, preset=None, overrides=_nest_chain_config(flat)
        )
        rendered = [mono.samples]
    expected = tmp_path / "facade.wav"
    _write_channel_output(str(expected), rendered, rate)
    assert output.read_bytes() == expected.read_bytes()

    # The multiband block is read, not skipped: without it the render differs.
    plain = tmp_path / "plain.wav"
    plain_doc = tmp_path / "plain.json"
    plain_doc.write_text(json.dumps({"version": 2, "params": {"loudness.targetLufs": -14.0}}))
    assert (
        _run(["mastering", str(source), "--chain-config", str(plain_doc), "--output", str(plain)])
        == 0
    )
    capsys.readouterr()
    assert plain.read_bytes() != output.read_bytes()


@pytest.mark.parametrize(
    "params",
    [
        {"loudness.targetLufs": -14.0, "crossover.cutoff0": 200.0},
        {"loudness.targetLufs": "loud"},
    ],
)
def test_a_document_the_library_refuses_exits_as_an_invalid_format(tmp_path, params) -> None:
    source = tmp_path / "input.wav"
    _write_source(source)
    document = tmp_path / "chain.json"
    document.write_text(json.dumps({"version": 2, "params": params}), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, "-m", "libsonare.cli", "mastering", str(source)]
        + ["--chain-config", str(document), "-o", str(tmp_path / "out.wav")],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 5, result.stderr


def test_a_version_1_document_is_refused_with_the_librarys_message(tmp_path) -> None:
    source = tmp_path / "input.wav"
    _write_source(source)
    document = tmp_path / "chain.json"
    document.write_text(json.dumps({"version": 1, "params": {}}), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, "-m", "libsonare.cli", "mastering", str(source)]
        + ["--chain-config", str(document), "-o", str(tmp_path / "out.wav")],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 5, result.stderr
    assert "version must be 2, or left out" in result.stderr
