"""Numeric-text agreement between the two CLIs: one grammar, one refusal per cause.

Each refusal row gives both front-ends the same argv and requires exit 2 and the
exact line ``argument <subject>: <requirement>`` from each, so neither reader can
drift from the other or from the stated text. Each acceptance row requires exit 0
and the same payload, which holds the accepted set itself in step: a sign, leading
space, an exponent, an underflow to 0 and a mode-name alias are numbers on both.
"""

from __future__ import annotations

import json
import math
from collections.abc import Callable
from pathlib import Path
from typing import Any

Runner = Callable[[str, str, list[str], float], dict[str, Any]]

_MODE_NAMES = "major, maj, minor, min, m, dorian, phrygian, lydian, mixolydian, locrian"
_INT_RANGE = "must be an integer in [-2147483648, 2147483647]"
_INT64_RANGE = "must be an integer in [-9223372036854775808, 9223372036854775807]"

# (argv, the one stderr line both must print).
REFUSAL_ROWS: tuple[tuple[list[str], str], ...] = (
    (["scale-quantize", "abc"], "argument midi: must be a number"),
    (["scale-quantize", ""], "argument midi: must be a number"),
    (["scale-quantize", "1_0"], "argument midi: must be a number"),
    (["scale-quantize", "3 "], "argument midi: must be a number"),
    (["scale-quantize", "0x10"], "argument midi: must be a number"),
    (["scale-quantize", "1e"], "argument midi: must be a number"),
    (["scale-quantize", "nan"], "argument midi: must be finite"),
    (["scale-quantize", "Infinity"], "argument midi: must be finite"),
    (["scale-quantize", "1e999"], "argument midi: must be finite"),
    (["scale-quantize", "3.5e38"], "argument midi: must be finite"),
    (
        ["scale-quantize", "60", "--reference-midi", "1_0"],
        "argument --reference-midi: must be a number",
    ),
    (
        ["scale-quantize", "60", "--reference-midi", "inf"],
        "argument --reference-midi: must be finite",
    ),
    (["scale-quantize", "60", "--root", "1.5"], "argument --root: must be an integer"),
    (["scale-quantize", "60", "--root", "1e1"], "argument --root: must be an integer"),
    (["scale-quantize", "60", "--root", "0x3"], "argument --root: must be an integer"),
    (["scale-quantize", "60", "--root", "3 "], "argument --root: must be an integer"),
    (["scale-quantize", "60", "--root", "99999999999"], f"argument --root: {_INT_RANGE}"),
    (
        ["scale-quantize", "60", "--mode-mask", "lydianx"],
        f"argument --mode-mask: must be an integer mask or one of {_MODE_NAMES}",
    ),
    (["synthesize-rir", "--seed", "1.0", "-o", "{output}"], "argument --seed: must be an integer"),
    (
        ["synthesize-rir", "--seed", "99999999999999999999", "-o", "{output}"],
        f"argument --seed: {_INT64_RANGE}",
    ),
)

# argv lists both must accept with the same payload.
ACCEPTANCE_ROWS: tuple[list[str], ...] = (
    ["scale-quantize", "+2", "--json"],
    ["scale-quantize", " 2", "--json"],
    ["scale-quantize", "1e1", "--json"],
    ["scale-quantize", "1.", "--json"],
    ["scale-quantize", ".5", "--json"],
    ["scale-quantize", "2.5E0", "--json"],
    ["scale-quantize", "1e-50", "--json"],
    ["scale-quantize", "60", "--root", "+3", "--json"],
    ["scale-quantize", "60", "--root", " 3", "--json"],
    ["scale-quantize", "61", "--mode-mask", "MAJ", "--json"],
    ["scale-quantize", "61", "--mode-mask", "m", "--json"],
)


def _same_payload(native: Any, python: Any) -> bool:
    if isinstance(native, dict) and isinstance(python, dict):
        return native.keys() == python.keys() and all(
            _same_payload(native[key], python[key]) for key in native
        )
    if isinstance(native, (int, float)) and isinstance(python, (int, float)):
        return math.isclose(float(native), float(python), rel_tol=1e-9, abs_tol=0.0)
    return bool(native == python)


def _tail(result: dict[str, Any]) -> str:
    return repr(result["stderr"].strip()[-200:])


def run_numeric_refusals(
    run: Runner,
    native_executable: str,
    python_executable: str,
    directory: Path,
    timeout: float,
    report: list[tuple[str, str]],
) -> int:
    """Run every row on both front-ends; returns the number of rows compared."""
    output = str(directory / "numeric_text.wav")
    for template, expected in REFUSAL_ROWS:
        argv = [output if token == "{output}" else token for token in template]
        for surface, executable in (("native", native_executable), ("python", python_executable)):
            result = run(surface, executable, argv, timeout)
            label = f"numeric.{surface}.{argv!r}"
            if result["returncode"] != 2:
                detail = result.get("error") or f"exit {result['returncode']}"
                report.append(("fail", f"{label}: expected exit 2, got {detail}: {_tail(result)}"))
            lines = result["stderr"].splitlines()
            if not any(line == expected or line.endswith(f": {expected}") for line in lines):
                report.append(("fail", f"{label}: no line {expected!r} in {_tail(result)}"))
    for argv in ACCEPTANCE_ROWS:
        payloads = {}
        for surface, executable in (("native", native_executable), ("python", python_executable)):
            result = run(surface, executable, argv, timeout)
            if result["returncode"] != 0:
                detail = result.get("error") or f"exit {result['returncode']}"
                report.append(
                    ("fail", f"numeric.{surface}.{argv!r}: refused ({detail}) {_tail(result)}")
                )
                continue
            payloads[surface] = json.loads(result["stdout"])
        if len(payloads) == 2 and not _same_payload(payloads["native"], payloads["python"]):
            report.append(
                (
                    "fail",
                    (
                        f"numeric.{argv!r}: payloads differ\n  native: {payloads['native']}\n"
                        f"  python: {payloads['python']}"
                    ),
                )
            )
    return len(REFUSAL_ROWS) + len(ACCEPTANCE_ROWS)
