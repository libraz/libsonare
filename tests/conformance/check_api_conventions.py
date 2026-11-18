"""Hold the published surfaces to the interface conventions a reviewer used to check by eye.

Each rule is a predicate over the per-surface inventory (``tools/api/inventory.py``),
so a symbol enters the population the moment it is exported. A rule reports
three numbers: the records it read, the violations it found, and the exemptions
that excused one. Reading zero records fails, because a predicate that stopped
matching anything looks exactly like a clean surface.

Two tables sit beside each rule, both keyed by ``surface:name``:

* ``exempt`` -- a symbol the rule's own text places outside it, with the reason.
* ``pending`` -- a known violation with the owner who fixes it.

An entry in either table that matches no violation fails, so each one expires with
the divergence it describes.

Run ``python3 tests/conformance/check_api_conventions.py [--rule NAME]``.
"""

from __future__ import annotations

import argparse
import dataclasses
import sys
from collections.abc import Callable, Iterable
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@dataclasses.dataclass(frozen=True)
class Finding:
    key: str
    detail: str


@dataclasses.dataclass(frozen=True)
class Rule:
    name: str
    summary: str
    population: Callable[[dict], bool]
    violation: Callable[[dict], str | None]
    exempt: dict[str, str] = dataclasses.field(default_factory=dict)
    pending: dict[str, str] = dataclasses.field(default_factory=dict)


@dataclasses.dataclass
class Verdict:
    rule: str
    read: int
    violations: list[Finding]
    excused: list[str]
    deferred: list[str]
    stale: list[str]

    @property
    def failed(self) -> bool:
        return self.read == 0 or bool(self.violations) or bool(self.stale)


def record_key(record: dict) -> str:
    return f"{record['surface']}:{record['name']}"


def _js(record: dict) -> bool:
    return record["surface"] in ("node", "wasm")


def _argument_less(record: dict) -> bool:
    sigs = record.get("sigs") or []
    return bool(sigs) and all(not sig.get("params") for sig in sigs)


def _count_method_is_a_getter(record: dict) -> str | None:
    if record.get("ts_kind") == "method" and _argument_less(record):
        return "an argument-less count is a readonly getter, not a method"
    return None


RULES: list[Rule] = [
    Rule(
        name="getters",
        summary="argument-less *Count members of a Node/WASM class are getters",
        population=lambda r: (
            _js(r)
            and r["kind"] == "method"
            and not r.get("static")
            and r.get("member", "").endswith("Count")
        ),
        violation=_count_method_is_a_getter,
    ),
]


def judge(rule: Rule, records: Iterable[dict]) -> Verdict:
    read = 0
    violations: list[Finding] = []
    excused: list[str] = []
    deferred: list[str] = []
    for record in records:
        if not rule.population(record):
            continue
        read += 1
        detail = rule.violation(record)
        if detail is None:
            continue
        key = record_key(record)
        if key in rule.exempt:
            excused.append(key)
        elif key in rule.pending:
            deferred.append(key)
        else:
            violations.append(Finding(key, detail))
    hit = set(excused) | set(deferred)
    stale = sorted(k for k in (*rule.exempt, *rule.pending) if k not in hit)
    return Verdict(rule.name, read, violations, sorted(excused), sorted(deferred), stale)


def load_records(root: Path = ROOT) -> list[dict]:
    sys.path.insert(0, str(root / "tools" / "api"))
    import inventory

    inv = inventory.build(root)
    failures = inventory.scan_failures(inv)
    if failures:
        raise SystemExit("inventory scan failed:\n  " + "\n  ".join(failures))
    return inv["records"]


def report(verdicts: list[Verdict]) -> int:
    status = 0
    for v in verdicts:
        print(
            f"{v.rule}: read={v.read} violations={len(v.violations)} "
            f"exempt={len(v.excused)} pending={len(v.deferred)} stale={len(v.stale)}"
        )
        if v.read == 0:
            print("  read nothing: the predicate no longer matches the surface")
        for finding in v.violations:
            print(f"  {finding.key}: {finding.detail}")
        for key in v.deferred:
            print(f"  pending {key}")
        for key in v.stale:
            print(f"  stale table entry {key}: no longer a violation, delete it")
        status |= v.failed
    return 1 if status else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rule", action="append", help="run only this rule (repeatable)")
    args = parser.parse_args(argv)
    rules = [r for r in RULES if not args.rule or r.name in args.rule]
    unknown = set(args.rule or ()) - {r.name for r in RULES}
    if unknown:
        parser.error(f"unknown rule(s): {', '.join(sorted(unknown))}")
    records = load_records()
    return report([judge(rule, records) for rule in rules])


if __name__ == "__main__":
    raise SystemExit(main())
