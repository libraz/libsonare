#!/usr/bin/env python3
"""Keep the WASM exception-scope list in step with the units that catch.

emscripten defaults to ``DISABLE_EXCEPTION_CATCHING=1``, which elides landing
pads at compile time.  ``-fexceptions`` on the link line only enables the
runtime support; it cannot restore a ``catch`` that was never emitted.  A
translation unit compiled into the WASM ``sonare`` target without
``-fexceptions`` therefore has every one of its ``catch`` arms silently deleted,
and a C-ABI unit loses ``SONARE_C_TRY`` / ``SONARE_C_CATCH`` entirely: the throw
escapes past the error-code translation and reaches JS raw.

The failure is invisible.  It is not a compile error, not a link error, and not
a test failure unless a test happens to assert the exact error code of a path
that throws deep inside the C ABI.  The flag is applied per source file
(``SONARE_WASM_EXCEPTION_SOURCES`` in ``src/CMakeLists.txt``) because a
whole-target ``-fexceptions`` costs several times as much binary for no
additional behaviour -- and a hand-maintained file list drifts, which is what
this check exists to catch.

Reads the build's own ``compile_commands.json``, so it verifies the flags that
were actually passed rather than re-parsing the CMake source.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = ROOT / "bindings" / "wasm" / "build-wasm"

# A unit "catches" if it spells a catch arm directly or pulls one in through the
# C-ABI guard macros.
_CATCH = re.compile(r"\bcatch\s*\(")
_MACROS = ("SONARE_C_TRY", "SONARE_C_CATCH")

# Objects of the WASM module target; sibling static libraries are linked in and
# are NOT covered by this check (see the module docstring's scope note).
_TARGET_MARKER = "CMakeFiles/sonare.dir/"


def catching_units(build_dir: Path) -> tuple[list[str], list[str], int]:
    """Return (covered, uncovered, module-unit-count), paths relative to ``src/``."""
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise SystemExit(
            f"missing {database}\n"
            "Build the WASM module first: (cd bindings/wasm && yarn build:wasm)"
        )
    entries = json.loads(database.read_text())
    covered: list[str] = []
    uncovered: list[str] = []
    module_units = 0
    for entry in entries:
        command = entry.get("command", "")
        if _TARGET_MARKER not in command:
            continue
        module_units += 1
        source = Path(entry["file"])
        try:
            text = source.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if not (_CATCH.search(text) or any(m in text for m in _MACROS)):
            continue
        name = str(source).split("/src/", 1)[-1]
        (covered if "-fexceptions" in command else uncovered).append(name)
    return sorted(covered), sorted(uncovered), module_units


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    args = parser.parse_args()

    covered, uncovered, module_units = catching_units(args.build_dir)
    if module_units == 0:
        raise SystemExit(
            f"no `sonare` module translation units found in {args.build_dir}; "
            "the build database does not look like a WASM module build"
        )
    # A feature-reduced configuration can legitimately compile no catching unit
    # at all -- SONARE_WASM_ANALYSIS_ONLY builds 11 units, none of which catch --
    # so an empty set is a pass, not a broken database.
    print(f"module translation units: {module_units}")
    print(f"  of which catch: {len(covered) + len(uncovered)}")
    print(f"  compiled with -fexceptions: {len(covered)}")
    if uncovered:
        print(
            "\nThese units catch but were compiled without -fexceptions, so every",
            "catch arm in them was deleted:",
            *(f"  {name}" for name in uncovered),
            "\nAdd them to SONARE_WASM_EXCEPTION_SOURCES in src/CMakeLists.txt.",
            sep="\n",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
