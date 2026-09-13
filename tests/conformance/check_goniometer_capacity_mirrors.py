#!/usr/bin/env python3
"""Hold the goniometer read cap equal across the core ring and its two mirrors.

``ChannelStrip::kGoniometerCapacity`` sizes the goniometer ring, and the Node
and Python facades each carry a hand-copied literal of it. Those literals are
load-bearing rather than documentary: each caps a caller-supplied point count
before it sizes a buffer, so a mirror above the ring turns an untrusted number
into an oversized allocation -- one the Node addon cannot even catch, since
``NAPI_DISABLE_CPP_EXCEPTIONS`` makes the resulting ``std::bad_alloc``
terminate the process. The number cannot be deleted, so it is guarded here.

The WASM facade names the core constant directly and is checked for that rather
than for a value: a literal appearing there would be a third mirror.

Extraction is qualified by the declaring class, because a second, unrelated
``kGoniometerCapacity`` of 512 exists on ``MeterTelemetryTap``; a pattern keyed
on the bare identifier reads whichever it reaches first. A declaration this
check cannot locate is a failure, not a quiet pass -- a run that finds nothing
and succeeds reads as coverage.
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Declaration(typing.NamedTuple):
    """One located constant: where it is, what scope declares it, its value."""

    path: str
    line: int
    scope: str | None
    name: str
    value: int

    @property
    def display(self) -> str:
        return f"{self.path}:{self.line}"


class Source(typing.NamedTuple):
    """A constant to locate: a C++ member, a C++ namespace-scope one, or Python."""

    path: str
    name: str
    language: str
    scope: str | None = None


CORE = Source("src/mixing/channel_strip.h", "kGoniometerCapacity", "c++", scope="ChannelStrip")

MIRRORS = (
    Source("bindings/node/src/addon/mixer/sends_metering.cpp", "kGoniometerReadCap", "c++"),
    Source("bindings/python/src/libsonare/_mixing.py", "_GONIOMETER_READ_CAP", "python"),
)

# Reads the core constant instead of copying it, and must keep doing so.
QUALIFIED_USE = ("src/wasm/bindings/mixing/mixing_automation.cpp", "ChannelStrip::kGoniometerCapacity")


def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines and its length."""
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


_CPP_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)

