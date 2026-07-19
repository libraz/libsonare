"""Public command-line routing and artifact contracts."""

from __future__ import annotations

import ast
import inspect
import json
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

import pytest


def _write_tone_wav(
    path: Path, *, seconds: float = 0.6, sample_rate: int = 22050, freq: float = 220.0
) -> None:
    """Write a short mono PCM16 tone so command wiring runs on decodable audio.

    A missing-file invocation fails inside ``_load_audio`` before the parser
    wiring is ever exercised, so parser-configuration regressions only surface
    when a command receives valid audio.
    """
    frame_count = int(seconds * sample_rate)
    frames = bytearray()
    for index in range(frame_count):
        value = int(0.3 * 32767 * math.sin(2.0 * math.pi * freq * index / sample_rate))
        frames += struct.pack("<h", value)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(bytes(frames))


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
    "chords": ("cmd_chords", ("n_fft", "hop_length")),
    "mel": ("cmd_mel", ("n_fft", "hop_length", "n_mels")),
    "chroma": ("cmd_chroma", ("n_fft", "hop_length")),
    "spectral": ("cmd_spectral", ("n_fft", "hop_length")),
    "timbre": ("cmd_timbre", ("n_fft", "hop_length", "n_mels")),
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


@pytest.mark.parametrize("command", ["chords", "timbre"])
def test_fft_consumer_commands_run_end_to_end(tmp_path, command) -> None:
    """FFT/mel-consuming commands must parse and run on real audio.

    Regression guard: ``chords``/``timbre`` previously inherited only the shared
    parser (no ``--n-fft``/``--hop-length``/``--n-mels``) and crashed with an
    ``AttributeError`` on any valid input. A hand-built ``Namespace`` bypasses
    the parser and cannot catch this, so drive the installed console script.
    """
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)
    result = _run_console(command, "--json", str(source))
    assert result.returncode == 0, result.stderr
    json.loads(result.stdout)


def test_key_default_n_fft_resolves_to_4096(tmp_path) -> None:
    """``key`` keeps the 4096 analysis default when ``--n-fft`` is omitted.

    The command warns only when an explicit sub-4096 value is supplied, so the
    absence/presence of the warning is an observable proxy for the sentinel
    (``default=None`` -> 4096) resolution and matches the native CLI.
    """
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)

    default_run = _run_console("key", "--json", str(source))
    assert default_run.returncode == 0, default_run.stderr
    assert "prefers --n-fft >= 4096" not in default_run.stderr

    explicit_run = _run_console("key", "--n-fft", "2048", "--json", str(source))
    assert explicit_run.returncode == 0, explicit_run.stderr
    assert "prefers --n-fft >= 4096" in explicit_run.stderr


def test_key_default_matches_library_detect_key(tmp_path) -> None:
    """The default ``key`` CLI result matches ``detect_key`` (library default 4096)."""
    from libsonare import cli, detect_key

    source = tmp_path / "tone.wav"
    _write_tone_wav(source)

    result = _run_console("key", "--json", str(source))
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)

    # Load through the same facade the CLI uses so the samples are identical.
    samples, sr = cli._load_audio(str(source))
    key = detect_key(samples, sample_rate=sr)
    assert payload["root"] == key.root.value
    assert payload["mode"] == key.mode.value


@pytest.mark.parametrize("needs_audio", [False, True])
def test_rir_sabine_flag_is_wired(tmp_path, needs_audio) -> None:
    """``synthesize-rir``/``room-morph`` expose the Sabine late-reverb model.

    Both routes previously offered no model choice (Eyring only). A ``--sabine``
    run must succeed and produce a different tail than the default Eyring run.
    """
    default_out = tmp_path / "default.wav"
    sabine_out = tmp_path / "sabine.wav"
    if needs_audio:
        source = tmp_path / "tone.wav"
        _write_tone_wav(source, seconds=0.4)
        base = ["room-morph", str(source)]
    else:
        base = ["synthesize-rir"]

    default_run = _run_console(*base, "-o", str(default_out))
    assert default_run.returncode == 0, default_run.stderr
    sabine_run = _run_console(*base, "-o", str(sabine_out), "--sabine")
    assert sabine_run.returncode == 0, sabine_run.stderr

    assert default_out.read_bytes() != sabine_out.read_bytes()


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
