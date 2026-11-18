"""Regenerate or check the per-binding processor-name declarations.

The shipped processor-name set has exactly one origin: the tracked capability
catalog, which is itself generated from the running library. The Node, WASM and
Python type declarations are rendered from it so no surface can carry a
hand-maintained second copy of the list.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any, NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _repo import REPO_ROOT

CATALOG_PATH = REPO_ROOT / "tools/capability-catalog.json"


class Block(NamedTuple):
    """One generated block of a declaration file, named by its marker pair."""

    name: str
    render: Callable[[Any], str]


class Target(NamedTuple):
    """One declaration file and the generated blocks it carries."""

    path: Path
    comment: str
    blocks: tuple[Block, ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--catalog", type=Path, default=CATALOG_PATH, help="tracked JSON catalog path"
    )
    parser.add_argument(
        "--check", action="store_true", help="fail instead of rewriting a stale declaration"
    )
    return parser.parse_args()


def solo_processor_names(catalog: Any) -> list[str]:
    """Return every id the solo apply path accepts, in catalog order.

    The catalog covers every id a host can surface: the offline registry, the
    realtime insert factory, and the reference-pair registry. Pair processors
    take a second reference input and are declared through their own type, so
    the solo set is every other entry. Runtime tests hold that equivalence by
    diffing this set against ``processor_names()``.
    """
    if not isinstance(catalog, dict) or not isinstance(catalog.get("processors"), list):
        raise ValueError(  # noqa: TRY004 -- one error class per document
            "catalog must be an object with a processors array"
        )
    names: list[str] = []
    for processor in catalog["processors"]:
        if not isinstance(processor, dict) or "id" not in processor or "kind" not in processor:
            raise ValueError("every catalog processor needs an id and a kind")
        if processor["kind"] != "pair":
            names.append(str(processor["id"]))
    if not names:
        raise ValueError("catalog lists no solo processors")
    if len(names) != len(set(names)):
        raise ValueError("catalog lists a duplicate solo processor id")
    return names


def render_solo_typescript(catalog: Any) -> str:
    names = solo_processor_names(catalog)
    lines = [
        "/**",
        " * Every processor name `masteringProcessorNames()` can return, and therefore",
        " * every name `masteringApplyProcessor` / `masteringApplyProcessorStereo` accept.",
        " */",
        "export const SOLO_PROCESSORS = [",
    ]
    lines += [f"  '{name}'," for name in names]
    lines += [
        "] as const;",
        "",
        "/** A processor name the solo apply path accepts; see `SOLO_PROCESSORS`. */",
        "export type SoloProcessor = (typeof SOLO_PROCESSORS)[number];",
    ]
    return "\n".join(lines)


def render_solo_pyi(catalog: Any) -> str:
    names = solo_processor_names(catalog)
    lines = [
        "# Every processor name mastering_processor_names() can return, and therefore",
        "# every name mastering_apply_processor() / mastering_apply_processor_stereo() accept.",
        "SoloProcessor: TypeAlias = Literal[",
    ]
    lines += [f'    "{name}",' for name in names]
    lines += ["]"]
    return "\n".join(lines)


def insert_processor_names(catalog: Any) -> list[str]:
    """Return every processor id the realtime insert factory builds, in catalog order."""
    names = [
        str(processor["id"])
        for processor in catalog["processors"]
        if processor.get("realtimeInsertable")
    ]
    if not names:
        raise ValueError("catalog lists no insertable processors")
    return names


def render_insert_names(catalog: Any) -> str:
    lines = [
        "/** Every processor id the realtime insert factory can build. */",
        "export const MASTERING_INSERT_NAMES = [",
    ]
    lines += [f"  '{name}'," for name in insert_processor_names(catalog)]
    lines += [
        "] as const;",
        "",
        "/** A processor id the realtime insert factory builds; see `MASTERING_INSERT_NAMES`. */",
        "export type MasteringInsertName = (typeof MASTERING_INSERT_NAMES)[number];",
    ]
    return "\n".join(lines)


def mixing_scene_presets(catalog: Any) -> list[str]:
    presets = catalog.get("presets", {}).get("mixingScene")
    if not isinstance(presets, list) or not presets:
        raise ValueError("catalog lists no mixing scene presets")
    return [str(name) for name in presets]


def render_scene_presets(catalog: Any) -> str:
    lines = [
        "/** Every built-in mixing scene `mixingScenePreset` can return. */",
        "export const MIX_SCENE_PRESETS = [",
    ]
    lines += [f"  '{name}'," for name in mixing_scene_presets(catalog)]
    lines += [
        "] as const;",
        "",
        "/** A built-in mixing scene name; see `MIX_SCENE_PRESETS`. */",
        "export type MixScenePreset = (typeof MIX_SCENE_PRESETS)[number];",
    ]
    return "\n".join(lines)


def ts_key(name: str) -> str:
    """Return @p name as an object-literal key, quoted when it is not an identifier."""
    return name if re.fullmatch(r"[A-Za-z_$][A-Za-z0-9_$]*", name) else f"'{name}'"


def ts_param_type(param: dict[str, Any]) -> str:
    kind = param["type"]
    if kind == "number":
        return "number"
    if kind == "boolean":
        return "boolean"
    if kind == "string":
        return "string"
    if kind == "array":
        return "readonly number[]"
    if kind == "enum":
        choices = param.get("choices") or []
        if not choices:
            raise ValueError(f"enum param {param['name']} lists no choices")
        return " | ".join(f"'{choice['name']}'" for choice in choices)
    raise ValueError(f"unknown param type {kind} for {param['name']}")


TS_LINE_WIDTH = 100


# Catalog unit words spelled the way a reader writes them; any other unit is printed as given.
UNIT_TEXT = {"dBPerOct": "dB/oct", "inPerSec": "in/s", "percent": "%"}


def ts_number(value: float) -> str:
    return str(int(value)) if float(value).is_integer() else repr(float(value))


def ts_doc(param: dict[str, Any]) -> str:
    """One metadata line per parameter: unit, range and default, as the catalog states them."""
    parts = []
    unit = param.get("unit")
    if unit and unit != "none":
        parts.append(f"Unit: {UNIT_TEXT.get(unit, unit)}.")
    lo, hi = param.get("min"), param.get("max")
    if param.get("maxRelativeTo") == "nyquist":
        hi = "Nyquist"
    if lo is not None or hi is not None:
        low = "-inf" if lo is None else ts_number(lo)
        high = "inf" if hi is None else hi if isinstance(hi, str) else ts_number(hi)
        left = "(" if lo is None or param.get("minExclusive") else "["
        right = ")" if hi is None or param.get("maxExclusive") else "]"
        parts.append(f"Range: {left}{low}, {high}{right}.")
    default = param.get("default")
    if default is None:
        parts.append("Default: unset.")
    elif param["type"] == "boolean":
        parts.append(f"Default: {'true' if default else 'false'}.")
    elif param["type"] == "enum":
        name = next((c["name"] for c in param["choices"] if c["value"] == default), None)
        parts.append(f"Default: '{name}'." if name is not None else f"Default: {default}.")
    else:
        parts.append(f"Default: {ts_number(default)}.")
    return f"    /** {' '.join(parts)} */"


def ts_member(param: dict[str, Any]) -> list[str]:
    """Render one optional member, wrapping a long union the way the formatter does."""
    key = ts_key(param["name"])
    kind = ts_param_type(param)
    doc = ts_doc(param)
    line = f"    {key}?: {kind};"
    if len(line) <= TS_LINE_WIDTH or " | " not in kind:
        return [doc, line]
    alternatives = kind.split(" | ")
    return (
        [doc, f"    {key}?:"]
        + [f"      | {alt}" for alt in alternatives[:-1]]
        + [f"      | {alternatives[-1]};"]
    )


def render_param_map(catalog: Any) -> str:
    lines = [
        "/**",
        " * Parameters of each catalog processor id, keyed as in `masteringInsertParamInfo`.",
        " * Every key is optional; an enum-valued key takes one of its `choices` names.",
        " */",
        "export interface MasteringProcessorParamMap {",
    ]
    for processor in catalog["processors"]:
        params = processor["params"]
        lines.append(f"  /** Parameters of the `{processor['id']}` processor. */")
        if not params:
            lines.append(f"  '{processor['id']}': Record<string, never>;")
            continue
        lines.append(f"  '{processor['id']}': {{")
        for p in params:
            lines += ts_member(p)
        lines.append("  };")
    lines += [
        "}",
        "",
        "/** The params object of processor `P`. */",
        "export type MasteringProcessorParams<P extends keyof MasteringProcessorParamMap> =",
        "  MasteringProcessorParamMap[P];",
    ]
    return "\n".join(lines)


TYPESCRIPT_BLOCKS = (
    Block("SoloProcessor", render_solo_typescript),
    Block("MasteringInsertName", render_insert_names),
    Block("MasteringProcessorParamMap", render_param_map),
    Block("MixScenePreset", render_scene_presets),
)

TARGETS = (
    Target(REPO_ROOT / "bindings/node/src/types_mastering.ts", "//", TYPESCRIPT_BLOCKS),
    Target(REPO_ROOT / "bindings/wasm/src/public_types_mastering.ts", "//", TYPESCRIPT_BLOCKS),
    Target(
        REPO_ROOT / "bindings/python/src/libsonare/analyzer.pyi",
        "#",
        (Block("SoloProcessor", render_solo_pyi),),
    ),
    Target(
        REPO_ROOT / "bindings/python/src/libsonare/_types_mastering.py",
        "#",
        (Block("SoloProcessor", render_solo_pyi),),
    ),
)


def splice(text: str, target: Target, block: Block, catalog: Any) -> str:
    """Replace one generated block in @p text, keeping the marker lines."""
    body = block.render(catalog)
    begin = f"{target.comment} BEGIN GENERATED {block.name} (make processor-types)"
    end = f"{target.comment} END GENERATED {block.name}"
    start = text.find(begin)
    if start < 0:
        raise ValueError(f"{target.path} is missing the marker: {begin}")
    stop = text.find(end, start)
    if stop < 0:
        raise ValueError(f"{target.path} is missing the marker: {end}")
    return f"{text[: start + len(begin)]}\n{body}\n{text[stop:]}"


def main() -> int:
    args = parse_args()
    try:
        catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
        solo_processor_names(catalog)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"could not read the capability catalog: {exc}", file=sys.stderr)
        return 2

    stale: list[Path] = []
    for target in TARGETS:
        try:
            current = target.path.read_text(encoding="utf-8")
            rendered = current
            for block in target.blocks:
                rendered = splice(rendered, target, block, catalog)
        except (OSError, ValueError) as exc:
            print(f"could not generate processor types: {exc}", file=sys.stderr)
            return 2
        if current == rendered:
            continue
        if args.check:
            stale.append(target.path)
            continue
        target.path.write_text(rendered, encoding="utf-8")
        print(f"wrote processor types: {target.path.relative_to(REPO_ROOT)}")

    if args.check:
        if stale:
            for path in stale:
                print(
                    f"processor types are stale: {path.relative_to(REPO_ROOT)} "
                    "(run make processor-types)",
                    file=sys.stderr,
                )
            return 1
        print("processor types are current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
