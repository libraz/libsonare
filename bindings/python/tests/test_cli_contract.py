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
from types import SimpleNamespace

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


def _truncated_smf() -> bytes:
    body = bytes([0x00, 0x90, 0x3C, 0x64, 0x00, 0xFF, 0x01, 0x7F])
    return (
        b"MThd"
        + (6).to_bytes(4, "big")
        + (0).to_bytes(2, "big")
        + (1).to_bytes(2, "big")
        + (480).to_bytes(2, "big")
        + b"MTrk"
        + len(body).to_bytes(4, "big")
        + body
    )


TOP_LEVEL_ROUTES = (
    "version",
    "doctor",
    "info",
    "bpm",
    "key",
    "beats",
    "downbeats",
    "onsets",
    "chords",
    "sections",
    "analyze",
    "mel",
    "chroma",
    "spectral",
    "pitch",
    "hpss",
    "decompose-stems",
    "pitch-correct",
    "pitch-correct-timevarying",
    "note-move",
    "scale-quantize",
    "note-stretch",
    "polyphonic-notes",
    "polyphonic-render",
    "pitch-shift",
    "time-stretch",
    "normalize",
    "trim-silence",
    "split-silence",
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
    "mastering-pair-processor",
    "mastering-stereo-analyze",
    "mastering-chain",
    "master",
    "mastering-streaming",
    "declip",
    "repair",
    "mastering-presets",
    "mastering-suggest",
    "mastering-profile",
    "project",
    "midi-render",
    "transcribe",
    "suggest-mix",
    "mixing-presets",
    "mixing-preset",
    "mix",
    "mix-strip",
)


def test_key_profile_aliases_match_native_cli() -> None:
    """Every documented native key-profile alias is accepted by Python too."""
    from libsonare._cli_common import _parse_key_profile
    from libsonare.types import KeyProfile

    assert _parse_key_profile("budge") is KeyProfile.BELLMAN_BUDGE


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
    payload = json.loads(result.stdout)
    assert set(payload) == {"cli", "cli_version", "lib_version"}
    assert payload["cli"] == "python"
    assert payload["cli_version"] == payload["lib_version"]


def test_cli_contract_inventory_reads_argparse_action_defaults(monkeypatch, capsys) -> None:
    """Inventory output follows a mutated action, not a shadow option table."""
    from libsonare import cli

    parser = cli._build_parser()
    chroma = cli._inventory_subparsers(parser)["chroma"]
    n_fft = next(action for action in chroma._actions if action.dest == "n_fft")
    n_fft.default = 4096
    monkeypatch.setattr(cli, "_build_parser", lambda: parser)

    cli._dump_cli_contract()
    payload = json.loads(capsys.readouterr().out)
    command = next(command for command in payload["commands"] if command["path"] == "chroma")
    assert [option["name"] for option in command["options"]] == [
        "json",
        "n-fft",
        "hop-length",
    ]
    option = next(option for option in command["options"] if option["name"] == "n-fft")
    assert option["default"] == 4096


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


@pytest.mark.parametrize(
    ("command", "effect_args"),
    [
        ("pitch-shift", ("--semitones", "0")),
        ("time-stretch", ("--rate", "1")),
        ("hpss", ()),
    ],
)
def test_effect_commands_require_output(tmp_path, command, effect_args) -> None:
    """Audio-rendering effect commands require -o, matching the native CLI.

    Running one without an output destination is a parameter error (exit 3), not
    a silent no-op, so the same argv fails identically on the native and Python
    surfaces.
    """
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)
    missing = _run_console(command, str(source), *effect_args)
    assert missing.returncode == 3, missing.stderr
    out = tmp_path / "out.wav"
    ok = _run_console(command, str(source), "-o", str(out), *effect_args)
    assert ok.returncode == 0, ok.stderr


@pytest.mark.parametrize(
    ("command", "required_flag"),
    [("pitch-shift", "--semitones required"), ("time-stretch", "--rate required")],
)
def test_transform_commands_require_their_primary_parameter(
    tmp_path, command, required_flag
) -> None:
    """Primary transform parameters cannot silently select their no-op defaults."""
    source = tmp_path / "tone.wav"
    output = tmp_path / "out.wav"
    _write_tone_wav(source)

    result = _run_console(command, str(source), "-o", str(output))
    assert result.returncode == 3
    assert required_flag in result.stderr


def test_cli_invalid_parameter_exit_code_matches_native_pitch_command(tmp_path) -> None:
    """Semantic validation happens after parsing and uses exit 3 on both CLIs."""
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)

    result = _run_console("pitch", str(source), "--algorithm", "typo")
    assert result.returncode == 3
    assert "--algorithm must be 'yin' or 'pyin'" in result.stderr


