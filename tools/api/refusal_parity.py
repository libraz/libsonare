"""Check that Node and WASM refuse the same mistake with the same class and text.

Input is the two refusal captures ``make refusal-spelling-check`` writes, one JSON line per error
that crossed a facade while that surface's suite ran. A refusal is keyed by its entry, the subject
its message names, and the refused value read back from the call's arguments at that subject (the
whole argument list when the subject does not resolve), so a pair joins only when both surfaces
were handed the same input. Where that value does not decide the refusal on one surface (two texts
under one key), those rows are keyed again by the whole argument list. A key that still holds two
texts on one surface, which the other surface does not match, is decided by the receiver's state
rather than the call (a bound read off the object), so it counts as undecided and is not compared.
A joined pair whose class or text differs fails, unless
``tests/conformance/refusal_parity_parked.json`` parks it; a parked entry that excuses nothing
fails too, so an entry expires with the divergence it names.

Usage: python3 tools/api/refusal_parity.py NODE.jsonl WASM.jsonl [--report]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PARKED = ROOT / "tests/conformance/refusal_parity_parked.json"

ENTRY_PREFIX = re.compile(r"^([A-Za-z][\w.]*): ")
SUBJECT = re.compile(r"^([A-Za-z_][\w]*(?:\[\d+\])?(?:\.[A-Za-z_][\w]*(?:\[\d+\])?)*)\s")
STEP = re.compile(r"([A-Za-z_]\w*)|\[(\d+)\]")
MISSING = object()


def subject_of(message: str) -> str | None:
    """The subject a refusal names: the path that opens its body, after the entry prefix."""
    body = ENTRY_PREFIX.sub("", message, count=1)
    match = SUBJECT.match(body)
    return match.group(1) if match else None


def resolve(value: object, steps: list[tuple[str | None, str | None]]) -> object:
    for key, index in steps:
        if key is not None:
            if not isinstance(value, dict) or key not in value:
                return MISSING
            value = value[key]
        else:
            if not isinstance(value, list) or int(index) >= len(value):
                return MISSING
            value = value[int(index)]
    return value


def find(value: object, steps: list[tuple[str | None, str | None]], depth: int = 0) -> object:
    """The value at ``steps`` under ``value`` or under the first object below it that has the path."""
    found = resolve(value, steps)
    if found is not MISSING or depth > 3:
        return found
    children = (
        value.values() if isinstance(value, dict) else value if isinstance(value, list) else ()
    )
    for child in children:
        found = find(child, steps, depth + 1)
        if found is not MISSING:
            return found
    return MISSING


def refused_value(row: dict, subject: str | None) -> object:
    args = row.get("args")
    if args is None:
        return MISSING
    if subject is not None:
        steps = [(m.group(1), m.group(2)) for m in STEP.finditer(subject)]
        found = find(args, steps)
        if found is not MISSING:
            return {"at": subject, "value": found}
    return {"args": args}


def key_of(row: dict, whole: bool = False) -> tuple[str, str, str] | None:
    subject = subject_of(row["message"])
    value = refused_value(row, None if whole else subject)
    if value is MISSING:
        return None
    return (row["entry"], subject or "", json.dumps(value, sort_keys=True))


def load(path: Path) -> list[dict]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def outcome(row: dict) -> tuple[str, str]:
    return (row.get("errorClass") or "", row["message"])


def ambiguous(rows: list[dict]) -> set[tuple]:
    """Keys under which one surface recorded more than one refusal: the value alone does not decide."""
    seen: dict[tuple, set[tuple[str, str]]] = {}
    for row in rows:
        key = key_of(row)
        if key is not None:
            seen.setdefault(key, set()).add(outcome(row))
    return {key for key, outcomes in seen.items() if len(outcomes) > 1}


def index(rows: list[dict], refine: set[tuple]) -> tuple[dict[tuple, set[tuple[str, str]]], int]:
    out: dict[tuple, set[tuple[str, str]]] = {}
    unkeyed = 0
    for row in rows:
        key = key_of(row)
        if key is None:
            unkeyed += 1
            continue
        if key in refine:
            key = key_of(row, whole=True)
        out.setdefault(key, set()).add(outcome(row))
    return out, unkeyed


def compare(node_rows: list[dict], wasm_rows: list[dict], parked: list[dict]) -> dict:
    refine = ambiguous(node_rows) | ambiguous(wasm_rows)
    node, node_unkeyed = index(node_rows, refine)
    wasm, wasm_unkeyed = index(wasm_rows, refine)
    shared = set(node) & set(wasm)
    undecided = {
        key
        for key in shared
        if node[key] != wasm[key] and (len(node[key]) > 1 or len(wasm[key]) > 1)
    }
    joined = sorted(shared - undecided)
    used = [False] * len(parked)
    differing = []
    excused = []
    for key in joined:
        if node[key] == wasm[key]:
            continue
        pair = {
            "entry": key[0],
            "subject": key[1],
            "input": json.loads(key[2]),
            "node": sorted(node[key]),
            "wasm": sorted(wasm[key]),
        }
        hit = next(
            (i for i, p in enumerate(parked) if p["entry"] == key[0] and p["subject"] == key[1]),
            None,
        )
        if hit is None:
            differing.append(pair)
        else:
            used[hit] = True
            excused.append(pair)
    return {
        "compared": len(joined),
        "node_only": len(set(node) - set(wasm)),
        "wasm_only": len(set(wasm) - set(node)),
        "unkeyed": {"node": node_unkeyed, "wasm": wasm_unkeyed},
        "refined": len(refine),
        "undecided": len(undecided),
        "differing": differing,
        "excused": excused,
        "stale_parked": [p for p, u in zip(parked, used) if not u],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("node", type=Path)
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--report", action="store_true", help="print every differing pair")
    args = parser.parse_args(argv)
    parked = json.loads(PARKED.read_text(encoding="utf-8"))["parked"]
    result = compare(load(args.node), load(args.wasm), parked)
    print(
        f"refusal parity: compared={result['compared']} node_only={result['node_only']}"
        f" wasm_only={result['wasm_only']} unkeyed_node={result['unkeyed']['node']}"
        f" unkeyed_wasm={result['unkeyed']['wasm']} refined={result['refined']}"
        f" undecided={result['undecided']}"
        f" parked={len(result['excused'])}"
        f" differing={len(result['differing'])}"
    )
    if result["compared"] == 0:
        print("refusal parity: no pair joined; a capture without arguments compares nothing")
        return 1
    for pair in result["differing"] if args.report or result["differing"] else []:
        print(json.dumps(pair, sort_keys=True))
    for entry in result["stale_parked"]:
        print(f"stale parked entry (no divergence left to excuse): {json.dumps(entry)}")
    return 1 if result["differing"] or result["stale_parked"] else 0


if __name__ == "__main__":
    sys.exit(main())
