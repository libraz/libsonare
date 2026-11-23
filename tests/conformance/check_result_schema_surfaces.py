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

import ast
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ts_surface_walk as walk

REPO_ROOT = Path(__file__).resolve().parents[2]

CONFORMANCE = REPO_ROOT / "tests/conformance"
NODE = REPO_ROOT / "bindings/node/src"
WASM = REPO_ROOT / "bindings/wasm/src"

# One entry per published result, keyed by its list's accessor: where the list is
# defined, the TypeScript type its paths are rooted at, and the file on each
# surface that declares that type. A root-array writer roots its paths at the
# element, so its paths open with the `[]` segment and the type named here is the
# element type. Tonal balance on log bands writes the linear-band shape and list,
# so the one family covers both. `delegated` names a subtree whose contents
# another check owns; see delegate().
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
        "source": REPO_ROOT / "src/mastering/api/insert_param_catalog.cpp",
        "root": "MasteringInsertParamInfo",
        "surfaces": {
            "node": NODE / "mastering_chain.ts",
            "wasm": WASM / "mastering_core.ts",
            # The Python result types are dataclasses: the runtime module and
            # the stub that mypy reads each declare the same shape.
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_capabilities.py",
        },
    },
    "processor_catalog_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/named_processor_registry.cpp",
        "root": "MasteringProcessorCatalogEntry",
        "surfaces": {
            "node": NODE / "mastering_chain.ts",
            "wasm": WASM / "mastering_core.ts",
            # The Python result types are dataclasses: the runtime module and
            # the stub that mypy reads each declare the same shape.
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_capabilities.py",
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
            # A shipped JSON Schema is a declaration of the same shape for a
            # consumer with no TypeScript, so it answers to the same writer.
            "jsonschema": REPO_ROOT / "schemas/mixer-scene.schema.json",
        },
    },
    "audio_profile_schema_paths": {
        "source": REPO_ROOT / "src/mastering/assistant/audio_profile.cpp",
        "root": "MasteringAudioProfile",
        "surfaces": {
            "node": NODE / "mastering_chain.ts",
            "wasm": WASM / "mastering_core.ts",
            # TypedDicts over the parsed JSON, so their keys are the wire's own.
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "repair_analysis_schema_paths": {
        "source": REPO_ROOT / "src/mastering/assistant/repair_session.cpp",
        "root": "MasteringRepairAnalysis",
        "surfaces": {
            "node": NODE / "repair_session.ts",
            "wasm": WASM / "repair_session.ts",
        },
    },
    "repair_apply_schema_paths": {
        "source": REPO_ROOT / "src/mastering/assistant/repair_session.cpp",
        "root": "MasteringRepairStageReports",
        "surfaces": {
            "node": NODE / "repair_session.ts",
            "wasm": WASM / "repair_session.ts",
        },
    },
    "assistant_result_schema_paths": {
        "source": REPO_ROOT / "src/mastering/assistant/suggester.cpp",
        "root": "MasteringAssistantResult",
        # The suggested chain is the chain-parameter contract: the wire carries
        # it as a {version, params} document and the facades as the nested config
        # a caller passes back, and the parameter check holds both to the core.
        "delegated": {"chainConfig": CONFORMANCE / "check_mastering_param_surfaces.py"},
        "surfaces": {
            "node": NODE / "types_mastering.ts",
            "wasm": WASM / "public_types_mastering.ts",
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "streaming_preview_schema_paths": {
        "source": REPO_ROOT / "src/mastering/maximizer/streaming_preview.cpp",
        "root": "MasteringStreamingPreviewResult",
        "surfaces": {
            "node": NODE / "types_mastering.ts",
            "wasm": WASM / "public_types_mastering.ts",
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "match_reference_loudness_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/named_processor.cpp",
        "root": "MatchReferenceLoudnessResult",
        "surfaces": {
            "node": NODE / "types_mastering.ts",
            "wasm": WASM / "public_types_mastering.ts",
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "match_tonal_balance_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/named_processor.cpp",
        "root": "MatchTonalBalanceResult",
        "surfaces": {
            "node": NODE / "types_mastering.ts",
            "wasm": WASM / "public_types_mastering.ts",
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "match_eq_curve_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/named_processor.cpp",
        "root": "MatchEqCurveResult",
        "surfaces": {
            "node": NODE / "types_mastering.ts",
            "wasm": WASM / "public_types_mastering.ts",
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "match_reference_delay_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/named_processor.cpp",
        "root": "MatchEstimateReferenceDelaySamplesResult",
        "surfaces": {
            "node": NODE / "types_mastering.ts",
            "wasm": WASM / "public_types_mastering.ts",
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "stereo_mono_compat_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/named_processor.cpp",
        "root": "StereoMonoCompatCheckResult",
        "surfaces": {
            "node": NODE / "types_mastering.ts",
            "wasm": WASM / "public_types_mastering.ts",
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "stereo_mono_compat_log_bands_schema_paths": {
        "source": REPO_ROOT / "src/mastering/api/named_processor.cpp",
        "root": "StereoMonoCompatCheckLogBandsResult",
        "surfaces": {
            "node": NODE / "types_mastering.ts",
            "wasm": WASM / "public_types_mastering.ts",
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_mastering_results.py",
        },
    },
    "capability_catalog_schema_paths": {
        "source": REPO_ROOT / "src/c_api/sonare_c_mastering_apply.cpp",
        "root": "CapabilityCatalog",
        "surfaces": {
            "node": NODE / "types_capabilities.ts",
            "wasm": WASM / "public_types.ts",
            # The Python result types are dataclasses: the runtime module and
            # the stub that mypy reads each declare the same shape.
            "python": REPO_ROOT / "bindings/python/src/libsonare/_types_capabilities.py",
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


def _resolve(node: object, schema: dict) -> object:
    """Follow `$ref` to the node it names, or None when it does not resolve."""
    for _ in range(16):  # a cyclic $ref would otherwise spin here
        if not isinstance(node, dict) or "$ref" not in node:
            return node
        ref = node["$ref"]
        if not isinstance(ref, str) or not ref.startswith("#/"):
            return None
        target: object = schema
        for part in ref[2:].split("/"):
            if not isinstance(target, dict) or part not in target:
                return None
            target = target[part]
        node = target
    return None


def _descend(schema: dict, segments: list[str]) -> object:
    """The schema node `segments` names, or None when the walk cannot continue.

    A `[]` segment steps through `items`, which the TypeScript walk cannot do
    because a TS array type names its element inline. Keeping it here means a
    path that is an array on one side and an object on the other is caught
    rather than flattened away.
    """
    node: object = schema
    for segment in segments:
        name = segment.removesuffix("[]")
        node = _resolve(node, schema)
        props = node.get("properties") if isinstance(node, dict) else None
        if not isinstance(props, dict) or name not in props:
            return None
        node = props[name]
        if segment.endswith("[]"):
            node = _resolve(node, schema)
            if not isinstance(node, dict) or "items" not in node:
                return None
            node = node["items"]
    return node


def scan_json_schema(paths: list[str], text: str, root: str) -> tuple[list, list, int]:
    """scan() for a shipped JSON Schema, with the same three return meanings."""
    schema = json.loads(text)
    # Anchored on the declared title for the same reason the TypeScript walk is
    # anchored on a root type: a file-wide key search would accept a path that
    # only some sub-schema carries.
    if not isinstance(schema, dict) or schema.get("title") != root:
        return [], list(paths), 0
    missing: list[str] = []
    unreached: list[str] = []
    comparisons = 0
    for path in paths:
        segments = path.split(".")
        parent = _descend(schema, segments[:-1])
        resolved = _resolve(parent, schema) if parent is not None else None
        props = resolved.get("properties") if isinstance(resolved, dict) else None
        # No properties block is "the schema says nothing about this object",
        # which is undecidable rather than absent -- the same verdict the
        # TypeScript walk gives a Record<string, unknown>. A $ref that does not
        # resolve lands here too, so a cycle cannot be reported as a missing
        # leaf, which would claim the block was reached.
        if not isinstance(props, dict):
            unreached.append(path)
            continue
        comparisons += 1
        leaf = segments[-1]
        name = leaf.removesuffix("[]")
        if name not in props:
            missing.append(path)
    return missing, unreached, comparisons


def _class_fields(tree: ast.Module) -> tuple[dict[str, dict[str, set[str]]], set[str]]:
    """Each class's annotated fields with the class names each annotation mentions,
    and the names of the classes that are dataclasses."""
    classes: dict[str, dict[str, set[str]]] = {}
    dataclasses: set[str] = set()
    for node in tree.body:
        if not isinstance(node, ast.ClassDef):
            continue
        if any(_names_dataclass(decorator) for decorator in node.decorator_list):
            dataclasses.add(node.name)
        fields: dict[str, set[str]] = {}
        for item in node.body:
            if isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
                fields[item.target.id] = {
                    n.id for n in ast.walk(item.annotation) if isinstance(n, ast.Name)
                }
        classes[node.name] = fields
    return classes, dataclasses


def _names_dataclass(decorator: ast.expr) -> bool:
    """True for `@dataclass`, `@dataclass(...)` and `@dataclasses.dataclass(...)`."""
    target = decorator.func if isinstance(decorator, ast.Call) else decorator
    name = (
        target.attr
        if isinstance(target, ast.Attribute)
        else getattr(target, "id", None)
    )
    return name == "dataclass"


def _snake_case(segment: str) -> str:
    """`truePeakDbtp` -> `true_peak_dbtp`."""
    return re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", segment).lower()


def scan_python(
    paths: list[str], text: str, root: str, imported: tuple[str, ...] = ()
) -> tuple[list, list, int]:
    """scan() for a Python module of result classes, with the same three return meanings.

    Anchored on the root class like the TypeScript walk. A segment is followed
    through every class its field's annotation names, so `list[Element]` and
    `Element | None` both reach `Element`. A dataclass's attribute is the
    snake_case spelling of the published camelCase segment, so the segment is
    converted before it is looked up there; a TypedDict carries the wire key
    itself and is matched verbatim. @p imported holds the texts of the modules
    @p text imports from; their classes are reachable, and the module's own
    class wins a name both declare.
    """
    classes: dict[str, dict[str, set[str]]] = {}
    dataclasses: set[str] = set()
    for source in (*imported, text):
        module_classes, module_dataclasses = _class_fields(ast.parse(source))
        classes.update(module_classes)
        dataclasses -= module_classes.keys()
        dataclasses |= module_dataclasses

    def field(body: str, segment: str) -> str:
        return _snake_case(segment) if body in dataclasses else segment

    if root not in classes:
        return [], list(paths), 0
    missing: list[str] = []
    unreached: list[str] = []
    comparisons = 0
    for path in paths:
        segments = [
            segment for segment in (s.replace("[]", "") for s in path.split(".")) if segment
        ]
        if not segments:
            unreached.append(path)
            continue
        bodies = [root]
        for segment in segments[:-1]:
            bodies = [
                target
                for body in bodies
                for target in classes[body].get(field(body, segment), set())
                if target in classes
            ]
            if not bodies:
                break
        if not bodies:
            unreached.append(path)
            continue
        comparisons += 1
        if not any(field(body, segments[-1]) in classes[body] for body in bodies):
            missing.append(path)
    return missing, unreached, comparisons


def delegate(paths: list[str], delegated: dict[str, Path]) -> tuple[list[str], list[str]]:
    """Drop the paths under each delegated subtree; return (kept, problems).

    The subtree's root stays compared, so the block must still be declared. A
    delegation is a claim that another check covers the contents, so an owner
    check that does not exist, or a prefix with nothing under it, is a problem
    rather than a silent skip.
    """
    problems: list[str] = []
    kept = list(paths)
    for prefix, owner in delegated.items():
        under = [p for p in kept if p.startswith((prefix + ".", prefix + "[]"))]
        if not owner.is_file():
            problems.append(f"{prefix}: delegated to {owner.name}, which does not exist")
        if not under:
            problems.append(f"{prefix}: delegated, but the list has no path under it")
        kept = [p for p in kept if p not in under]
    return kept, problems


def scan_surface(paths: list[str], path: Path, root: str) -> tuple[list, list, int]:
    """Walk one surface with the reader its file kind calls for.

    The one place a surface is matched to a walker: a second copy of this test
    would let the self-test and the check disagree about what a file is, and the
    disagreement would surface as every path being unreachable rather than as a
    wrong reader.
    """
    if path.suffix == ".json":
        return scan_json_schema(paths, path.read_text(), root)
    if path.suffix in (".py", ".pyi"):
        return scan_python(paths, path.read_text(), root, python_imported_modules(path))
    return scan(paths, with_imported_declarations(path), root)


def python_imported_modules(path: Path) -> tuple[str, ...]:
    """The texts of the sibling modules `path` imports names from, one hop deep.

    The Python analogue of with_imported_declarations: a record shared between
    result families is declared once and imported where the root lives, so a
    `from ._x import Name` is followed to `_x` (a stub prefers `_x.pyi`).
    """
    suffixes = (path.suffix, ".py" if path.suffix == ".pyi" else ".pyi")
    texts: list[str] = []
    seen: set[Path] = set()
    for node in ast.walk(ast.parse(path.read_text())):
        if not isinstance(node, ast.ImportFrom) or node.level != 1 or not node.module:
            continue
        stem = node.module.replace(".", "/")
        module = next(
            (m for m in (path.parent / f"{stem}{suffix}" for suffix in suffixes) if m.is_file()),
            None,
        )
        if module is not None and module not in seen:
            seen.add(module)
            texts.append(module.read_text())
    return tuple(texts)


_RELATIVE_MODULE = re.compile(
    r"""^(?:import|export)\s+(?:type\s+)?(?:\{[^}]*\}|\*)\s+from\s+['"](\./[^'"]+)['"]""",
    re.MULTILINE,
)


def with_imported_declarations(path: Path, depth: int = 3) -> str:
    """Return `path`'s text followed by the sibling modules it imports from.

    A nested type such as an array element is often declared once and imported
    where the root lives, sometimes through a barrel that re-exports it; reading
    only the root's file would report every path beneath it as unreachable.
    Relative imports and re-exports are followed up to @p depth modules deep.
    """
    parts: list[str] = []
    seen: set[Path] = set()
    frontier = [path]
    for _ in range(depth + 1):
        following: list[Path] = []
        for module in frontier:
            if module in seen or not module.is_file():
                continue
            seen.add(module)
            text = module.read_text()
            parts.append(text)
            for spec in _RELATIVE_MODULE.findall(text):
                stem = spec[2:].removesuffix(".js")
                following.append(module.parent / f"{stem}.ts")
        frontier = following
    return "\n".join(parts)


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
            segment for segment in (s.replace("[]", "") for s in path.split(".")) if segment
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
    for label, family in FAMILIES.items():
        paths = schema_paths(family["source"].read_text(), label)
        if not paths:
            print(
                f"{label}: no literal path list was read -- it moved, was renamed, or computes its entries"
            )
            failed = True
            continue
        paths, problems = delegate(paths, family.get("delegated", {}))
        for problem in problems:
            print(f"{label}: {problem}")
            failed = True
        for prefix, owner in family.get("delegated", {}).items():
            print(f"{label}: {prefix}.* delegated to {owner.name}")
        root = family["root"]
        for side, path in family["surfaces"].items():
            missing, unreached, comparisons = scan_surface(paths, path, root)
            print(f"{label} [{side}]: paths {len(paths)} | comparisons {comparisons}")
            for entry in unreached:
                print(f"NOT COMPARED {entry} [{side}]: no such path under {root}")
            for entry in missing:
                print(f"MISSING {entry} [{side}]: {root} reaches the block but lacks the leaf")
            if missing or unreached or comparisons == 0:
                failed = True
    if failed:
        return 1
    surfaces = sum(len(family["surfaces"]) for family in FAMILIES.values())
    print(
        f"every schema path of {len(FAMILIES)} results is declared on each of its {surfaces} surfaces"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
