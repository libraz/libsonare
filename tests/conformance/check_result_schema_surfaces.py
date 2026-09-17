"""Every result schema path must be declared on both TypeScript result types.

Several results cross to user code as a JSON string each facade parses and
casts. The core publishes the dotted paths each of those writers emits, and a
C++ test holds every writer to its own list by set equality -- so a writer and
its list cannot drift apart. Nothing holds the TypeScript interfaces to any of
them: the cast is unchecked, `tsc` has nothing to object to, and the parity tool
compares function signatures rather than result shapes.

Every published list is covered here rather than a chosen few, because a check
over some of a set of siblings reads as a check over the set.

Two properties decide whether this check is worth anything, and both fail
silently towards a clean report:

The walk is anchored on each family's root type rather than on a file. `bpm` is
declared by four interfaces in one of these files, so a file-wide match would
accept a path the root does not carry.

A path whose block cannot be reached is a failure. A comparison that did not run
and a comparison that found nothing are the same output otherwise, so the number
of comparisons actually performed is printed beside the findings, and a family
that compared nothing fails.

The lists are read out of the C++ source textually, so each one must be written
as a literal initializer. A list that computes any of its entries is rejected
rather than read partially -- an under-read list reports a clean surface while
describing less of it than it claims.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ts_surface_walk as walk

REPO_ROOT = Path(__file__).resolve().parents[2]

NODE = REPO_ROOT / "bindings/node/src"
WASM = REPO_ROOT / "bindings/wasm/src"

# One entry per published list: where it is defined, the TypeScript type its
# paths are rooted at, and the file on each surface that declares that type. A
# root-array writer roots its paths at the element, so its paths open with the
# `[]` segment and the type named here is the element type.
FAMILIES = {
    "analysis_result_schema_paths": {
        "source": REPO_ROOT / "src/analysis/analysis_json.cpp",
        "root": "AnalysisResult",
        "surfaces": {
            "node": NODE / "types_analysis.ts",
            "wasm": WASM / "public_types_music.ts",
        },
    },
    "meter_result_schema_paths": {
        "source": REPO_ROOT / "src/analysis/analysis_json.cpp",
        "root": "MeterEstimate",
        "surfaces": {
            "node": NODE / "types_analysis.ts",
            "wasm": WASM / "public_types_music.ts",
        },
    },
    "insert_param_info_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/insert_factory.cpp",
        "root": "MasteringInsertParamInfo",
        "surfaces": {
            "node": NODE / "mastering_chain.ts",
            "wasm": WASM / "mastering_core.ts",
        },
    },
    "processor_catalog_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/named_processor_registry.cpp",
        "root": "MasteringProcessorCatalogEntry",
        "surfaces": {
            "node": NODE / "mastering_chain.ts",
            "wasm": WASM / "mastering_core.ts",
        },
    },
    "mix_assistant_result_schema_paths": {
        "source": REPO_ROOT / "src/mixing/assistant/suggester.cpp",
        "root": "MixAssistantResult",
        "surfaces": {
            "node": NODE / "types_mixing.ts",
            "wasm": WASM / "public_types_mixing.ts",
        },
    },
    "scene_schema_paths": {
        "source": REPO_ROOT / "src/mixing/api/scene_json.cpp",
        "root": "MixSceneDocument",
        "surfaces": {
            "node": NODE / "types_mixing.ts",
            "wasm": WASM / "public_types_mixing.ts",
        },
    },
    "audio_profile_schema_paths": {
        "source": REPO_ROOT / "src/mastering/assistant/audio_profile.cpp",
        "root": "MasteringAudioProfile",
        "surfaces": {
            "node": NODE / "mastering_chain.ts",
            "wasm": WASM / "mastering_core.ts",
        },
    },
    "capability_catalog_schema_paths": {
        "source": REPO_ROOT / "src/c_api/sonare_c_mastering_apply.cpp",
        "root": "CapabilityCatalog",
        "surfaces": {
            "node": NODE / "types_capabilities.ts",
            "wasm": WASM / "public_types.ts",
        },
    },
}

_DECLARATION = r"\(\)\s*\{\s*static const std::vector<std::string> paths = \{"


def schema_paths(source: str, accessor: str) -> list[str] | None:
    """The dotted paths `accessor` lists, or None when it is not a literal list.

    None and an empty list are different answers and the caller must keep them
    apart: one says the list could not be read, the other that it is empty.
    """
    match = re.search(re.escape(accessor) + _DECLARATION, source)
    if match is None:
        return None
    body = walk.brace_body(source, match.end() - 1)
    if body is None:
        return None
    literals = re.findall(r'"([^"]*)"', body)
    remainder = re.sub(r'"[^"]*"', "", body[1:-1])
    remainder = re.sub(r"//[^\n]*", "", remainder)
    if re.sub(r"[\s,]", "", remainder):
        return None
    return literals


def scan(paths: list[str], text: str, root: str) -> tuple[list, list, int]:
    """Return (missing, unreached, comparisons) for `paths` against one surface."""
    roots = walk.named_type_bodies(text, root)
    if not roots:
        return [], list(paths), 0
    missing: list[str] = []
    unreached: list[str] = []
    comparisons = 0
    for path in paths:
        # An array segment names the element type rather than a property, and a
        # root array leaves an empty leading segment once its brackets are gone.
        segments = [
            segment
            for segment in (s.replace("[]", "") for s in path.split("."))
            if segment
        ]
        if not segments:
            unreached.append(path)
            continue
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
    failed = False
    for accessor, family in FAMILIES.items():
        paths = schema_paths(family["source"].read_text(), accessor)
        if not paths:
            print(
                f"{accessor}: no literal path list was read -- it moved, was renamed, or computes its entries"
            )
            failed = True
            continue
        root = family["root"]
        for side, path in family["surfaces"].items():
            missing, unreached, comparisons = scan(paths, path.read_text(), root)
            print(
                f"{accessor} [{side}]: paths {len(paths)} | comparisons {comparisons}"
            )
            for entry in unreached:
                print(f"NOT COMPARED {entry} [{side}]: no such path under {root}")
            for entry in missing:
                print(
                    f"MISSING {entry} [{side}]: {root} reaches the block but lacks the leaf"
                )
            if missing or unreached or comparisons == 0:
                failed = True
    if failed:
        return 1
    print(
        f"every schema path of {len(FAMILIES)} results is declared on both TypeScript surfaces"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
