"""Hold the published surfaces to the interface conventions a reviewer used to check by eye.

Each rule is a predicate over the per-surface inventory (``tools/api/inventory.py``),
so a symbol enters the population the moment it is exported. A rule reports
three numbers: the records it read, the violations it found, and the exemptions
that excused one. Reading zero records fails, because a predicate that stopped
matching anything looks exactly like a clean surface.

Two tables sit beside each rule, both keyed by ``surface:name``:

* ``exempt`` -- a symbol the rule's own text places outside it, with the reason.
* ``pending`` -- a known violation with the fix it awaits.

An entry in either table that matches no violation fails, so each one expires with
the divergence it describes.

Run ``python3 tests/conformance/check_api_conventions.py [--rule NAME]``.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import sys
from collections.abc import Callable, Iterable
from pathlib import Path

ROOT = Path(os.environ.get("LIBSONARE_ROOT") or Path(__file__).resolve().parents[2])


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
    # Builds synthetic records from source text for a rule the inventory cannot see.
    scan: Callable[[Path], list[dict]] | None = None
    # Builds synthetic records from the inventory for a rule that compares records.
    derive: Callable[[list[dict]], list[dict]] | None = None


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


def _members(records: Iterable[dict], kind: str = "method") -> dict[tuple[str, str, str], dict]:
    """Index member records by (surface, owner, member)."""
    return {
        (r["surface"], r["owner"], r.get("member", "")): r for r in records if r["kind"] == kind
    }


def _returns(record: dict | None) -> str | None:
    sigs = (record or {}).get("sigs") or []
    return sigs[0].get("returns") if sigs else None


_WORKER_TYPE = re.compile(r"Worker|AudioWorkletNode")


def _worker_backed(cls: dict) -> bool:
    """A class whose constructor takes or creates a Worker, or an AudioWorkletNode."""
    if cls["name"].endswith("Processor"):
        return False
    if "Worker" in cls["name"]:
        return True
    return any(
        _WORKER_TYPE.search(p.get("type", ""))
        for sig in cls.get("sigs") or []
        for p in sig.get("params", [])
    )


def _worker_teardown_records(records: list[dict]) -> list[dict]:
    methods = _members(records)
    out: list[dict] = []
    for r in records:
        if not _js(r):
            continue
        if r["kind"] == "class" and _worker_backed(r):
            at = (r["surface"], r["name"])
            out.append(
                {
                    "surface": r["surface"],
                    "kind": "worker-class",
                    "name": r["name"],
                    "destroy": methods.get((*at, "destroy")),
                    "async_dispose": (*at, "[Symbol.asyncDispose]") in methods,
                }
            )
        elif r["kind"] in ("field", "option") and r.get("member", "").startswith(
            "terminateWorkerOn"
        ):
            out.append(r)
    return out


def _worker_teardown_violation(record: dict) -> str | None:
    if record["kind"] != "worker-class":
        if record["member"] != "terminateWorkerOnDestroy":
            return "the option that ends the worker is named terminateWorkerOnDestroy"
        return None
    if record["destroy"] is None:
        return "a worker-backed class has no destroy()"
    if _returns(record["destroy"]) != "Promise<void>":
        return "destroy() of a worker-backed class returns a Promise"
    if not record["async_dispose"]:
        return "a worker-backed class implements [Symbol.asyncDispose]()"
    return None


_NUMERIC_TYPE = re.compile(
    r"^(number|float|int|Float32Array|np\.ndarray|tuple\[float|readonly number)"
)

# (stem, unit token) pairs: a measurement named by its stem ends in the unit's spelling.
_LOUDNESS_UNITS = (
    (re.compile(r"loudness_?range", re.I), "Lu"),
    (re.compile(r"true_?peak(?!_?oversample)", re.I), "Dbtp"),
    (re.compile(r"integrated|momentary|short_?term", re.I), "Lufs"),
)


def _loudness_unit(member: str) -> str | None:
    for stem, unit in _LOUDNESS_UNITS:
        if stem.search(member):
            return unit
    return None


def _has_unit_token(record: dict, unit: str) -> bool:
    member = record["member"]
    if record["surface"] == "python":
        return re.search(rf"(^|_){unit.lower()}(_|$)", member) is not None
    return re.search(rf"{unit}(?![a-z])", member) is not None


def _loudness_name_violation(record: dict) -> str | None:
    unit = _loudness_unit(record["member"])
    if unit and not _has_unit_token(record, unit):
        return f"a loudness measurement name carries its unit: {unit}"
    return None


_NUMERIC_UNION = re.compile(r"(\s*-?\d+\s*\|)+\s*-?\d+(\s*\|\s*undefined)?")
_ENUM_BASES = ("IntEnum", "IntFlag", "Enum", "Flag")
_COUNT_NOT_A_SET = "a channel count that admits one or two, not a closed set of choices"


def _closed_set_population(r: dict) -> bool:
    if r["surface"] in ("node", "wasm"):
        return (
            r["kind"] in ("field", "option")
            and _NUMERIC_UNION.fullmatch(r.get("type", "")) is not None
        )
    return (
        r["surface"] == "python"
        and r["kind"] in ("class", "type")
        and any(b in _ENUM_BASES for b in r.get("bases") or [])
    )


def _closed_set_violation(r: dict) -> str | None:
    if r["surface"] == "python":
        return "a Python enum other than ErrorCode and PitchClass is a Literal of string tokens"
    return "a closed set is a string union, not a numeric union"


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
    Rule(
        name="async-teardown",
        summary="worker-backed JS classes destroy() asynchronously and implement asyncDispose",
        population=lambda r: r["kind"] == "worker-class" or r["kind"] in ("field", "option"),
        violation=_worker_teardown_violation,
        derive=_worker_teardown_records,
        pending={
            "wasm:VocalEditWorkerDraft": "give the worker draft an async destroy() and [Symbol.asyncDispose]()",
            "wasm:OpfsClipPageProviderOptions.terminateWorkerOnClose": "rename to terminateWorkerOnDestroy",
            "wasm:OpfsClipStreamOptions.terminateWorkerOnClose": "rename to terminateWorkerOnDestroy",
        },
    ),
    Rule(
        name="loudness-names",
        summary="loudness range, true peak and integrated/momentary/short-term names end in Lu/Dbtp/Lufs",
        population=lambda r: (
            r["surface"] in ("node", "wasm", "python")
            and r["kind"] == "field"
            and "." not in r.get("member", "")
            and _NUMERIC_TYPE.match(r.get("type", "")) is not None
            and _loudness_unit(r.get("member", "")) is not None
        ),
        violation=_loudness_name_violation,
    ),
    Rule(
        name="string-closed-sets",
        summary="closed sets are string unions; only ErrorCode and PitchClass stay Python enums",
        population=_closed_set_population,
        violation=_closed_set_violation,
        exempt={
            "python:ErrorCode": "the rule keeps ErrorCode as an IntEnum",
            "python:PitchClass": "the rule keeps PitchClass as an IntEnum",
            **{
                key: _COUNT_NOT_A_SET
                for key in (
                    "node:RealtimeVoiceChangerOptions.numChannels",
                    "node:StreamingMasteringChainConfig.numChannels",
                    "node:RealtimeVoiceChanger.processInterleavedInto(numChannels)",
                    "node:RealtimeVoiceChanger.processInterleavedWithCarrierInto(numChannels)",
                    "wasm:RealtimeVoiceChangerInterleavedBuffer.numChannels",
                    "wasm:RealtimeVoiceChangerOptions.numChannels",
                    "wasm:SonareRealtimeVoiceChangerWorkletProcessorOptions.numChannels",
                    "wasm:StreamingMasteringChainConfig.numChannels",
                    "wasm:RealtimeVoiceChanger.createRealtimeInterleavedBuffer(numChannels)",
                    "wasm:RealtimeVoiceChanger.createRealtimePlanarBuffer(numChannels)",
                    "wasm:RealtimeVoiceChanger.processInterleavedInto(numChannels)",
                    "wasm:RealtimeVoiceChanger.processInterleavedWithCarrierInto(numChannels)",
                )
            },
        },
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
    parser.add_argument("--records-json", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    rules = [r for r in RULES if not args.rule or r.name in args.rule]
    unknown = set(args.rule or ()) - {r.name for r in RULES}
    if unknown:
        parser.error(f"unknown rule(s): {', '.join(sorted(unknown))}")
    cached: list[list[dict]] = []

    def inventory_records() -> list[dict]:
        if not cached:
            if args.records_json:
                cached.append(json.loads(Path(args.records_json).read_text())["records"])
            else:
                cached.append(load_records())
        return cached[0]

    def records_for(rule: Rule) -> list[dict]:
        if rule.scan:
            return rule.scan(ROOT)
        records = inventory_records()
        return rule.derive(records) if rule.derive else records

    return report([judge(rule, records_for(rule)) for rule in rules])


if __name__ == "__main__":
    raise SystemExit(main())
