"""Vocabulary census over the public-interface inventory, as markdown on stdout.

Counts per surface of leading verbs, synonym families, lifecycle spellings,
parameter / flag / field names, unit suffixes and value tokens, plus the names
present on only some binding surfaces. It is a reading aid for spotting one
concept under two names; it enforces nothing. Reads the inventory module
directly, or a saved ``inventory.py --json`` dump via ``--inventory-file``.
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from domains import snake

BINDINGS = ("wasm", "node", "python")
COLUMNS = ("wasm", "node", "python", "c", "cli")
CALLABLE = {"function", "method"}
LIFECYCLE = re.compile(
    r"dispose|close|destroy|free|delete|release|terminate|shutdown|__enter__|__exit__|__del__",
    re.IGNORECASE,
)
SYNONYMS = {
    "obtain": (
        "get",
        "fetch",
        "read",
        "query",
        "load",
        "open",
        "list",
        "find",
        "lookup",
        "peek",
        "snapshot",
        "describe",
        "inspect",
    ),
    "construct": (
        "create",
        "make",
        "new",
        "build",
        "init",
        "from",
        "add",
        "insert",
        "append",
        "push",
        "register",
        "attach",
        "bind",
    ),
    "teardown": (
        "destroy",
        "dispose",
        "close",
        "free",
        "release",
        "delete",
        "remove",
        "clear",
        "reset",
        "detach",
        "unbind",
        "unregister",
        "stop",
        "terminate",
        "abort",
        "cancel",
    ),
    "mutate": (
        "set",
        "configure",
        "apply",
        "update",
        "replace",
        "edit",
        "change",
        "assign",
        "put",
        "patch",
        "modify",
    ),
    "derive": (
        "compute",
        "calculate",
        "calc",
        "estimate",
        "detect",
        "analyze",
        "analyse",
        "measure",
        "extract",
        "evaluate",
        "infer",
        "track",
    ),
    "run": ("process", "render", "run", "execute", "perform", "bounce", "export", "play", "start"),
    "convert": (
        "to",
        "convert",
        "as",
        "parse",
        "serialize",
        "encode",
        "decode",
        "import",
        "dump",
        "save",
        "write",
    ),
    "predicate": ("is", "has", "can", "supports", "should", "was", "needs"),
}
UNIT_SUFFIXES = (
    "db",
    "hz",
    "ms",
    "sec",
    "seconds",
    "samples",
    "sample",
    "frames",
    "frame",
    "bpm",
    "cents",
    "semitones",
    "percent",
    "ratio",
    "lufs",
    "dbtp",
    "beats",
    "ticks",
    "ppq",
)
UNITLESS = re.compile(
    r"(^|_)(threshold|attack|release|gain|level|freq|frequency|cutoff|time|duration|delay|length"
    r"|position|offset|start|end|pitch|tempo|rate|width|depth|amount|mix|ceiling|knee|hold|decay"
    r"|fade|latency|lookahead)$"
)
EXAMPLES = 6


def _column(surface: str) -> str:
    return "cli" if surface.startswith("cli") else surface


def _key(name: str) -> str:
    return snake(name.lstrip("-").replace("-", "_").rstrip("?"))


def _callable_name(r: dict) -> str:
    base = r["member"] or r["name"]
    if base.startswith("["):
        return base
    if r["surface"] == "c":
        base = base.removeprefix("sonare_")
    return snake(base)


def _counted(title: str, data: dict, examples: dict, cols=COLUMNS, minimum: int = 1) -> list[str]:
    out = [
        f"## {title}",
        "",
        "| key | " + " | ".join(cols) + " | examples |",
        "|---" * (len(cols) + 2) + "|",
    ]
    for k in sorted(data):
        c = data[k]
        if sum(c.values()) < minimum:
            continue
        ex = ", ".join(sorted(examples[k])[:EXAMPLES])
        out.append(f"| {k} | " + " | ".join(str(c[s]) for s in cols) + f" | {ex} |")
    return out + [""]


def render(records: list[dict]) -> str:
    verbs: dict = collections.defaultdict(collections.Counter)
    verb_names: dict = collections.defaultdict(lambda: collections.defaultdict(set))
    c_tokens: collections.Counter = collections.Counter()
    c_trailing: collections.Counter = collections.Counter()
    life: dict = collections.defaultdict(lambda: collections.defaultdict(set))
    classes: dict = collections.defaultdict(set)
    params: dict = collections.defaultdict(collections.Counter)
    pex: dict = collections.defaultdict(set)
    fields: dict = collections.defaultdict(collections.Counter)
    fex: dict = collections.defaultdict(set)
    tokens: dict = collections.defaultdict(collections.Counter)
    tex: dict = collections.defaultdict(set)
    presence: dict = collections.defaultdict(set)

    for r in records:
        s, kind = r["surface"], r["kind"]
        col = _column(s)
        if kind == "class":
            classes[r["name"]].add(s)
        if kind in CALLABLE:
            nm = _callable_name(r)
            if s == "c":
                c_tokens.update(nm.split("_"))
                c_trailing[nm.rsplit("_", 1)[-1]] += 1
            elif s in BINDINGS:
                v = nm.split("_")[0]
                verbs[v][s] += 1
                verb_names[v][s].add((r["owner"] + "." if r["owner"] else "") + nm)
            if kind == "function" and s in BINDINGS:
                presence[snake(r["name"])].add(s)
            if r["owner"] and LIFECYCLE.search(r["member"]):
                life[r["owner"]][col].add(r["member"])
            if s == "c" and LIFECYCLE.search(r["name"]):
                owner = re.sub(r"_(destroy|free|release|close|delete)$", "", nm)
                life[owner]["c"].add(nm)
        elif kind in ("option", "flag"):
            k = _key(r["member"])
            params[k][col] += 1
            pex[k].add(r["name"])
        elif kind == "field":
            k = _key(r["member"])
            fields[k][col] += 1
            fex[k].add(r["name"])
        elif kind == "value":
            tok = r.get("token") or r["member"]
            tokens[tok][col] += 1
            tex[tok].add(r["owner"])
        if kind == "class" and s in BINDINGS:
            presence[r["name"]].add(s)

    out = [
        "# Vocabulary census",
        "",
        (
            "Counts are records per surface; `cli` sums both front-ends. Use it to spot one concept "
            "under two names. Generated by `tools/api/census.py`."
        ),
        "",
        "## Leading verb of every function and method",
        "",
        "| verb | wasm | node | python | examples |",
        "|---|---|---|---|---|",
    ]
    for v, c in sorted(verbs.items(), key=lambda x: (-sum(x[1].values()), x[0])):
        if sum(c.values()) < 2:
            continue
        ex = sorted(set().union(*verb_names[v].values()))[:EXAMPLES]
        out.append(f"| {v} | {c['wasm']} | {c['node']} | {c['python']} | {', '.join(ex)} |")
    out += ["", "## Synonym families", ""]
    for fam, vs in SYNONYMS.items():
        out += [f"### {fam}", ""]
        for v in vs:
            if v not in verb_names:
                continue
            js = sorted(verb_names[v]["wasm"] | verb_names[v]["node"])
            py = sorted(verb_names[v]["python"])
            out.append(
                f"- **{v}** ({len(js)} js / {len(py)} py): js: {', '.join(js)} — py: {', '.join(py)}"
            )
        out.append("")
    out += ["## C ABI tokens", ""]
    for fam, vs in SYNONYMS.items():
        hits = [f"{v}={c_tokens[v]}" for v in vs if c_tokens[v]]
        if hits:
            out.append(f"- {fam}: {', '.join(hits)}")
    top = sorted(c_trailing.items(), key=lambda x: (-x[1], x[0]))[:60]
    out += ["- trailing tokens (top 60): " + ", ".join(f"{k}={v}" for k, v in top), ""]
    out += [
        "## Lifecycle spellings per owner",
        "",
        "| owner | " + " | ".join(COLUMNS[:4]) + " |",
        "|---|---|---|---|---|",
    ]
    for owner in sorted(life):
        out.append(
            f"| {owner} | "
            + " | ".join(", ".join(sorted(life[owner][s])) for s in COLUMNS[:4])
            + " |"
        )
    bare = sorted(k for k in classes if k not in life)
    out += ["", f"Classes with no lifecycle-looking member ({len(bare)}): {', '.join(bare)}", ""]
    out += _counted(
        "Every parameter and flag name (snake_case); neighbours are synonym candidates", params, pex
    )
    out += _counted("Every field name (snake_case) used at least twice", fields, fex, minimum=2)
    out += _counted("Every value token", tokens, tex)
    out += ["## Unit suffixes on parameter and field names", ""]
    names = set(params) | set(fields)
    for sfx in UNIT_SUFFIXES:
        xs = sorted(k for k in names if k.endswith("_" + sfx))
        out.append(f"- `_{sfx}` ({len(xs)}): {', '.join(xs)}")
    unitless = sorted(k for k in names if UNITLESS.search(k))
    out += [
        f"- unit-less names that denote a physical quantity ({len(unitless)}): {', '.join(unitless)}",
        "",
    ]
    out += ["## Functions and classes on some binding surfaces only", ""]
    groups: dict = collections.defaultdict(list)
    for name, ss in presence.items():
        if len(ss) < len(BINDINGS):
            groups["+".join(s for s in BINDINGS if s in ss)].append(name)
    for g, names_ in sorted(groups.items(), key=lambda x: (-len(x[1]), x[0])):
        out.append(f"- only on {g} ({len(names_)}): {', '.join(sorted(names_))}")
    return "\n".join(out).rstrip("\n") + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--inventory-file", type=Path, help="read a saved inventory.py --json dump")
    ap.add_argument("--root", type=Path, default=HERE.parents[1])
    args = ap.parse_args(argv)
    if args.inventory_file:
        inv = json.loads(args.inventory_file.read_text(encoding="utf-8"))
    else:
        import inventory

        inv = inventory.build(args.root)
    sys.stdout.write(render(inv["records"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
