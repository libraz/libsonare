"""Check that every public one-shot result has a corpus case or a reasoned skip."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/api"))
sys.path.insert(0, str(ROOT / "tools/parity"))

import inventory
from allowlist import load as load_allowlist
from compare import _ALIAS_COVERAGE, _CORE_COVERAGE
from extractors import c_api
from normalize import canonical_key


def c_entries(c: c_api.Extraction) -> set[str]:
    records = {row.raw_name for row in c.records}
    opaque = {
        match["name"]
        for header in (ROOT / "include/sonare").glob("*.h")
        for match in inventory._C_OPAQUE.finditer(header.read_text(encoding="utf-8"))
    }
    entries = set()
    for fn in c.functions:
        params = [(p.raw_name, p.type.replace("const", "").strip(" *")) for p in fn.params]
        if any(typ in opaque for _, typ in params):
            continue
        if fn.returns in records or any(
            c_api._OUT_NAME_RE.search(p.raw_name)
            and not p.type.startswith("const ")
            and "*" in p.type
            and (typ in records or (p.type == "char**" and "json" in name))
            for p, (name, typ) in zip(fn.params, params, strict=True)
        ):
            entries.add(fn.raw_name)
    return entries


def returns_record(annotation: str, records: set[str]) -> bool:
    # Unwrap the asynchronous form without treating array/tuple elements as
    # whole result records. Inline records and dictionary results count too.
    for wrapper in ("Promise", "Readonly"):
        if annotation.startswith(f"{wrapper}<") and annotation.endswith(">"):
            annotation = annotation[len(wrapper) + 1 : -1]
    if annotation.startswith(("{", "Record<", "dict[")):
        return True
    for part in annotation.split(" | "):
        match = re.fullmatch(r"([\w.]+)(?:<.*>|\[.*\])?", part)
        if match and match[1].split(".")[-1] in records:
            return True
    return False


def facade_entries(surface: str) -> set[str]:
    if surface == "python":
        rows, _, _ = inventory.extract_python(ROOT)
        data_types = {row["owner"] for row in rows if row["kind"] == "field"}
        return {
            row["name"]
            for row in rows
            if row["kind"] == "function"
            and any(returns_record(sig["returns"], data_types) for sig in row["sigs"])
        }

    entries = inventory.NODE_ENTRIES if surface == "node" else inventory.wasm_entries(ROOT).values()
    rows = inventory._run_ts(ROOT, f"bindings/{surface}", sorted(set(entries)))
    data_types = {
        row["name"]
        for row in rows
        if row["kind"] in ("interface", "type")
        and any(member["kind"] == "property" for member in row["members"])
    }
    return {
        row["name"]
        for row in rows
        if row["kind"] in ("function", "const")
        and any(returns_record(sig["returns"], data_types) for sig in row["sigs"])
    }


def check(surface: str, candidates: set[str], covered: set[str], skips: dict[str, str]) -> bool:
    if not candidates:
        print(f"{surface}: public result entry extraction is empty", file=sys.stderr)
        return False
    missing = candidates - covered - skips.keys()
    stale = skips.keys() - (candidates - covered)
    empty = {name for name, reason in skips.items() if not reason.strip() or "\n" in reason}
    for names, label in (
        (missing, "has no result corpus case or skip"),
        (stale, "has a stale result corpus skip"),
        (empty, "needs a one-line result corpus skip reason"),
    ):
        for name in sorted(names):
            print(f"{surface}:{name} {label}", file=sys.stderr)
    return not (missing or stale or empty)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("surface", choices=("c", "node", "wasm", "python"))
    surface = parser.parse_args().surface
    corpus = json.loads((ROOT / "tests/conformance/public_result_corpus.json").read_text())
    section = corpus["one_shot_results"]
    c = c_api.extract(ROOT)
    c_result_entries = c_entries(c)
    c_names = {fn.raw_name for fn in c.functions}
    surface_only = load_allowlist(ROOT / "tools/parity/allowlist.toml").surface_only
    ok = True
    for name in dict.fromkeys(("c", surface)):
        raw = c_result_entries if name == "c" else facade_entries(name)
        candidates = {canonical_key(entry, name) for entry in raw}
        facade_only = set(surface_only.get("any", ())) | set(surface_only.get(name, ()))
        keys = set()
        for case in section["cases"]:
            if name not in case.get("surfaces", (name,)):
                continue
            key = canonical_key(case["entry"], "c")
            if alias := case.get("apis", {}).get(name):
                actual = canonical_key(alias, name)
                core_alias = _CORE_COVERAGE.get(key)
                record_name = key.removesuffix("_json")
                record_alias = actual == record_name
                async_alias = name == "node" and actual == f"{record_name}_async"
                # A facade-only aggregate can use a C handle operation or component
                # reader as its reference. Its test assembles the public result.
                composed_reference = (
                    actual in candidates
                    and actual in facade_only
                    and case["entry"] in c_names
                    and case["entry"] not in c_result_entries
                    and "c" in case.get("surfaces", ("c",))
                )
                if (
                    not record_alias
                    and not composed_reference
                    and not async_alias
                    and actual not in _ALIAS_COVERAGE.get(key, ())
                    and (core_alias is None or actual != core_alias[0])
                ):
                    print(
                        f"{name}:{alias} is not a known alias of {case['entry']}", file=sys.stderr
                    )
                    ok = False
                    continue
                key = actual
            keys.add(key)
        skips = {
            entry: row["reason"]
            for entry, row in section["skips"].items()
            if name in row["surfaces"]
        }
        ok = check(name, candidates, keys, skips) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
