#!/usr/bin/env python3
"""Every analysis result schema path must be declared on both TS result types.

`analysis_result_schema_paths()` is the core's own list of the dotted paths the
analysis result serializes, and a C++ test already holds the emitted object to
it -- so the serializer and the list cannot drift apart. Nothing holds the
TypeScript interfaces to the same list, and the result crosses to user code as a
JSON string that each facade parses and casts, so a path added to the core and
to the list reaches Node and WASM as a field their types do not mention. The
cast is unchecked, `tsc` has nothing to object to, and the parity tool compares
function signatures rather than result shapes.

This is the largest public result shape in the library, which is why it gets its
own check rather than waiting for a field to go missing.

The walk is anchored on the root interface rather than the file: `bpm` is
declared by four interfaces in one of these files, so a file-wide match would
accept a path the root does not carry. The path syntax carries `[]` for an array
element, which names the element type rather than a property, so it is stripped
before the segment is looked up. A path whose block cannot be reached fails
rather than passes -- a comparison that did not run and one that found nothing
are otherwise the same output.
"""

from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_SOURCE = REPO_ROOT / "src/analysis/analysis_json.cpp"
ROOT_INTERFACE = "AnalysisResult"
TS_SURFACES = {
    "node": REPO_ROOT / "bindings/node/src/types_analysis.ts",
    "wasm": REPO_ROOT / "bindings/wasm/src/public_types_music.ts",
}

# The TypeScript walk -- union members, named types, brace matching -- is the
# same one the mastering parameter check needs, so it is imported rather than
# repeated.
_WALK_PATH = Path(__file__).resolve().parent / "check_mastering_param_surfaces.py"
_SPEC = importlib.util.spec_from_file_location("libsonare_ts_walk", _WALK_PATH)
assert _SPEC is not None and _SPEC.loader is not None
walk = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(walk)

_LIST_RE = re.compile(
    r"analysis_result_schema_paths\(\)\s*\{(.*?)\n  return paths;", re.S
)


def schema_paths(source: str) -> list[str]:
    """The dotted paths the core lists for its analysis result."""
    body = _LIST_RE.search(source)
    if body is None:
        return []
    return re.findall(r'"([^"]+)"', body.group(1))


def scan(
    paths: list[str], text: str, root: str = ROOT_INTERFACE
) -> tuple[list, list, int]:
    """Return (missing, unreached, comparisons) for `paths` against one surface."""
    roots = walk.named_type_bodies(text, root)
    if not roots:
        return [], [(path, "root interface not found") for path in paths], 0
    missing: list[str] = []
    unreached: list[str] = []
    comparisons = 0
    for path in paths:
        segments = [segment.replace("[]", "") for segment in path.split(".")]
        bodies = list(roots)
        reached = True
        for segment in segments[:-1]:
            nxt: list[str] = []
            for body in bodies:
                nxt += walk.property_bodies(body, segment, text)
            if not nxt:
                reached = False
                break
            bodies = nxt
        if not reached:
            unreached.append(path)
            continue
        comparisons += 1
        if not any(walk.declares_leaf(body, segments[-1]) for body in bodies):
            missing.append(path)
    return missing, unreached, comparisons


def main() -> int:
    paths = schema_paths(SCHEMA_SOURCE.read_text())
    if not paths:
        print("no schema paths were read -- the core's list moved or was renamed")
        return 1

    failed = False
    for side, ts_path in TS_SURFACES.items():
        missing, unreached, comparisons = scan(paths, ts_path.read_text())
        print(f"{side}: paths {len(paths)} | comparisons {comparisons}")
        for path in unreached:
            print(
                f"NOT COMPARED {path} [{side}]: no block of that path exists under the root"
            )
        for path in missing:
            print(
                f"MISSING {path} [{side}]: the block exists but does not declare the leaf"
            )
        if missing or unreached or comparisons == 0:
            failed = True
    if failed:
        return 1
    print("every analysis schema path is declared on both TypeScript result types")
    return 0


if __name__ == "__main__":
    sys.exit(main())
