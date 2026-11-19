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
        return f"must be an integer in [{_bound_text(low)}, {_bound_text(high)}]"
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
        ["project", "bounce", "{input}", "--channels", "3", "-o", "{output}"],
        "argument --channels: must be one of 1, 2, 6, 8; leave it out for the scene master's width",
    ),
    (
        ["midi-render", "{input}", "--channels", "3", "-o", "{output}"],
        "argument --channels: must be one of 1, 2, 6, 8; leave it out for the scene master's width",
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
