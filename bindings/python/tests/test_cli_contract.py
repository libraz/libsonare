"""Public command-line routing and artifact contracts."""

from __future__ import annotations

import ast
import inspect
import json
import subprocess
import sys
import wave
from pathlib import Path

import pytest

TOP_LEVEL_ROUTES = (
    "version",
    "info",
    "bpm",
    "key",
    "beats",
    "downbeats",
    "onsets",
    "chords",
    "analyze",
    "mel",
    "chroma",
    "spectral",
    "pitch",
    "hpss",
    "pitch-correct",
    "note-stretch",
    "pitch-shift",
    "time-stretch",
    "normalize",
    "trim-silence",
    "resample",
    "voice-change",
    "voice-presets",
    "voice-preset",
    "voice-preset-validate",
    "acoustic",
    "estimate-room",
    "synthesize-rir",
    "room-morph",
    "rhythm",
    "dynamics",
    "timbre",
    "lufs",
    "onset-envelope",
    "nnls-chroma",
    "tempogram",
    "plp",
    "mastering",
    "mastering-processor",
    "eq",
    "mastering-processors",
    "mastering-pair-processors",
    "mastering-pair-analyses",
    "mastering-pair-analyze",
    "mastering-chain",
    "master",
    "mastering-streaming",
    "declip",
    "mastering-presets",
    "mastering-suggest",
    "mastering-profile",
    "project",
    "midi-render",
    "mixing-presets",
    "mixing-preset",
    "mix",
)

PROJECT_ROUTES = (
    "abi",
    "new",
    "validate",
    "compile",
    "bounce",
    "export-smf",
    "import-smf",
    "export-midi2",
    "import-midi2",
    "synth-presets",
)

FFT_OPTION_CONSUMERS = {
    "key": ("cmd_key", ("n_fft", "hop_length")),
    "mel": ("cmd_mel", ("n_fft", "hop_length", "n_mels")),
    "chroma": ("cmd_chroma", ("n_fft", "hop_length")),
    "spectral": ("cmd_spectral", ("n_fft", "hop_length")),
    "onset-envelope": ("cmd_onset_envelope", ("n_fft", "hop_length", "n_mels")),
    "tempogram": ("cmd_tempogram", ("n_fft", "hop_length", "n_mels")),
    "plp": ("cmd_plp", ("n_fft", "hop_length", "n_mels")),
}


def _console_script() -> Path:
    script = Path(sys.executable).parent / "sonare"
    assert script.is_file(), f"installed console script is missing: {script}"
    return script


def _run_console(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(_console_script()), *args], capture_output=True, text=True)


def _cli_tree() -> ast.AST:
    source = Path(__file__).parents[1] / "src" / "libsonare" / "cli.py"
    return ast.parse(source.read_text(encoding="utf-8"))


def _project_tree() -> ast.AST:
    source = Path(__file__).parents[1] / "src" / "libsonare" / "_cli_project.py"
    return ast.parse(source.read_text(encoding="utf-8"))


def _parser_routes(tree: ast.AST, owner: str) -> set[str]:
    routes: set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            continue
        if node.func.attr != "add_parser" or not isinstance(node.func.value, ast.Name):
            continue
        if node.func.value.id != owner or not node.args:
            continue
        name = node.args[0]
        if isinstance(name, ast.Constant) and isinstance(name.value, str):
            routes.add(name.value)
    return routes


def _handler_routes(tree: ast.AST) -> set[str]:
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        is_commands = any(
            isinstance(target, ast.Name) and target.id == "commands" for target in node.targets
        )
        if not is_commands:
            continue
        assert isinstance(node.value, ast.Dict)
        return {
            key.value
            for key in node.value.keys
            if isinstance(key, ast.Constant) and isinstance(key.value, str)
        }
    raise AssertionError("commands dispatch table was not found")


def _project_handler_routes(tree: ast.AST) -> set[str]:
    routes: set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Compare) or len(node.ops) != 1 or len(node.comparators) != 1:
            continue
        left = node.left
        value = node.comparators[0]
        if (
            isinstance(left, ast.Name)
            and left.id == "subcommand"
            and isinstance(node.ops[0], ast.Eq)
            and isinstance(value, ast.Constant)
            and isinstance(value.value, str)
        ):
            routes.add(value.value)
    return routes


def test_installed_console_script_executes_entry_point() -> None:
    """Exercise the generated wheel-style shim, not ``python -m``."""
    result = _run_console("version", "--json")
    assert result.returncode == 0, result.stderr
    assert isinstance(json.loads(result.stdout)["lib_version"], str)


def test_route_manifest_matches_parsers_and_dispatch_tables() -> None:
    """Every public parser route has a handler and remains parseable through help."""
    cli_tree = _cli_tree()
    expected_top = set(TOP_LEVEL_ROUTES)
    expected_project = set(PROJECT_ROUTES)

    assert _parser_routes(cli_tree, "sub") == expected_top
    assert _handler_routes(cli_tree) == expected_top
    assert _parser_routes(cli_tree, "project_sub") == expected_project - {"validate", "compile"}
    assert _project_handler_routes(_project_tree()) == expected_project

    for route in TOP_LEVEL_ROUTES:
        result = _run_console(route, "--help")
        assert result.returncode == 0, f"{route}: {result.stderr}"
    for route in PROJECT_ROUTES:
        result = _run_console("project", route, "--help")
        assert result.returncode == 0, f"project {route}: {result.stderr}"


def test_fft_option_manifest_reaches_each_consuming_handler() -> None:
    """Every advertised FFT option is statically wired into its command handler."""
    from libsonare import cli

    for command, (handler_name, option_names) in FFT_OPTION_CONSUMERS.items():
        help_result = _run_console(command, "--help")
        assert help_result.returncode == 0, help_result.stderr
        handler_source = inspect.getsource(getattr(cli, handler_name))
        for option_name in option_names:
            cli_spelling = f"--{option_name.replace('_', '-')}"
            assert cli_spelling in help_result.stdout
            assert f"args.{option_name}" in handler_source


def test_readme_project_synth_presets_route_is_installed_smoke() -> None:
    readme = (Path(__file__).parents[1] / "README.md").read_text(encoding="utf-8")
    assert "sonare project synth-presets" in readme
    assert "sonare synth-presets" not in readme

    result = _run_console("project", "synth-presets", "--json")
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert isinstance(payload["presets"], list)
    assert payload["presets"]


@pytest.mark.parametrize(
    ("has_input", "has_output", "expected_code"),
    [(False, False, 0), (False, True, 3), (True, False, 3), (True, True, 0)],
)
def test_mix_input_output_contract(tmp_path, has_input, has_output, expected_code) -> None:
    """All four input/output combinations have an explicit artifact contract."""
    arguments = ["mix", "--preset", "vocalReverbSend", "--json"]
    output = tmp_path / "mix.wav"
    if has_input:
        for index in range(2):
            source = tmp_path / f"stem-{index}.wav"
            with wave.open(str(source), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(48000)
                wav.writeframes(b"\0\0" * 513)
            arguments.extend(("--input", str(source)))
    if has_output:
        arguments.extend(("--output", str(output)))

    result = _run_console(*arguments)

    assert result.returncode == expected_code, result.stderr
    assert output.exists() is (has_input and has_output)
    if output.exists():
        with wave.open(str(output), "rb") as wav:
            assert wav.getnchannels() == 2
            assert wav.getnframes() >= 513
