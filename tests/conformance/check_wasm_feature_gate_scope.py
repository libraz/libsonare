#!/usr/bin/env python3
"""Check that a WASM module links nothing from a subsystem its own build turned off.

A feature flag is read as a promise about the shipped binary: a module configured
with ``-DBUILD_MASTERING=OFF`` is assumed to carry no mastering code.  Nothing
enforced that.  The promise is not quite true either -- three translation units
sit in the always-compiled core list on purpose, because an always-compiled
caller needs them -- so the useful check is not "zero" but "exactly the residue
someone wrote a reason for".

Two things this deliberately does not do:

* It does not measure size.  ``tools/check_wasm_size.py`` says why in its own
  docstring: emscripten output is not byte-reproducible across machines or
  toolchain patch releases.  A size or hash expectation fails on every runner
  that did not write it, and a size that moved says nothing about *what* moved.
* It does not read ``dist/<module>.sources.json``.  That manifest records the
  source roots whole rather than the link closure, by design, so both modules
  attest the same 1082 files and it cannot answer this question at all.

What it reads instead is the link line the build actually issued, through
``check_wasm_exception_scope.module_objects()``, and the configured flag values
out of the build directory's own ``CMakeCache.txt``.  Both are properties of the
build that produced the objects, so neither can drift from it.

The gate-to-directory map is derived from ``src/CMakeLists.txt`` rather than
written here: a directory whose sources are almost entirely inside ``if(BUILD_X)``
blocks belongs to those gates, and moving a file moves the map with it.  A
hand-written map is the failure this check would otherwise inherit -- a first
draft of it over-reported ``analysis/acoustic/`` as the acoustic-sim gate and
under-reported the pitch editor, in one run.
"""

from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_wasm_exception_scope import module_objects  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CMAKELISTS = ROOT / "src/CMakeLists.txt"

# A directory counts as belonging to its gates once nearly all of its sources sit
# inside them; the remainder is what this check reports. Demanding all of them
# would retire every directory that carries one deliberate exception, which is
# precisely the case worth watching.
_OWNERSHIP_RATIO = 0.90

# Gates that describe how the library is packaged rather than which subsystem is
# compiled, so no directory belongs to them.
_NON_SUBSYSTEM_GATES = {"WASM", "SHARED", "TESTING", "BENCH", "CLI"}

# Each entry names a translation unit that is linked although a gate covering its
# directory is off, and why. `src/CMakeLists.txt` carries the full reasoning at
# each one's place in the always-compiled list; the one-line reason here is what
# makes a new arrival visible as a new arrival.
ALLOWED_RESIDUE = {
    "mastering/eq/band_strings.cpp": (
        "pure string<->enum mapping over a header-only enum, called by the "
        "always-compiled eq_band_json.cpp"
    ),
    "mixing/downmix.cpp": (
        "always-compiled core/audio_io.cpp folds every multichannel source to "
        "mono through downmix()"
    ),
    "mixing/api/scene_json.cpp": (
        "listed in the always-compiled core list; unlike its two siblings it "
        "carries no reason there, which is worth settling"
    ),
}

_SOURCE_RE = re.compile(r"^\s*([\w/.\-]+\.(?:cpp|cc|c|mm))\s*$")


def _gated_sources() -> tuple[dict[str, set[str]], set[str]]:
    """Map each BUILD_ gate to the sources inside its blocks, plus every source.

    Nesting matters and is the reason for the stack: a file inside
    ``if(BUILD_MASTERING)`` and then ``if(BUILD_FX)`` needs both, so it belongs
    to each. A non-gate ``if(...)`` still pushes a frame, or ``endif()`` would
    pop the wrong one.
    """
    stack: list[list[str]] = []
    gated: dict[str, set[str]] = collections.defaultdict(set)
    every: set[str] = set()
    for line in CMAKELISTS.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("if("):
            stack.append(re.findall(r"\bBUILD_(\w+)\b", stripped))
            continue
        if stripped.startswith(("elseif(", "else()")):
            if stack:
                stack[-1] = []
            continue
        if stripped.startswith("endif()"):
            if stack:
                stack.pop()
            continue
        match = _SOURCE_RE.match(line)
        if match is None:
            continue
        source = match.group(1)
        every.add(source)
        for frame in stack:
            for gate in frame:
                gated[gate].add(source)
    return gated, every


def _prefixes(source: str) -> list[str]:
    parts = source.split("/")
    return ["/".join(parts[:i]) + "/" for i in range(1, len(parts))]


