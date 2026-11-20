"""Refusal-text agreement between the two CLIs for every declared option domain.

Each row of tests/conformance/cli_option_domains.json names an option whose accepted
set is narrower than its type. Both front-ends are given one value outside that set and
must refuse it with the exit class the row declares and with the line
``argument --X: <predicate>``, the predicate generated here from the row in the
library's vocabulary (src/util/refusal_predicates.h) rather than read from either
front-end. Every input positional names a file that cannot be opened, so a refusal
that waits for the input to be read fails as a file error instead of passing. The
rows a run could not compare -- a command one build does not carry -- are counted
and named, never folded into a pass.

The cross-option rows hold the command validators to the same order: each pairs
options that are legal alone and refused together, again beside an unreadable input.
"""

from __future__ import annotations

import json
import math
import re
import struct
from collections.abc import Callable
from pathlib import Path
from typing import Any

from _repo import REPO_ROOT

DOMAINS_PATH = REPO_ROOT / "tests" / "conformance" / "cli_option_domains.json"

# Anything decidable from the command line alone is refused with this exit.
EXIT_USAGE = 2

# An input no front-end can open; reading it before validating fails the row.
UNREADABLE_INPUT = "/nonexistent/sonare-conformance/unreadable.wav"

# One line of either front-end's stderr; the prefixes around it differ by design.
_REFUSAL_TEXT = re.compile(r"argument --[\w-]+: must [^\n]*$", re.MULTILINE)


def _bound_text(value: float) -> str:
    """The library's ``bound_text``: the shortest decimal reading back as ``value``.

    A bound a 32-bit float holds exactly is written as that float's shortest decimal.
    """

    def as_float32(number: float) -> float:
        return float(struct.unpack("f", struct.pack("f", number))[0])

    def bare(text: str) -> str:
        mantissa, e, exponent = text.partition("e")
        if not e:
            return text
        sign = "-" if exponent.startswith("-") else ""
        return f"{mantissa}e{sign}{exponent.lstrip('+-').lstrip('0') or '0'}"

    if math.isfinite(value) and as_float32(value) == value:
        for digits in range(6, 10):
            text = f"{value:.{digits}g}"
            if as_float32(float(text)) == as_float32(value):
                return bare(text)
    for digits in range(6, 17):
        text = f"{value:.{digits}g}"
        if float(text) == value:
            return bare(text)
    return bare(f"{value:.17g}")


def _integer_bound_text(value: float) -> str:
    """The library's ``integer_bound_text``: a whole bound in plain digits."""
    if math.isfinite(value) and float(value).is_integer() and abs(value) < 2.0**63:
        return str(int(value))
    return _bound_text(value)


def expected_predicate(domain: dict[str, Any], scalar_type: str) -> str:
    """The predicate both front-ends must state for ``domain``, in the library's words."""
    if domain["choices"]:
        return "must be one of " + ", ".join(str(choice) for choice in domain["choices"])
    low, high = domain["minimum"], domain["maximum"]
    if scalar_type == "integer" and low is not None:
        low = low + 1 if domain["exclusive_minimum"] else low
        if high is None:
            return f"must be an integer of at least {int(low)}"
        high = high - 1 if domain["exclusive_maximum"] else high
        return f"must be an integer in [{_integer_bound_text(low)}, {_integer_bound_text(high)}]"
    if low is not None and high is not None:
        opening = "(" if domain["exclusive_minimum"] else "["
        closing = ")" if domain["exclusive_maximum"] else "]"
        return f"must be finite and in {opening}{_bound_text(low)}, {_bound_text(high)}{closing}"
    if low is not None:
        word = "greater than" if domain["exclusive_minimum"] else "at least"
        return f"must be finite and {word} {_bound_text(low)}"
    word = "less than" if domain["exclusive_maximum"] else "at most"
    return f"must be finite and {word} {_bound_text(high)}"


Runner = Callable[[str, str, list[str], float], dict[str, Any]]


def _number_text(value: float, integer: bool) -> str:
    if integer or float(value).is_integer():
        return str(int(value))
    return repr(float(value))


