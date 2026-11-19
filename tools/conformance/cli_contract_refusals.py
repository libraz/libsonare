"""Refusal-text agreement between the two CLIs for every declared option domain.

Each row of tests/conformance/cli_option_domains.json names an option whose accepted
set is narrower than its type. Both front-ends are given one value outside that set and
must refuse it with the exit class the row declares and with the line
``argument --X: <predicate>``, the predicate generated here from the row in the
library's vocabulary (src/util/refusal_predicates.h) rather than read from either
front-end. The rows a run could not compare -- a command one build does not carry --
are counted and named, never folded into a pass.
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

EXIT_FOR_CLASS = {"usage": 2, "invalid_parameter": 3}

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


def _argv(path: str, option: str, value: str, required: list[str]) -> list[str]:
    # The value under test comes first, so the Python parser reaches it before anything else.
    return [*path.split("."), f"--{option}", value, *required]


def run_domain_refusals(
    run: Runner,
    native_executable: str,
    python_executable: str,
    native_inventory: dict[str, dict[str, Any]],
    python_inventory: dict[str, dict[str, Any]],
    directory: Path,
    timeout: float,
    report: list[tuple[str, str]],
) -> tuple[int, list[str]]:
    """Compare both front-ends' refusal of every declared domain.

    Returns the number of rows compared and the rows that could not be, each with its reason.
    """
    declaration = json.loads(DOMAINS_PATH.read_text(encoding="utf-8"))
    required_parameter: dict[str, set[str]] = {}
    for record in declaration["requiredInvalidParameter"]:
        required_parameter.setdefault(record["command"], set()).add(record["option"])

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
            if record["name"] != name
            and (record["required"] or record["name"] in required_parameter.get(path, set()))
            for token in (f"--{record['name']}", inside_value(record, directory))
        ]
        argv = _argv(path, name, outside_value(row, option["type"]), required)
        expected_text = f"argument --{name}: {expected_predicate(row, option['type'])}"
        expected_exit = EXIT_FOR_CLASS[row["reject_exit"]]
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
