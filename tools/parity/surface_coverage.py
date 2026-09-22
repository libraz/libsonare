"""Render the tracked per-runtime capability matrix from the parity extractors.

libsonare advertises one engine reachable from four runtimes. That claim is true
at the level of the DSP core and false if read as "every C entry point exists
everywhere", so the repository publishes the actual per-domain reach rather than
leaving a reader to infer it. This tool derives that table.

It reuses ``check_parity`` rather than re-deciding what "exposed" means: the
parity checker already resolves class methods, handle-prefix stripping and the
verified rename aliases, and a second implementation of those rules would drift
from the first. A C entry point counts as reachable on a surface unless the
checker raised a coverage finding for it — with one deliberate exception, the
lifecycle / memory helpers, which the facades answer with a constructor, GC or
RAII instead of a function and which are therefore reachable by construction.

Whether a gap is allowlisted does NOT change the table. An allowlist entry says
a divergence was reviewed; it does not put a capability back into a runtime, and
a matrix that hid reviewed gaps would restate the claim it exists to qualify.

Domains come from the public header a C function is declared in, so a new header
adds a row without anyone maintaining a list.

Usage:
    python3 tools/parity/surface_coverage.py            # rewrite the table
    python3 tools/parity/surface_coverage.py --check    # fail on a stale table
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import check_parity
import compare
from extractors import c_api, cli

DEFAULT_OUTPUT = Path("tools/parity/surface-coverage.md")

#: Table columns. The two CLI front-ends are separate binaries and get separate
#: columns: the parity checker's ``cli`` surface is their union, which answers
#: "reachable from a command line" and therefore cannot show a capability one
#: front-end ships and the other does not. A single merged column read as a
#: runtime is the misreading this split removes.
COLUMNS = ("python", "node", "wasm", "cli_python", "cli_native")
COLUMN_TITLES = {
    "python": "Python",
    "node": "Node",
    "wasm": "WASM",
    "cli_python": "CLI (python)",
    "cli_native": "CLI (native)",
}
#: Column -> the CLI front-end its report narrows the ``cli`` surface to.
CLI_FRONT_ENDS = {"cli_python": "python", "cli_native": "native"}

# Header stem -> the domain name a reader recognizes. A header with no entry
# falls back to its stem, so an unlisted new header still gets a row; what the
# map buys is that the split project headers read as one domain instead of five.
DOMAIN_TITLES = {
    "": "core (analysis, IO, conversion)",
    "types_functions": "core (analysis, IO, conversion)",
    "acoustic": "room acoustics",
    "effects": "creative effects",
    "engine": "realtime engine",
    "features": "feature extraction",
    "mastering": "mastering",
    "metering": "metering",
    "mixing": "mixing & routing",
    "project": "project & arrangement",
    "project_annotate": "project & arrangement",
    "project_core": "project & arrangement",
    "project_edit": "project & arrangement",
    "project_external_stems": "project & arrangement",
    "project_instruments": "project & arrangement",
    "project_midi": "project & arrangement",
    "streaming": "streaming",
    "transcribe": "transcription",
    "voice_changer": "voice changer",
}


def domain_of(header_path: str) -> str:
    """Domain title for the public header a C declaration came from."""
    stem = Path(header_path).stem
    prefix = "sonare_c"
    suffix = stem[len(prefix) :].lstrip("_") if stem.startswith(prefix) else stem
    return DOMAIN_TITLES.get(suffix, suffix.replace("_", " "))


def unreachable_keys(report, surfaces) -> dict[str, set[str]]:
    """Per surface, the canonical C keys the parity checker found no route to.

    Lifecycle and memory helpers are excluded: the facades answer those with the
    object model, which is a different shape, not a missing capability.
    """
    missing: dict[str, set[str]] = {surface: set() for surface in surfaces}
    for finding in report.findings:
        if finding.category != "coverage" or finding.surface not in missing:
            continue
        if not finding.key or compare._is_lifecycle_key(finding.key):
            continue
        missing[finding.surface].add(finding.key)
    return missing


def unreachable_by_column(root: Path, report) -> dict[str, set[str]]:
    """Per table column, the C keys with no route — CLI split by front-end.

    The two CLI columns come from re-running the checker with its ``cli``
    extractor narrowed to one front-end, so the reachability verdict is the same
    one the merged surface gets. Deciding it here instead would be a second
    implementation of the alias and handle rules, which is what this module
    exists not to do.
    """
    missing = unreachable_keys(report, ("python", "node", "wasm"))
    for column, front_end in CLI_FRONT_ENDS.items():
        narrowed = check_parity.run(
            root=root,
            extractors={"cli": lambda r, _fe=front_end: cli.extract(r, _fe)},
        )
        missing[column] = unreachable_keys(narrowed, ("cli",))["cli"]
    return missing


def c_declaration_headers(root: Path) -> dict[str, str]:
    """Canonical C key -> the public header that declares it."""
    return {sig.key: sig.file for sig in c_api.extract(root).functions}


def build_rows(missing, c_signatures: dict[str, str]) -> list[tuple[str, int, dict[str, int]]]:
    """(domain, C entry points, reachable count per column), domain-sorted."""
    totals: dict[str, int] = {}
    reached: dict[str, dict[str, int]] = {}
    for key, header in c_signatures.items():
        domain = domain_of(header)
        totals[domain] = totals.get(domain, 0) + 1
        counts = reached.setdefault(domain, {column: 0 for column in COLUMNS})
        for column in COLUMNS:
            if key not in missing[column]:
                counts[column] += 1
    return [(domain, totals[domain], reached[domain]) for domain in sorted(totals)]


def render(missing, c_signatures: dict[str, str]) -> str:
    rows = build_rows(missing, c_signatures)
    lines = [
        "# Runtime capability matrix",
        "",
        (
            "One C++ core, four hand-written runtimes. This table is what "
            '"the same engine everywhere" means concretely: per domain, how many of '
            "the C ABI's entry points each runtime can reach."
        ),
        "",
        (
            "**Generated — do not edit.** Run `make surface-coverage` to regenerate; "
            "`make surface-coverage-check` fails on a stale copy. The reachability "
            "decision is the parity checker's, so class methods, handle-prefix "
            "renames and verified aliases all count as reached; see "
            "[README.md](README.md) for how that decision is made."
        ),
        "",
        (
            "A gap here is a statement about reach, not about quality: both CLIs are "
            "a curated subset by design, and WASM cannot expose the host filesystem or "
            "anything that needs threads. An allowlisted divergence still counts as "
            "a gap, because a reviewed absence is still an absence."
        ),
        "",
        (
            "The two command-line front-ends get a column each because they are two "
            "binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity "
            "checker compares their union against the C ABI, so a capability only one "
            "of them ships is not drift there — this table is where that difference "
            "is visible. Which commands the two must keep identical is a separate "
            "contract, in `tests/conformance/cli_contract_v2.json`."
        ),
        "",
        "| domain | C entry points | " + " | ".join(COLUMN_TITLES[c] for c in COLUMNS) + " |",
        "|---|---:|" + "---:|" * len(COLUMNS),
    ]
    grand_total = 0
    grand_reached = {column: 0 for column in COLUMNS}
    for domain, total, counts in rows:
        grand_total += total
        cells = []
        for column in COLUMNS:
            grand_reached[column] += counts[column]
            cells.append(f"{counts[column]}/{total}")
        lines.append(f"| {domain} | {total} | " + " | ".join(cells) + " |")
    totals_cells = [f"{grand_reached[column]}/{grand_total}" for column in COLUMNS]
    lines.append(
        f"| **all domains** | **{grand_total}** | "
        + " | ".join(f"**{c}**" for c in totals_cells)
        + " |"
    )
    lines.append("")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--check", action="store_true", help="fail instead of rewriting a stale table"
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = _HERE.parent.parent
    report = check_parity.run(root=root)
    rendered = render(unreachable_by_column(root, report), c_declaration_headers(root))
    if args.check:
        if not args.output.exists():
            print(f"runtime capability matrix is missing: {args.output}", file=sys.stderr)
            return 1
        if args.output.read_text(encoding="utf-8") != rendered:
            print(
                f"runtime capability matrix is stale: {args.output} (run make surface-coverage)",
                file=sys.stderr,
            )
            return 1
        print(f"runtime capability matrix is current: {args.output}")
        return 0
    args.output.write_text(rendered, encoding="utf-8")
    print(f"wrote runtime capability matrix: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