def test_cli_unknown_project_subcommand_is_a_usage_error(tmp_path) -> None:
    """An unknown project route stays an argparse-style usage error (exit 2)."""
    result = _run_console("project", "not-a-subcommand")
    assert result.returncode == 2


# Every spelling of a value that parses as a float but is not one. The overflow
# literals are here because Python widens them to infinity where the native
# strtof reports a range error: both surfaces have to refuse, whichever way the
# conversion failed.
_NON_FINITE_LITERALS = ("nan", "NaN", "inf", "Infinity", "1e400")
_NEGATIVE_NON_FINITE_LITERALS = ("-nan", "-inf", "-1e400")

# One float option per declaration shape the parser uses: a numeric default, an
# assistant-gated option, a domain-narrowed converter, a None default, an option
# whose two spellings share a destination, and a positional. The mastering rows
# appear with and without --assistant because that flag selects a different
# consumer for the same value.
_NON_FINITE_FLOAT_SITES = (
    (["mastering", "input.wav"], "--target-lufs"),
    (["mastering", "input.wav", "--assistant"], "--target-lufs"),
    (["mastering", "input.wav", "--assistant"], "--speech-mono-amount"),
    (["pitch", "input.wav"], "--fmin"),
    (["normalize", "input.wav", "-o", "out.wav"], "--target-db"),
    (["time-stretch", "input.wav", "-o", "out.wav"], "--rate"),
    (["eq", "input.wav"], "--detector-delay-ms"),
    (["acoustic", "input.wav"], "--min-decay-db"),
    (["scale-quantize"], None),
)


def _float_typed_actions(parser, path=()):
    """Every parser action whose ``type=`` converts an argv token to a float.

    Derived from the parser rather than from a list, so an option added
    tomorrow is covered without anyone remembering to name it here.
    """
    from libsonare import cli

    for action in parser._actions:  # noqa: SLF001
        if not callable(action.type):
            continue
        try:
            converted = action.type("0.5")
        except Exception:  # noqa: BLE001 - a converter that refuses 0.5 is not a float option
            continue
        if isinstance(converted, float):
            yield " ".join((*path, action.dest)), action
    for name, child in cli._inventory_subparsers(parser).items():
        yield from _float_typed_actions(child, (*path, name))


def _non_finite_argv(prefix: list[str], option: str | None, literal: str) -> list[list[str]]:
    """Every argv that carries @p literal into @p option.

    A value beginning with ``-`` has only the attached spelling: argparse reads
    ``-inf`` as an option-like token, and the native parser's negative-number
    matcher does not accept it either.
    """
    if option is None:
        return [[*prefix, literal]]
    spellings = [[*prefix, f"{option}={literal}"]]
    if not literal.startswith("-"):
        spellings.append([*prefix, option, literal])
    return spellings


def test_no_cli_option_converts_its_value_with_the_unchecked_float_builtin() -> None:
    """``float`` accepts NaN and Inf, so no option may be declared with it."""
    from libsonare import cli

    actions = list(_float_typed_actions(cli._build_parser()))
    # Non-vacuity: the walk has to be finding the float options at all.
    assert len(actions) > 50
    assert [name for name, action in actions if action.type is float] == []


@pytest.mark.parametrize("literal", _NON_FINITE_LITERALS + _NEGATIVE_NON_FINITE_LITERALS)
def test_every_float_typed_cli_option_refuses_a_non_finite_value(literal) -> None:
    """The refusal belongs to the option, not to the handful that opted in."""
    import argparse

    from libsonare import cli

    accepted = []
    for name, action in _float_typed_actions(cli._build_parser()):
        try:
            action.type(literal)
        except (argparse.ArgumentTypeError, ValueError):
            continue
        accepted.append(name)
    assert accepted == []


@pytest.mark.parametrize("literal", _NON_FINITE_LITERALS + _NEGATIVE_NON_FINITE_LITERALS)
def test_a_non_finite_option_value_is_a_usage_error(literal) -> None:
    """Exit 2, the class the native CLI reports for the same argv.

    The native parser refuses a non-finite value before dispatch
    (``validate_numeric_option_values``), which is a usage-stage rejection; the
    Python CLI reaches it through the parser's type conversion.
    """
    from libsonare import cli

    for prefix, option in _NON_FINITE_FLOAT_SITES:
        # Non-vacuity: the same argv with a finite value has to parse, or the
        # exit below would be some unrelated usage error.
        finite = _non_finite_argv(prefix, option, "0.5" if option else "60.5")[0]
        cli._build_parser().parse_args(finite)

        for argv in _non_finite_argv(prefix, option, literal):
            with pytest.raises(SystemExit) as raised:
                cli._build_parser().parse_args(argv)
            assert raised.value.code == 2, argv


