"""Hold the JS facades to entry-named refusals: a ratchet on the two old shapes.

A Node or WASM facade refusal is raised bare through the helpers in
``internal/entry.ts`` (``refuse`` / ``refuseType`` / ``invalidState`` /
``codedError``) and named by the entry the consumer called (``@entryClass``,
``withEntry`` or the native wrapper). Two spellings predate that and are counted
here per surface: a raw ``new RangeError(`` / ``new TypeError(`` /
``new SonareError(`` outside ``entry.ts``, and a helper parameter that carries
the caller's name as text (``fnName`` and its siblings).

The counts in ``facade_refusal_ratchet.json`` are a line, not a target: a count
above it fails, and a count below it fails too until the file is rewritten with
``--write``, so a migration lowers the line in the same commit and it cannot
drift back up. Comments are stripped before matching; a scan reading no file
fails, so a moved tree cannot pass as clean.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path(__file__).resolve().parent / "facade_refusal_ratchet.json"
SURFACES = {"node": "bindings/node/src", "wasm": "bindings/wasm/src"}
# The helpers' own home, and a generated table.
EXEMPT = {"internal/entry.ts", "internal/native_entries.ts"}
FN_PARAM_NAMES = ("fnName", "fn", "fnLabel", "entryName", "caller")

RAW_ERROR = re.compile(r"\bnew\s+(?:RangeError|TypeError|SonareError)\s*\(")
FN_PARAM = re.compile(
    r"[(,]\s*(?:" + "|".join(FN_PARAM_NAMES) + r")\s*\??\s*:\s*(?:string|RequestFn)\b"
)
KINDS = {"raw-error": RAW_ERROR, "fn-name-param": FN_PARAM}


def strip_comments(text: str) -> str:
    """``text`` with comments and quoted-string contents blanked.

    Template literals are kept whole, since their ``${}`` holes are code.
    """
    out: list[str] = []
    i, n = 0, len(text)
    quote = ""
    while i < n:
        c = text[i]
        if quote:
            keep = quote == "`" or c == quote
            if c == "\\" and i + 1 < n:
                if quote == "`":
                    out.append(text[i : i + 2])
                i += 2
                continue
            if keep:
                out.append(c)
            if c == quote:
                quote = ""
            i += 1
        elif c in "'\"`":
            quote = c
            out.append(c)
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            stop = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, stop))
            i = stop
        else:
            out.append(c)
            i += 1
    return "".join(out)


def count_text(text: str) -> Counter:
    """Occurrences of each kind in one source text, comments excluded."""
    code = strip_comments(text)
    return Counter({kind: len(pattern.findall(code)) for kind, pattern in KINDS.items()})


def scan(root: Path) -> dict[str, dict]:
    """Per surface: files scanned, total per kind, and per-file counts."""
    result: dict[str, dict] = {}
    for surface, rel in SURFACES.items():
        base = root / rel
        totals: Counter = Counter({kind: 0 for kind in KINDS})
        per_file: dict[str, Counter] = {}
        scanned = 0
        for path in sorted(base.rglob("*.ts")):
            name = path.relative_to(base).as_posix()
            if path.name.endswith(".d.ts") or name in EXEMPT:
                continue
            scanned += 1
            counts = count_text(path.read_text(encoding="utf-8"))
            totals.update(counts)
            if any(counts.values()):
                per_file[name] = counts
        result[surface] = {"scanned": scanned, "totals": dict(totals), "files": per_file}
    return result


def evaluate(found: dict[str, dict], baseline: dict[str, dict]) -> list[str]:
    """Every way ``found`` departs from the recorded line."""
    errors: list[str] = []
    for surface, data in found.items():
        if data["scanned"] == 0:
            errors.append(f"{surface}: scanned no facade source under {SURFACES[surface]}")
            continue
        recorded = baseline.get(surface, {})
        for kind, count in data["totals"].items():
            line = recorded.get(kind)
            if line is None:
                errors.append(f"{surface} {kind}: no recorded line; run with --write")
            elif count > line:
                worst = sorted(
                    ((c[kind], f) for f, c in data["files"].items() if c[kind]), reverse=True
                )[:8]
                where = ", ".join(f"{f} {c}" for c, f in worst)
                errors.append(
                    f"{surface} {kind}: {count} exceeds the recorded {line}; raise the refusal "
                    f"through refuse/refuseType/invalidState/codedError and name it with the "
                    f"entry, not a passed-in name ({where})"
                )
            elif count < line:
                errors.append(
                    f"{surface} {kind}: {count} is below the recorded {line}; lower the line "
                    f"with --write in the same change"
                )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true", help="record the current counts")
    args = parser.parse_args(argv)
    found = scan(ROOT)
    for surface, data in found.items():
        totals = " ".join(f"{k}={v}" for k, v in data["totals"].items())
        print(f"facade refusal ratchet: {surface} scanned={data['scanned']} {totals}")
    if args.write:
        if any(data["scanned"] == 0 for data in found.values()):
            print("refusing to record a scan that read no file", file=sys.stderr)
            return 1
        BASELINE.write_text(
            json.dumps({s: d["totals"] for s, d in found.items()}, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"wrote {BASELINE.relative_to(ROOT)}")
        return 0
    baseline = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else {}
    errors = evaluate(found, baseline)
    for error in errors:
        print(f"  FAIL {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
