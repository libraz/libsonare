#!/usr/bin/env python3
"""Every result schema path must be declared on both TS result types.

The core publishes two lists of the dotted paths its results serialize, one for
the analysis result and one for the meter estimate, and a C++ test already holds
each emitted object to its own list -- so a serializer and its list cannot drift
apart. Nothing holds the TypeScript interfaces to either. These results cross to
user code as a JSON string each facade parses and casts, so a path added to the
core and to its list arrives on Node and WASM as a field their types do not
mention: the cast is unchecked, `tsc` has nothing to object to, and the parity
tool compares function signatures rather than result shapes.

Both lists are covered, because a check over one of two siblings published by
the same source reads as a check over the pair.

The walk is anchored on the root interface rather than the file: `bpm` is
declared by four interfaces in one of these files, so a file-wide match would
accept a path the root does not carry. The path syntax carries `[]` for an array
element, which names the element type rather than a property, so it is stripped
before the segment is looked up. A path whose block cannot be reached fails
rather than passes -- a comparison that did not run and one that found nothing
are otherwise the same output.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ts_surface_walk as walk  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_SOURCE = REPO_ROOT / "src/analysis/analysis_json.cpp"
TS_SURFACES = {
    "node": REPO_ROOT / "bindings/node/src/types_analysis.ts",
    "wasm": REPO_ROOT / "bindings/wasm/src/public_types_music.ts",
}
# Both lists the source publishes, each with the interface its paths are rooted
# at. Covering one of two siblings would read as covering the pair.
SCHEMAS = {
    "analysis_result_schema_paths": "AnalysisResult",
    "meter_result_schema_paths": "MeterEstimate",
}
ROOT_INTERFACE = SCHEMAS["analysis_result_schema_paths"]


def schema_paths(
    source: str, accessor: str = "analysis_result_schema_paths"
) -> list[str]:
    """The dotted paths the core lists under `accessor`."""
    body = re.search(
        re.escape(accessor) + r"\(\)\s*\{(.*?)\n  return paths;", source, re.S
    )
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
    source = SCHEMA_SOURCE.read_text()
    surfaces = {side: path.read_text() for side, path in TS_SURFACES.items()}

    failed = False
    for accessor, root in SCHEMAS.items():
        paths = schema_paths(source, accessor)
        if not paths:
            print(
                f"no paths were read from {accessor} -- the list moved or was renamed"
            )
            failed = True
            continue
        for side, text in surfaces.items():
            missing, unreached, comparisons = scan(paths, text, root)
            print(
                f"{accessor} [{side}]: paths {len(paths)} | comparisons {comparisons}"
            )
            for path in unreached:
                print(f"NOT COMPARED {path} [{side}]: no such path under {root}")
            for path in missing:
                print(
                    f"MISSING {path} [{side}]: {root} reaches the block but lacks the leaf"
                )
            if missing or unreached or comparisons == 0:
                failed = True
    if failed:
        return 1
    print("every schema path is declared on both TypeScript result types")
    return 0


if __name__ == "__main__":
    sys.exit(main())