@pytest.mark.parametrize("interrupt_point", ["parse", "dispatch"])
def test_an_interrupt_reports_like_every_other_failure(
    monkeypatch, capsys, interrupt_point
) -> None:
    """Ctrl-C exits 11 with one Error line instead of a traceback.

    KeyboardInterrupt is a BaseException, so the dispatch-only ``except
    Exception`` never saw it and the interpreter printed its own traceback --
    and exit 11 was unreachable from this surface as a result.
    """
    from libsonare import cli
    from libsonare._cli_common import EXIT_CANCELLED

    def interrupt(*args, **kwargs):
        raise KeyboardInterrupt

    if interrupt_point == "parse":
        monkeypatch.setattr(cli, "_build_parser", interrupt)
    else:
        monkeypatch.setattr(cli, "cmd_version", interrupt)
    monkeypatch.setattr(sys, "argv", ["sonare", "version"])

    with pytest.raises(SystemExit) as raised:
        cli.main()
    assert raised.value.code == EXIT_CANCELLED
    assert capsys.readouterr().err.splitlines() == ["Error: cancelled"]


def test_an_interrupt_and_an_error_share_one_reporting_path(monkeypatch, capsys) -> None:
    """The same single Error line, with each failure's own exit code."""
    from libsonare import cli
    from libsonare._cli_common import EXIT_CANCELLED, EXIT_INVALID_PARAMETER

    monkeypatch.setattr(sys, "argv", ["sonare", "version"])
    for failure, expected, line in (
        (KeyboardInterrupt(), EXIT_CANCELLED, "Error: cancelled"),
        (ValueError("bad value"), EXIT_INVALID_PARAMETER, "Error: bad value"),
    ):
        monkeypatch.setattr(
            cli, "cmd_version", lambda args, exc=failure: (_ for _ in ()).throw(exc)
        )
        with pytest.raises(SystemExit) as raised:
            cli.main()
        assert raised.value.code == expected
        assert capsys.readouterr().err.splitlines() == [line]


def _flag_arity_actions(parser, path=()):
    """Every store_true / store_false action, with the parser that OWNS it.

    The owning parser matters: an option declared on a subparser is not in the
    top-level parser's option table, and the expansion happens at the level that
    can resolve the name -- which is the level argparse hands the tokens to.
    """
    import argparse

    from libsonare import cli

    for action in parser._actions:
        if isinstance(action, (argparse._StoreTrueAction, argparse._StoreFalseAction)):
            yield " ".join((*path, action.dest)), parser, action
    for name, child in cli._inventory_subparsers(parser).items():
        yield from _flag_arity_actions(child, (*path, name))


# The native parser accepts an inline value on EVERY flag-arity option, reading
# these four spellings (case-insensitively) as "off" and anything else as "on".
_FALSE_FLAG_SPELLINGS = ("false", "0", "no", "off", "FALSE", "Off")
_TRUE_FLAG_SPELLINGS = ("true", "yes", "1", "", "anything")


def test_every_flag_option_accepts_the_inline_value_form() -> None:
    """The divergence was total, so a partial fix would be a third behaviour.

    Checked against every store_true / store_false action the parser declares
    rather than the handful the finding named, so a flag added later inherits
    the form instead of quietly reopening the gap.
    """
    from libsonare import cli

    actions = list(_flag_arity_actions(cli._build_parser()))
    # Non-vacuity: the walk has to be finding the flags at all.
    assert len(actions) > 50

    refused = []
    for name, owner, action in actions:
        for option in action.option_strings:
            if not option.startswith("--"):
                continue
            for value in _FALSE_FLAG_SPELLINGS + _TRUE_FLAG_SPELLINGS:
                token = f"{option}={value}"
                if owner._expand_flag_values([token]) == [token]:
                    refused.append(f"{name}: {token}")
    assert refused == []


@pytest.mark.parametrize("value", _FALSE_FLAG_SPELLINGS)
def test_a_false_inline_flag_value_leaves_the_flag_off(value) -> None:
    """Only these four spellings turn a flag off, and case does not matter."""
    from libsonare import cli

    args = cli._build_parser().parse_args(["version", f"--json={value}"])
    assert args.json is False
    # Still recorded as supplied: presence is about what the caller spelled, not
    # about the value it carried.
    assert "json" in args._supplied_options


