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

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ts_surface_walk import camel, declares_array_leaf, declares_leaf, resolve_blocks

REPO_ROOT = Path(__file__).resolve().parents[2]
PARAM_SOURCE = REPO_ROOT / "src/mastering/api/chain_params.cpp"
FIELD_TABLES = REPO_ROOT / "src/mastering/api/param_field_tables.h"
TS_SURFACES = {
    "node": REPO_ROOT / "bindings/node/src/types_mastering.ts",
    "wasm": REPO_ROOT / "bindings/wasm/src/public_types_mastering.ts",
}

_KEY_RE = re.compile(r'key\s*==\s*"([^"]+)"')
_COMPRESSOR_MACRO_RE = re.compile(
    r"#define\s+SONARE_FIELDS_COMPRESSOR\(X\)((?:[^\n]*\\\n)*[^\n]*)"
)
_FIELD_RE = re.compile(r'X\(\s*"([^"]+)"')

MULTIBAND = "dynamics.multibandComp"
# The array spelling the TS types declare: element 0 stands for every index.
CROSSOVER_KEYS = [
    f"{MULTIBAND}.crossover.cutoffsHz.0",
    f"{MULTIBAND}.crossover.slope",
    f"{MULTIBAND}.crossover.mode",
    f"{MULTIBAND}.crossover.firKernelSize",
]


def parameter_keys(source: str) -> list[str]:
    """Every dotted key the setter compares against, deduplicated and ordered."""
    return sorted(set(_KEY_RE.findall(source)))


def band_field_keys(tables: str) -> list[str]:
    """`bands.0.<field>` for every field of the chain's compressor X-macro."""
    macro = _COMPRESSOR_MACRO_RE.search(tables)
    fields = _FIELD_RE.findall(macro.group(1)) if macro else []
    return [f"{MULTIBAND}.bands.0.{field}" for field in fields]


def indexed_keys(tables: str) -> list[str]:
    """The indexed multiband families, which no `key == "..."` literal spells."""
    return sorted(set(CROSSOVER_KEYS + band_field_keys(tables)))


def scan(keys: list[str], surfaces: dict[str, str]) -> tuple[list, list, int]:
    """Return (missing, unreached, comparisons) for `keys` against `surfaces`."""
    missing: list[tuple[str, str]] = []
    unreached: list[tuple[str, str]] = []
    comparisons = 0
    for key in keys:
        segments = key.split(".")
        indexed_leaf = segments[-1].isdigit()
        leaf = camel(segments[-2] if indexed_leaf else segments[-1])
        for side, text in surfaces.items():
            bodies = resolve_blocks(text, segments[: -2 if indexed_leaf else -1])
            if not bodies:
                unreached.append((key, side))
                continue
            comparisons += 1
            declares = declares_array_leaf if indexed_leaf else declares_leaf
            if not any(declares(body, leaf) for body in bodies):
                missing.append((key, side))
    return missing, unreached, comparisons


def main() -> int:
    literal_keys = parameter_keys(PARAM_SOURCE.read_text())
    indexed = indexed_keys(FIELD_TABLES.read_text())
    keys = sorted(set(literal_keys + indexed))
    if not band_field_keys(FIELD_TABLES.read_text()):
        print("no band fields were read -- SONARE_FIELDS_COMPRESSOR moved")
        return 1
    surfaces = {side: path.read_text() for side, path in TS_SURFACES.items()}
    missing, unreached, comparisons = scan(keys, surfaces)

    print(
        f"keys: {len(keys)} (indexed: {len(indexed)}) | comparisons: {comparisons} | surfaces: {len(surfaces)}"
    )
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
