#!/usr/bin/env python3
"""Generate the mixer-scene token cases of the core validation corpus from the schema.

Every closed token position in schemas/mixer-scene.schema.json becomes cases in the
`mix_scene_document` section of tests/conformance/core_validation_refusals.json: each
member the position admits is an accepted scene, each member of the shared set the
position excludes (`not: {enum: [...]}`) is refused, and one non-member is refused
with the loader's "must be one of" text built from the schema's own list. Every
surface loads the corpus through its own loaders, so a schema enum that differs
from the loader's set fails on all of them rather than in one schema-only test.

Hand-written cases (any id without the `token:` prefix) are kept in place; the
generated ones follow them. `--check` exits 1 when the file is not what this
script would write.
"""

from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = ROOT / "schemas/mixer-scene.schema.json"
CORPUS_PATH = ROOT / "tests/conformance/core_validation_refusals.json"
SECTION = "mix_scene_document"
PREFIX = "token:"
BASE_CASE = "clean"
SECTION_HEAD = f'  "{SECTION}": {{\n'

# Required members an array item needs when the base scene has no item to set a
# token on, keyed by the item's $defs name. A missing entry stops the generator.
SEEDS: dict[str, dict[str, Any]] = {
    "insert": {"processor": "utility.gain"},
}


def _resolve(schema: dict, node: dict) -> tuple[dict, str | None]:
    """Follow a local $ref, keeping sibling keywords; returns the node and the def name."""
    name = None
    while "$ref" in node:
        ref = node["$ref"]
        target: Any = schema
        for part in ref[2:].split("/"):
            target = target[part]
        name = ref.rsplit("/", 1)[-1]
        siblings = {key: value for key, value in node.items() if key != "$ref"}
        node = {**target, **siblings}
    return node, name


Position = tuple[list[str | int], list[Any], list[Any], list[Any], dict]


def _positions(schema: dict) -> list[Position]:
    """(path, admitted, excluded, full set, seeds per array prefix) per enum leaf."""
    found: list[Position] = []

    def walk(node: dict, path: list[str | int], seeds: dict) -> None:
        node, _ = _resolve(schema, node)
        if "enum" in node:
            excluded = node.get("not", {}).get("enum", [])
            admitted = [value for value in node["enum"] if value not in excluded]
            found.append((path, admitted, excluded, node["enum"], dict(seeds)))
        for key, child in node.get("properties", {}).items():
            walk(child, [*path, key], seeds)
        if "items" in node:
            item, name = _resolve(schema, node["items"])
            seed = (name, item.get("required", []), SEEDS.get(name or "", {}))
            walk(node["items"], [*path, 0], {**seeds, (*path, 0): seed})

    walk(schema, [], {})
    return found


def _dotted(path: list[str | int]) -> str:
    text = ""
    for step in path:
        text += f"[{step}]" if isinstance(step, int) else (f".{step}" if text else step)
    return text


def _place(scene: dict, path: list[str | int], value: Any, seeds: dict) -> dict:
    scene = copy.deepcopy(scene)
    node: Any = scene
    for index, step in enumerate(path[:-1]):
        following = path[index + 1]
        if isinstance(step, int):
            while len(node) <= step:
                name, required, seed = seeds[tuple(path[: index + 1])]
                missing = [key for key in required if key not in seed]
                if missing:
                    raise SystemExit(f"{name}: required {missing} has no entry in SEEDS")
                node.append(copy.deepcopy(seed))
            node = node[step]
        else:
            node = node.setdefault(step, [] if isinstance(following, int) else {})
    node[path[-1]] = value
    return scene


def _non_member(members: list[Any]) -> Any:
    if all(isinstance(value, int) for value in members):
        return next(value for value in range(1, 1 << 16) if value not in members)
    return "notAToken"


def _one_of(path: str, members: list[Any]) -> str:
    if all(isinstance(value, int) for value in members):
        listed = [str(value) for value in members]
        return f"{path} must be {', '.join(listed[:-1])} or {listed[-1]}"
    return f"{path} must be one of {', '.join(members)}"


def _cases(schema: dict, base: dict) -> tuple[list[dict], list[dict]]:
    accepted: list[dict] = []
    refused: list[dict] = []
    for path, admitted, excluded, full, seeds in _positions(schema):
        where = _dotted(path)
        for value in admitted:
            accepted.append(
                {"id": f"{PREFIX}{where}={value}", "scene": _place(base, path, value, seeds)}
            )
        for value in excluded:
            refused.append(
                {
                    "id": f"{PREFIX}{where}={value}",
                    "scene": _place(base, path, value, seeds),
                    "cause": "format",
                    "code": "InvalidFormat",
                    "message": f"{where} must not be {' or '.join(excluded)}",
                }
            )
        other = _non_member(full)
        refused.append(
            {
                "id": f"{PREFIX}{where}={other}",
                "scene": _place(base, path, other, seeds),
                "cause": "format",
                "code": "InvalidFormat",
                "message": _one_of(where, full),
            }
        )
    return accepted, refused


def _inline(value: Any) -> str:
    """The corpus spelling of one case or small map: `{ "k": v, ... }` on one line."""
    if isinstance(value, dict):
        return (
            "{ "
            + ", ".join(
                f"{json.dumps(k)}: {json.dumps(v, ensure_ascii=False)}" for k, v in value.items()
            )
            + " }"
        )
    return json.dumps(value, ensure_ascii=False)


def _render_section(section: dict) -> str:
    lines = [SECTION_HEAD]
    keys = list(section)
    for index, key in enumerate(keys):
        comma = "," if index < len(keys) - 1 else ""
        value = section[key]
        if isinstance(value, list) and value and isinstance(value[0], dict):
            lines.append(f'    "{key}": [\n')
            lines.extend(
                f"      {_inline(item)}{',' if i < len(value) - 1 else ''}\n"
                for i, item in enumerate(value)
            )
            lines.append(f"    ]{comma}\n")
        else:
            lines.append(f'    "{key}": {_inline(value)}{comma}\n')
    lines.append("  }\n")
    return "".join(lines)


def render(text: str, schema: dict) -> str:
    corpus = json.loads(text)
    if list(corpus)[-1] != SECTION:
        raise SystemExit(f"{SECTION} must stay the last section of {CORPUS_PATH.name}")
    section = corpus[SECTION]
    kept_accepted = [case for case in section["accepted"] if not case["id"].startswith(PREFIX)]
    kept_refused = [case for case in section["refused"] if not case["id"].startswith(PREFIX)]
    base = next(case["scene"] for case in kept_accepted if case["id"] == BASE_CASE)
    accepted, refused = _cases(schema, base)
    section = {**section, "accepted": kept_accepted + accepted, "refused": kept_refused + refused}
    head, sep, _ = text.partition(SECTION_HEAD)
    if not sep:
        raise SystemExit(f"{SECTION} section head not found")
    return head + _render_section(section) + "}\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="fail when the corpus is not up to date"
    )
    args = parser.parse_args()
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    current = CORPUS_PATH.read_text(encoding="utf-8")
    wanted = render(current, schema)
    json.loads(wanted)
    if args.check:
        if wanted != current:
            print(
                f"{CORPUS_PATH.relative_to(ROOT)} is stale: run make scene-token-cases",
                file=sys.stderr,
            )
            return 1
        print(f"scene token cases: up to date ({wanted.count(PREFIX)} cases)")
        return 0
    CORPUS_PATH.write_text(wanted, encoding="utf-8")
    print(f"scene token cases: wrote {wanted.count(PREFIX)} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
