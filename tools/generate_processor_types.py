#!/usr/bin/env python3
"""Regenerate or check the per-binding processor-name declarations.

The shipped processor-name set has exactly one origin: the tracked capability
catalog, which is itself generated from the running library. The Node, WASM and
Python type declarations are rendered from it so no surface can carry a
hand-maintained second copy of the list.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Callable, NamedTuple

REPO_ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = REPO_ROOT / "tools/capability-catalog.json"

BEGIN_MARKER = "BEGIN GENERATED SoloProcessor (make processor-types)"
END_MARKER = "END GENERATED SoloProcessor"


class Target(NamedTuple):
    """One declaration file and how its generated block is spelled."""

    path: Path
    comment: str
    render: Callable[[list[str]], str]


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
        raise ValueError("catalog must be an object with a processors array")
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


def render_typescript(names: list[str]) -> str:
    lines = [
        "/**",
        " * Every processor name `masteringProcessorNames()` can return, and therefore",
        " * every name `masteringProcess` / `masteringProcessStereo` accept.",
        " */",
        "export const SOLO_PROCESSORS = [",
    ]
    lines += [f"  '{name}'," for name in names]
    lines += [
        "] as const;",
        "",
        "export type SoloProcessor = (typeof SOLO_PROCESSORS)[number];",
    ]
    return "\n".join(lines)


def render_pyi(names: list[str]) -> str:
    lines = [
        "# Every processor name mastering_processor_names() can return, and therefore",
        "# every name mastering_process() / mastering_process_stereo() accept.",
        "SoloProcessor: TypeAlias = Literal[",
    ]
    lines += [f'    "{name}",' for name in names]
    lines += ["]"]
    return "\n".join(lines)


TARGETS = (
    Target(REPO_ROOT / "bindings/node/src/types_mastering.ts", "//", render_typescript),
    Target(REPO_ROOT / "bindings/wasm/src/public_types_mastering.ts", "//", render_typescript),
    Target(REPO_ROOT / "bindings/python/src/libsonare/analyzer.pyi", "#", render_pyi),
)


def splice(text: str, target: Target, body: str) -> str:
    """Replace the generated block in @p text, keeping the marker lines."""
    begin = f"{target.comment} {BEGIN_MARKER}"
    end = f"{target.comment} {END_MARKER}"
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
        names = solo_processor_names(catalog)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"could not read the capability catalog: {exc}", file=sys.stderr)
        return 2

    stale: list[Path] = []
    for target in TARGETS:
        try:
            current = target.path.read_text(encoding="utf-8")
            rendered = splice(current, target, target.render(names))
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
        print(f"processor types are current: {len(names)} solo processors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
