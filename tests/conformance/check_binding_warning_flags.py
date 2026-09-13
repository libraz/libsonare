#!/usr/bin/env python3
"""Hold every binding-layer translation unit to the same warning contract as the core.

The core targets carry ``-Wall -Wextra -Wpedantic -Werror``, so an unhandled
enumerator -- the failure an exhaustive switch is written to produce -- stops the
build. The two hand-written binding layers each reach the compiler through their
own toolchain, and one of them once compiled with no warnings enabled at all:
the WASM module target received a single compile option and never the shared
warning variable, so the whole of ``src/wasm/`` was outside the bar while every
sibling library was inside it. Nothing announced that, because the absence of a
diagnostic looks exactly like the absence of a defect.

WHY THE BUILD DATABASE AND NOT THE CMAKE SOURCE
-----------------------------------------------
A grep for the warning variable in ``CMakeLists.txt`` answers a different
question. The flags have to survive a generator expression, emcmake and
cmake-js, and land on the actual compile line; the variable can be named in a
``target_compile_options`` call that evaluates to nothing. Only the compilation
database says what the compiler was invoked with, which is why this reads the
database each layer's own build wrote.

WHAT IS EXCLUDED, AND WHY IT IS NOT A HOLE
------------------------------------------
Third-party sources are compiled without the flags on purpose -- the warning bar
is this project's, not its dependencies' -- so they are excluded by path rather
than by silence. Everything else the build compiled from this repository is
required to carry them.

Each layer carries the number of units it covered when it was written. A
database that stopped naming this repository's sources would otherwise pass by
matching nothing, and an empty set satisfies every requirement.
"""

from __future__ import annotations

import argparse
import json
import sys
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Every flag a binding translation unit must be compiled with: the same set the
# core targets carry, so a binding is held to one bar rather than a weaker one.
REQUIRED_FLAGS = ("-Wall", "-Wextra", "-Wpedantic", "-Werror")

# Compiled without the flags by design. The bar is this project's own.
EXCLUDED_PARTS = ("third_party",)


class Layer(typing.NamedTuple):
    """One binding layer, its build tree, and how to rebuild it."""

    name: str
    build_dir: Path
    rebuild: str
    #: Repository-relative roots whose translation units this layer must cover.
    sources: tuple[str, ...]
    #: Units covered when this entry was written; a scan below it has stopped
    #: reading the layer rather than found it clean.
    floor: int


LAYERS = (
    Layer(
        name="WASM module",
        build_dir=Path("bindings/wasm/build-wasm"),
        rebuild="(cd bindings/wasm && yarn build:wasm:full)",
        sources=("src/",),
        floor=150,
    ),
    Layer(
        name="Node addon",
        build_dir=Path("bindings/node/build"),
        rebuild="(cd bindings/node && yarn build)",
        sources=("bindings/node/src/",),
        floor=25,
    ),
)


def _command_of(entry: dict) -> str:
    if "command" in entry:
        return entry["command"]
    return " ".join(entry.get("arguments", ()))


def _repo_relative(path: str, root: Path) -> str | None:
    try:
        return str(Path(path).resolve().relative_to(root))
    except (ValueError, OSError):
        return None


def audit(layer: Layer, root: Path = ROOT) -> tuple[list[str], list[str]]:
    """Return (covered, uncovered) unit names for one layer.

    Raises SystemExit when the layer has not been built, because an absent
    database is an unanswered question rather than a pass.
    """
    database = root / layer.build_dir / "compile_commands.json"
    if not database.is_file():
        raise SystemExit(
            f"missing {database}\nBuild the {layer.name} first: {layer.rebuild}"
        )
    covered: list[str] = []
    uncovered: list[str] = []
    for entry in json.loads(database.read_text(encoding="utf-8")):
        name = _repo_relative(entry.get("file", ""), root)
        if name is None:
            continue
        if not any(name.startswith(prefix) for prefix in layer.sources):
            continue
        if any(part in Path(name).parts for part in EXCLUDED_PARTS):
            continue
        command = _command_of(entry)
        missing = [flag for flag in REQUIRED_FLAGS if flag not in command.split()]
        if missing:
            uncovered.append(f"{name}  missing {' '.join(missing)}")
        else:
            covered.append(name)
    return covered, uncovered


def evaluate(layer: Layer, covered: list[str], uncovered: list[str]) -> list[tuple[str, list[str]]]:
    """Every failure class for one layer, as (heading, lines)."""
    failures: list[tuple[str, list[str]]] = []
    total = len(covered) + len(uncovered)
    if total < layer.floor:
        failures.append(
            (
                f"{layer.name}: found {total} translation units of this repository, "
                f"below the {layer.floor} it covered when this entry was written -- "
                "the scan has stopped reading the layer, and an empty set carries "
                "every flag",
                [],
            )
        )
    if uncovered:
        failures.append(
            (
                f"{layer.name}: these compile without the warning contract the core "
                "targets carry, so a diagnostic here prints at most instead of "
                "failing the build",
                [f"  {line}" for line in uncovered],
            )
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()

    failed = False
    for layer in LAYERS:
        covered, uncovered = audit(layer, args.root)
        print(f"{layer.name}: {len(covered) + len(uncovered)} units, {len(covered)} at the bar")
        for heading, lines in evaluate(layer, covered, uncovered):
            print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