def gated_directories() -> dict[str, set[str]]:
    """Map each directory prefix to the set of gates its sources sit behind."""
    gated, every = _gated_sources()
    subsystem = {g: s for g, s in gated.items() if g not in _NON_SUBSYSTEM_GATES}
    by_dir: dict[str, set[str]] = collections.defaultdict(set)
    for source in every:
        for prefix in _prefixes(source):
            by_dir[prefix].add(source)

    owned: dict[str, set[str]] = {}
    for prefix, sources in by_dir.items():
        if len(sources) < 2:
            continue
        gates = {g for g, s in subsystem.items() if s & sources}
        if not gates:
            continue
        covered = {src for src in sources if any(src in subsystem[g] for g in gates)}
        if len(covered) / len(sources) >= _OWNERSHIP_RATIO:
            owned[prefix] = gates

    # Keep the shallowest owned directory on each path: a nested one adds no
    # reach and would report the same object twice under two prefixes.
    return {
        prefix: gates
        for prefix, gates in owned.items()
        if not any(prefix != other and prefix.startswith(other) for other in owned)
    }


def disabled_gates(build_dir: Path) -> set[str]:
    """Every BUILD_ gate the build directory recorded as off.

    Read from the cache rather than from the command that configured it: the
    cache is what the build used, and a script's flag list can drift from the
    directory it was last run against.
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise SystemExit(f"{build_dir}: no CMakeCache.txt, so its configuration is unknown")
    off = set()
    for line in cache.read_text(encoding="utf-8").splitlines():
        match = re.match(r"^BUILD_(\w+):BOOL=(\w+)$", line.strip())
        if match and match.group(2).upper() in {"OFF", "FALSE", "NO", "0"}:
            off.add(match.group(1))
    return off - _NON_SUBSYSTEM_GATES


def _source_of(obj: Path) -> str | None:
    """The repository-relative source a CMake object path was compiled from."""
    text = str(obj)
    marker = ".dir/"
    if marker not in text:
        return None
    tail = text.split(marker, 1)[1]
    if not tail.endswith(".o"):
        return None
    tail = tail[: -len(".o")]
    # An out-of-tree unit is addressed through `__/`; nothing under src/ is.
    return None if tail.startswith("__/") else tail


def check(build_dir: Path) -> tuple[list[str], dict[str, int]]:
    owned = gated_directories()
    off = disabled_gates(build_dir)
    objects = module_objects(build_dir)

    # An entry only has something to excuse where a gate covering it is off, so
    # applicability is decided per build before use is judged. The same rule the
    # parity allowlist follows: a pattern in front of a check that never ran was
    # never consulted, and retiring it for being unused would be wrong.
    applicable = {
        source
        for source in ALLOWED_RESIDUE
        for prefix, gates in owned.items()
        if source.startswith(prefix) and gates & off
    }

    findings: list[str] = []
    resolved = 0
    allowed_seen: set[str] = set()
    for obj in objects:
        source = _source_of(obj)
        if source is None:
            continue
        resolved += 1
        for prefix, gates in owned.items():
            if not source.startswith(prefix):
                continue
            blocking = sorted(gates & off)
            if not blocking:
                continue
            if source in ALLOWED_RESIDUE:
                allowed_seen.add(source)
                continue
            findings.append(
                f"{source} is linked although BUILD_{'/BUILD_'.join(blocking)} "
                f"is off for this build ({prefix} belongs to that gate)"
            )
    for source in sorted(applicable - allowed_seen):
        findings.append(
            f"{source} is allowed as residue and its gate is off here, but it "
            "was not linked -- the entry excuses nothing and should go"
        )

    reach = {
        "gates_off": len(off),
        "gated_directories": len(owned),
        "objects_linked": len(objects),
        "sources_resolved": resolved,
        "residue_applicable": len(applicable),
        "residue_allowed": len(allowed_seen),
    }
    return findings, reach


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", action="append", required=True, type=Path)
    args = parser.parse_args()

    failed = False
    for build_dir in args.build_dir:
        findings, reach = check(build_dir)
        # Reach is printed on success as well as failure. A run that resolved no
        # source, or found no gated directory, has measured nothing, and without
        # these numbers that is indistinguishable from a clean tree.
        summary = ", ".join(f"{key}={value}" for key, value in reach.items())
        if reach["sources_resolved"] == 0 or reach["gated_directories"] == 0:
            print(f"{build_dir}: reached nothing ({summary})", file=sys.stderr)
            failed = True
            continue
        if findings:
            print(f"{build_dir}: feature gate scope failed ({summary})", file=sys.stderr)
            for finding in findings:
                print(f"  - {finding}", file=sys.stderr)
            failed = True
            continue
        if not reach["gates_off"]:
            print(f"{build_dir}: no gate is off, nothing to check ({summary})")
            continue
        print(f"{build_dir}: feature gate scope OK ({summary})")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