_PY_LEXICAL = re.compile(
    r"""\#[^\n]*
      | '''.*?'''
      | \"\"\".*?\"\"\"
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)

# Braces, class heads and integer constexpr declarations, read in one left-to-
# right pass so a declaration can be attributed to the class body it sits in.
_EVENT = re.compile(
    r"(?P<open>\{)"
    r"|(?P<close>\})"
    r"|(?P<semi>;)"
    r"|\b(?:class|struct)\s+(?P<scope>\w+)"
    r"|\b(?:static\s+)?constexpr\s+[\w:]+\s+(?P<name>\w+)\s*=\s*(?P<value>[^;{]*)"
)


def strip_cpp(text: str) -> str:
    """Comments and literals blanked, offsets and line numbers preserved."""
    return _CPP_LEXICAL.sub(_blank, text)


def strip_python(text: str) -> str:
    return _PY_LEXICAL.sub(_blank, text)


def _integer(text: str) -> int | None:
    match = re.fullmatch(r"\s*(\d+)[uUlL]*\s*", text)
    return int(match.group(1)) if match else None


def cpp_declarations(text: str, path: str) -> list[Declaration]:
    """Every integer constexpr in @p text, each carrying the class that declares it."""
    stripped = strip_cpp(text)
    found: list[Declaration] = []
    stack: list[tuple[str, int]] = []
    depth = 0
    pending: str | None = None
    for event in _EVENT.finditer(stripped):
        if event.group("open") is not None:
            depth += 1
            if pending is not None:
                stack.append((pending, depth))
                pending = None
        elif event.group("close") is not None:
            if stack and stack[-1][1] == depth:
                stack.pop()
            depth -= 1
        elif event.group("semi") is not None:
            pending = None
        elif event.group("scope") is not None:
            pending = event.group("scope")
        else:
            pending = None
            value = _integer(event.group("value"))
            if value is None:
                continue
            # A member is declared at its class body's own depth; anything
            # deeper is a local inside a member function.
            scope = stack[-1][0] if stack and stack[-1][1] == depth else None
            found.append(
                Declaration(
                    path=path,
                    line=stripped.count("\n", 0, event.start()) + 1,
                    scope=scope,
                    name=event.group("name"),
                    value=value,
                )
            )
    return found


def python_declarations(text: str, path: str) -> list[Declaration]:
    """Module-level integer assignments, as the Python mirror's counterpart."""
    found: list[Declaration] = []
    for node in ast.parse(text).body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target, value = node.targets[0], node.value
        if isinstance(target, ast.Name) and isinstance(value, ast.Constant):
            if isinstance(value.value, int) and not isinstance(value.value, bool):
                found.append(
                    Declaration(path, node.lineno, None, target.id, value.value)
                )
    return found


def find(root: Path, source: Source) -> Declaration | None:
    """The one declaration @p source names, or None -- never a near miss."""
    text = _read(root, source.path)
    if text is None:
        return None
    declarations = (
        cpp_declarations(text, source.path)
        if source.language == "c++"
        else python_declarations(text, source.path)
    )
    matches = [
        declaration
        for declaration in declarations
        if declaration.name == source.name and declaration.scope == source.scope
    ]
    return matches[0] if len(matches) == 1 else None


def _read(root: Path, relative: str) -> str | None:
    path = root / relative
    return path.read_text(encoding="utf-8") if path.is_file() else None


def reads(root: Path, source: Source, declaration: Declaration) -> int:
    """How often the mirror is read, the declaration itself excluded."""
    text = _read(root, source.path)
    if text is None:
        return 0
    stripped = strip_cpp(text) if source.language == "c++" else strip_python(text)
    lines = stripped.splitlines()
    del lines[declaration.line - 1]
    return len(re.findall(rf"\b{re.escape(source.name)}\b", "\n".join(lines)))


def evaluate(root: Path = ROOT) -> list[str]:
    """Every disagreement, as one line each. Empty means the mirrors hold."""
    failures: list[str] = []

    core = find(root, CORE)
    if core is None:
        failures.append(
            f"{CORE.path}: no `{CORE.name}` declared by `{CORE.scope}`. The mirrors are "
            "checked against it, so a rename, a move or a second declaration of the same "
            "name leaves them unmeasured -- re-point this check at the declaration."
        )

    for mirror in MIRRORS:
        declaration = find(root, mirror)
        if declaration is None:
            failures.append(
                f"{mirror.path}: no `{mirror.name}`. It bounds a caller-supplied point "
                "count against the ring, so it cannot be dropped or renamed silently."
            )
            continue
        if core is not None and declaration.value != core.value:
            failures.append(
                f"{declaration.display}: {mirror.name} is {declaration.value}, but "
                f"{CORE.scope}::{CORE.name} at {core.display} is {core.value}. A mirror "
                "below the ring drops points the ring holds; one above it sizes the working "
                "buffer past anything a read can fill."
            )
        if reads(root, mirror, declaration) == 0:
            failures.append(
                f"{declaration.display}: {mirror.name} is declared but never read, so the "
                "cap it carries is applied to nothing."
            )

    path, symbol = QUALIFIED_USE
    text = _read(root, path)
    if text is None or symbol not in strip_cpp(text):
        failures.append(
            f"{path}: no `{symbol}`. This facade reads the core constant rather than "
            "copying it, and a literal here would be a mirror nothing measures."
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()

    core = find(args.root, CORE)
    print(f"core: {CORE.scope}::{CORE.name} = {core.value if core else 'not found'}")
    for mirror in MIRRORS:
        declaration = find(args.root, mirror)
        print(f"mirror: {mirror.name} = {declaration.value if declaration else 'not found'}")

    failures = evaluate(args.root)
    for line in failures:
        print(line, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