def outside_value(domain: dict[str, Any], scalar_type: str) -> str:
    """One value of the option's scalar type that ``domain`` refuses."""
    integer = scalar_type == "integer"
    choices = domain["choices"]
    if choices:
        numbers = [float(choice) for choice in choices if _is_number(choice)]
        if numbers and len(numbers) == len(choices):
            return _number_text(max(numbers) + 1, integer)
        return "not-a-choice"
    if domain["minimum"] is not None:
        minimum = float(domain["minimum"])
        return _number_text(minimum if domain["exclusive_minimum"] else minimum - 1, integer)
    maximum = float(domain["maximum"])
    return _number_text(maximum if domain["exclusive_maximum"] else maximum + 1, integer)


def inside_value(option: dict[str, Any], directory: Path) -> str:
    """A value a required option accepts, so the refusal under test is the first one reached."""
    domain = option.get("domain")
    if domain:
        if domain["choices"]:
            return str(domain["choices"][0])
        if domain["minimum"] is not None:
            minimum = float(domain["minimum"])
            value = minimum + 1 if domain["exclusive_minimum"] else minimum
            return _number_text(value, option["type"] == "integer")
    if option["type"] == "path":
        return str(directory / f"refusal_{option['name']}.out")
    if option["type"] in {"integer", "number"}:
        return "1"
    return "x"


def _is_number(text: object) -> bool:
    try:
        return math.isfinite(float(str(text)))
    except ValueError:
        return False


def positional_tokens(positionals: list[dict[str, Any]]) -> list[str]:
    """One token per required positional: an unreadable input for a path, else ``1``."""
    return [
        UNREADABLE_INPUT if record["type"] == "path" else "1"
        for record in positionals
        if record["required"]
    ]


def _argv(
    path: str, positionals: list[str], option: str, value: str, required: list[str]
) -> list[str]:
    # The value under test comes before the other options, so the Python parser
    # reaches it before anything else it could refuse.
    return [*path.split("."), *positionals, f"--{option}", value, *required]


def run_domain_refusals(
    run: Runner,
    native_executable: str,
    python_executable: str,
    native_inventory: dict[str, dict[str, Any]],
    python_inventory: dict[str, dict[str, Any]],
    positionals: dict[str, list[dict[str, Any]]],
    directory: Path,
    timeout: float,
    report: list[tuple[str, str]],
) -> tuple[int, list[str]]:
    """Compare both front-ends' refusal of every declared domain.

    ``positionals`` maps a command path to its declared positionals; a shared command
    with none recorded is still compared, without an input, and named in the second
    list so the ordering it leaves unchecked is visible.

    Returns the number of rows compared and the rows that could not be, or could be
    only without an input, each with its reason.
    """
    declaration = json.loads(DOMAINS_PATH.read_text(encoding="utf-8"))
    compared = 0
    not_compared: list[str] = []
    for row in declaration["domains"]:
        path, name = row["command"], row["option"]
        key = f"{path} --{name}"
        if path not in native_inventory:
            not_compared.append(f"{key}: not in the native build")
            continue
        if path not in python_inventory:
            not_compared.append(f"{key}: not carried by the Python CLI")
            continue
        options = native_inventory[path]["options"]
        option = next(record for record in options if record["name"] == name)
        required = [
            token
            for record in options
            if record["name"] != name and record["required"]
            for token in (f"--{record['name']}", inside_value(record, directory))
        ]
        if path not in positionals:
            not_compared.append(f"{key}: no positional record, compared without an input")
        argv = _argv(
            path,
            positional_tokens(positionals.get(path, [])),
            name,
            outside_value(row, option["type"]),
            required,
        )
        expected_text = f"argument --{name}: {expected_predicate(row, option['type'])}"
        expected_exit = EXIT_USAGE
        texts = {}
        for surface, executable in (("native", native_executable), ("python", python_executable)):
            result = run(surface, executable, argv, timeout)
            label = f"refusal.{surface}.{key}"
            if result["returncode"] != expected_exit:
                detail = result.get("error") or f"exit {result['returncode']}"
                report.append(
                    (
                        "fail",
                        (
                            f"{label}: expected exit {expected_exit}, got {detail}: "
                            f"{result['stderr'].strip()[-200:]!r}"
                        ),
                    )
                )
            match = _REFUSAL_TEXT.search(result["stderr"])
            if match is None:
                report.append(
                    (
                        "fail",
                        (
                            f"{label}: no option-domain refusal text in "
                            f"{result['stderr'].strip()[-200:]!r}"
                        ),
                    )
                )
            texts[surface] = match.group(0) if match else None
            if match is not None and match.group(0) != expected_text:
                report.append(("fail", f"{label}: {match.group(0)!r} is not {expected_text!r}"))
        compared += 1
        if None not in texts.values() and texts["native"] != texts["python"]:
            report.append(
                (
                    "fail",
                    (
                        f"refusal.{key}: the front-ends word it differently\n"
                        f"  native: {texts['native']}\n  python: {texts['python']}"
                    ),
                )
            )
    return compared, not_compared