@pytest.mark.parametrize("value", _TRUE_FLAG_SPELLINGS)
def test_any_other_inline_flag_value_turns_the_flag_on(value) -> None:
    """Anything that is not one of the four false literals reads as on."""
    from libsonare import cli

    args = cli._build_parser().parse_args(["version", f"--json={value}"])
    assert args.json is True


def test_an_inline_value_on_a_valued_option_is_untouched() -> None:
    """Only flag-arity options gain the form; a valued option keeps argparse's."""
    from libsonare import cli

    args = cli._build_parser().parse_args(["mastering", "input.wav", "--target-lufs=-9.5"])
    assert args.target_lufs == pytest.approx(-9.5)
    # And a valued option still refuses a value it cannot parse.
    with pytest.raises(SystemExit) as raised:
        cli._build_parser().parse_args(["mastering", "input.wav", "--target-lufs=nope"])
    assert raised.value.code == 2


def test_inline_flag_values_match_the_native_cli() -> None:
    """The two surfaces agree spelling for spelling, read from the native source.

    The false-literal set is declared in C++ and mirrored in Python, so the two
    lists are compared directly rather than assumed to agree.
    """
    native = (
        Path(__file__).resolve().parents[3] / "tools" / "cli" / "sonare_cli_args.cpp"
    ).read_text(encoding="utf-8")
    from libsonare.cli import _FALSE_FLAG_LITERALS

    for literal in _FALSE_FLAG_LITERALS:
        assert f'lowered == "{literal}"' in native, literal
    # And nothing the native side accepts is missing from the Python set.
    start = native.index("bool is_false_flag_literal")
    body = native[start : native.index("}", start)]
    assert body.count("lowered ==") == len(_FALSE_FLAG_LITERALS)


def _native_function_body(source: str, signature: str) -> str:
    """The body of one native CLI function, read from its own source."""
    start = source.index(signature)
    return source[start : source.index("\n}", start)]


def _native_processing_source() -> str:
    return (
        Path(__file__).resolve().parents[3] / "tools" / "cli" / "sonare_cli_processing.cpp"
    ).read_text(encoding="utf-8")


def test_polyphonic_edit_fields_match_the_native_cli() -> None:
    """The accepted ``--edit`` field set is read from the native source, not assumed.

    Both CLIs have to spell one field the same way or a script written against
    either stops working on the other, so the two lists are compared directly.
    """
    from libsonare.cli import _POLYPHONIC_EDIT_FIELDS

    body = _native_function_body(_native_processing_source(), "void apply_polyphonic_note_edit")
    for field in _POLYPHONIC_EDIT_FIELDS:
        assert f'field == "{field}"' in body, field
    # And nothing the native side accepts is missing from the Python set.
    assert body.count("field == ") == len(_POLYPHONIC_EDIT_FIELDS)


def test_polyphonic_edit_flag_literals_match_the_native_cli() -> None:
    """``--edit N.muted=VALUE`` reads the same literals on both CLIs."""
    from libsonare._cli_effects import _parse_polyphonic_edit_bool
    from libsonare.cli import _FALSE_FLAG_LITERALS, _TRUE_FLAG_LITERALS

    body = _native_function_body(_native_processing_source(), "bool parse_edit_bool")
    for literal in _TRUE_FLAG_LITERALS:
        assert f'lowered == "{literal}"' in body, literal
        assert _parse_polyphonic_edit_bool("muted", literal.upper()) is True
    for literal in _FALSE_FLAG_LITERALS:
        assert f'lowered == "{literal}"' in body, literal
        assert _parse_polyphonic_edit_bool("muted", literal.upper()) is False
    assert body.count("lowered ==") == len(_TRUE_FLAG_LITERALS) + len(_FALSE_FLAG_LITERALS)


def test_a_non_finite_option_value_exits_two_from_the_console_script(tmp_path) -> None:
    """The parse-time refusal survives the real process boundary."""
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)

    result = _run_console("mastering", str(source), "--target-lufs=nan")
    assert result.returncode == 2
    assert "finite" in result.stderr


def test_pitch_correct_cli_reaches_requested_pitch(tmp_path) -> None:
    """The CLI must inherit the library's immediate constant-transpose contract."""
    import libsonare

    source = tmp_path / "tone.wav"
    output = tmp_path / "corrected.wav"
    _write_tone_wav(source, seconds=1.0, freq=220.0)
    result = _run_console(
        "pitch-correct",
        str(source),
        "--current-midi",
        "57",
        "--target-midi",
        "60",
        "-o",
        str(output),
    )
    assert result.returncode == 0, result.stderr

    with libsonare.Audio.from_file(str(output)) as corrected:
        detected = libsonare.pitch_pyin(
            corrected.data,
            sample_rate=corrected.sample_rate,
            frame_length=2048,
            hop_length=512,
            fmin=100.0,
            fmax=1000.0,
        )
    expected_hz = 220.0 * 2 ** (3 / 12)
    cents_error = 1200.0 * math.log2(detected.median_f0 / expected_hz)
    assert abs(cents_error) < 5.0


