"""Check the public error reason vocabularies against the core header."""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = "src/util/error_reason.h"
NATIVE_BOUNDARIES = ("bindings/node/src/addon/sonare_wrap_utils.cpp", "src/wasm/bindings.cpp")
TYPESCRIPT = ("bindings/node/src/errors.ts", "bindings/wasm/src/errors.ts")
PYTHON = "bindings/python/src/libsonare/_errors.py"


def core_reasons(text: str) -> dict[str, str]:
    enum = re.search(r"enum class ErrorReason\s*\{([^{}]+)\};", text)
    if enum is None:
        raise ValueError("missing ErrorReason enum")
    declarations = enum[1].strip().removesuffix(",").split(",")
    names = [declaration.strip() for declaration in declarations]
    if any(re.fullmatch(r"\w+", name) is None for name in names):
        raise ValueError("unrecognized ErrorReason enumerator")
    pairs = re.findall(r'case ErrorReason::(\w+):\s*return "([^"]+)";', text)
    if not names or sorted(names) != sorted(name for name, _ in pairs):
        raise ValueError("ErrorReason enum and token cases differ")
    return dict(pairs)


def native_boundary_reasons(text: str) -> list[str]:
    inventory = re.search(
        r"std::array<sonare::ErrorReason,\s*(\d+)>\s+\w+\s*=\s*\{([^{}]+)\};", text
    )
    if inventory is None:
        raise ValueError("missing native reason inventory")
    names = re.findall(r"\bsonare::ErrorReason::(\w+)", inventory[2])
    if len(names) != int(inventory[1]):
        raise ValueError("native reason inventory does not match its declared size")
    return names


def typescript_tokens(text: str) -> list[str]:
    union = re.search(r"\bexport type ErrorReason\s*=\s*([^;]+);", text)
    if union is None:
        raise ValueError("missing ErrorReason type")
    source = union[1].strip()
    tuple_type = re.fullmatch(r"\(?typeof\s+(\w+)\)?\[number\]", source)
    if tuple_type is not None:
        name = re.escape(tuple_type[1])
        declaration = re.search(rf"\bconst {name}\s*=\s*\[([^\]]+)\]\s*as const", text)
        if declaration is None:
            raise ValueError("missing ErrorReason token tuple")
        alternatives = declaration[1].strip().rstrip(",").split(",")
    else:
        alternatives = source.lstrip("|").split("|")
    return [string_token(alternative.strip()) for alternative in alternatives]


def string_token(text: str) -> str:
    match = re.fullmatch(r"""(['"])([a-z][A-Za-z0-9]*)\1""", text)
    if match is None:
        raise ValueError(f"expected a string literal, found {text!r}")
    return match[2]


def python_tokens(text: str) -> list[str]:
    for node in ast.parse(text).body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(
            isinstance(target, ast.Name) and target.id == "ErrorReason" for target in node.targets
        ):
            continue
        literal = node.value
        if not (
            isinstance(literal, ast.Subscript)
            and isinstance(literal.value, ast.Name)
            and literal.value.id == "Literal"
        ):
            raise ValueError("ErrorReason must be a Literal")
        elements = literal.slice.elts if isinstance(literal.slice, ast.Tuple) else [literal.slice]
        if any(
            not isinstance(element, ast.Constant) or not isinstance(element.value, str)
            for element in elements
        ):
            raise ValueError("ErrorReason contains a non-string token")
        return [element.value for element in elements if isinstance(element, ast.Constant)]
    raise ValueError("missing ErrorReason type")


def main() -> int:
    try:
        reasons = core_reasons((ROOT / CORE).read_text(encoding="utf-8"))
        if len(reasons) != len(set(reasons.values())):
            raise ValueError("duplicate core reason token")
    except (OSError, ValueError) as error:
        print(f"{CORE}: {error}", file=sys.stderr)
        return 1

    expected = set(reasons.values())
    failures = []
    readers = [
        *((path, typescript_tokens, expected) for path in TYPESCRIPT),
        (PYTHON, python_tokens, expected),
        *((path, native_boundary_reasons, set(reasons)) for path in NATIVE_BOUNDARIES),
    ]
    for path, read_tokens, wanted in readers:
        try:
            tokens = read_tokens((ROOT / path).read_text(encoding="utf-8"))
            if not tokens or len(tokens) != len(set(tokens)):
                raise ValueError("reason tokens are empty or duplicated")
        except (OSError, SyntaxError, ValueError) as error:
            failures.append(f"{path}: {error}")
            continue
        actual = set(tokens)
        if actual != wanted:
            failures.append(
                f"{path}: missing {sorted(wanted - actual)}, unexpected {sorted(actual - wanted)}"
            )
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        return 1
    print(f"ErrorReason matches the core header on three facades ({len(expected)} tokens).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
