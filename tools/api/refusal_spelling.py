"""Ratchet: does every refusal a Node or WASM caller can provoke name fields the way the caller spelled them?

Input is a capture: one JSON line per error that crossed the facade's public
surface while the surface's whole vitest suite ran (``make refusal-spelling``).
A snake_case token in a refusal is a C spelling the camelCase caller never
wrote, unless the caller wrote it (an object key or a string value it passed)
or ``refusal_spelling_ratchet.json`` excuses it as a non-field token. The count
of refusals carrying such a token may only fall, and reading no refusal fails.

Usage: python3 tools/api/refusal_spelling.py --surface node CAPTURE.jsonl [--write] [--report]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RATCHET = ROOT / "tests/conformance/refusal_spelling_ratchet.json"

SNAKE = re.compile(r"(?<![\w.\"'/-])([a-z][a-z0-9]*(?:_[a-z0-9]+)+)(?![\w\"'(/-])")
QUOTED = re.compile(r"\"[^\"]*\"|'[^']*'|`[^`]*`")
ENTRY_PREFIX = re.compile(r"^[A-Za-z][\w.]*: ")


def leaked_tokens(row: dict, excused: set[str]) -> list[str]:
    """The snake_case tokens of a refusal the caller did not write and nothing excuses."""
    body = ENTRY_PREFIX.sub("", row["message"], count=1)
    bare = QUOTED.sub(" ", body)
    keys = set(row.get("keys", []))
    strings = row.get("strings", [])
    out = []
    for match in SNAKE.finditer(bare):
        token = match.group(1)
        if token in keys or token in excused or any(token in s for s in strings):
            continue
        out.append(token)
    return out


def measure(rows: list[dict], excused: set[str]) -> dict:
    distinct: dict[tuple[str, str], dict] = {}
    for row in rows:
        distinct.setdefault((row["entry"], row["message"]), row)
    leaks = []
    tokens: Counter[str] = Counter()
    for (entry, message), row in sorted(distinct.items()):
        found = leaked_tokens(row, excused)
        if found:
            leaks.append({"entry": entry, "message": message, "tokens": found, "file": row["file"]})
            tokens.update(set(found))
    return {
        "refusals_read": len(distinct),
        "entries_read": len({entry for entry, _ in distinct}),
        "leaks": leaks,
        "tokens": tokens,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--surface", choices=("node", "wasm"), required=True)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--write", action="store_true", help="lower the ratchet to today's count")
    parser.add_argument("--report", action="store_true", help="list every refusal that leaks")
    args = parser.parse_args(argv)

    ratchet = json.loads(RATCHET.read_text(encoding="utf-8"))
    excused = set(ratchet["excused_tokens"])  # token -> why it is not a field
    rows = [json.loads(line) for line in args.capture.read_text().splitlines() if line.strip()]
    result = measure(rows, excused)
    count = len(result["leaks"])
    allowed = ratchet["surfaces"][args.surface]["leaking_refusals"]
    print(
        f"refusal spelling ({args.surface}): {result['refusals_read']} distinct refusals read across "
        f"{result['entries_read']} entries; {count} name a field in C spelling "
        f"({len(result['tokens'])} distinct tokens); ratchet {allowed}"
    )
    if args.report:
        for leak in result["leaks"]:
            print(f"  {leak['entry']}: {leak['tokens']} <- {leak['message'][:160]} ({leak['file']})")
    if result["refusals_read"] == 0:
        print("refusal spelling: the capture holds no refusal; the suite fed nothing", file=sys.stderr)
        return 1
    if args.write:
        ratchet["surfaces"][args.surface]["leaking_refusals"] = count
        RATCHET.write_text(json.dumps(ratchet, indent=2) + "\n", encoding="utf-8")
        return 0
    if count > allowed:
        print(f"refusal spelling: {count} > {allowed}; a new refusal names a C field", file=sys.stderr)
        return 1
    if count < allowed:
        print(f"refusal spelling: {count} < {allowed}; lower the ratchet with --write", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