def test_project_import_smf_cli_preserves_salvaged_truncation(tmp_path) -> None:
    source = tmp_path / "truncated.mid"
    output = tmp_path / "project.json"
    source.write_bytes(_truncated_smf())
    result = _run_console("project", "import-smf", "--smf", str(source), "--output", str(output))
    assert result.returncode == 0, result.stderr
    assert output.exists()


def test_trim_silence_output_is_optional(tmp_path) -> None:
    """trim-silence doubles as analysis (it reports the trimmed length), so its
    output stays optional and it exits 0 without -o, matching the native CLI."""
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)
    result = _run_console("trim-silence", str(source), "--json")
    assert result.returncode == 0, result.stderr


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


def test_project_synth_presets_route_is_installed_smoke() -> None:
    # CLI reference lives on the docs site rather than the package README, so this
    # only smoke-tests that the `sonare project synth-presets` route is installed
    # and returns a preset list.
    result = _run_console("project", "synth-presets", "--json")
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert isinstance(payload["presets"], list)
    assert payload["presets"]


def test_info_and_lufs_json_match_the_native_cli_schema(tmp_path) -> None:
    """The shared CLI commands expose the native canonical JSON field names."""
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)

    info_result = _run_console("info", "--json", str(source))
    assert info_result.returncode == 0, info_result.stderr
    info = json.loads(info_result.stdout)
    assert set(info) == {
        "path",
        "duration",
        "sample_rate",
        "channels",
        "samples",
        "peak_db",
        "rms_db",
    }
    assert info["path"] == str(source)
    assert info["channels"] == 1

    lufs_result = _run_console("lufs", "--json", str(source))
    assert lufs_result.returncode == 0, lufs_result.stderr
    lufs = json.loads(lufs_result.stdout)
    assert set(lufs) == {"integrated_lufs", "momentary_lufs", "short_term_lufs", "loudness_range"}

    analyze_result = _run_console("analyze", "--json", str(source))
    assert analyze_result.returncode == 0, analyze_result.stderr
    analyze = json.loads(analyze_result.stdout)
    assert set(analyze) == {
        "bpm",
        "bpm_confidence",
        "key",
        "time_signature",
        "beats",
        "downbeat_indices",
        "downbeat_phase",
        "chords",
        "sections",
        "timbre",
        "dynamics",
        "rhythm",
        "form",
    }
    assert isinstance(analyze["beats"], list)
    # Downbeats are indices into beats, so every one has to address a beat.
    assert isinstance(analyze["downbeat_indices"], list)
    assert all(0 <= index < len(analyze["beats"]) for index in analyze["downbeat_indices"])
    assert isinstance(analyze["downbeat_phase"], int)


def _minimal_analysis_result(*, sections: list[object] | None = None) -> SimpleNamespace:
    """Build the smallest analysis result accepted by the JSON CLI formatter."""
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
        downbeat_indices=[],
        downbeat_phase=0,
        chords=[],
        sections=[] if sections is None else sections,
        timbre=None,
        dynamics=None,
        rhythm=None,
        form="A",
    )


def test_analyze_flags_are_accepted_and_forwarded(monkeypatch, capsys) -> None:
    """The analyze parser's options reach the existing analysis API unchanged."""
    import libsonare
    from libsonare import _cli_analysis, cli

    parser = cli._build_parser()
    defaults = parser.parse_args(["analyze", "input.wav"])
    assert defaults.with_seventh is False
    assert defaults.no_hpss is False
    assert defaults.chroma_highpass == 80.0
    assert defaults.meter_candidates == ""
    assert defaults.meter_denominator == 4

    captured: dict[str, object] = {}

    def fake_analyze(samples, **kwargs):
        captured.update(kwargs)
        return _minimal_analysis_result()

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
        # Omitted on the command line, so the core keeps its own default set
        # rather than receiving an empty one, which it rejects.
        "meter_candidate_numerators": None,
        "meter_denominator": 4,
    }


@pytest.mark.parametrize("command", ["analyze", "spectral"])
@pytest.mark.parametrize("option", ["-o", "--output"])
def test_analysis_commands_reject_ignored_output_option(tmp_path, command, option) -> None:
    """Analysis commands reject destinations instead of silently discarding them."""
    source = tmp_path / "tone.wav"
    output = tmp_path / "ignored.wav"
    _write_tone_wav(source)

    result = _run_console(command, "--json", str(source), option, str(output))
    assert result.returncode == 2, result.stderr
    assert not output.exists()


