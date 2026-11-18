"""Render UPGRADING.md text from the migration ledgers and rules.toml.

One section per rule (in rules.toml order) that at least one ledger entry cites:
its title, summary and before/after examples, then one ``| 1.x | 2.0.0 |`` table
per surface (and per WASM entry) listing each old name with its new name,
"removed", or the change a caller must make. Entries citing ``individual`` are
rendered last under that rule's own heading. Output is deterministic.
"""

from __future__ import annotations

import argparse
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_LEDGER_DIR = HERE / "migration"
DEFAULT_RULES = HERE / "migration" / "rules.toml"

LEDGER_SURFACES = ("wasm", "node", "python", "c", "cli")
SURFACE_TITLES = {"wasm": "WASM", "node": "Node", "python": "Python", "c": "C", "cli": "CLI"}
FENCES = {"wasm": "ts", "node": "ts", "python": "python", "c": "c", "cli": "sh"}
INTRO = (
    "This guide lists every public name 2.0.0 renames, removes or reshapes, grouped by the "
    "convention that motivated the change."
)


def _load(path: Path) -> dict:
    return tomllib.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def _cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def _target(e: dict) -> str:
    if "new" in e:
        return f"`{_cell(e['new'])}`"
    if "removed" in e:
        return "removed"
    text = _cell(str(e.get("changed", "")))
    return f"`{_cell(e['to'])}`: {text}" if "to" in e else text


def _old(surface: str, e: dict) -> str:
    text = f"`{_cell(e['old'])}`"
    if surface == "cli" and e.get("frontend"):
        text += f" ({e['frontend']} CLI only)"
    return text


def render(ledger_dir: Path, rules_file: Path) -> str:
    rules = _load(rules_file).get("rule", [])
    entries: dict[str, list[tuple[str, dict]]] = {}
    for s in LEDGER_SURFACES:
        for e in _load(ledger_dir / f"{s}.toml").get("entry", []):
            entries.setdefault(e.get("rule", ""), []).append((s, e))
    ordered = [r for r in rules if r.get("slug") != "individual"]
    ordered += [r for r in rules if r.get("slug") == "individual"]
    out = ["# Upgrading to 2.0.0", "", INTRO, ""]
    for rule in ordered:
        rows = entries.get(rule.get("slug"), [])
        if not rows:
            continue
        out += [
            f"## {rule.get('title', rule.get('slug'))}",
            "",
            rule.get("summary", "").strip(),
            "",
        ]
        for ex in rule.get("example", []):
            surface = ex.get("surface", "")
            fence = FENCES.get(surface, "")
            label = SURFACE_TITLES.get(surface, surface)
            out += [
                f"{label}, 1.x:",
                "",
                f"```{fence}",
                ex.get("before", "").strip("\n"),
                "```",
                "",
            ]
            out += [
                f"{label}, 2.0.0:",
                "",
                f"```{fence}",
                ex.get("after", "").strip("\n"),
                "```",
                "",
            ]
        groups: dict[tuple[str, str], list[dict]] = {}
        for s, e in rows:
            entry = e.get("entry", ".") if s == "wasm" else ""
            groups.setdefault((s, entry), []).append(e)
        for s in LEDGER_SURFACES:
            for (gs, entry), es in sorted(groups.items()):
                if gs != s:
                    continue
                title = SURFACE_TITLES[s] + (f" (`{entry}`)" if s == "wasm" else "")
                out += [f"### {title}", "", "| 1.x | 2.0.0 |", "|---|---|"]
                lines = sorted({f"| {_old(s, e)} | {_target(e)} |" for e in es})
                out += lines + [""]
    return "\n".join(out).rstrip("\n") + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--output", type=Path, help="write to this path instead of stdout")
    ap.add_argument("--ledger-dir", type=Path, default=DEFAULT_LEDGER_DIR)
    ap.add_argument("--rules-file", type=Path, default=DEFAULT_RULES)
    args = ap.parse_args(argv)
    text = render(args.ledger_dir, args.rules_file)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