# (argv, the one line both front-ends must print); each is refused with exit 2. ``{input}`` is an unreadable
# input and ``{output}`` a scratch path; each pair is legal option by option.
CROSS_OPTION_ROWS: tuple[tuple[list[str], str], ...] = (
    (
        ["bpm", "{input}", "-o", "{output}"],
        "Command 'bpm' does not produce a file output; remove -o/--output",
    ),
    (["suggest-mix"], "suggest-mix requires at least one --input"),
    (["suggest-mix", "--input", "=x"], "--input track id must not be empty: =x"),
    (["suggest-mix", "--input", "a="], "--input requires a file path: a="),
    (
        ["suggest-mix", "--input", "{input}", "--params", "novalue"],
        "invalid parameter entry: 'novalue' (expected key=value)",
    ),
    (
        ["suggest-mix", "--input", "{input}", "--params", "tempoBpm=abc"],
        "invalid numeric value for parameter 'tempoBpm'",
    ),
    (
        ["suggest-mix", "--input", "{input}", "--params", "enable_eq=1"],
        "assistant param enable_eq is an earlier spelling; write enableEq",
    ),
    (
        ["suggest-mix", "--input", "{input}", "--params", "nosuchkey=1"],
        "unknown key nosuchkey (expected enableBalance, enableDynamics, enableEq, enableGain, enableHighPass, enableImage, enableStructure, eqMaxCutDb, hopLength, mixBusHeadroomDbtp, nFft, suggestionStrength, targetTrackLufs, tempoBpm)",
    ),
    (
        ["suggest-mix", "--input", "{input}", "--params", "tempoBpm=100", "--tempo-bpm", "120"],
        "--tempo-bpm and --params tempoBpm= set the same value",
    ),
    (
        ["mix", "--preset", "vocalReverbSend", "--input", "=x", "-o", "{output}"],
        "--input strip id must not be empty: =x",
    ),
    (
        ["mix", "--preset", "vocalReverbSend", "--input", "a=", "-o", "{output}"],
        "--input requires a file path: a=",
    ),
    (
        ["project", "bounce", "{input}", "-o", "{output}", "--synth", "nosuchsynth"],
        "argument --synth: unknown synth preset 'nosuchsynth'",
    ),
    (
        ["midi-render", "{input}", "-o", "{output}", "--synth", "nosuchsynth"],
        "argument --synth: unknown synth preset 'nosuchsynth'",
    ),
    (
        ["project", "bounce", "{input}", "-o", "{output}", "--audio", "nofile"],
        "argument --audio: must be <source_id>=FILE",
    ),
    (
        ["project", "bounce", "{input}", "-o", "{output}", "--audio", "0=a.wav"],
        "argument --audio: <source_id> must be an integer in [1, 4294967295]",
    ),
    (
        ["project", "align-takes", "{input}", "-o", "{output}", "--reference-source", "1"]
        + ["--audio", "nofile"],
        "argument --audio: must be <source_id>=FILE",
    ),
    (
        ["project", "abi", "-o", "{output}"],
        "Command 'project' does not produce a file output; remove -o/--output",
    ),
    (["key", "{input}", "--profile", "bogus"], "invalid key profile: bogus"),
    (["key", "{input}", "--modes", "bogus"], "invalid mode: bogus"),
    (["key", "{input}", "--modes", ""], "--modes must contain at least one mode"),
    (["chords", "{input}", "--key-root", "X"], "invalid pitch class: X"),
    (["chords", "{input}", "--key-mode", "bogus"], "invalid mode: bogus"),
    (["analyze", "{input}", "--meter-candidates", "3,x"], "invalid meter numerator: x"),
    (
        ["key", "{input}", "--hpss", "--hop-length", "8"],
        "--hop-length must be at least 16 with --hpss (got 8)",
    ),
    (
        ["rhythm", "{input}", "--bpm-min", "200", "--bpm-max", "100"],
        "--bpm-max must be greater than --bpm-min (--bpm-min 200, --bpm-max 100)",
    ),
    (
        ["rhythm", "{input}", "--bpm-min", "250"],
        "--bpm-max must be greater than --bpm-min (--bpm-min 250, --bpm-max 200)",
    ),
    (
        ["pitch", "{input}", "--f0-max-hz", "50"],
        "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz 65, --f0-max-hz 50)",
    ),
    (
        ["vocal-notes", "{input}", "--f0-min-hz", "3000"],
        "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz 3000, --f0-max-hz 2093)",
    ),
    (
        ["note-move", "{input}", "--onset-sample", "0", "--onset-sec", "0", "-o", "{output}"],
        "give --onset-sample or --onset-sec, not both",
    ),
    (
        ["note-stretch", "{input}", "--offset-sample", "9", "--offset-sec", "1", "-o", "{output}"],
        "give --offset-sample or --offset-sec, not both",
    ),
    (["repair", "{input}"], "--output is required unless --detect is given"),
    (
        ["repair", "{input}", "--detect", "-o", "{output}"],
        "--output cannot be combined with --detect",
    ),
    (
        ["repair", "{input}", "--detect", "--explain"],
        "--explain cannot be combined with --detect",
    ),
    (
        ["repair", "{input}", "--detect", "--preset", "pop"],
        "--preset cannot be combined with --detect",
    ),
    (
        ["repair", "{input}", "--detect", "--stages", "declip"],
        "--stages cannot be combined with --detect",
    ),
    (
        ["repair", "{input}", "--detect", "--params", "repair.declip.enabled=1"],
        "--params cannot be combined with --detect",
    ),
    (
        ["repair", "{input}", "--detect", "--bits", "24"],
        "--bits cannot be combined with --detect",
    ),
    (
        ["repair", "{input}", "-o", "{output}", "--stages", "declip", "--preset", "pop"],
        "--stages cannot be combined with --preset",
    ),
    (
        ["repair", "{input}", "-o", "{output}", "--explain", "--stages", "declip"],
        "--explain cannot be combined with --stages",
    ),
    (
        ["repair", "{input}", "-o", "{output}", "--explain", "--preset", "pop"],
        "--explain cannot be combined with --preset",
    ),
    (
        ["repair", "{input}", "-o", "{output}", "--params", "eq.enabled=1"],
        "--params key must be a repair.* field: eq.enabled",
    ),
    (
        ["repair", "{input}", "-o", "{output}", "--params", "novalue"],
        "argument --params: must be a comma-separated list of key=value entries",
    ),
    (
        [
            "repair",
            "{input}",
            "-o",
            "{output}",
            "--stages",
            "declip",
            "--params",
            "repair.denoise.nFft=512",
        ],
        "repair.denoise.nFft names a stage --stages does not run",
    ),
    (
        ["repair", "{input}", "-o", "{output}", "--params", "repair.bogus.x=1"],
        "unknown key params.repair.bogus.x",
    ),
    (
        [
            "repair",
            "{input}",
            "-o",
            "{output}",
            "--stages",
            "declip",
            "--params",
            "repair.bogus.x=1,repair.denoise.nFft=512",
        ],
        "unknown key params.repair.bogus.x",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--params", "bogus.x=1"],
        "unknown key params.bogus.x",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--params", "novalue"],
        "argument --params: must be a comma-separated list of key=value entries",
    ),
    (
        ["mastering-profile", "{input}", "--params", "=1"],
        "argument --params: must be a comma-separated list of key=value entries",
    ),
    (
        [
            "mastering-pair-processor",
            "{input}",
            "--processor",
            "match.abCrossfade",
            "--reference",
            "{input}",
            "--params",
            "mix=",
        ],
        "argument --params: must be a comma-separated list of key=value entries",
    ),
    (
        [
            "mastering-pair-analyze",
            "{input}",
            "--analysis",
            "match.tonalBalance",
            "--reference",
            "{input}",
            "--params",
            "a=1,,b=2",
        ],
        "argument --params: must be a comma-separated list of key=value entries",
    ),
    (
        ["mastering-suggest", "{input}", "--params", "novalue"],
        "argument --params: must be a comma-separated list of key=value entries",
    ),
    (
        ["mastering-suggest", "{input}", "--params", "target_platform=club"],
        "--params target_platform is an earlier spelling; write targetPlatform",
    ),
    (
        ["mastering-suggest", "{input}", "--target-lufs", "-12", "--params", "targetLufs=-12"],
        "--target-lufs and --params targetLufs= set the same value",
    ),
    (
        [
            "mastering-processor",
            "{input}",
            "--processor",
            "dynamics.compressor",
            "--params",
            "nosuchkey=1",
        ],
        "unknown --params key for dynamics.compressor: nosuchkey",
    ),
    (
        ["mastering-processor", "{input}", "--processor", "dynamics.compressor", "--params", "x"],
        "argument --params: must be a comma-separated list of key=value entries",
    ),
    (
        ["eq", "{input}", "--params", "band0.gainDb=1", "--frequency-hz", "1500", "--q", "2"],
        "--frequency-hz cannot be combined with --params",
    ),
    (
        ["eq", "{input}", "--params", "nosuchkey=1"],
        "unknown --params key for eq.equalizer: nosuchkey",
    ),
    (
        ["eq", "{input}", "--params", "novalue"],
        "argument --params: must be a comma-separated list of key=value entries",
    ),
    (["mix"], "mix needs --scene or --preset"),
    (
        ["mix", "--scene", "{input}", "--preset", "vocalReverbSend"],
        "--scene is mutually exclusive with --preset",
    ),
    (
        ["mix", "--preset", "vocalReverbSend", "--input", "{input}"],
        "mix with --input requires --output",
    ),
    (
        ["mix", "--preset", "vocalReverbSend", "-o", "{output}"],
        "mix with --output requires at least one --input",
    ),
    (
        ["mastering", "{input}"],
        "mastering needs --preset or --chain-config, or --assistant (which may take --preset); loudness-only normalisation is mastering-normalize-loudness",
    ),
    (
        ["mastering", "{input}", "--chain-config", "{output}", "--preset", "pop"],
        "--chain-config is mutually exclusive with --preset and --assistant",
    ),
    (
        ["mastering", "{input}", "--chain-config", "{output}", "--assistant"],
        "--chain-config is mutually exclusive with --preset and --assistant",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--target-lufs", "-14"],
        "--target-lufs requires --assistant",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--ceiling-dbtp", "-1"],
        "--ceiling-dbtp requires --assistant",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--enable-repair"],
        "--enable-repair requires --assistant",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--explain"],
        "--explain requires --assistant",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--target-platform", "streaming"],
        "--target-platform requires --assistant",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--no-streaming-safe"],
        "--no-streaming-safe requires --assistant",
    ),
    (
        ["mastering", "{input}", "--preset", "pop", "--speech-mono-amount", "0.5"],
        "--speech-mono-amount requires --assistant",
    ),
    (
        [
            "mastering-streaming",
            "{input}",
            "--platforms",
            '[{"name":"a","target_lufs":-14,"ceilingDb":-1}]',
        ],
        "argument --platforms: unknown key platforms[0].ceilingDb (renamed to platforms[0].ceiling_dbtp)",
    ),
    (
        [
            "mastering-streaming",
            "{input}",
            "--platforms",
            '[{"name":"a","target_lufs":-14,"ceiling_dbtp":-1,"z":0}]',
        ],
        "argument --platforms: unknown key platforms[0].z (expected name, target_lufs, ceiling_dbtp)",
    ),
    (
        ["mastering-streaming", "{input}", "--platforms", '[{"name":"a","target_lufs":-14}]'],
        "argument --platforms: platforms[0].ceiling_dbtp must be given",
    ),
    (
        [
            "mastering-streaming",
            "{input}",
            "--platforms",
            '[{"name":1,"target_lufs":-14,"ceiling_dbtp":-1}]',
        ],
        "argument --platforms: platforms[0].name must be a string",
    ),
    (
        [
            "mastering-streaming",
            "{input}",
            "--platforms",
            '[{"name":"","target_lufs":-14,"ceiling_dbtp":-1}]',
        ],
        "argument --platforms: platforms[0].name must not be empty",
    ),
    (
        [
            "mastering-streaming",
            "{input}",
            "--platforms",
            '[{"name":"a","target_lufs":true,"ceiling_dbtp":-1}]',
        ],
        "argument --platforms: platforms[0].target_lufs must be a number",
    ),
    (
        [
            "mastering-streaming",
            "{input}",
            "--platforms",
            '[{"name":"a","target_lufs":-14,"ceiling_dbtp":1e39}]',
        ],
        "argument --platforms: platforms[0].ceiling_dbtp must be finite",
    ),
    (
        ["mastering-streaming", "{input}", "--platforms", '{"name":"a"}'],
        "argument --platforms: must be a JSON array of platform objects",
    ),
    (
        ["mastering-streaming", "{input}", "--platforms", "[1]"],
        "argument --platforms: must be a JSON array of platform objects",
    ),
    (
        ["mastering-streaming", "{input}", "--platforms", "[NaN]"],
        "argument --platforms: must be a JSON array of platform objects",
    ),
    (
        ["project", "bounce", "{input}", "--channels", "3", "-o", "{output}"],
        "argument --channels: must be one of 1, 2, 6, 8; leave it out for the scene master's width",
    ),
    (
        ["midi-render", "{input}", "--channels", "3", "-o", "{output}"],
        "argument --channels: must be one of 1, 2, 6, 8; leave it out for the scene master's width",
    ),
    (
        ["fade", "{input}", "-o", "{output}"],
        "give --fade-in-sec or --fade-out-sec",
    ),
    (
        ["filter", "{input}", "-o", "{output}", "--type", "low-pass"],
        "--cutoff-hz is required when --type is high-pass or low-pass",
    ),
    (
        ["filter", "{input}", "-o", "{output}", "--type", "notch", "--bandwidth-hz", "100"],
        "--center-hz is required when --type is band-pass or notch",
    ),
    (
        ["filter", "{input}", "-o", "{output}", "--type", "notch", "--center-hz", "1000"],
        "--bandwidth-hz is required when --type is band-pass or notch",
    ),
    (
        [
            "filter",
            "{input}",
            "-o",
            "{output}",
            "--type",
            "high-pass",
            "--cutoff-hz",
            "1000",
            "--center-hz",
            "1000",
        ],
        "--center-hz does not apply when --type is high-pass or low-pass",
    ),
    (
        [
            "filter",
            "{input}",
            "-o",
            "{output}",
            "--type",
            "band-pass",
            "--center-hz",
            "1000",
            "--bandwidth-hz",
            "100",
            "--cutoff-hz",
            "1000",
        ],
        "--cutoff-hz does not apply when --type is band-pass or notch",
    ),
    (
        [
            "filter",
            "{input}",
            "-o",
            "{output}",
            "--type",
            "notch",
            "--center-hz",
            "1000",
            "--bandwidth-hz",
            "100",
            "--order",
            "4",
        ],
        "--order must be 2 when --type is band-pass or notch",
    ),
    (
        ["hpss", "{input}", "--harmonic-only", "--percussive-only", "-o", "{output}"],
        "hpss output modes are mutually exclusive: choose one of --harmonic-only, "
        "--percussive-only, or --with-residual",
    ),
    (
        ["trim-silence", "{input}", "--threshold-db", "-50", "--top-db", "30"],
        "--threshold-db and --top-db are mutually exclusive",
    ),
    (
        ["voice-change", "{input}", "--preset", "neutralMonitor", "--preset-json", "x.json"]
        + ["-o", "{output}"],
        "voice-change preset selectors are mutually exclusive: choose one of --preset, "
        "--preset-json, or --preset-pack",
    ),
    (
        ["voice-change", "{input}", "--preset-pack", "p.json", "-o", "{output}"],
        "--preset-pack requires --preset to select an entry",
    ),
    (
        ["voice-change", "{input}", "--set", "a=1", "-o", "{output}"],
        "--set requires --preset, --preset-json, or --preset-pack",
    ),
    (
        ["voice-change", "{input}", "--preset", "neutralMonitor", "--set", "=1", "-o", "{output}"],
        "invalid --set assignment: =1",
    ),
    (
        ["voice-change", "{input}", "--preset", "neutralMonitor", "--set", "..=1"]
        + ["-o", "{output}"],
        "empty --set path",
    ),
    (["voice-preset-validate", "p.json", "--set", "nopath"], "invalid --set assignment: nopath"),
    (
        ["voice-change", "{input}", "--preset", "noSuchPreset", "-o", "{output}"],
        "argument --preset: must be one of neutralMonitor, brightIdol, softWhisper, "
        "deepNarrator, robotMascot, darkVillain, hardTune, robotMonotone, choirHarmony, "
        "vocoderStrings, megaphone, radioVoice",
    ),
    (
        ["polyphonic-render", "{input}", "--edit", "0.bogus=1", "-o", "{output}"],
        "argument --edit: FIELD must be one of pitch_shift_semitones, gain_db, "
        "time_offset_samples, time_stretch_ratio, formant_shift_semitones, "
        "vibrato_depth_change, drift_change, muted",
    ),
    (
        ["transcribe", "{input}", "--reattack-ratio", "0.5", "-o", "{output}"],
        "--reattack-ratio must be 0 (no split) or greater than 1",
    ),
    (
        ["transcribe", "{input}", "--min-note-division", "8", "--min-note-ms", "50"]
        + ["-o", "{output}"],
        "--min-note-division and --min-note-ms cannot both be given",
    ),
    (
        ["transcribe", "{input}", "--polyphonic", "--monophonic-model", "singing"]
        + ["-o", "{output}"],
        "--monophonic-model singing cannot be combined with --polyphonic",
    ),
    (
        ["transcribe", "{input}", "--f0-min-hz", "100", "--f0-max-hz", "50", "-o", "{output}"],
        "--f0-max-hz must be greater than --f0-min-hz (--f0-min-hz 100, --f0-max-hz 50)",
    ),
)


def run_cross_option_refusals(
    run: Runner,
    native_executable: str,
    python_executable: str,
    directory: Path,
    timeout: float,
    report: list[tuple[str, str]],
) -> int:
    """Run every cross-option row on both front-ends; returns the number of rows compared."""
    tokens = {"{input}": UNREADABLE_INPUT, "{output}": str(directory / "cross_option.wav")}
    for template, expected in CROSS_OPTION_ROWS:
        argv = [tokens.get(token, token) for token in template]
        for surface, executable in (("native", native_executable), ("python", python_executable)):
            result = run(surface, executable, argv, timeout)
            label = f"cross-option.{surface}.{argv!r}"
            tail = repr(result["stderr"].strip()[-200:])
            if result["returncode"] != EXIT_USAGE:
                detail = result.get("error") or f"exit {result['returncode']}"
                report.append(
                    ("fail", f"{label}: expected exit {EXIT_USAGE}, got {detail}: {tail}")
                )
            lines = result["stderr"].splitlines()
            if not any(line == expected or line.endswith(f": {expected}") for line in lines):
                report.append(("fail", f"{label}: no line {expected!r} in {tail}"))
    return len(CROSS_OPTION_ROWS)