def test_analyze_json_section_type_is_lowercase_kebab(monkeypatch, capsys) -> None:
    """Section types use the Python semantic enum spelling in JSON."""
    import libsonare
    from libsonare import SectionType, _cli_analysis, cli

    section = SimpleNamespace(type=SectionType.PRE_CHORUS, start=0.0, end=1.0)
    monkeypatch.setattr(
        libsonare, "analyze", lambda samples, **kwargs: _minimal_analysis_result(sections=[section])
    )
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))

    args = cli._build_parser().parse_args(["analyze", "--json", "input.wav"])
    assert cli.cmd_analyze(args) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["sections"] == [{"type": "pre-chorus", "start": 0.0, "end": 1.0}]


def test_spectral_json_has_closed_unrounded_feature_stats(monkeypatch, capsys) -> None:
    """Spectral JSON reports every native statistic plus the shared frame count."""
    import statistics

    import libsonare
    from libsonare import _cli_analysis, cli

    values = {
        "centroid": [0.1234567, 0.7654321],
        "bandwidth": [1.234567, 2.345678],
        "rolloff": [3.456789, 4.567891],
        "flatness": [0.1111111, 0.2222222],
        "zcr": [0.3333333, 0.4444444],
        "rms": [0.5555555, 0.6666666],
    }
    for name, feature_values in values.items():
        monkeypatch.setattr(
            libsonare,
            {
                "centroid": "spectral_centroid",
                "bandwidth": "spectral_bandwidth",
                "rolloff": "spectral_rolloff",
                "flatness": "spectral_flatness",
                "zcr": "zero_crossing_rate",
                "rms": "rms_energy",
            }[name],
            lambda *args, _values=feature_values, **kwargs: _values,
        )
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))

    args = cli._build_parser().parse_args(["spectral", "--json", "input.wav"])
    assert cli.cmd_spectral(args) == 0
    payload = json.loads(capsys.readouterr().out)

    assert set(payload) == {"n_frames", "features"}
    assert payload["n_frames"] == 2
    assert set(payload["features"]) == set(values)
    for name, feature_values in values.items():
        stats = payload["features"][name]
        assert set(stats) == {"mean", "std", "min", "max"}
        assert stats["mean"] == statistics.mean(feature_values)
        assert stats["std"] == statistics.pstdev(feature_values)
        assert stats["min"] == min(feature_values)
        assert stats["max"] == max(feature_values)


def test_mastering_cli_uses_requested_true_peak_oversample(tmp_path) -> None:
    """Mastering forwards the CLI oversampling factor and reports the applied value."""
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)

    result = _run_console("mastering", "--json", "--true-peak-oversample", "8", str(source))

    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["true_peak_oversample"] == 8


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


def test_analyze_human_output_has_no_ansi_when_not_a_tty(tmp_path) -> None:
    """The human-readable analyze output must not emit ANSI color codes when
    stdout is a pipe (a subprocess is not a TTY), so redirected output stays free
    of control bytes."""
    source = tmp_path / "tone.wav"
    _write_tone_wav(source)
    result = _run_console("analyze", str(source))
    assert result.returncode == 0, result.stderr
    assert "\x1b" not in result.stdout


def test_array_stats_uses_population_standard_deviation() -> None:
    """Spectral summary ``std`` matches the native population statistic.

    The sample deviation of the same input is 1.2909944, so the tolerance here
    is far tighter than the gap the assertion has to discriminate.
    """
    from libsonare._cli_common import _array_stats

    assert _array_stats([1.0, 2.0, 3.0, 4.0], with_count=False)["std"] == pytest.approx(
        1.118033988749895
    )


@pytest.mark.parametrize(
    ("stdout_tty", "stderr_tty", "no_color", "expected"),
    [
        (True, True, False, True),
        (True, False, False, False),
        (False, True, False, False),
        (True, True, True, False),
    ],
)
def test_cli_color_requires_both_ttys_and_no_color_absent(
    monkeypatch, stdout_tty, stderr_tty, no_color, expected
) -> None:
    """ANSI output follows the native both-stream TTY and ``NO_COLOR`` contract."""
    from types import SimpleNamespace

    from libsonare import _cli_common

    class _Stream:
        def __init__(self, is_tty: bool) -> None:
            self._is_tty = is_tty

        def isatty(self) -> bool:
            return self._is_tty

    monkeypatch.setattr(
        _cli_common,
        "sys",
        SimpleNamespace(stdout=_Stream(stdout_tty), stderr=_Stream(stderr_tty)),
    )
    if no_color:
        monkeypatch.setenv("NO_COLOR", "")
    else:
        monkeypatch.delenv("NO_COLOR", raising=False)

    assert _cli_common._color_enabled() is expected


