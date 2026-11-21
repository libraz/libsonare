"""Hold each facade literal that stands in for a core default equal to the core constant.

Some C entries take a quantity as a required argument while the facades let the
caller omit it, so each facade spells the core's default as a literal of its own
(``beta ?? 2`` on Node and WASM, ``beta: float = 2.0`` in Python). The parity
default check cannot see a default written inside a request normalizer, so this
check reads those spellings directly. Each row names the core constant and the
wording of each facade site with the number as a capture group, so a moved value
is reported as a mismatch rather than vanishing from the scan. Each site carries
the number of matches it had when written, counted over the whole surface tree:
wording that stops matching fails instead of quietly measuring less, and a new
site nobody listed fails until its row's count is raised.

Run: python3 tests/conformance/check_facade_default_literals.py
Self-tests: python3 tests/conformance/test_facade_default_literals.py
"""

from __future__ import annotations

import re
import sys
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Site(typing.NamedTuple):
    surface: str
    path: str  # a file, or a directory scanned recursively for SOURCE_SUFFIXES
    pattern: str  # one capture group: the literal
    count: int


SOURCE_SUFFIXES = (".ts", ".py", ".pyi")


class Literal(typing.NamedTuple):
    constant: str
    header: str
    sites: tuple[Site, ...]


ROWS: tuple[Literal, ...] = (
    Literal(
        constant="kDecomposeDefaultBeta",
        header="src/effects/decompose.h",
        sites=(
            Site("node", "bindings/node/src", r"\bbeta \?\? ([-\d.]+)", 1),
            Site("wasm", "bindings/wasm/src", r"\bbeta \?\? ([-\d.]+)", 1),
            # Only the librosa-named decompose (runtime + stub) states it.
            Site("python", "bindings/python/src/libsonare", r"\bbeta: float = ([-\d.]+)", 2),
        ),
    ),
    Literal(
        constant="kDefaultDrainRecords",
        header="src/engine/realtime_engine.h",
        sites=(
            # Node and WASM leave an omitted count to the native default.
            Site("node", "bindings/node/src", r"\bmaxRecords = (\d+)", 0),
            Site("wasm", "bindings/wasm/src", r"\bmaxRecords = (\d+)", 0),
            Site("python", "bindings/python/src/libsonare", r"\bmax_records: int = (\d+)", 8),
        ),
    ),
)

_NUMBER = r"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)[fF]?"


def core_value(text: str, constant: str) -> float | None:
    m = re.search(rf"\bconstexpr\s+[\w:]+\s+{re.escape(constant)}\s*=\s*{_NUMBER}\s*;", text)
    return float(m.group(1)) if m else None


def evaluate(root: Path, rows: tuple[Literal, ...] = ROWS) -> tuple[list[str], int, int]:
    """Problems, constants checked and sites matched."""
    problems: list[str] = []
    matched = 0
    for row in rows:
        header = root / row.header
        value = (
            core_value(header.read_text(encoding="utf-8"), row.constant)
            if header.is_file()
            else None
        )
        if value is None:
            problems.append(f"{row.constant}: not found in {row.header}")
            continue
        for site in row.sites:
            path = root / site.path
            files = (
                sorted(f for f in path.rglob("*") if f.suffix in SOURCE_SUFFIXES)
                if path.is_dir()
                else [path]
                if path.is_file()
                else []
            )
            found = [
                lit
                for f in files
                for lit in re.findall(site.pattern, f.read_text(encoding="utf-8"), re.MULTILINE)
            ]
            where = f"{row.constant}: {site.surface} {site.path}"
            if len(found) != site.count:
                problems.append(f"{where}: {len(found)} site(s) match, {site.count} expected")
            matched += len(found)
            for literal in found:
                if float(literal) != value:
                    problems.append(f"{where}: literal {literal} differs from the core's {value:g}")
    return problems, len(rows), matched


def main() -> int:
    problems, rows, matched = evaluate(ROOT)
    print(f"facade default literals: {rows} constants, {matched} sites matched")
    if rows == 0 or matched == 0:
        print("FAIL: nothing was compared")
        return 1
    for p in problems:
        print(f"FAIL: {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
