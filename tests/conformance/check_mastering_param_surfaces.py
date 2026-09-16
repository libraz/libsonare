#!/usr/bin/env python3
"""Every mastering chain parameter key must be declared on both TS config types.

The chain's parameter setter accepts dotted string keys, so a new key is a
string literal in one `.cpp` and nothing else in the tree has to change for it
to work. The Python surface is guarded -- its stub signatures are compared
against the runtime -- while Node and WASM declare the same configuration as
hand-written nested object types that nothing compares to anything. `make
parity` reads function signatures and is blind to the interior of a config
blob, and `tsc` cannot object to a field an interface simply lacks, so a key
added to the core and to Python alone leaves both TS types silently incomplete.

The check walks each key's path through the TS type, resolving named types and
union members, and looks for the leaf inside the block the key names. Matching
the leaf anywhere in the file is not enough: a generic leaf (`mode`, `enabled`)
occurs in many blocks, so an unscoped match reports a missing field as present.

A key whose block cannot be reached counts as a failure rather than a pass. A
comparison that did not run and a comparison that found nothing are the same
output otherwise, and this check has no value if it can report clean while
being blind to part of its population.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PARAM_SOURCE = REPO_ROOT / "src/mastering/api/chain_params.cpp"
TS_SURFACES = {
    "node": REPO_ROOT / "bindings/node/src/types_mastering.ts",
    "wasm": REPO_ROOT / "bindings/wasm/src/public_types_mastering.ts",
}

_KEY_RE = re.compile(r'key\s*==\s*"([^"]+)"')


def parameter_keys(source: str) -> list[str]:
    """Every dotted key the setter compares against, deduplicated and ordered."""
    return sorted(set(_KEY_RE.findall(source)))


def camel(name: str) -> str:
    head, *rest = name.split("_")
    return head + "".join(part.capitalize() for part in rest)


def _brace_body(text: str, start: int) -> str | None:
    """The `{...}` block beginning at `start`, including both braces."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    return None


def _skip_trivia(text: str, index: int) -> int:
    """Advance past whitespace and comments, which sit between union members."""
    while index < len(text):
        if text[index] in " \t\r\n":
            index += 1
        elif text.startswith("/*", index):
            close = text.find("*/", index)
            index = len(text) if close == -1 else close + 2
        elif text.startswith("//", index):
            newline = text.find("\n", index)
            index = len(text) if newline == -1 else newline + 1
        else:
            return index
    return index


def named_type_bodies(whole: str, name: str) -> list[str]:
    """Bodies of `interface <name> {...}` / `type <name> = {...}`."""
    bodies = []
    pattern = re.compile(r"\b(?:interface|type)\s+" + re.escape(name) + r"\b[^{=]*[={]")
    for match in pattern.finditer(whole):
        brace = whole.find("{", match.start())
        if brace == -1:
            continue
        body = _brace_body(whole, brace)
        if body is not None:
            bodies.append(body)
    return bodies


def property_bodies(text: str, prop: str, whole: str) -> list[str]:
    """Every object body a property may hold, across a union and through aliases."""
    bodies: list[str] = []
    header = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(prop) + r"\s*\??\s*:")
    for match in header.finditer(text):
        index = _skip_trivia(text, match.end())
        while index < len(text):
            if text[index] == "|":
                index = _skip_trivia(text, index + 1)
                continue
            if text[index] == "{":
                body = _brace_body(text, index)
                if body is None:
                    break
                bodies.append(body)
                index = _skip_trivia(text, index + len(body))
                continue
            identifier = re.match(r"[A-Za-z_]\w*(\[\])?", text[index:])
            if identifier is None:
                break
            bodies += named_type_bodies(whole, identifier.group(0).replace("[]", ""))
            index = _skip_trivia(text, index + identifier.end())
    return bodies


def declares_leaf(body: str, leaf: str) -> bool:
    return (
        re.search(r"(?<![A-Za-z0-9_])" + re.escape(leaf) + r"\s*\??\s*:", body)
        is not None
    )


def scan(keys: list[str], surfaces: dict[str, str]) -> tuple[list, list, int]:
    """Return (missing, unreached, comparisons) for `keys` against `surfaces`."""
    missing: list[tuple[str, str]] = []
    unreached: list[tuple[str, str]] = []
    comparisons = 0
    for key in keys:
        segments = key.split(".")
        leaf = camel(segments[-1])
        for side, text in surfaces.items():
            bodies = [text]
            reached = True
            for segment in segments[:-1]:
                nxt: list[str] = []
                for body in bodies:
                    nxt += property_bodies(body, camel(segment), text)
                if not nxt:
                    reached = False
                    break
                bodies = nxt
            if not reached:
                unreached.append((key, side))
                continue
            comparisons += 1
            if not any(declares_leaf(body, leaf) for body in bodies):
                missing.append((key, side))
    return missing, unreached, comparisons


def main() -> int:
    keys = parameter_keys(PARAM_SOURCE.read_text())
    surfaces = {side: path.read_text() for side, path in TS_SURFACES.items()}
    missing, unreached, comparisons = scan(keys, surfaces)

    print(f"keys: {len(keys)} | comparisons: {comparisons} | surfaces: {len(surfaces)}")
    if not keys or comparisons == 0:
        print("nothing was compared -- the key source or the type files moved")
        return 1
    for key, side in unreached:
        print(
            f"NOT COMPARED {key} [{side}]: no block of that path exists on this surface"
        )
    for key, side in missing:
        print(f"MISSING {key} [{side}]: the block exists but does not declare the leaf")
    if missing or unreached:
        return 1
    print("every parameter key is declared on both TypeScript configuration types")
    return 0


if __name__ == "__main__":
    sys.exit(main())