def test_synthesize_rir_json_reports_sample_rate(tmp_path) -> None:
    """RIR JSON includes the generated WAV's sample rate like the native CLI."""
    output = tmp_path / "rir.wav"
    result = _run_console(
        "synthesize-rir",
        "--sample-rate",
        "44100",
        "--max-seconds",
        "0.05",
        "--output",
        str(output),
        "--json",
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert payload["sample_rate"] == 44100
    with wave.open(str(output), "rb") as rendered:
        assert rendered.getframerate() == 44100


@pytest.mark.parametrize("command", ["abi", "synth-presets"])
def test_project_stdout_only_subcommands_reject_output(tmp_path, command) -> None:
    """project abi / synth-presets / compile write their result to stdout only.
    Passing -o would silently discard the destination, so it fails loudly and
    writes nothing rather than exiting 0 without a file."""
    dest = tmp_path / "unwanted.txt"
    result = _run_console("project", command, "-o", str(dest))
    assert result.returncode != 0
    assert not dest.exists()


def test_voice_preset_json_flag_is_accepted_noop(tmp_path) -> None:
    """voice-preset always prints JSON; the --json flag is an accepted no-op and
    the output is identical with or without it."""
    without = _run_console("voice-preset", "--preset", "neutral-monitor")
    assert without.returncode == 0, without.stderr
    json.loads(without.stdout)
    with_flag = _run_console("voice-preset", "--preset", "neutral-monitor", "--json")
    assert with_flag.returncode == 0, with_flag.stderr
    assert with_flag.stdout == without.stdout


def test_project_validate_surfaces_diagnostics_field(tmp_path) -> None:
    """validate reports the loader's repair diagnostics; a clean project yields an
    empty diagnostics list and stays valid, including under --strict."""
    proj = tmp_path / "clean.sonare"
    created = _run_console("project", "new", "-o", str(proj))
    assert created.returncode == 0, created.stderr
    result = _run_console("project", "validate", "--in", str(proj), "--json")
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert payload["valid"] is True
    assert payload["diagnostics"] == []
    assert payload["diagnostic_count"] == 0
    strict = _run_console("project", "validate", "--in", str(proj), "--strict", "--json")
    assert strict.returncode == 0, strict.stderr


def test_project_validate_strict_writes_canonical_artifact_before_exit(tmp_path) -> None:
    """Strict mode preserves a parsed payload and writes its canonical JSON."""
    warning = {
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
    source = tmp_path / "warning.json"
    output = tmp_path / "canonical.json"
    source.write_text(json.dumps(warning), encoding="utf-8")

    result = _run_console(
        "project",
        "validate",
        "--in",
        str(source),
        "--strict",
        "--output",
        str(output),
        "--json",
    )

    assert result.returncode == 9, result.stderr
    payload = json.loads(result.stdout)
    assert payload["valid"] is True
    assert payload["diagnostic_count"] == len(payload["diagnostics"]) > 0
    canonical = json.loads(output.read_text(encoding="utf-8"))
    assert isinstance(canonical["clips"], list)


def test_project_validate_malformed_document_uses_invalid_format_exit(tmp_path) -> None:
    """CLI malformed-project errors use C-ABI InvalidFormat (exit 5)."""
    source = tmp_path / "malformed.json"
    source.write_text("{ this is not valid json ", encoding="utf-8")
    result = _run_console("project", "validate", "--in", str(source), "--json")
    assert result.returncode == 5
    assert result.stdout == ""


@pytest.mark.parametrize("stored_rate", [44100, 96000])
def test_project_bounce_uses_the_project_own_sample_rate_by_default(tmp_path, stored_rate) -> None:
    """A non-48000 project bounces without --sample-rate at its own stored rate.

    Regression: `project bounce` used to pass a hardcoded 48000 to the C ABI,
    so any project stored at a different rate was rejected outright.
    """
    proj = tmp_path / "project.sonare"
    wav = tmp_path / "bounce.wav"
    created = _run_console("project", "new", "-o", str(proj), "--sample-rate", str(stored_rate))
    assert created.returncode == 0, created.stderr

    result = _run_console(
        "project", "bounce", "--in", str(proj), "-o", str(wav), "--frames", "256", "--json"
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["sample_rate"] == stored_rate
    with wave.open(str(wav), "rb") as rendered:
        assert rendered.getframerate() == stored_rate


@pytest.mark.parametrize(
    ("stored_rate", "expected_header"),
    [(44100.5, 44101), (44100.4, 44100), (22050.25, 22050)],
)
def test_project_bounce_renders_a_fractional_project_rate(
    tmp_path, stored_rate, expected_header
) -> None:
    """A rate the WAV header cannot carry still renders, at the project's rate.

    Without --sample-rate the CLI must leave the option at the C ABI's ``<= 0``
    sentinel, which is the only path that reaches the render with the stored
    rate's full precision. Pinning the rounded integer instead made the ABI's
    own equality check reject the project outright. The header still reports
    the nearest integer, rounded away from zero so it agrees with the native
    CLI's ``std::lround`` on a ``.5`` tie rather than with Python's round-half-
    to-even.
    """
    proj = tmp_path / "project.sonare"
    wav = tmp_path / "bounce.wav"
    proj.write_text(
        json.dumps({"version": 1, "sample_rate": stored_rate, "tracks": [], "clips": []}),
        encoding="utf-8",
    )

    result = _run_console(
        "project", "bounce", "--in", str(proj), "-o", str(wav), "--frames", "64", "--json"
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["sample_rate"] == expected_header
    with wave.open(str(wav), "rb") as rendered:
        assert rendered.getframerate() == expected_header

    # An explicit rate is still checked against the stored one, so the sentinel
    # did not turn the mismatch check off.
    rejected = _run_console(
        "project",
        "bounce",
        "--in",
        str(proj),
        "-o",
        str(tmp_path / "rejected.wav"),
        "--frames",
        "64",
        "--sample-rate",
        str(int(stored_rate)),
    )
    assert rejected.returncode == 3
    assert "does not match the project's sample rate" in rejected.stderr


def test_project_bounce_accepts_an_explicit_sample_rate_matching_the_project(tmp_path) -> None:
    proj = tmp_path / "project.sonare"
    wav = tmp_path / "bounce.wav"
    created = _run_console("project", "new", "-o", str(proj), "--sample-rate", "44100")
    assert created.returncode == 0, created.stderr

    result = _run_console(
        "project",
        "bounce",
        "--in",
        str(proj),
        "-o",
        str(wav),
        "--frames",
        "256",
        "--sample-rate",
        "44100",
        "--json",
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["sample_rate"] == 44100
    with wave.open(str(wav), "rb") as rendered:
        assert rendered.getframerate() == 44100


def test_project_bounce_rejects_an_explicit_sample_rate_disagreeing_with_the_project(
    tmp_path,
) -> None:
    """An explicit --sample-rate that disagrees with the project's stored rate
    fails loudly (exit 3, matching the native CLI) and names the mismatch,
    instead of silently rendering at the wrong rate or writing a partial file."""
    proj = tmp_path / "project.sonare"
    wav = tmp_path / "bounce.wav"
    created = _run_console("project", "new", "-o", str(proj), "--sample-rate", "44100")
    assert created.returncode == 0, created.stderr

    result = _run_console(
        "project",
        "bounce",
        "--in",
        str(proj),
        "-o",
        str(wav),
        "--frames",
        "256",
        "--sample-rate",
        "48000",
        "--json",
    )
    assert result.returncode == 3, result.stderr
    assert "44100" in result.stderr
    assert "48000" in result.stderr
    assert not wav.exists()


def test_mastering_cli_reports_ceiling_limited_loudness(tmp_path) -> None:
    """The default JSON payload carries the computed ceiling-clamp flag.

    Without it the only machine-readable answer to "did this reach the delivery
    target?" is a manual diff of output_lufs against target_lufs.
    """
    import libsonare

    source = tmp_path / "tone.wav"
    _write_tone_wav(source)

    limited = _run_console(
        "mastering", "--json", "--target-lufs", "-6", "--ceiling-db", "-12", str(source)
    )
    assert limited.returncode == 0, limited.stderr
    limited_payload = json.loads(limited.stdout)
    assert limited_payload["loudness_target_limited"] is True

    reachable = _run_console(
        "mastering", "--json", "--target-lufs", "-20", "--ceiling-db", "-1", str(source)
    )
    assert reachable.returncode == 0, reachable.stderr
    assert json.loads(reachable.stdout)["loudness_target_limited"] is False

    # The CLI payload must agree with the library entry points on the same input.
    with libsonare.Audio.from_file(str(source)) as audio:
        samples, sample_rate = audio.data, audio.sample_rate
    named = libsonare.mastering_process(
        "maximizer.loudnessOptimize",
        samples,
        sample_rate=sample_rate,
        params={"targetLufs": -6.0, "ceilingDb": -12.0},
    )
    assert named.loudness_target_limited is limited_payload["loudness_target_limited"]
