"""Render UPGRADING.md from the migration ledgers, rules.toml and guide.toml.

One section per surface (guide.toml order) with its lead, then one subsection per
domain (tools/api/domains.py order) holding that surface's ledger entries for the
domain, grouped under the title of the rule each entry cites: a ``| 1.x | 2.0.0 |``
table listing each old name with its new name, "removed", or the change a caller
must make. WASM rows name their package entry point. A closing section states
every rule with its summary and before/after examples. Output is deterministic;
``--check`` fails when a file differs from it instead of writing.
"""

from __future__ import annotations

import argparse
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_LEDGER_DIR = HERE / "migration"
DEFAULT_RULES = HERE / "migration" / "rules.toml"
DEFAULT_GUIDE = HERE / "migration" / "guide.toml"
DEFAULT_OUTPUT = HERE.parent.parent / "UPGRADING.md"

FENCES = {"wasm": "ts", "node": "ts", "python": "python", "c": "c", "cli": "sh"}


def _load(path: Path) -> dict:
    return tomllib.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def _cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def _target(e: dict) -> str:
    if "new" in e:
        return f"`{_cell(e['new'])}`"
    if "removed" in e:
        if e["removed"] is True:
            return f"removed, no replacement: {_cell(str(e.get('no_replacement', '')))}"
        return f"removed: {_cell(e['removed'])}"
    text = _cell(str(e.get("changed", "")))
    return f"`{_cell(e['to'])}`: {text}" if "to" in e else text


def _old(surface: str, e: dict) -> str:
    text = f"`{_cell(e['old'])}`"
    if surface == "cli" and e.get("frontend"):
        text += f" ({e['frontend']} CLI only)"
    if surface == "wasm" and e.get("entry", ".") != ".":
        text += f" (`{e['entry']}`)"
    return text


def render(ledger_dir: Path, rules_file: Path, guide_file: Path) -> str:
    guide = _load(guide_file)
    rules = _load(rules_file).get("rule", [])
    surfaces = guide.get("surface", [])
    domains = guide.get("domain", [])
    rule_order = {r.get("slug"): i for i, r in enumerate(rules)}
    # Entries citing `individual` close each domain, after every named convention.
    individual = len(rules)
    rule_order["individual"] = individual
    rule_titles = {r.get("slug"): r.get("title", r.get("slug")) for r in rules}

    cited: set[str] = set()
    out = [f"# {guide.get('title', '')}", ""]
    for paragraph in guide.get("intro", []):
        out += [paragraph.strip(), ""]

    for surface in surfaces:
        key = surface["key"]
        entries = _load(ledger_dir / f"{key}.toml").get("entry", [])
        domain_keys = {d["key"] for d in domains}
        for e in entries:
            # A row the guide has no section for would vanish from the render.
            if e.get("domain") not in domain_keys or e.get("rule", "individual") not in rule_order:
                raise ValueError(
                    f"{key}.toml entry {e.get('old')!r}: domain {e.get('domain')!r} or "
                    f"rule {e.get('rule')!r} has no section in the guide"
                )
        if not entries:
            continue
        out += [f"## {surface['title']}", "", surface.get("lead", "").strip(), ""]
        for domain in domains:
            in_domain = [e for e in entries if e.get("domain") == domain["key"]]
            if not in_domain:
                continue
            out += [f"### {surface['title']}: {domain['title']}", ""]
            by_rule: dict[str, list[dict]] = {}
            for e in in_domain:
                by_rule.setdefault(e.get("rule", "individual"), []).append(e)
            cited.update(by_rule)
            for slug in sorted(by_rule, key=lambda s: (rule_order.get(s, individual), s)):
                out += [f"#### {rule_titles.get(slug, slug)}", "", "| 1.x | 2.0.0 |", "|---|---|"]
                out += sorted({f"| {_old(key, e)} | {_target(e)} |" for e in by_rule[slug]})
                out += [""]

    titles = {s["key"]: s["title"] for s in surfaces}
    out += ["## The conventions behind the changes", ""]
    for rule in rules:
        # A rule no ledger entry cites still breaks callers when it shows a before and after.
        if rule.get("slug") not in cited and not rule.get("example"):
            continue
        out += [
            f"### {rule.get('title', rule.get('slug'))}",
            "",
            rule.get("summary", "").strip(),
            "",
        ]
        for ex in rule.get("example", []):
            surface = ex.get("surface", "")
            fence = FENCES.get(surface, "")
            label = titles.get(surface, surface)
            for when, text in (("1.x", ex.get("before", "")), ("2.0.0", ex.get("after", ""))):
                out += [f"{label}, {when}:", "", f"```{fence}", text.strip("\n"), "```", ""]
    return "\n".join(out).rstrip("\n") + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    ap.add_argument("--stdout", action="store_true", help="print instead of writing --output")
    ap.add_argument(
        "--check", action="store_true", help="fail when --output differs from the render"
    )
    ap.add_argument("--ledger-dir", type=Path, default=DEFAULT_LEDGER_DIR)
    ap.add_argument("--rules-file", type=Path, default=DEFAULT_RULES)
    ap.add_argument("--guide-file", type=Path, default=DEFAULT_GUIDE)
    args = ap.parse_args(argv)
    text = render(args.ledger_dir, args.rules_file, args.guide_file)
    if args.stdout:
        sys.stdout.write(text)
        return 0
    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.exists() else None
        if current != text:
            print(
                f"{args.output} does not match the migration ledgers; run `make upgrade-notes`",
                file=sys.stderr,
            )
            return 1
        return 0
    args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
