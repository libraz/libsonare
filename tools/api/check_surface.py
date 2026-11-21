"""Check the public interface against the migration ledgers and the naming conventions.

Three parts, all over the per-surface inventory (tools/api/inventory.py):

- ledger validation: every [[entry]] in tools/api/migration/*.toml is well formed;
- ledger check: a removed or renamed name is absent, a new name is present;
- convention checks: one check per convention slug (``slug`` or ``slug:aspect``),
  each reporting how many candidates it examined and which of them violate it.

Each name takes its domain from the generated tools/api/domains.json
(``--regenerate-domains`` rewrites it from the tree); a ledger's new name takes its
old name's, and a ledger domain disagreeing with the file is a validation problem.

A check that examined zero candidates fails in an unfiltered run, because an
extractor that stopped feeding it reads exactly like a clean tree. Violations may be excused by
tools/api/surface_allowlist.toml (categories listed at its head) or, for the Python surface of a
librosa-named function, by tools/api/librosa_mirrors.toml. A name ``unit-in-name`` would
flag whose quantity has no unit is listed, with its class and range, in
tools/api/dimensionless_names.toml. ``--audit-allowlist``
fails on any allowlist entry that excused nothing in a full run; ``--audit-only`` (which
implies it) makes that and the validation problems the only causes of a non-zero exit.

Exit 0 only when nothing failed.
"""

from __future__ import annotations

import argparse
import collections
import fnmatch
import importlib.util
import json
import re
import shlex
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import domains

# The facade-to-C name mapping is parity's; load it under its own name rather than putting
# tools/parity on the import path, where its module names would shadow nothing but could.
_spec = importlib.util.spec_from_file_location(
    "parity_normalize", HERE.parent / "parity" / "normalize.py"
)
parity_normalize = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(parity_normalize)

ROOT = HERE.parents[1]
SURFACES = ("wasm", "node", "python", "c", "cli-python", "cli-native")
LEDGER_SURFACES = ("c", "python", "cli", "wasm", "node")
KINDS = (
    "function",
    "method",
    "class",
    "type",
    "field",
    "option",
    "value",
    "export",
    "command",
    "flag",
)
WASM_ENTRIES = (".", "./analysis", "./worklet", "./worker", "./vocal-edit-worker")
RULE_SLUGS = (
    "sample-rate-required",
    "sample-rate-name",
    "teardown",
    "async-teardown",
    "seconds-suffix",
    "unit-in-name",
    "loudness-names",
    "absent-not-sentinel",
    "library-owned-defaults",
    "non-finite-is-absent",
    "input-always-scanned",
    "token-case",
    "digit-tokens",
    "string-closed-sets",
    "typed-results",
    "array-results",
    "matrix-2d",
    "no-internal-types",
    "frozen-results",
    "snake-case-only",
    "cancellation",
    "progress-callback",
    "librosa-defaults",
    "onset-strength",
    "tempo-range",
    "one-positional-form",
    "midi-module-functions",
    "options-struct",
    "c-create",
    "c-free",
    "c-count-types",
    "c-error-path",
    "error-class-by-cause",
    "entry-named-refusals",
    "getters",
    "collections",
    "one-entry-per-operation",
    "mastering-names",
    "construction",
    "planar-channels",
    "stereo-pair",
    "num-channels",
    "no-positional-bools",
    "strip-ref",
    "high-pass",
    "frequency-bounds",
    "pitch-semitones",
    "type-names",
    "input-types",
    "one-input-form",
    "aliases-removed",
    "abi-probes",
    "worklet-scope",
    "engine-vocabulary",
    "vocal-edit-model",
    "cli-positional-input",
    "cli-front-end-parity",
    "cpp-include",
    "doc-comments",
    "config-keys",
    "old-documents-refused",
    "python-public-names",
    "individual",
)
ALLOW_CATEGORIES = (
    "librosa-mirror",
    "platform",
    "wire-format",
    "hot-path",
    "standard-symbol",
    "scalar",
    "distinct-operation",
)
# A category that only one kind of finding can use.
_CATEGORY_CHECKS = {
    "standard-symbol": ("token-case",),
    "scalar": ("one-positional-form",),
    "distinct-operation": ("options-struct:suffix",),
}
EXAMPLE_SURFACES = ("wasm", "node", "python", "c", "cli")

DEFAULT_LEDGER_DIR = HERE / "migration"
# The release the ledgers migrate from: every row's old name existed there.
BASELINE_TAG = "v1.10.0"
# Today's extractors run once over the BASELINE_TAG tree, frozen so an extractor change
# cannot shrink the baseline along with the tip. Regenerate with --write-baseline.
DEFAULT_BASELINE = HERE / "migration" / f"baseline-{BASELINE_TAG}.tsv"
_BASELINE_COLUMNS = (
    "surface",
    "kind",
    "name",
    "entries",
    "owner",
    "member",
    "domain",
    "ctor_private",
)
DEFAULT_RULES = HERE / "migration" / "rules.toml"
DEFAULT_MIRRORS = HERE / "librosa_mirrors.toml"
DEFAULT_ALLOWLIST = HERE / "surface_allowlist.toml"
DEFAULT_DIMENSIONLESS = HERE / "dimensionless_names.toml"
DEFAULT_CATALOG = ROOT / "tools" / "capability-catalog.json"
DEFAULT_DOCUMENT_TYPES = HERE / "document_types.toml"
DIMENSIONLESS_CLASSES = ("ratio", "probability", "correlation", "normalized", "magnitude", "index")
DEFAULT_DOMAINS = HERE / "domains.json"


# ------------------------------------------------------------------ inputs


def load_toml(path: Path, problems: list[str], label: str) -> dict | None:
    if not path.exists():
        problems.append(f"{label}: file not found: {path}")
        return None
    try:
        return tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        problems.append(f"{label}: TOML parse error: {e}")
        return None


def load_rules(path: Path, problems: list[str]) -> list[dict]:
    data = load_toml(path, problems, "rules.toml")
    if data is None:
        return []
    rules = data.get("rule", [])
    slugs = [r.get("slug", "") for r in rules]
    for i, r in enumerate(rules, 1):
        slug = r.get("slug", "")
        if slug not in RULE_SLUGS:
            problems.append(f"rules.toml rule {i} ({slug}): slug outside the closed set")
        for key in ("title", "summary"):
            if not isinstance(r.get(key), str) or not r.get(key):
                problems.append(f"rules.toml rule {i} ({slug}): missing {key}")
        for ex in r.get("example", []):
            if ex.get("surface") not in EXAMPLE_SURFACES:
                problems.append(
                    f"rules.toml rule {i} ({slug}): example surface {ex.get('surface')!r} unknown"
                )
            if not isinstance(ex.get("before"), str) or not isinstance(ex.get("after"), str):
                problems.append(f"rules.toml rule {i} ({slug}): example needs before and after")
    for slug in sorted(set(RULE_SLUGS) - set(slugs)):
        problems.append(f"rules.toml: slug {slug} is not defined")
    for slug, n in sorted(collections.Counter(slugs).items()):
        if n > 1:
            problems.append(f"rules.toml: slug {slug} is defined {n} times")
    return rules


def _cli_tables(records: list[dict]) -> tuple[set[str], dict[str, set[str]]]:
    """Commands and long flags of both CLI front-ends, unioned: an example may use either."""
    cmds = {r["name"] for r in records if r["surface"] in CLI and r["kind"] == "command"}
    flags: dict[str, set[str]] = collections.defaultdict(set)
    for r in records:
        if r["surface"] in CLI and r["kind"] == "flag":
            flags[r["owner"]].add(r["member"])
    return cmds, flags


def _cli_short_aliases(contract: dict) -> dict[str, set[str]]:
    """Single-letter aliases per command, which only the contract's option table records."""
    table = contract.get("inventory", {}).get("expected_options", {})
    return {
        c: {a for o in opts for a in o.get("aliases", []) if len(a) == 1}
        for c, opts in table.items()
    }


def cli_example_problems(
    rules: list[dict], records: list[dict], base_records: list[dict] | None, contract: dict
) -> tuple[list[str], list[str]]:
    """Each CLI example names a command that exists and only flags that command takes.

    `after` is read against the current tree, `before` against the baseline release. A
    single-letter flag is checked against the contract's alias table, which lists only the
    commands whose options it records; the rest are reported as not compared.
    """
    problems: list[str] = []
    skipped: list[str] = []
    short = _cli_short_aliases(contract)
    sides = [("after", "the current CLI", _cli_tables(records), short)]
    if base_records is not None:
        sides.append(("before", BASELINE_TAG, _cli_tables(base_records), None))
    else:
        skipped.append(f"rules.toml CLI example before lines against {BASELINE_TAG} (no baseline)")
    for i, r in enumerate(rules, 1):
        slug = r.get("slug", "")
        for ex in r.get("example", []):
            if ex.get("surface") != "cli":
                continue
            for key, label, (cmds, flags), aliases in sides:
                for line in str(ex.get(key, "")).splitlines():
                    line = line.split(" #", 1)[0].strip()
                    if not line.startswith("sonare "):
                        continue
                    words = shlex.split(line)[1:]
                    name = words[0] if words else ""
                    if len(words) > 1 and f"{words[0]}.{words[1]}" in cmds:
                        name = f"{words[0]}.{words[1]}"
                    where = f"rules.toml rule {i} ({slug}): {key} example `{line}`"
                    if name not in cmds:
                        problems.append(f"{where}: {label} has no command {name!r}")
                        continue
                    for w in words:
                        if re.fullmatch(r"-\d[\d.]*", w) or not w.startswith("-"):
                            continue
                        flag = w.split("=", 1)[0]
                        if flag.startswith("--"):
                            if flag[2:] not in flags.get(name, set()):
                                problems.append(f"{where}: {label} {name} has no {flag}")
                        elif aliases is None or name not in aliases:
                            skipped.append(f"{where}: {flag} (no alias table for {name})")
                        elif flag[1:] not in aliases[name]:
                            problems.append(f"{where}: {label} {name} has no {flag}")
    return problems, skipped


MIRROR_EVIDENCE = ("in-tree", "recalled")


def load_mirrors(path: Path, problems: list[str]) -> dict[str, str]:
    """Map mirror name to evidence; a malformed entry is a validation problem."""
    data = load_toml(path, problems, "librosa_mirrors.toml")
    if data is None:
        return {}
    out = {}
    for k in sorted(set(data) - {"mirror"}):
        problems.append(f"librosa_mirrors.toml: unknown top-level key {k!r}")
    for i, m in enumerate(data.get("mirror", []), 1):
        name, evidence = m.get("name"), m.get("evidence")
        if not isinstance(name, str) or not name:
            problems.append(f"librosa_mirrors.toml mirror {i}: missing name")
            continue
        label = f"librosa_mirrors.toml mirror {i} ({name})"
        for k in sorted(set(m) - {"name", "evidence"}):
            problems.append(f"{label}: unknown key {k!r}")
        if evidence not in MIRROR_EVIDENCE:
            problems.append(f"{label}: evidence must be in-tree or recalled, not {evidence!r}")
        if name in out:
            problems.append(f"{label}: listed twice")
        out[name] = evidence if isinstance(evidence, str) else ""
    return out


def load_dimensionless(path: Path, problems: list[str], records: list[dict]) -> set[str]:
    """Surface-neutral names whose unit-less quantity is declared; a name matching no candidate is stale."""
    data = load_toml(path, problems, "dimensionless_names.toml")
    if data is None:
        return set()
    for k in sorted(set(data) - {"name"}):
        problems.append(f"dimensionless_names.toml: unknown top-level key {k!r}")
    flagged = {
        dimensionless_key(r["name"])
        for r in records
        if r["kind"] in ("option", "field", "flag")
        and (hit := _unit_hit(r)) is not None
        and hit[1] is None
    }
    out: set[str] = set()
    for i, n in enumerate(data.get("name", []), 1):
        names = n.get("names")
        label = f"dimensionless_names.toml name {i}"
        for k in sorted(set(n) - {"names", "class", "range", "evidence"}):
            problems.append(f"{label}: unknown key {k!r}")
        if not (isinstance(names, list) and names and all(isinstance(x, str) for x in names)):
            problems.append(f"{label}: names must be a non-empty list of strings")
            continue
        label += f" ({names[0]})"
        if n.get("class") not in DIMENSIONLESS_CLASSES:
            problems.append(f"{label}: class must be one of {', '.join(DIMENSIONLESS_CLASSES)}")
        rng = n.get("range")
        if not (
            isinstance(rng, list)
            and len(rng) == 2
            and all(isinstance(x, (int, float)) for x in rng)
            and rng[0] <= rng[1]
        ):
            problems.append(f"{label}: range must be [low, high]")
        if not isinstance(n.get("evidence"), str) or not n["evidence"].strip():
            problems.append(f"{label}: evidence is required")
        for name in names:
            key = dimensionless_key(name)
            if key in out:
                problems.append(f"{label}: {name} listed twice")
            elif key not in flagged:
                problems.append(f"{label}: {name} names nothing unit-in-name would flag")
            out.add(key)
    return out


_EVIDENCE_AT = re.compile(r"^([\w./-]+):(\d+): \S")


def load_document_types(
    path: Path, problems: list[str], records: list[dict], root: Path
) -> set[tuple[str, str]]:
    """(surface, type) pairs holding a core document's keys, closed over the types their fields name.

    A name resolves to a Python TypedDict and to a TS interface or alias on each JS surface that has
    one; the Python half must hold a camelCase key, since that is all it exempts there.
    """
    data = load_toml(path, problems, "document_types.toml")
    if data is None:
        return set()
    for k in sorted(set(data) - {"document"}):
        problems.append(f"document_types.toml: unknown top-level key {k!r}")
    types = {
        (r["surface"], r["name"])
        for r in records
        if r["kind"] == "type"
        and ((r["surface"] == "python" and "TypedDict" in r.get("bases", [])) or r["surface"] in JS)
    }
    fields = collections.defaultdict(list)
    for r in records:
        if r["kind"] == "field" and (r["surface"], r["owner"]) in types:
            fields[(r["surface"], r["owner"])].append(r)
    out: set[tuple[str, str]] = set()
    seen: set[str] = set()
    for i, d in enumerate(data.get("document", []), 1):
        name = d.get("type")
        label = f"document_types.toml document {i}"
        if not isinstance(name, str) or not name:
            problems.append(f"{label}: type is required")
            continue
        label += f" ({name})"
        for k in sorted(set(d) - {"type", "evidence"}):
            problems.append(f"{label}: unknown key {k!r}")
        ev = d.get("evidence")
        m = _EVIDENCE_AT.match(ev) if isinstance(ev, str) else None
        if not m:
            problems.append(
                f"{label}: evidence is required, as path:line: what the core does with it"
            )
        elif not (root / m.group(1)).is_file():
            problems.append(f"{label}: evidence names {m.group(1)}, which is not in the tree")
        if name in seen:
            problems.append(f"{label}: listed twice")
            continue
        seen.add(name)
        roots = [(s, n) for s, n in sorted(types) if n == name]
        if not roots:
            problems.append(f"{label}: {name} is not a Python TypedDict or a TS interface")
            continue
        for surface, _ in roots:
            closure, todo = set(), [(surface, name)]
            while todo:
                t = todo.pop()
                if t in closure:
                    continue
                closure.add(t)
                for f in fields[t]:
                    todo += [
                        (surface, w)
                        for w in re.findall(r"\w+", f.get("type", ""))
                        if (surface, w) in types
                    ]
            if surface == "python" and not any(
                re.search(r"[a-z][A-Z]", f["member"]) for t in closure for f in fields[t]
            ):
                problems.append(f"{label}: has no camelCase key, so there is nothing to declare")
            out |= closure
    return out


@dataclass
class Allow:
    index: int
    check: str
    surface: str
    name: str
    category: str
    reason: str
    used: int = 0


def load_allowlist(path: Path, problems: list[str], check_ids: set[str]) -> list[Allow]:
    data = load_toml(path, problems, "surface_allowlist.toml")
    if data is None:
        return []
    out = []
    for i, a in enumerate(data.get("allow", []), 1):
        entry = Allow(
            i,
            a.get("check", ""),
            a.get("surface", ""),
            a.get("name", ""),
            a.get("category", ""),
            a.get("reason", ""),
        )
        label = f"surface_allowlist.toml allow {i}"
        if entry.check not in check_ids:
            problems.append(f"{label}: unknown check {entry.check!r}")
        if entry.category not in ALLOW_CATEGORIES:
            problems.append(f"{label}: unknown category {entry.category!r}")
        elif entry.check not in _CATEGORY_CHECKS.get(entry.category, (entry.check,)):
            problems.append(f"{label}: category {entry.category} does not apply to {entry.check}")
        if entry.surface not in SURFACES + ("cli",):
            problems.append(f"{label}: unknown surface {entry.surface!r}")
        if not entry.name or not entry.reason:
            problems.append(f"{label}: name and reason are required")
        out.append(entry)
    return out


# ------------------------------------------------------------------ ledger


@dataclass
class LedgerEntry:
    surface: str
    index: int
    data: dict
    valid: bool = True

    @property
    def label(self) -> str:
        return f"{self.surface}.toml entry {self.index} ({self.data.get('old', '?')})"


_LEDGER_KEYS = {
    "kind",
    "old",
    "new",
    "removed",
    "changed",
    "to",
    "no_replacement",
    "test",
    "domain",
    "rule",
    "entry",
    "frontend",
}


def load_ledgers(ledger_dir: Path, problems: list[str]) -> list[LedgerEntry]:
    out = []
    for s in LEDGER_SURFACES:
        data = load_toml(ledger_dir / f"{s}.toml", problems, f"{s}.toml")
        if data is None:
            continue
        for i, e in enumerate(data.get("entry", []), 1):
            out.append(LedgerEntry(s, i, e))
    return out


def validate_ledger(entries: list[LedgerEntry], slugs: set[str], root: Path) -> list[str]:
    problems = []
    for le in entries:
        e = le.data
        bad = []
        for k in sorted(set(e) - _LEDGER_KEYS):
            bad.append(f"unknown key {k!r}")
        if not isinstance(e.get("old"), str) or not e.get("old"):
            bad.append("missing old")
        forms = [k for k in ("new", "removed", "changed") if k in e]
        if len(forms) != 1:
            bad.append(f"needs exactly one of new / removed / changed, has {len(forms)}")
        if "removed" in e:
            note, why = e["removed"], e.get("no_replacement")
            if note is True:
                if not isinstance(why, str) or not why.strip():
                    bad.append(
                        "removed needs the caller's replacement as its text, or true with"
                        " no_replacement = <reason>"
                    )
            elif not isinstance(note, str) or not note.strip():
                bad.append("removed must be the caller's replacement, or true")
            elif why is not None:
                bad.append("no_replacement is only for removed = true")
        elif "no_replacement" in e:
            bad.append("no_replacement is only for removed = true")
        if "new" in e and (not isinstance(e["new"], str) or not e["new"]):
            bad.append("new must be a name")
        if e.get("kind") not in KINDS:
            bad.append(f"unknown kind {e.get('kind')!r}")
        if e.get("domain") not in domains.DOMAINS:
            bad.append(f"unknown domain {e.get('domain')!r}")
        if e.get("rule") not in slugs:
            bad.append(f"rule {e.get('rule')!r} is not defined in rules.toml")
        if "changed" in e:
            test = e.get("test")
            if not isinstance(test, str) or "::" not in test:
                bad.append('changed requires test = "<path>::<substring>"')
            else:
                tpath, sub = test.split("::", 1)
                p = root / tpath
                if not p.is_file():
                    bad.append(f"test file {tpath} does not exist")
                elif not sub or sub not in p.read_text(encoding="utf-8", errors="replace"):
                    bad.append(f"test file {tpath} does not contain {sub!r}")
        elif "test" in e:
            bad.append("test is only for changed entries")
        if "to" in e:
            if "changed" not in e:
                bad.append("to is only for changed entries; a plain rename is new")
            elif not isinstance(e["to"], str) or not e["to"] or e["to"] == e.get("old"):
                bad.append("to must name the member the changed one became")
        if "frontend" in e:
            if le.surface != "cli":
                bad.append("frontend is only allowed in cli.toml")
            elif e["frontend"] not in ("python", "native"):
                bad.append(f"frontend must be python or native, not {e['frontend']!r}")
        if "entry" in e:
            if le.surface != "wasm":
                bad.append("entry is only allowed in wasm.toml")
            elif e["entry"] not in WASM_ENTRIES:
                bad.append(f"unknown WASM entry {e['entry']!r}")
        if bad:
            le.valid = False
            problems.extend(f"{le.label}: {b}" for b in bad)
    problems += _ledger_redundancy(entries)
    return problems


def _ledger_redundancy(entries: list[LedgerEntry]) -> list[str]:
    """Two rows for one name, and a removal under an owner whose removal already speaks for it."""
    seen, removed, out = {}, set(), []
    for le in entries:
        e = le.data
        if not isinstance(e.get("old"), str):
            continue
        key = (le.surface, e.get("kind"), e["old"], e.get("entry", "."), e.get("frontend"))
        if key in seen:
            le.valid = False
            out.append(f"{le.label}: a second row for {e['old']} (first at {seen[key]})")
        else:
            seen[key] = le.label
        if "removed" in e:
            removed.add((le.surface, e["old"], e.get("entry", ".")))
    for le in entries:
        e = le.data
        # A member row that says where the member went (new / changed) still informs.
        if (
            "removed" not in e
            or not isinstance(e.get("old"), str)
            or not isinstance(e.get("kind"), str)
        ):
            continue
        owner = next(
            (
                o
                for o in _owner_names(e["kind"], e["old"])
                if (le.surface, o, e.get("entry", ".")) in removed
            ),
            None,
        )
        if owner:
            le.valid = False
            out.append(f"{le.label}: {owner} is removed, which already covers removing {e['old']}")
    return out


def load_domains(path: Path, problems: list[str]) -> dict[str, dict]:
    """Read the frozen name -> domain assignment; an unreadable file is a validation problem."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        problems.append(f"{path.name}: missing; run with --regenerate-domains")
        return {}
    except json.JSONDecodeError as exc:
        problems.append(f"{path.name}: {exc}")
        return {}
    out = {}
    for key, v in sorted(data.items()) if isinstance(data, dict) else []:
        ok = (
            isinstance(v, dict)
            and set(v) == {"domain", "surfaces"}
            and v["domain"] in domains.DOMAINS
            and isinstance(v["surfaces"], list)
        )
        if ok:
            out[key] = v
        else:
            problems.append(f"{path.name}: {key}: needs domain (one of the six) and surfaces")
    if not isinstance(data, dict):
        problems.append(f"{path.name}: top level must be an object")
    return out


def _ledger_key(name: str) -> str:
    return domains.symbol_key(domains.top_level(name))


def ledger_domains(entries: list[LedgerEntry], frozen: dict[str, dict]):
    """Return (renames, problems): new-name keys mapped to their old entry's domain, and
    the entries whose domain disagrees with the frozen assignment."""
    renames, problems = {}, []
    for le in entries:
        e = le.data
        if not le.valid or e["kind"] in ("command", "flag"):
            continue
        old_key = _ledger_key(e["old"])
        target = e.get("new") or e.get("to")
        new_key = _ledger_key(target) if target else None
        expected = frozen.get(old_key) or (frozen.get(new_key) if new_key else None)
        if expected and e["domain"] != expected["domain"]:
            problems.append(
                f"{le.label}: domain {e['domain']!r} disagrees with domains.json;"
                f" expected {expected['domain']!r}"
            )
        if new_key and e["kind"] in domains.UNIFY_KINDS:
            renames[new_key] = expected["domain"] if expected else e["domain"]
    return renames, problems


def ledger_surfaces(le: LedgerEntry) -> list[str]:
    if le.surface != "cli":
        return [le.surface]
    fe = le.data.get("frontend")
    return [f"cli-{fe}"] if fe else ["cli-python", "cli-native"]


# A changed text that opens by calling the member a getter (TS) or property (Python).
_GETTER_CLAIM = {
    "wasm": r"^(?:an? |readonly )*(getter)\b",
    "node": r"^(?:an? |readonly )*(getter)\b",
    "python": r"^(?:an? |readonly )*(property)\b",
}


def _is_getter(r: dict | None) -> bool:
    if r is None:
        return False
    if r["surface"] == "python":
        return any("property" in d for d in r.get("decorators", []))
    return r.get("ts_kind") == "accessor"


def check_ledger(entries, lookup, selected, by_key=None, baseline=None):
    """Return (failures, changed) for valid entries on the selected surfaces.

    ``by_key`` maps a lookup key to its record, for the getter claim; ``baseline``
    is the lookup of the release the ledger migrates from, or None when it could
    not be built.
    """
    failures, changed = [], []
    by_key = by_key or {}
    # A changed member may change kind with it (a method that became a field).
    named = {(s, n, e) for (s, _k, n, e) in lookup}
    for le in entries:
        if not le.valid:
            continue
        e = le.data
        kind, old = e["kind"], e["old"]
        entry = e.get("entry", ".") if le.surface == "wasm" else None
        tag = f" [{entry}]" if entry and entry != "." else ""
        for s in ledger_surfaces(le):
            if s not in selected:
                continue
            here = (s, kind, old, entry) in lookup
            msgs = []
            if baseline is not None and (s, kind, old, entry) not in baseline:
                msgs.append(f"old name not in {BASELINE_TAG}")
            if "removed" in e:
                if here:
                    msgs.append("removed name still present")
            elif "new" in e:
                if here:
                    msgs.append("old name still present")
                if (s, kind, e["new"], entry) not in lookup:
                    msgs.append(f"new name {e['new']} not present")
            else:
                target = e.get("to", old)
                if "to" in e:
                    if (s, old, entry) in named:
                        msgs.append("old name still present")
                    if (s, target, entry) not in named:
                        msgs.append(f"to name {target} not present")
                elif (s, old, entry) not in named:
                    msgs.append("changed name not present; a renamed member names its target in to")
                claim = _GETTER_CLAIM.get(s)
                m = claim and re.match(claim, e["changed"])
                if m and (s, target, entry) in named:
                    rec = by_key.get((s, kind, target, entry)) or by_key.get(
                        (s, "method", target, entry)
                    )
                    if not _is_getter(rec):
                        msgs.append(f"text claims a {m.group(1)} but {target} is not one")
                changed.append(
                    {
                        "surface": s,
                        "kind": kind,
                        "name": old + tag,
                        "domain": e["domain"],
                        "text": e["changed"],
                    }
                )
            if msgs:
                failures.append(
                    {
                        "surface": s,
                        "kind": kind,
                        "name": old + tag,
                        "domain": e["domain"],
                        "message": "; ".join(msgs),
                    }
                )
    return failures, changed


# Names a caller never spells: a C or TypeScript parameter is passed by position, so its
# rename breaks no source. A Python parameter is a keyword and is never in this class.
UNSPELLED_NAMES = {
    ("c", "option"): "C parameter names",
    ("node", "option"): "TypeScript parameter names",
    ("wasm", "option"): "TypeScript parameter names",
}


def _owner_names(kind: str, name: str) -> list[str]:
    """Names whose ledger row speaks for this one, innermost first."""
    out = []
    if kind == "flag":
        return [name.split(" --", 1)[0]]
    m = re.match(r"^(.*)\((\w+)\)$", name)
    base = m.group(1) if m else name
    if m:
        out.append(base)
    m = re.match(r"^([\w.]+)=", base)
    if m:
        base = m.group(1)
        out.append(base)
    parts = base.split(".")
    out += [".".join(parts[:i]) for i in range(len(parts) - 1, 0, -1)]
    return out


def check_completeness(entries, lookup, baseline, selected):
    """Return (orphans, tally): baseline names gone at tip that no row carries.

    A gone name is carried by its own row of the same kind, by a removed owner, or by a
    changed or renamed owner under whose current name the checker finds it again. A name no
    caller spells (UNSPELLED_NAMES) is counted apart. ``tally`` maps (surface, bucket) to a
    count, the buckets being compared, exempt, own, owner and uncovered.
    """
    rows = collections.defaultdict(list)
    for le in entries:
        if not le.valid:
            continue
        for s in ledger_surfaces(le):
            rows[(s, le.data["old"])].append(le.data)
    present = {(s, n) for (s, _k, n, _e) in lookup}
    tally = collections.Counter()
    for s, _k, _n, _e in baseline:
        if s in selected:
            tally[(s, "compared")] += 1
    orphans = []
    for key in sorted(baseline - lookup, key=lambda k: (k[0], k[1], k[2], k[3] or "")):
        s, kind, name, entry = key
        if s not in selected:
            continue
        if (s, kind) in UNSPELLED_NAMES:
            tally[(s, "exempt")] += 1
            continue
        if any(r["kind"] == kind for r in rows[(s, name)]):
            tally[(s, "own")] += 1
            continue
        if any(_owner_carries(s, o, name, rows[(s, o)], present) for o in _owner_names(kind, name)):
            tally[(s, "owner")] += 1
            continue
        tally[(s, "uncovered")] += 1
        orphans.append(
            {
                "surface": s,
                "kind": kind,
                "name": name + (f" [{entry}]" if entry and entry != "." else ""),
            }
        )
    return orphans, tally


def _owner_carries(surface, owner, name, owner_rows, present) -> bool:
    """Whether a row on ``owner`` carries its member ``name``."""
    for hit in owner_rows:
        if "removed" in hit:
            return True
        current = hit.get("new") or hit.get("to") or owner
        if (surface, current + name[len(owner) :]) in present:
            return True
    return False


# ------------------------------------------------------------------ checks


@dataclass
class Ctx:
    records: list[dict]
    mirrors: dict[str, str]
    python_all: set[str]
    cli_only: dict[str, list[str]]
    by_owner: dict = field(default_factory=dict)
    all_records: list[dict] = field(default_factory=list)
    dimensionless: set = field(default_factory=set)
    catalog: dict = field(default_factory=dict)
    documents: set = field(default_factory=set)
    # check id -> what it could not compare, one line each
    not_compared: dict = field(default_factory=lambda: collections.defaultdict(list))

    def __post_init__(self):
        idx = collections.defaultdict(list)
        for r in self.records:
            if r["owner"]:
                idx[(r["surface"], r["owner"])].append(r)
        self.by_owner = idx

    def mirror(self, r: dict) -> bool:
        if r["surface"] != "python":
            return False
        if r["kind"] == "function":
            return r["name"] in self.mirrors
        if r["kind"] == "option":
            return r["owner"] in self.mirrors
        return False

    def members(self, r: dict) -> list[dict]:
        return self.by_owner.get((r["surface"], r["name"]), [])


def bare(r: dict) -> str:
    return r["member"] or r["name"]


def snake(name: str) -> str:
    return domains.snake(name.lstrip("-"))


def recs(ctx, surfaces, kinds):
    return [r for r in ctx.records if r["surface"] in surfaces and r["kind"] in kinds]


BINDINGS = ("wasm", "node", "python")
JS = ("wasm", "node")
LANG = ("wasm", "node", "python", "c")
CLI = ("cli-python", "cli-native")
ALL = SURFACES


def chk_seconds_suffix(ctx):
    cands = [
        r
        for r in recs(ctx, ALL, ("option", "field", "function", "method", "flag"))
        if not ctx.mirror(r)
    ]
    bad = [
        (r, "name ends in seconds; use the Sec suffix")
        for r in cands
        if re.search(r"(Seconds|_seconds|-seconds|^seconds)$", bare(r))
    ]
    return len(cands), bad


_STEMS = (
    "fade_in",
    "fade_out",
    "start_time",
    "end_time",
    "threshold",
    "attack",
    "release",
    "cutoff",
    "delay",
    "duration",
    "fade",
    "lookahead",
    "latency",
    "offset",
    "time",
    "frequency",
    "freq",
    "ceiling",
)
_UNITS = (
    "sec",
    "ms",
    "hz",
    "db",
    "cents",
    "semitones",
    "ppq",
    "m",
    "deg",
    "bpm",
    "db_oct",
    "samples",
    "frames",
    "sample",
    "frame",
    "lufs",
    "lu",
    "dbtp",
)


def _unit_split(name: str):
    """(stem, unit) when the snake name ends with stem or stem_unit, else None.

    A unit word anywhere else in the name (sample_start_offset) names the unit as well.
    """
    s = snake(name)
    words = s.split("_")
    for stem in _STEMS:
        for unit in _QUANTITY_WORDS:
            if s == f"{stem}_{unit}" or s.endswith(f"_{stem}_{unit}"):
                return stem, unit
        if s == stem or s.endswith("_" + stem):
            head = words[: len(words) - len(stem.split("_"))]
            return stem, next((w for w in head if w in _QUANTITY_WORDS), None)
    return None


# An element index states its quantity as plainly as a unit word does.
_QUANTITY_WORDS = (*_UNITS, "index")


_NUMERIC_WORD = re.compile(
    r"\b(?:number|bigint|float|int|double|long|short|unsigned|size_t|u?int(?:8|16|32|64)_t"
    r"|(?:Float|Int|Uint|BigInt|BigUint)(?:8|16|32|64)Array)\b"
)


def _numeric_type(r: dict) -> bool:
    """False when the declared type cannot hold a number (a token, a flag, an object, a record list)."""
    ty = r.get("type")
    if not ty:
        return True
    if r.get("boolean") or r.get("enum_ref") or ty.lstrip().startswith("{"):
        return False
    return bool(_NUMERIC_WORD.search(ty))


def _unit_hit(r: dict):
    """unit-in-name's verdict input for one record: (stem, unit), or None when it names no quantity."""
    # A CLI switch takes no value, so it carries no quantity to name a unit for.
    if (r["kind"] == "flag" and r.get("boolean")) or not _numeric_type(r):
        return None
    return _unit_split(bare(r))


_C_ENUM_OPEN = re.compile(r"^(?:@brief\s+)?An?\s+@ref\s+(Sonare\w+)")
_C_INT = re.compile(r"\b(?:u?int(?:8|16|32|64)_t|int|unsigned)\b")


def _c_int_field(r: dict) -> bool:
    return (
        r["surface"] == "c"
        and r["kind"] == "field"
        and "*" not in r.get("type", "")
        and bool(_C_INT.search(r.get("type", "")))
    )


def _c_enums(records: list[dict]) -> set[str]:
    return {r["name"] for r in records if r["surface"] == "c" and r.get("c_kind") == "enum"}


def mark_c_enum_fields(records: list[dict]) -> int:
    """Tag each C int field whose doc opens "A @ref Sonare<Enum>" with that enum; return the count."""
    enums, n = _c_enums(records), 0
    for r in records:
        if _c_int_field(r) and (m := _C_ENUM_OPEN.match(r.get("doc_text", ""))) and m[1] in enums:
            r["enum_ref"] = m[1]
            n += 1
    return n


def chk_c_enum_field_doc(ctx):
    """An int field that carries an enumerator says so the one way mark_c_enum_fields reads."""
    enums = _c_enums(ctx.all_records)
    # Two-word suffixes only: a bare "mode" field is not evidence of SonareMode.
    by_name = {
        snake(e.removeprefix("Sonare")): e for e in enums if "_" in snake(e.removeprefix("Sonare"))
    }
    cands, bad = 0, []
    for r in recs(ctx, ("c",), ("field",)):
        if not _c_int_field(r):
            continue
        f = r["member"]
        named = [e for s, e in by_name.items() if f == s or f.endswith("_" + s)]
        cited = [e for e in re.findall(r"@ref\s+(Sonare\w+)", r.get("doc_text", "")) if e in enums]
        if not (named or cited or r.get("enum_ref")):
            continue
        cands += 1
        if not r.get("enum_ref"):
            enum = (cited or named)[0]
            bad.append((r, f"carries a {enum}; open its doc with 'A @ref {enum}'"))
    return cands, bad


def dimensionless_key(name: str) -> str:
    """One spelling for a name on every surface: no Sonare / sonare_ prefix, snake_case words."""
    return re.sub(
        r"[A-Za-z][A-Za-z0-9_]*",
        lambda m: snake(re.sub(r"^(?:Sonare(?=[A-Z])|sonare_)", "", m[0])),
        name,
    )


def _js_declaration(r: dict):
    """A JS field's declaration; a type alias projecting another type's field shares it."""
    if r["surface"] not in JS or r["kind"] != "field" or not r.get("line"):
        return None
    return (r["surface"], r["file"], r["line"], r["member"])


def chk_unit_in_name(ctx):
    cands, bad = 0, []
    listed = {
        d
        for r in ctx.all_records
        if dimensionless_key(r["name"]) in ctx.dimensionless and (d := _js_declaration(r))
    }
    for r in recs(ctx, ALL, ("option", "field", "flag")):
        if ctx.mirror(r):
            continue
        hit = _unit_hit(r)
        if hit is None:
            continue
        cands += 1
        if hit[1] is None and dimensionless_key(r["name"]) not in ctx.dimensionless:
            if _js_declaration(r) in listed:
                continue
            bad.append((r, f"'{hit[0]}' names a physical quantity; add its unit suffix"))
    return cands, bad


# A loudness quantity names the one unit it is measured in.
_LOUDNESS_UNITS = (
    ("true_peak", "dbtp"),
    ("loudness_range", "lu"),
    ("lra", "lu"),
    ("integrated", "lufs"),
    ("momentary", "lufs"),
    ("short_term", "lufs"),
)
# A leading verb makes the name a switch that turns the measurement on, not the level itself.
_SWITCH_VERBS = {"measure", "enable", "use", "compute", "with"}


def _wrong_unit(name: str) -> str | None:
    words = snake(name).split("_")
    if words[0] in _SWITCH_VERBS:
        return None
    for quantity, unit in _LOUDNESS_UNITS:
        q = quantity.split("_")
        for i in range(len(words) - len(q) + 1):
            if words[i : i + len(q)] != q:
                continue
            if quantity == "lra":
                return "spell lra as loudness_range, which is measured in lu"
            rest = words[i + len(q) :]
            given = next((w for w in rest if w in _QUANTITY_WORDS), None)
            if given == unit:
                return None
            if given is None and rest not in ([], ["l"], ["r"]):
                return None
            label = quantity.replace("_", " ")
            return f"{label} is measured in {unit}; name it ..._{unit}" + (
                f", not ..._{given}" if given else ""
            )
    return None


# A peak or ceiling whose doc says it is a true peak is the same quantity under another name.
_TRUE_PEAK_DOC = re.compile(r"true[- ]peak|dbtp", re.IGNORECASE)


def _peak_in_db(name: str) -> bool:
    words = snake(name).split("_")
    unit = next((w for w in reversed(words) if w in _QUANTITY_WORDS), None)
    return bool({"peak", "ceiling"} & set(words)) and unit == "db"


def chk_wrong_unit(ctx):
    cands, bad = 0, []
    for r in recs(ctx, ALL, ("option", "field", "flag", "function", "method")):
        if ctx.mirror(r) or (r["kind"] == "flag" and r.get("boolean")) or not _numeric_type(r):
            continue
        name = bare(r)
        if any(q in snake(name) for q, _ in _LOUDNESS_UNITS):
            cands += 1
            if (msg := _wrong_unit(name)) is not None:
                bad.append((r, msg))
        elif _peak_in_db(name):
            cands += 1
            if _TRUE_PEAK_DOC.search(r.get("doc_text", "")):
                bad.append(
                    (
                        r,
                        "its doc describes a true peak, which is measured in dbtp; name it ..._dbtp, not ..._db",
                    )
                )
    return cands, bad


# The unit word a catalog unit asks of a parameter name; a unit with no word (ratio, none,
# count ...) asks for none, so a name carrying one disagrees with it. A size in samples (nFft,
# hopLength) is a count and may go without the word; sampleRate is the one name for a rate.
_CATALOG_UNIT_WORDS = {
    "dB": "db",
    "dBFS": "db",
    "dBTP": "dbtp",
    "LUFS": "lufs",
    "LU": "lu",
    "ms": "ms",
    "s": "sec",
    "Hz": "hz",
    "cents": "cents",
    "semitones": "semitones",
    "samples": "samples",
    "dBPerOct": "db_oct",
    "m": "m",
    "deg": "deg",
    "bpm": "bpm",
}


def _name_unit(name: str) -> str | None:
    s = snake(name.rstrip("0123456789")).rstrip("_")
    return next(
        (u for u in sorted(_UNITS, key=len, reverse=True) if s == u or s.endswith("_" + u)), None
    )


# A position component (x / y / z, sourceX ...) is named for its axis; its length unit is stated
# once by the group that owns the coordinates, so the name passes only where that doc says it.
_AXIS = re.compile(r"[xyz]|.*[a-z0-9][XYZ]")
_LENGTH_UNITS = {"m"}
_LENGTH_DOC = re.compile(r"\bmet(?:re|er)s?\b|\bin m\b", re.IGNORECASE)


def _group_doc(ctx, owner: str, group: dict) -> str:
    name = f"MasteringProcessorParamMap.{owner}"
    docs = [r.get("doc_text", "") for r in ctx.all_records if r["name"] == name]
    return " ".join(docs + [group.get("description") or ""])


def chk_catalog_unit_suffix(ctx):
    cands, bad = 0, []
    cat = ctx.catalog
    groups = [("chain", cat.get("chain", {}))]
    groups += [(p.get("id", "?"), p) for p in cat.get("processors", [])]
    for owner, group in groups:
        for q in group.get("params", []):
            name, unit = q["name"].rsplit(".", 1)[-1], q.get("unit")
            want, given = _CATALOG_UNIT_WORDS.get(unit), _name_unit(name)
            if given is None and unit in _LENGTH_UNITS and _AXIS.fullmatch(name):
                cands += 1
                if not _LENGTH_DOC.search(_group_doc(ctx, owner, group)):
                    r = {
                        "surface": "catalog",
                        "kind": "param",
                        "name": f"{owner}.{q['name']}",
                        "domain": "mastering",
                    }
                    bad.append(
                        (
                            r,
                            f"an axis component in {unit}; the group's doc does not state the length unit",
                        )
                    )
                continue
            if given is None and (want in (None, "samples") or snake(name).endswith("sample_rate")):
                continue
            cands += 1
            if want == given:
                continue
            r = {
                "surface": "catalog",
                "kind": "param",
                "name": f"{owner}.{q['name']}",
                "domain": "mastering",
            }
            msg = (
                f"catalog unit {unit} asks for ..._{want}"
                if want
                else f"catalog unit {unit} has no unit word"
            )
            bad.append((r, f"{msg}; the name says {given or 'no unit'}"))
    return cands, bad


_SR_BAD = {"sr", "sampleRateHz", "rate", "fs", "sample_rate_hz"}


def chk_sample_rate_name(ctx):
    cands, bad = 0, []
    for r in recs(ctx, LANG, ("option", "field")):
        if ctx.mirror(r):
            continue
        n = bare(r)
        if r["surface"] == "python" and n == "sr":
            continue
        if n in _SR_BAD or n in ("sampleRate", "sample_rate"):
            cands += 1
            if n in _SR_BAD:
                bad.append((r, f"spell the sample rate sampleRate / sample_rate, not {n}"))
    return cands, bad


_PY_AUDIO_PARAMS = {"y", "samples", "left", "right", "channels"}


def _js_handle_method_types(ctx) -> set[tuple[str, str]]:
    """(surface, type) pairs only instance methods take; such a handle carries its own rate."""
    kinds = collections.defaultdict(set)
    for r in ctx.all_records:
        if r["surface"] not in JS or not r.get("sigs"):
            continue
        if r["kind"] == "method":
            kind = "static" if r.get("static") else "instance"
        elif r["kind"] in ("function", "class"):
            kind = "entry"
        else:
            continue
        for s in r["sigs"]:
            for p in s["params"]:
                for t in re.findall(r"[A-Za-z_]\w*", p["type"]):
                    kinds[(r["surface"], t)].add(kind)
    return {k for k, v in kinds.items() if v == {"instance"}}


def _py_rate_scoped(r) -> bool:
    """Free functions, constructors and factories; an instance method runs at its handle's rate."""
    if r["kind"] != "method":
        return True
    return r["member"] == "__init__" or bool(
        {"classmethod", "staticmethod"} & set(r.get("decorators", []))
    )


def _c_rate_entries(ctx) -> dict[str, tuple[str, bool]]:
    """Parity key -> (C function, whether it takes a sample rate as a parameter or a struct field).

    A ``_ex`` variant folds into its base key, as parity folds it; the rate counts if either has it.
    """
    rated_structs = {
        r["owner"]
        for r in ctx.all_records
        if r["surface"] == "c" and r["kind"] == "field" and r["member"] == "sample_rate"
    }
    out: dict[str, tuple[str, bool]] = {}
    for r in ctx.all_records:
        if r["surface"] != "c" or r["kind"] != "function":
            continue
        key = parity_normalize.canonical_key(r["name"], "c")
        key = key.removesuffix("_ex")
        rated = any(
            p["name"] == "sample_rate"
            or re.sub(r"\bconst\b|\*", "", p["type"]).strip() in rated_structs
            for s in r.get("sigs", [])
            for p in s["params"]
        )
        base, had = out.get(key, (r["name"], False))
        if not r["name"].endswith("_ex"):
            base = r["name"]
        out[key] = (base, had or rated)
    return out


def _rate_free_c_entry(entries, surface, name) -> str | None:
    """The C entry an operation maps to when that entry takes no sample rate; None otherwise."""
    hit = entries.get(parity_normalize.canonical_key(name, surface))
    return hit[0] if hit and not hit[1] else None


def chk_sample_rate_required(ctx):
    """Audio in, sample rate required, wherever the C entry the operation maps to takes one."""
    cands, bad = 0, []
    entries = _c_rate_entries(ctx)
    skipped = ctx.not_compared["sample-rate-required"]
    handle_types = _js_handle_method_types(ctx)
    for r in recs(ctx, JS, ("type",)):
        if not re.search(r"(Request|Options)$", r["name"]):
            continue
        if (r["surface"], r["name"]) in handle_types:
            continue
        fields = {m["member"]: m for m in ctx.members(r) if m["kind"] == "field"}
        if "samples" not in fields and "channels" not in fields:
            continue
        op = re.sub(r"(Request|Options)$", "", r["name"])
        c_name = _rate_free_c_entry(entries, r["surface"], op)
        if c_name:
            skipped.append(
                f"{r['surface']} {r['kind']} {r['name']} ({c_name} takes no sample rate)"
            )
            continue
        cands += 1
        sr = fields.get("sampleRate")
        if sr is None or sr.get("optional"):
            bad.append((r, "takes samples but sampleRate is not a required field"))
    for r in recs(ctx, ("python",), ("function", "method")):
        if ctx.mirror(r) or not _py_rate_scoped(r):
            continue
        params = {p["name"]: p for s in r.get("sigs", []) for p in s["params"]}
        audio = bool(set(params) & _PY_AUDIO_PARAMS)
        sr = params.get("sample_rate")
        if not audio and sr is None:
            continue
        c_name = r["kind"] == "function" and _rate_free_c_entry(entries, "python", r["name"])
        if c_name:
            skipped.append(
                f"{r['surface']} {r['kind']} {r['name']} ({c_name} takes no sample rate)"
            )
            continue
        cands += 1
        # With no audio to read a rate off, an absent rate is its own question
        # (the rate-less answer), and None is how Python says absent.
        if (
            sr is not None
            and sr["optional"]
            and (audio or parity_normalize.canonical_default(sr.get("default")) != "none")
        ):
            bad.append((r, "sample_rate has a default; make it required"))
        elif sr is None and "sr" not in params:
            bad.append((r, "takes audio but has no sample_rate parameter"))
    return cands, bad


def chk_sample_rate_family(ctx):
    """Python: `sr` (default 22050) only on listed librosa mirrors, `sample_rate` everywhere else."""
    cands, bad = 0, []
    for r in recs(ctx, ("python",), ("function", "method")):
        params = {p["name"]: p for s in r.get("sigs", []) for p in s["params"]}
        cands += 1
        has_sr, has_rate = "sr" in params, "sample_rate" in params
        if has_sr and has_rate:
            bad.append((r, "carries both sr and sample_rate"))
        if ctx.mirror(r):
            if has_rate and not has_sr:
                bad.append((r, "listed librosa mirror spells its rate sample_rate; use sr=22050"))
            elif has_sr and params["sr"].get("default") != "22050":
                bad.append(
                    (r, f"listed librosa mirror has sr={params['sr'].get('default')}, not sr=22050")
                )
        elif has_sr:
            bad.append((r, "sr is reserved for librosa mirrors; use a required sample_rate"))
    return cands, bad


def chk_mirror_present(ctx):
    names = {r["name"] for r in recs(ctx, ("python",), ("function",))}
    bad = []
    for name in sorted(ctx.mirrors):
        if name not in names:
            d, _ = domains.classify("python", name, "")
            r = {"surface": "python", "kind": "function", "name": name, "domain": d}
            bad.append((r, "listed in librosa_mirrors.toml but not a public Python function"))
    return len(ctx.mirrors), bad


_JS_TEARDOWN = {
    "destroy",
    "dispose",
    "close",
    "delete",
    "[Symbol.dispose]",
    "[Symbol.asyncDispose]",
}


def chk_teardown_js(ctx):
    cands, bad = 0, []
    for r in recs(ctx, JS, ("class",)):
        names = {
            m["member"] for m in ctx.members(r) if m["kind"] == "method" and not m.get("static")
        }
        if not names & _JS_TEARDOWN:
            continue
        cands += 1
        if "destroy" not in names:
            bad.append((r, "has a teardown member but no destroy()"))
        if not names & {"[Symbol.dispose]", "[Symbol.asyncDispose]"}:
            bad.append((r, "has a teardown member but no [Symbol.dispose] / [Symbol.asyncDispose]"))
        for extra in sorted(names & {"dispose", "close", "delete"}):
            bad.append((r, f"teardown is spelled destroy(), not {extra}()"))
    return cands, bad


def chk_teardown_python(ctx):
    cands, bad = 0, []
    for r in recs(ctx, ("python",), ("class", "type")):
        names = {m["member"] for m in ctx.members(r) if m["kind"] == "method"}
        if not names & {"close", "destroy", "delete", "__enter__"}:
            continue
        cands += 1
        for need in ("close", "__enter__", "__exit__"):
            if need not in names:
                bad.append((r, f"has a teardown member but no {need}"))
        for extra in sorted(names & {"destroy", "delete"}):
            bad.append((r, f"teardown is spelled close(), not {extra}()"))
    return cands, bad


_WASM_TYPE = re.compile(r"\bWasm[A-Z]\w*")


def _texts(r):
    out = [r.get("type", "")]
    for s in r.get("sigs", []):
        out.append(s.get("returns", ""))
        out.extend(p["type"] for p in s["params"])
    return " ".join(t for t in out if t)


def chk_no_internal_types(ctx):
    cands = recs(
        ctx, ("wasm",), ("function", "method", "class", "type", "field", "option", "export")
    )
    private = {r["name"] for r in cands if r["kind"] == "class" and r.get("ctor_private")}
    bad = []
    for r in cands:
        if r["kind"] == "class" and r["name"] in private:
            r = dict(r, sigs=[])
        if (
            r["kind"] == "option"
            and r["owner"].endswith(".constructor")
            and r["owner"][:-12] in private
        ):
            continue
        hits = sorted(set(_WASM_TYPE.findall(_texts(r))))
        if hits:
            bad.append((r, "exposes internal type " + ", ".join(hits)))
    return len(cands), bad


def chk_worklet_scope(ctx):
    cands = [r for r in recs(ctx, JS, KINDS) if not r["owner"]]
    bad = [
        (r, f"declared in internal module {r['file']}")
        for r in cands
        if re.search(r"bindings/(wasm|node)/src/internal/", r["file"])
    ]
    return len(cands), bad


def chk_progress_callback(ctx):
    cands = recs(ctx, LANG, ("function", "method"))
    bad = [
        (r, "progress is an option of the operation, not a WithProgress twin")
        for r in cands
        if re.search(r"(WithProgress|_with_progress)$", bare(r))
    ]
    return len(cands), bad


_C_SUFFIX = re.compile(r"_(ex|ex2|v2|checked|with_options|with_progress)$")
# Extensions that name what they act on rather than a second form of their base: the
# initializer of the struct the base takes, and the release of a distinct result type.
_C_STRUCTURAL_EXTENSION = (
    (re.compile(r"_(?:\w+_)?(?:options|config)_init$"), "struct initializers"),
    (re.compile(r"^sonare_free_"), "releases of a distinct result type"),
)
_C_EXTENSION_MESSAGE = "is {base} plus _{rest}, a second form of {base}"
# The decided channel forms of one operation: a left/right pair, N planar channels, and one
# interleaved buffer. A twin is one only while it differs from its base in the audio alone.
_C_CHANNEL_FORMS = ("stereo", "linked", "interleaved")
_C_CHANNEL_FORM_MESSAGE = (
    "is the _{form} channel form of {base} but differs from it after the audio"
)
_C_AUDIO_TYPES = {"const float*", "const float* const*"}
_C_AUDIO_EXTENTS = re.compile(r"^(?:length|frames|num_channels|\w+_length)$")


def _c_after_audio(rec: dict, form: str = "") -> list[tuple[str, str, str]] | None:
    """(compared type, name, spelled type) past the audio buffers and their extents, then
    the return type. A twin's ``<Form>Result`` compares as its base's ``Result``, and a
    ``_stereo`` twin's ``<x>_left`` / ``<x>_right`` buffer pair as its base's ``<x>``."""
    sigs = rec.get("sigs") or []
    if not sigs:
        return None
    out = []
    for p in sigs[0]["params"]:
        t = " ".join(p.get("type", "").split())
        name = p.get("name", "")
        if t in _C_AUDIO_TYPES or ("*" not in t and _C_AUDIO_EXTENTS.match(name)):
            continue
        key = t.replace(f"{form.capitalize()}Result", "Result") if form else t
        if form == "stereo" and name.endswith("_right") and out:
            left = out[-1]
            if left[1] == name[: -len("_right")] + "_left" and left[0] == key:
                out[-1] = (key, name[: -len("_right")], f"{left[2]} {left[1]}, {t} {name}")
                continue
        out.append((key, name, t))
    returns = sigs[0].get("returns", "")
    return out + [(returns, "(return)", returns)]


def _c_channel_twin(rec: dict, base: dict, form: str) -> str | None:
    """None when the twin matches its base after the audio, else the differing parameters."""
    mine, theirs = _c_after_audio(rec, form), _c_after_audio(base)
    if mine is None or theirs is None:
        return None
    keys = lambda xs: [(k, n) for k, n, _ in xs]
    if keys(mine) == keys(theirs):
        return None

    def only(a, b):
        other = keys(b)
        return (
            ", ".join(
                t if " " in t and "," in t else f"{t} {n}" for k, n, t in a if (k, n) not in other
            )
            or "nothing else"
        )

    return f"base has {only(theirs, mine)}; twin has {only(mine, theirs)}"


# The decided sample formats of one read: the same call returning its buffers in another
# element type. A twin is one only while its parameters match and its result struct differs
# from the reference's in buffer element type alone. The reference is the float base, or,
# where there is none, the read's other sample format.
_C_SAMPLE_FORMATS = {"u8": "uint8_t", "i16": "int16_t"}
_C_SAMPLE_FORMAT_MESSAGE = "is the _{fmt} sample format of {base} but differs from it"
_C_SAMPLE_SIBLING_MESSAGE = "is the _{fmt} sample format beside {base} but differs from it"


def _c_sample_format_twin(ctx, rec: dict, fmt: str, ref: dict, ref_fmt: str = "") -> str | None:
    """None when ``rec`` is ``ref`` with buffers of another element type, else what differs.

    ``ref_fmt`` is empty for the float base, or the reference's own sample format."""
    norm = lambda t: " ".join(t.split())

    def side(r, f):
        suffix = f.upper()
        params, structs = [], {}
        for p in ((r.get("sigs") or [{}])[0]).get("params", []):
            t = norm(p.get("type", ""))
            m = re.match(rf"^(\w+){suffix}\*$", t) if f else None
            if m:
                structs[m.group(1)] = m.group(1) + suffix
                t = f"{m.group(1)}*"
            params.append((t, p.get("name", "")))
        return params, structs, ((r.get("sigs") or [{}])[0]).get("returns")

    mine, my_structs, my_ret = side(rec, fmt)
    theirs, their_structs, their_ret = side(ref, ref_fmt)
    diffs = []
    if mine != theirs or my_ret != their_ret:
        extra = [f"{t} {n}" for t, n in mine if (t, n) not in theirs]
        gone = [f"{t} {n}" for t, n in theirs if (t, n) not in mine]
        diffs.append(
            f"parameters: {ref['name']} has {', '.join(gone) or 'nothing else'};"
            f" twin has {', '.join(extra) or 'nothing else'}"
        )
    fields = collections.defaultdict(dict)
    for f in recs(ctx, ("c",), ("field",)):
        fields[f["name"].split(".")[0]][f["member"]] = norm(f.get("type", ""))

    def as_float(t, f):
        return t.replace(_C_SAMPLE_FORMATS[f], "float") if f else t

    for stem, twin in sorted(my_structs.items()):
        other = their_structs.get(stem, stem)
        rf, tf = fields.get(other, {}), fields.get(twin, {})
        gone = [m for m in rf if m not in tf]
        extra = [m for m in tf if m not in rf]
        retyped = [
            m
            for m in rf
            if m in tf and tf[m] != rf[m] and as_float(tf[m], fmt) != as_float(rf[m], ref_fmt)
        ]
        for label, ms in (
            ("lacks", gone),
            ("adds", extra),
            ("retypes beyond the element", retyped),
        ):
            if ms:
                diffs.append(f"{twin} {label} {', '.join(ms)}")
    return "; ".join(diffs) or None


def chk_options_suffix(ctx):
    cands = recs(ctx, ("c",), ("function",))
    by_name = {r["name"]: r for r in cands}
    names = set(by_name)
    bad = []
    structural: collections.Counter = collections.Counter()
    channel_forms = 0
    sample_formats = 0
    for r in cands:
        name = r["name"]
        fixed = _C_SUFFIX.search(name)
        if fixed:
            bad.append((r, f"suffix {fixed.group(0)} marks a second form of one operation"))
            continue
        fmt_m = re.search(r"_(" + "|".join(_C_SAMPLE_FORMATS) + r")$", name)
        stem = name[: fmt_m.start()] if fmt_m else ""
        siblings = [f"{stem}_{f}" for f in _C_SAMPLE_FORMATS if fmt_m and f != fmt_m.group(1)]
        sibling = next((x for x in siblings if x in names), None)
        if fmt_m and stem not in names and sibling:
            ref_fmt = sibling[len(stem) + 1 :]
            diff = _c_sample_format_twin(ctx, r, fmt_m.group(1), by_name[sibling], ref_fmt)
            if diff is None:
                sample_formats += 1
            else:
                msg = _C_SAMPLE_SIBLING_MESSAGE.format(fmt=fmt_m.group(1), base=sibling)
                bad.append((r, f"{msg}: {diff}"))
            continue
        tokens = name.split("_")
        base = next(
            (
                "_".join(tokens[:i])
                for i in range(len(tokens) - 1, 1, -1)
                if "_".join(tokens[:i]) in names
            ),
            None,
        )
        if base is None:
            continue
        label = next((lbl for rx, lbl in _C_STRUCTURAL_EXTENSION if rx.search(name)), None)
        if label:
            structural[label] += 1
            continue
        rest = name[len(base) + 1 :]
        if rest in _C_CHANNEL_FORMS:
            diff = _c_channel_twin(r, by_name[base], rest)
            if diff is None:
                channel_forms += 1
            else:
                msg = _C_CHANNEL_FORM_MESSAGE.format(form=rest, base=base)
                bad.append((r, f"{msg}: {diff}"))
            continue
        if rest in _C_SAMPLE_FORMATS:
            diff = _c_sample_format_twin(ctx, r, rest, by_name[base])
            if diff is None:
                sample_formats += 1
            else:
                msg = _C_SAMPLE_FORMAT_MESSAGE.format(fmt=rest, base=base)
                bad.append((r, f"{msg}: {diff}"))
            continue
        bad.append((r, _C_EXTENSION_MESSAGE.format(base=base, rest=rest)))
    if sample_formats:
        ctx.not_compared["options-struct:suffix"].append(
            f"{sample_formats} sample-format twins (_u8, _i16) match their base but for the"
            " buffer element type"
        )
    if channel_forms:
        ctx.not_compared["options-struct:suffix"].append(
            f"{channel_forms} channel-form twins (_stereo, _linked, _interleaved) match their"
            " base after the audio"
        )
    if structural:
        ctx.not_compared["options-struct:suffix"].append(
            "functions named as another plus an extension that names what they act on: "
            + "; ".join(f"{n} {lbl}" for lbl, n in sorted(structural.items()))
        )
    return len(cands), bad


def _c_snake(type_name: str) -> str:
    return domains.symbol_key(type_name)


def chk_options_struct_size(ctx):
    cands, bad = 0, []
    fns = {r["name"] for r in recs(ctx, ("c",), ("function",))}
    for r in recs(ctx, ("c",), ("type",)):
        if r.get("c_kind") != "struct" or not re.search(r"(Options|Config)$", r["name"]):
            continue
        cands += 1
        fields = sorted(
            (m for m in ctx.members(r) if m["kind"] == "field"), key=lambda m: m.get("index", 0)
        )
        first = fields[0] if fields else None
        if (
            not first
            or first["member"] != "struct_size"
            or first["type"].replace("const", "").strip() != "uint32_t"
        ):
            bad.append((r, "first field must be uint32_t struct_size"))
        init = f"sonare_{_c_snake(r['name'])}_init"
        if init not in fns:
            bad.append((r, f"no {init}()"))
        for m in fields:
            if m["member"] in ("schema_version", "struct_version"):
                bad.append((r, f"field {m['member']} duplicates struct_size"))
    return cands, bad


# Library-allocated plain buffers, one releaser per element kind; a closed set.
_C_PLAIN_BUFFER_RELEASERS = frozenset(
    {"sonare_free_string", "sonare_free_floats", "sonare_free_ints", "sonare_free_bytes"}
)


def chk_c_free(ctx):
    cands, bad = 0, []
    types = recs(ctx, ("c",), ("type",))
    structs = {_c_snake(t["name"]) for t in types if t.get("c_kind") == "struct"}
    handles = {_c_snake(t["name"]) for t in types if t.get("c_kind") == "opaque"}
    owning = {
        _c_snake(f["owner"]) for f in recs(ctx, ("c",), ("field",)) if "*" in f.get("type", "")
    }
    # An array of a struct that owns no memory itself is released as one buffer of elements.
    element_arrays = {s + "s" for s in structs - owning}
    for r in recs(ctx, ("c",), ("function",)):
        toks = r["name"].split("_")
        if not set(toks) & {"free", "destroy", "release"}:
            continue
        cands += 1
        n = r["name"]
        if n in _C_PLAIN_BUFFER_RELEASERS:
            continue
        m = re.fullmatch(r"sonare_free_(\w+)", n)
        if m and (m.group(1) in structs or m.group(1) in element_arrays):
            continue
        m = re.fullmatch(r"sonare_(\w+)_destroy", n)
        if m and m.group(1) in handles:
            continue
        bad.append(
            (
                r,
                "release is sonare_free_<result struct>, sonare_free_<ownerless element>s, sonare_<handle>_destroy or a plain-buffer releaser",
            )
        )
    return cands, bad


def chk_c_create(ctx):
    cands, bad = 0, []
    for r in recs(ctx, ("c",), ("function",)):
        if not re.fullmatch(r"sonare_\w+_create", r["name"]):
            continue
        cands += 1
        sig = r["sigs"][0]
        if sig["returns"] != "SonareError":
            bad.append((r, f"returns {sig['returns']}, not SonareError"))
        if not sig["params"] or not sig["params"][-1]["type"].replace(" ", "").endswith("**"):
            bad.append((r, "last parameter is not the out-pointer to the handle"))
    return cands, bad


_POS_SAMPLE = re.compile(
    r"(^|_)(start|end|position|pos|offset|onset|begin|cursor|playhead|from|to|at)_samples?$"
)


# Records whose bounded counts keep their fixed width: realtime event records packed for an
# SPSC queue or a UMP stream, where a size_t count breaks the packing and pads the record.
FIXED_LAYOUT_RECORDS = {
    "SonareEngineMidiEvent": "UMP event record (word_count uint8_t)",
    "SonareExternalMidiEvent": "external MIDI event record (byte_count uint32_t)",
    "SonareScopeTelemetryRecord": "SPSC telemetry record (band_count, point_count uint32_t)",
}


def chk_c_count_types(ctx):
    cands, bad = 0, []
    fixed: list[str] = []
    for r in recs(ctx, ("c",), ("option", "field")):
        n = r["member"]
        t = r.get("type", "").replace("const", "").replace("*", "").strip()
        if "[" in r.get("type", "") or "(*)" in r.get("type", ""):
            continue
        want = None
        if n == "length" or n.endswith(("_count", "_length")):
            want = "size_t"
        elif _POS_SAMPLE.search(n):
            want = "int64_t"
        elif n == "sample_rate":
            want = "int"
        if want is None:
            continue
        if r["kind"] == "field" and r["name"].split(".")[0] in FIXED_LAYOUT_RECORDS:
            fixed.append(r["name"])
            continue
        cands += 1
        if t != want:
            bad.append((r, f"{n} is {want}, not {t}"))
    if fixed:
        ctx.not_compared["c-count-types"].append(
            f"{len(fixed)} count member(s) of fixed-layout realtime records keep their width: "
            + "; ".join(f"{k}: {v}" for k, v in FIXED_LAYOUT_RECORDS.items())
        )
    return cands, bad


def chk_getters(ctx):
    cands = recs(ctx, BINDINGS, ("function", "method"))
    bad = [
        (r, "drop the get prefix")
        for r in cands
        if re.match(r"get[A-Z]", bare(r)) or bare(r).startswith("get_")
    ]
    return len(cands), bad


def chk_typed_results(ctx):
    cands = recs(ctx, JS, ("function", "method"))
    bad = [
        (r, "returns or takes a JSON string; return the parsed object")
        for r in cands
        if bare(r) not in ("toJson", "fromJson") and re.search(r"\bTypedJson\b", _texts(r))
    ]
    return len(cands), bad


def _top_level(text: str, sep: str) -> list[str]:
    """Split @p text on @p sep outside brackets."""
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        depth += ch == "["
        depth -= ch == "]"
        if ch == sep and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
    return parts + [text[start:].strip()]


def _bare_tuple(ret: str) -> bool:
    """A fixed-arity tuple[A, B] as the returned type; tuple[X, ...] is a homogeneous sequence."""
    for part in _top_level(ret, "|"):
        if (
            part.startswith("tuple[")
            and part.endswith("]")
            and _top_level(part[len("tuple[") : -1], ",")[-1] != "..."
        ):
            return True
    return False


def chk_array_results(ctx):
    cands, bad = 0, []
    for r in recs(ctx, ("python",), ("function", "method")):
        ret = " ".join(s.get("returns", "") for s in r.get("sigs", []))
        if not ret:
            continue
        cands += 1
        if re.search(r"\blist\[(float|int)\]", ret):
            bad.append((r, "returns a list of numbers; return an ndarray"))
        if _bare_tuple(ret) and not ctx.mirror(r):
            bad.append((r, "returns a bare tuple; return a typed result"))
    return cands, bad


def chk_frozen_results(ctx):
    rets = " ".join(
        s.get("returns", "")
        for r in recs(ctx, ("python",), ("function", "method"))
        for s in r.get("sigs", [])
    )
    used = set(re.findall(r"\b[A-Z]\w*", rets))
    cands, bad = 0, []
    for r in recs(ctx, ("python",), ("type",)):
        if not r.get("dataclass") or r["name"] not in used:
            continue
        cands += 1
        if not r.get("frozen"):
            bad.append((r, "result dataclass is not frozen=True"))
    return cands, bad


def chk_snake_case_only(ctx):
    """A key of a declared document type keeps the camelCase spelling the core parses."""
    cands = [
        r
        for r in recs(ctx, ("python",), ("function", "method", "option", "field"))
        if not (r["kind"] == "field" and (r["surface"], r["owner"]) in ctx.documents)
    ]
    bad = [(r, "camelCase name") for r in cands if re.search(r"[a-z][A-Z]", bare(r))]
    return len(cands), bad


def _positional_counts(r):
    return [len([p for p in s["params"] if not p.get("rest")]) for s in r.get("sigs", [])]


def _scalar_function(r: dict) -> bool:
    """Every parameter is a number or boolean scalar, at most four, and none is an options bag."""
    for s in r.get("sigs", []):
        params = s["params"]
        if len(params) > 4 or any(p.get("rest") for p in params):
            return False
        if any(p["type"].replace(" | undefined", "") not in ("number", "boolean") for p in params):
            return False
    return bool(r.get("sigs"))


def chk_one_positional_form(ctx):
    cands = [r for r in recs(ctx, JS, ("function", "method")) if r.get("sigs")]
    bad = [
        (r, f"{max(_positional_counts(r))} positional parameters; take a request object")
        for r in cands
        if max(_positional_counts(r), default=0) >= 4
    ]
    return len(cands), bad


def _named_setter_bool(r, sig) -> bool:
    """`setMuted(strip, muted)`: one trailing boolean that the setter's own name already labels."""
    bools = [p for p in sig["params"] if p["boolean"]]
    if r["kind"] != "method" or len(bools) != 1 or sig["params"][-1] is not bools[0]:
        return False
    name, member = bools[0]["name"], bare(r)
    return member.startswith("set") and member.endswith(name[:1].upper() + name[1:])


def chk_no_positional_bools(ctx):
    cands = [r for r in recs(ctx, JS, ("function", "method")) if r.get("sigs")]
    bad = []
    for r in cands:
        names = sorted(
            {
                p["name"]
                for s in r["sigs"]
                if not _named_setter_bool(r, s)
                for p in s["params"]
                if p["boolean"]
            }
        )
        if names:
            bad.append((r, "positional boolean " + ", ".join(names)))
    return len(cands), bad


# A closed-set token is one lowerCamel word, or a bare number such as a channel layout ('5.1').
_TOKEN = re.compile(r"^(?:[a-z][a-zA-Z0-9.]*|\d+(?:\.\d+)*)$")


# How each binding spells an enum member, mapped onto the cross-binding member name.
_MEMBER_NAME_MAP = {
    "wasm": lambda n: n,
    "node": lambda n: n,
    "python": lambda n: "".join(w.capitalize() for w in n.split("_")),
}


def _name_tables(records, surface: str) -> dict[str, frozenset]:
    """String sets on `surface` equal to one of its enums' member names under the surface's map."""
    enums = collections.defaultdict(set)
    for r in records:
        if (
            r["surface"] == surface
            and r["kind"] == "value"
            and r["owner"]
            and r.get("token") is None
        ):
            enums[r["owner"]].add(_MEMBER_NAME_MAP[surface](r["member"]))
    member_sets = {frozenset(v) for v in enums.values()}
    return {
        r["name"]: frozenset(r["literals"])
        for r in records
        if r["surface"] == surface
        and r["kind"] == "type"
        and r.get("literals")
        and frozenset(r["literals"]) in member_sets
    }


def _tokens(ctx):
    """Closed-set string tokens; an enum's name table is a set of member names, not tokens."""
    tables = {s: _name_tables(ctx.records, s) for s in BINDINGS}
    for r in recs(ctx, BINDINGS, ("value",)):
        if r.get("token") is not None and r["owner"] not in tables[r["surface"]]:
            yield r, r["token"]
    for r in recs(ctx, BINDINGS, ("option", "field")):
        if r.get("inline_literals"):
            if frozenset(r.get("literals", [])) in tables[r["surface"]].values():
                continue
            for t in r.get("literals", []):
                # A literal spelling its own type's name is a nominal tag (Error.name, a brand).
                if t != r["owner"]:
                    yield r, t


def chk_name_tables(ctx):
    """An enum name table carries the same names on every binding that declares it."""
    cands, bad = 0, []
    tables = {s: _name_tables(ctx.all_records, s) for s in BINDINGS}
    for r in recs(ctx, BINDINGS, ("type",)):
        mine = tables[r["surface"]].get(r["name"])
        if mine is None:
            continue
        for other in BINDINGS:
            if other == r["surface"]:
                continue
            peer = next(
                (
                    q
                    for q in ctx.all_records
                    if q["surface"] == other and q["kind"] == "type" and q["name"] == r["name"]
                ),
                None,
            )
            if peer is None:
                continue
            cands += 1
            theirs = frozenset(peer.get("literals") or [])
            if theirs != mine:
                diff = sorted(mine ^ theirs)
                bad.append((r, f"name table differs from {other}: {', '.join(diff)}"))
    return cands, bad


_TOKEN_IN_MESSAGE = re.compile(r"token '([^']*)'")


def _standard_symbol(r: dict, msg: str) -> bool:
    """A token with a word separator has a lowerCamel form, so no standard can excuse it."""
    m = _TOKEN_IN_MESSAGE.search(msg)
    return bool(m) and not re.search(r"[-_\s]", m.group(1))


def chk_token_case(ctx):
    cands, bad = 0, []
    for r, t in _tokens(ctx):
        cands += 1
        if not _TOKEN.match(t):
            bad.append((r, f"token '{t}' is not lowerCamel"))
    return cands, bad


def chk_digit_tokens(ctx):
    cands, bad = 0, []
    for r, t in _tokens(ctx):
        if not re.search(r"\d", t):
            continue
        cands += 1
        if re.search(r"[-_]", t):
            bad.append((r, f"token '{t}' separates digits with - or _"))
        elif re.search(r"\d(dB|db)", t):
            bad.append((r, f"token '{t}' spells the unit dB; use Db as in identifiers"))
    return cands, bad


_PASS = re.compile(r"(hpf|Hpf|HPF)|([hH]igh|[lL]ow|[bB]and)pass")


def _pass_word(m: re.Match) -> str:
    return "high" if m.group(1) else m.group(2).lower()


def chk_high_pass(ctx):
    """High-, low- and band-pass are two words in a name and in a token: highPass, lowPass, bandPass."""
    cands = ctx.records
    bad = []
    for r in cands:
        if m := _PASS.search(bare(r)):
            w = _pass_word(m)
            bad.append((r, f"spell {w}-pass as {w}Pass / {w}_pass"))
    # A value record's name is its token, so the name scan above already reads it.
    for r, t in _tokens(ctx):
        if r["kind"] != "value" and (m := _PASS.search(t)):
            w = _pass_word(m)
            bad.append((r, f"token '{t}' spells {w}-pass as one word; use {w}Pass"))
    return len(cands), bad


_CH_BAD = {"channelCount", "channel_count", "nChannels", "n_channels"}


def _is_array(t: str) -> bool:
    return bool(re.search(r"\[\]|Array|list\[|Sequence|ndarray|PlanarChannels|\*", t))


def chk_num_channels(ctx):
    cands, bad = 0, []
    for r in recs(ctx, LANG, ("option", "field")):
        n = bare(r)
        if n in _CH_BAD:
            cands += 1
            bad.append((r, f"{n}: a channel count is numChannels / num_channels"))
        elif n == "channels" and r["kind"] == "field":
            cands += 1
            if not _is_array(r.get("type", "")):
                bad.append((r, "field channels holds a count; channels is the planar array"))
    return cands, bad


_SEMI_BAD = {"semitones", "pitch_shift_semitones", "retune_semitones"}


def chk_pitch_semitones(ctx):
    cands, bad = 0, []
    for r in ctx.records:
        if r["kind"] == "value":
            continue
        s = snake(bare(r))
        if not s.endswith("semitones"):
            continue
        cands += 1
        if s in _SEMI_BAD:
            bad.append((r, f"{bare(r)} does not say which pitch quantity it moves"))
    return cands, bad


_FB = {"fmin", "fmax", "f0_min", "f0_max", "f_min", "f_max"}


def chk_frequency_bounds(ctx):
    cands, bad = 0, []
    for r in recs(ctx, ALL, ("option", "field", "flag")):
        if ctx.mirror(r):
            continue
        s = snake(bare(r))
        base = s.removesuffix("_hz")
        if base not in _FB:
            continue
        cands += 1
        if not s.endswith("_hz"):
            bad.append((r, f"{bare(r)} is a frequency bound; add Hz"))
    return cands, bad


def chk_frequency_centre(ctx):
    cands = [r for r in ctx.records if {"centre", "center"} & set(snake(bare(r)).split("_"))]
    bad = [(r, "spell centre as center") for r in cands if "centre" in snake(bare(r)).split("_")]
    return len(cands), bad


# A generated doc states catalog metadata one clause per line in this form; a doc made of
# nothing else describes no parameter, so it does not satisfy doc-comments.
_META_LINE = re.compile(r"^(Unit|Range|Default|Values|Scale): \S")
_META_ONLY = "doc states only the catalog metadata; add a description"


def _metadata_only(doc: str) -> bool:
    lines = [x.strip() for x in doc.splitlines() if x.strip()]
    return bool(lines) and all(_META_LINE.match(x) for x in lines)


# generated-catalog: records generated from the capability catalog, whose fields may be documented
# by catalog metadata alone. The catalog is the source of truth, and masteringInsertParamInfo
# returns the same facts at run time.
_GENERATED_CATALOG = frozenset({"MasteringProcessorParamMap"})


def chk_doc_comments(ctx):
    every = recs(ctx, LANG, ("function", "method", "class", "type", "field", "export"))
    # A member inherited from a dependency's declarations (Error.message) has no doc site here.
    cands = [r for r in every if "/node_modules/" not in r.get("file", "")]
    inherited = len(every) - len(cands)
    if inherited:
        ctx.not_compared["doc-comments"].append(
            f"{inherited} members declared in a dependency (node_modules), which carry no doc here"
        )
    generated = {
        _js_declaration(r)
        for r in cands
        if r["kind"] == "field" and r["name"].split(".")[0] in _GENERATED_CATALOG
    } - {None}

    def described(r):
        if not _metadata_only(r.get("doc_text", "")):
            return True
        return r["kind"] == "field" and _js_declaration(r) in generated

    bad = [
        (r, "no doc comment") if not r.get("has_doc") else (r, _META_ONLY)
        for r in cands
        if not r.get("has_doc") or not described(r)
    ]
    return len(cands), bad


_PY_ALLOWED = {
    "Any",
    "Literal",
    "Optional",
    "Union",
    "Callable",
    "Sequence",
    "Mapping",
    "Iterable",
    "Iterator",
    "NDArray",
    "TypeAlias",
    "Self",
    "None",
    "True",
    "False",
    "ClassVar",
    "Final",
    "Protocol",
    "TypedDict",
    "NotRequired",
    "Required",
    "Generator",
    "AsyncIterator",
    "AsyncIterable",
    "Awaitable",
    "Coroutine",
    "MutableMapping",
    "MutableSequence",
    "Collection",
    "Hashable",
    "Type",
    "Tuple",
    "List",
    "Dict",
    "Set",
    "FrozenSet",
    "TypeVar",
    "Generic",
    "IntEnum",
    "Enum",
    "StrEnum",
    "Path",
    "PathLike",
    "Buffer",
    "Never",
    "NoReturn",
    "TypeGuard",
    "Concatenate",
    "ParamSpec",
    "Unpack",
    "LiteralString",
    "BinaryIO",
    "TextIO",
    "IO",
    "Annotated",
    "SupportsFloat",
    "SupportsInt",
    "SupportsIndex",
    "Exception",
    "BaseException",
    "ValueError",
    "TypeError",
    "RuntimeError",
}


_SENTINEL_NUM = r"`*(?:-1|255|4294967295|0x(?:[fF]{2}){1,4})(?![\w.])`*"
_MARKER_NUM = r"`*(?:255|4294967295|0x(?:[fF]{2}){1,4})(?![\w.])`*"
_DOC_SENTINEL = re.compile(
    rf"(?<![\w.]){_SENTINEL_NUM}\s*(?:for|=|means|to mean)\s+(?:an?\s+|the\s+)?"
    r"(?:any|none|unknown|unset|no|not|all|every|auto|default|disabled?|off|the end|now|missing)\b"
    rf"|\bsentinel\W+(?:\w+\W+){{0,2}}?{_SENTINEL_NUM}|(?<![\w.]){_SENTINEL_NUM}\s+sentinel",
    re.IGNORECASE,
)
# An all-ones marker given a meaning ("`0xFFFFFFFF` for clock bytes", "255 tags ...").
_DOC_MARKER = re.compile(
    rf"(?<![\w.]){_MARKER_NUM}\s*(?:\([^)]*\)\s*)?"
    r"(?:for|=|means|to mean|tags|marks|denotes|signals|indicates|is|the|as)\b",
    re.IGNORECASE,
)
# A C constant whose name says absence does not make its value an identifier.
_ABSENCE_NAME = re.compile(
    r"(?:^|_)(?:NONE|ANY|ALL|UNSET|UNKNOWN|INVALID|DEFAULT|AUTO|NO|NULL)(?:_|$)"
)
# Zero standing for a default the library or project picks; "0 = no fade" is a real quantity.
_DOC_ZERO_DEFAULT = re.compile(
    r"(?<![\w.])`*0`*\s*(?:\([^)]*\)\s*)?(?:for|=|means|to mean|selects|uses|picks|keeps)\s+"
    r"(?:the\s+|an?\s+)?(?:(?:library|engine|project)(?:'s)?\s+)?(?:own|default|auto|automatic)\b",
    re.IGNORECASE,
)
# "no surface returns a -1 sentinel" states the rule rather than breaking it.
_DOC_NEGATED = re.compile(r"\b(?:no|never|not|nor)\b[^.;]*$", re.IGNORECASE)


def _c_identifier_values(ctx) -> set[int]:
    """Values the C ABI names as a constant, other than one whose name means absence."""
    out = set()
    for r in ctx.all_records:
        if r["surface"] != "c" or r.get("c_kind") != "macro" or _ABSENCE_NAME.search(r["name"]):
            continue
        try:
            out.add(int(r.get("type", "").rstrip("uUlL"), 0))
        except ValueError:
            pass
    return out


def chk_doc_sentinel(ctx):
    """A binding's docs never offer a magic number for "not given"; absence says it.

    An all-ones marker the C ABI names as a constant is an identifier, not a marker.
    """
    cands = [r for r in recs(ctx, BINDINGS + ("cli-python",), KINDS) if r.get("doc_text")]
    named = _c_identifier_values(ctx)
    bad = []
    for r in cands:
        text = r["doc_text"]
        markers = [
            m
            for m in _DOC_MARKER.finditer(text)
            if int(re.search(r"0x\w+|\d+", m.group(0)).group(0), 0) not in named
        ]
        for m in [*_DOC_SENTINEL.finditer(text), *markers, *_DOC_ZERO_DEFAULT.finditer(text)]:
            if not _DOC_NEGATED.search(text[max(0, m.start() - 40) : m.start()]):
                bad.append(
                    (r, f"doc offers a sentinel ({m.group(0)!r}); document the omitted value")
                )
                break
    return len(cands), bad


def _union_members(text: str) -> list[str]:
    """Top-level members of a TS union; a `|` inside brackets or generics belongs to a member."""
    out, depth, cur = [], 0, ""
    for i, ch in enumerate(text):
        if ch in "(<[{":
            depth += 1
        elif ch in ")]}" or (ch == ">" and text[i - 1 : i] != "="):
            depth -= 1
        if ch == "|" and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def chk_null_member(ctx):
    """A record field spells "not given" or "not produced" as an omitted field, never `null`."""
    cands = [
        r
        for r in recs(ctx, JS, ("field",))
        if r.get("type") and (r["surface"], r["owner"]) not in ctx.documents
    ]
    bad = [
        (r, "type lists null; make the field optional and leave it out")
        for r in cands
        if "null" in _union_members(r["type"])
    ]
    return len(cands), bad


def chk_python_public_names(ctx):
    cands, bad = 0, []
    seen = set()
    for r in recs(ctx, ("python",), ("function", "method", "option", "field")):
        text = (
            r.get("type", "")
            if r["kind"] in ("option", "field")
            else " ".join(s.get("returns", "") for s in r.get("sigs", []))
        )
        text = re.sub(r"""(['"]).*?\1""", "", text)
        for m in re.finditer(r"(?<![\w.])(_*[A-Z]\w*)", text):
            name = m.group(1)
            cands += 1
            if name in _PY_ALLOWED or name in ctx.python_all:
                continue
            if (r["name"], name) in seen:
                continue
            seen.add((r["name"], name))
            bad.append((r, f"annotation names {name}, which libsonare does not export"))
    for r in recs(ctx, ("python",), KINDS):
        if r["owner"] or "in_all" not in r:
            continue
        if r.get("modules") == ["__init__"]:
            continue
        cands += 1
        if not r["in_all"]:
            bad.append(
                (
                    r,
                    "public in libsonare."
                    + ",".join(m for m in r["modules"] if m != "__init__")
                    + " but not in __all__",
                )
            )
    return cands, bad


def chk_cli_front_end_parity(ctx):
    """Compares the two front-ends from the whole inventory, reporting the selected side(s)."""
    cands, bad = 0, []
    every = [r for r in ctx.all_records if r["surface"] in CLI]
    selected = {r["surface"] for r in ctx.records} & set(CLI)
    cmds = {
        s: {
            r["name"]: r
            for r in every
            if r["surface"] == s and r["kind"] == "command" and not r.get("group")
        }
        for s in CLI
    }
    flags = {
        s: {r["name"]: r for r in every if r["surface"] == s and r["kind"] == "flag"} for s in CLI
    }
    skip = set(ctx.cli_only.get("python_only", [])) | set(ctx.cli_only.get("native_only", []))
    for s, other in (("cli-python", "cli-native"), ("cli-native", "cli-python")):
        if s not in selected:
            continue
        for name, r in sorted(cmds[s].items()):
            cands += 1
            if name in skip:
                continue
            if name not in cmds[other]:
                bad.append((r, f"missing on {other}"))
        for name, r in sorted(flags[s].items()):
            if r["owner"] in skip or r["owner"] not in cmds[other]:
                continue
            cands += 1
            if name not in flags[other]:
                bad.append((r, f"missing on {other}"))
    return cands, bad


_CLI_BAD = {"in", "smf", "midi2", "sr", "out"}


def chk_cli_positional_input(ctx):
    cands = recs(ctx, CLI, ("flag",))
    bad = []
    for r in cands:
        f = r["member"]
        if f in _CLI_BAD or (f == "input" and r["owner"] not in ("mix", "suggest-mix")):
            bad.append((r, f"--{f} names an input; take it positionally"))
        elif f.startswith("use-"):
            bad.append((r, f"--{f}: name the feature, not the act of using it"))
    return len(cands), bad


@dataclass
class Check:
    id: str
    surfaces: tuple[str, ...]
    fn: object


CHECKS: tuple[Check, ...] = (
    Check("seconds-suffix", ALL, chk_seconds_suffix),
    Check("unit-in-name", ALL, chk_unit_in_name),
    Check("unit-in-name:wrong-unit", ALL, chk_wrong_unit),
    Check("catalog-unit-suffix", ALL, chk_catalog_unit_suffix),
    Check("sample-rate-name", LANG, chk_sample_rate_name),
    Check("sample-rate-name:family", ("python",), chk_sample_rate_family),
    Check("sample-rate-required", BINDINGS, chk_sample_rate_required),
    Check("librosa-mirror:present", ("python",), chk_mirror_present),
    Check("teardown:js", JS, chk_teardown_js),
    Check("teardown:python", ("python",), chk_teardown_python),
    Check("no-internal-types", ("wasm",), chk_no_internal_types),
    Check("worklet-scope", JS, chk_worklet_scope),
    Check("progress-callback", LANG, chk_progress_callback),
    Check("options-struct:suffix", ("c",), chk_options_suffix),
    Check("options-struct:struct-size", ("c",), chk_options_struct_size),
    Check("c-free", ("c",), chk_c_free),
    Check("c-create", ("c",), chk_c_create),
    Check("c-count-types", ("c",), chk_c_count_types),
    Check("c-enum-field-doc", ("c",), chk_c_enum_field_doc),
    Check("getters", BINDINGS, chk_getters),
    Check("typed-results", JS, chk_typed_results),
    Check("array-results", ("python",), chk_array_results),
    Check("frozen-results", ("python",), chk_frozen_results),
    Check("snake-case-only", ("python",), chk_snake_case_only),
    Check("one-positional-form", JS, chk_one_positional_form),
    Check("no-positional-bools", JS, chk_no_positional_bools),
    Check("token-case", BINDINGS, chk_token_case),
    Check("token-case:name-table", BINDINGS, chk_name_tables),
    Check("digit-tokens", BINDINGS, chk_digit_tokens),
    Check("high-pass", ALL, chk_high_pass),
    Check("num-channels", LANG, chk_num_channels),
    Check("pitch-semitones", ALL, chk_pitch_semitones),
    Check("frequency-bounds", ALL, chk_frequency_bounds),
    Check("frequency-bounds:centre", ALL, chk_frequency_centre),
    Check("doc-comments", LANG, chk_doc_comments),
    Check("absent-not-sentinel:doc", BINDINGS + ("cli-python",), chk_doc_sentinel),
    Check("absent-not-sentinel:null", JS, chk_null_member),
    Check("python-public-names", ("python",), chk_python_public_names),
    Check("cli-front-end-parity", CLI, chk_cli_front_end_parity),
    Check("cli-positional-input", CLI, chk_cli_positional_input),
)
CHECK_IDS = {c.id for c in CHECKS}
# Inputs a check cannot read yet on one surface; reported as not compared, never as clean.
UNREAD = {
    "absent-not-sentinel:doc": (
        "cli-native",
        "native CLI help (the registry carries no flag descriptions)",
    ),
    "catalog-unit-suffix": (
        "c",
        (
            "whether each processor applies every parameter the catalog advertises for it"
            " (a shared band reader can publish fields a processor never reads)"
        ),
    ),
}


# ------------------------------------------------------------------ driver


def _split(v: str | None) -> list[str]:
    return [x.strip() for x in v.split(",") if x.strip()] if v else []


def selected_surfaces(arg: str | None) -> tuple[str, ...]:
    if not arg:
        return SURFACES
    out = []
    for s in _split(arg):
        if s == "cli":
            out += ["cli-python", "cli-native"]
        elif s in SURFACES:
            out.append(s)
        else:
            raise SystemExit(f"unknown surface {s!r}")
    return tuple(x for x in SURFACES if x in out)


def build_lookup(records):
    """Keys every record by (surface, kind, name, entry); a public constructor is a method."""
    lookup = set()
    for r in records:
        names = [(r["kind"], r["name"])]
        if r["kind"] == "class" and not r.get("ctor_private"):
            names.append(("method", f"{r['name']}.constructor"))
        for kind, name in names:
            if r["surface"] == "wasm":
                for e in r["entries"] or ["."]:
                    lookup.add((r["surface"], kind, name, e))
            else:
                lookup.add((r["surface"], kind, name, None))
    return lookup


def domain_splits(records, want_domains=frozenset()):
    """Top-level names (snake_case, surface prefix stripped) that land in more than one domain.

    Informational: returns sorted (name, [(surface, domain), ...]); a surface holding the name
    in two domains lists them joined by '/'.
    """
    by: dict = collections.defaultdict(lambda: collections.defaultdict(set))
    for r in records:
        if r["owner"] or r["kind"] not in domains.UNIFY_KINDS or r["surface"] not in LANG:
            continue
        by[domains.symbol_key(r["name"])][r["surface"]].add(r["domain"])
    out = []
    for name in sorted(by):
        per = by[name]
        found = set().union(*per.values())
        if len(found) < 2 or (want_domains and not found & set(want_domains)):
            continue
        out.append((name, [(s, "/".join(sorted(per[s]))) for s in LANG if s in per]))
    return out


def line_of(v) -> str:
    return f"{v['surface']} {v['kind']} {v['name']}: {v['message']}"


# What a finding must look like for its category to excuse it, and why one that does not is refused.
_CATEGORY_FITS = {
    "scalar": (
        lambda r, msg: _scalar_function(r),
        "a scalar function takes at most four number or boolean parameters and no options",
    ),
    "standard-symbol": (
        _standard_symbol,
        "a token with a word separator has a lowerCamel form",
    ),
    "distinct-operation": (
        lambda r, msg: (
            not _C_SUFFIX.search(r["name"])
            and " channel form of " not in msg
            and " sample format " not in msg
        ),
        "a fixed second-form suffix, a channel form or a sample format names no other operation",
    ),
}


def write_baseline(path: Path, records: list[dict]) -> None:
    """Write the baseline names the completeness and old-name checks read, one per line."""
    lines = ["\t".join(_BASELINE_COLUMNS)]
    for r in records:
        cells = [
            r["surface"],
            r["kind"],
            r["name"],
            ",".join(r["entries"] or []),
            r.get("owner") or "",
            r.get("member") or "",
            r.get("domain") or "",
            "1" if r.get("ctor_private") else "",
        ]
        if any("\t" in c or "\n" in c for c in cells):
            raise ValueError(f"baseline name cannot be written as a line: {cells!r}")
        lines.append("\t".join(cells))
    lines[1:] = sorted(set(lines[1:]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_baseline(path: Path) -> list[dict]:
    rows = path.read_text(encoding="utf-8").splitlines()
    if not rows or tuple(rows[0].split("\t")) != _BASELINE_COLUMNS:
        raise SystemExit(f"{path}: header is not {chr(9).join(_BASELINE_COLUMNS)!r}")
    out = []
    for line in rows[1:]:
        r = dict(zip(_BASELINE_COLUMNS, line.split("\t")))
        r["entries"] = r["entries"].split(",") if r["entries"] else []
        r["ctor_private"] = r["ctor_private"] == "1"
        out.append(r)
    return out


def run(args) -> tuple[int, list[str], dict]:
    root = args.root
    if args.baseline_inventory_file:
        base_inv = json.loads(Path(args.baseline_inventory_file).read_text(encoding="utf-8"))
        why = ""
    elif args.inventory_file:
        base_inv, why = None, "the inventory was read from a file; pass --baseline-inventory-file"
    elif args.write_baseline:
        import inventory

        base_inv, why = inventory.build_at_tag(root, BASELINE_TAG)
        if base_inv is None:
            line = f"baseline {BASELINE_TAG} cannot be built: {why}"
            return 1, [line], {"error": line}
        write_baseline(args.baseline_file, base_inv["records"])
        line = f"wrote {len(base_inv['records'])} {BASELINE_TAG} names to {args.baseline_file}"
        return 0, [line], {"written": line}
    else:
        # A default run compares against the release; one that cannot is a failure, not a pass.
        if not args.baseline_file.is_file():
            line = f"baseline {args.baseline_file} is missing; regenerate it with --write-baseline"
            return 1, [line], {"error": line}
        base_inv, why = {"records": read_baseline(args.baseline_file)}, ""
    out: list[str] = []
    failed = False
    if args.inventory_file:
        inv = json.loads(Path(args.inventory_file).read_text(encoding="utf-8"))
    else:
        import inventory

        inv = inventory.build(root)
    records = inv["records"]
    c_enum_fields = mark_c_enum_fields(records)
    selected = selected_surfaces(args.surface)
    want_domains = set(_split(args.domain))
    for d in want_domains:
        if d not in domains.DOMAINS:
            raise SystemExit(f"unknown domain {d!r}")
    matches = _split(args.match)
    check_ids = _split(args.check)
    skip_ids = _split(args.skip_check)
    # A domain, match, check or skip filter selects a slice in which zero candidates is legitimate.
    narrowed = bool(want_domains or matches or check_ids or skip_ids)
    for c in check_ids + skip_ids:
        if c not in CHECK_IDS:
            raise SystemExit(f"unknown check {c!r}")

    if args.regenerate_domains:
        args.domains_file.write_text(domains.regenerate(records), encoding="utf-8")
    votes = domains.vote(records)
    ties = {k: v for k, v in votes.items() if v["tie"]}

    problems: list[str] = list(inv.get("errors", []))
    rules = load_rules(args.rules_file, problems)
    contract_path = root / "tests" / "conformance" / "cli_contract_v2.json"
    example_problems, example_skipped = cli_example_problems(
        rules,
        records,
        base_inv["records"] if base_inv else None,
        json.loads(contract_path.read_text(encoding="utf-8")) if contract_path.is_file() else {},
    )
    problems += example_problems
    slugs = {r.get("slug") for r in rules}
    mirrors = load_mirrors(args.mirrors_file, problems)
    allow = load_allowlist(args.allowlist_file, problems, CHECK_IDS)
    dimensionless = load_dimensionless(args.dimensionless_file, problems, records)
    documents = load_document_types(args.document_types_file, problems, records, root)
    entries = load_ledgers(args.ledger_dir, problems)
    problems += validate_ledger(entries, slugs, root)
    frozen = load_domains(args.domains_file, problems)
    renames, ledger_problems = ledger_domains(entries, frozen)
    problems += ledger_problems
    unassigned = domains.assign(records, frozen, renames)
    domains.inherit(records)
    paths = collections.Counter(r["domain_path"] for r in records)

    if args.inventory:
        for r in records:
            if r["surface"] in selected:
                out.append(
                    "\t".join(
                        [
                            r["surface"],
                            r["kind"],
                            r["name"],
                            ",".join(r["entries"]),
                            r["domain"],
                            f"{r['file']}:{r['line']}",
                        ]
                    )
                )
        return 0, out, {}

    report: dict = {
        "files": inv.get("files", {}),
        "counts": inv.get("counts", {}),
        "domain_paths": {p: paths.get(p, 0) for p in domains.PATHS},
        "domain_unassigned": unassigned,
        "domain_ties": {
            k: {"domain": v["domain"], **v["surfaces"]} for k, v in sorted(ties.items())
        },
    }
    out.append("Inventory:")
    for s in SURFACES:
        nf, nr = inv.get("files", {}).get(s, 0), inv.get("counts", {}).get(s, 0)
        out.append(f"  {s}: scanned {nf} files, {nr} records")
        if s in selected and (nf == 0 or nr == 0):
            out.append(f"  FAIL {s}: a surface with no files or no records is a broken extractor")
            failed = True
    out.append(
        "Domain classification paths: "
        + ", ".join(f"{k}={v}" for k, v in report["domain_paths"].items())
    )
    if args.regenerate_domains:
        out.append(f"Regenerated {args.domains_file.name}: {len(votes)} names")
    out.append(f"domain vote: {len(votes)} names, {len(ties)} by tie-break")
    out.append(f"unassigned: {len(unassigned)}")
    out += [f"  {k}" for k in unassigned]

    out.append(f"Validation problems: {len(problems)}")
    out += [f"  {p}" for p in problems]
    out += [f"  not compared: {s}" for s in example_skipped]
    failed |= bool(problems)

    def keep_entry(le):
        if want_domains and le.data.get("domain") not in want_domains:
            return False
        return not matches or any(m in str(le.data.get("old", "")) for m in matches)

    lookup = build_lookup(records)
    by_key = {}
    for r in records:
        for e in (r["entries"] or ["."]) if r["surface"] == "wasm" else [None]:
            by_key[(r["surface"], r["kind"], r["name"], e)] = r
    baseline = build_lookup(base_inv["records"]) if base_inv else None
    lfail, changed = check_ledger(
        [e for e in entries if keep_entry(e)], lookup, selected, by_key, baseline
    )
    out.append(f"Ledger failures: {len(lfail)}")
    out += [f"  {line_of(f)}" for f in sorted(lfail, key=line_of)]
    if baseline is None:
        out.append(f"  not compared: ledger old names against {BASELINE_TAG} ({why})")
        out.append(f"  not compared: {BASELINE_TAG} names gone with no row ({why})")
        orphans = []
    elif want_domains or matches:
        out.append(f"  not compared: {BASELINE_TAG} names gone with no row (--domain / --match)")
        orphans = []
    else:
        orphans, tally = check_completeness(entries, lookup, baseline, selected)
        out.append(f"{BASELINE_TAG} names gone with no ledger row: {len(orphans)}")
        out += [f"  {o['surface']} {o['kind']} {o['name']}" for o in orphans]
        for s in sorted(selected):
            exempt = ", ".join(
                sorted({why for (es, _k), why in UNSPELLED_NAMES.items() if es == s})
            )
            out.append(
                f"  completeness {s}: compared={tally[(s, 'compared')]}"
                f" exempt={tally[(s, 'exempt')]}{f' ({exempt})' if exempt else ''}"
                f" own-row={tally[(s, 'own')]} owner-verified={tally[(s, 'owner')]}"
                f" uncovered={tally[(s, 'uncovered')]}"
            )
            if not tally[(s, "compared")]:
                # An extractor that stopped reading the baseline agrees with every ledger.
                out.append(f"    FAIL {s}: no {BASELINE_TAG} name of this surface was compared")
                lfail.append(
                    {
                        "surface": s,
                        "kind": "",
                        "name": "",
                        "domain": "",
                        "message": f"no {BASELINE_TAG} name of this surface was compared",
                    }
                )
        dom = {(r["surface"], r["kind"], r["name"]): r["domain"] for r in base_inv["records"]}
        for o in orphans:
            d = dom.get((o["surface"], o["kind"], o["name"].split(" [")[0]), "")
            lfail.append({**o, "domain": d, "message": f"in {BASELINE_TAG}, gone, and no row"})
    out.append(f"Changed entries: {len(changed)}")
    out += [
        f"  {c['surface']} {c['kind']} {c['name']}: {c['text']}"
        for c in sorted(changed, key=lambda c: (c["surface"], c["kind"], c["name"]))
    ]
    failed |= bool(lfail)

    ctx = Ctx(
        [r for r in records if r["surface"] in selected],
        mirrors,
        set(inv.get("python_all", [])),
        inv.get("cli_only", {}),
        all_records=records,
        dimensionless=dimensionless,
        documents=documents,
        catalog=json.loads(args.catalog_file.read_text(encoding="utf-8"))
        if args.catalog_file.is_file()
        else {},
    )
    table = collections.Counter()
    for f in lfail:
        table[(f["domain"], f["surface"])] += 1
    report["checks"] = []
    out.append("Checks:")
    for chk in CHECKS:
        if check_ids and chk.id not in check_ids:
            continue
        if chk.id in skip_ids:
            out.append(f"  check {chk.id}: skipped")
            out.append(f"    not compared: {chk.id} (excluded by --skip-check)")
            continue
        if not set(chk.surfaces) & set(selected):
            continue
        n, raw = chk.fn(ctx)
        viol = []
        excused = 0
        for r, msg in raw:
            v = {
                "surface": r["surface"],
                "kind": r["kind"],
                "name": r["name"],
                "domain": r["domain"],
                "message": msg,
            }
            hit = next(
                (
                    a
                    for a in allow
                    if a.check == chk.id
                    and fnmatch.fnmatchcase(r["name"], a.name)
                    and (
                        a.surface == r["surface"]
                        or (a.surface == "cli" and r["surface"].startswith("cli"))
                    )
                ),
                None,
            )
            if hit and hit.category in _CATEGORY_FITS:
                fits, why = _CATEGORY_FITS[hit.category]
                if not fits(r, msg):
                    v["message"] = f"{msg}; allowlisted as {hit.category}, but {why}"
                    hit = None
            if hit:
                hit.used += 1
                excused += 1
                continue
            viol.append(v)
        viol = [
            v
            for v in viol
            if (not want_domains or v["domain"] in want_domains)
            and (not matches or any(m in v["name"] for m in matches))
        ]
        viol.sort(key=line_of)
        suffix = f", excused {excused}" if excused else ""
        out.append(f"  check {chk.id}: checked {n}, violations {len(viol)}{suffix}")
        if chk.id == "doc-comments":
            out.append(f"    metadata-only doc: {sum(v['message'] == _META_ONLY for v in viol)}")
        if n == 0 and narrowed:
            out.append(f"    not compared: {chk.id} (no candidates under this filter)")
        elif n == 0:
            out.append(
                f"    FAIL {chk.id}: no candidates on the selected surfaces; its extractor found nothing"
            )
            failed = True
        out += [f"    {line_of(v)}" for v in viol]
        out += [f"    not compared: {x}" for x in sorted(ctx.not_compared.get(chk.id, []))]
        if chk.id in UNREAD and UNREAD[chk.id][0] in selected:
            out.append(f"    not compared: {UNREAD[chk.id][1]}")
        if chk.id == "unit-in-name" and "c" in selected:
            out.append(
                f"    not compared: {c_enum_fields} C int fields read as enum-coded"
                " (doc opens 'A @ref Sonare<Enum>')"
            )
        failed |= bool(viol)
        for v in viol:
            table[(v["domain"], v["surface"])] += 1
        report["checks"].append(
            {
                "id": chk.id,
                "checked": n,
                "violations": viol,
                "excused": excused,
                "not_compared": sorted(ctx.not_compared.get(chk.id, [])),
            }
        )

    stale_entries = False
    if args.audit_allowlist:
        if args.surface or args.domain or args.match or args.check or args.skip_check:
            out.append(
                "--audit-allowlist needs a full run;"
                " drop --surface / --domain / --match / --check / --skip-check"
            )
            return 2, out, report
        stale = [a for a in allow if a.used == 0]
        out.append(
            f"Allowlist audit: {len(stale)} entr{'y' if len(stale) == 1 else 'ies'} excused nothing"
        )
        out += [f"  allow {a.index}: {a.check} {a.surface} {a.name}" for a in stale]
        failed |= bool(stale)
        stale_entries = bool(stale)

    splits = domain_splits(ctx.records, want_domains)
    out.append(f"domain splits: {len(splits)}")
    out += [f"  {name}: " + ", ".join(f"{s}={d}" for s, d in per) for name, per in splits]
    report["domain_splits"] = {name: dict(per) for name, per in splits}

    cols = [s for s in SURFACES if s in selected]
    out.append("Failures by domain x surface:")
    out.append("  | domain | " + " | ".join(cols) + " | total |")
    out.append("  |---|" + "---|" * (len(cols) + 1))
    for d in domains.DOMAINS:
        row = [table[(d, s)] for s in cols]
        out.append(f"  | {d} | " + " | ".join(str(x) for x in row) + f" | {sum(row)} |")
    tot = [sum(table[(d, s)] for d in domains.DOMAINS) for s in cols]
    out.append("  | total | " + " | ".join(str(x) for x in tot) + f" | {sum(tot)} |")
    report.update(
        {
            "problems": problems,
            "ledger_failures": lfail,
            "changed": changed,
            "table": {f"{d}/{s}": table[(d, s)] for d in domains.DOMAINS for s in cols},
        }
    )
    code = 1 if failed else 0
    if args.audit_only:
        code = 1 if stale_entries or problems else 0
    report["exit"] = code
    return code, out, report


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--all", action="store_true", help="every surface, ledger and check (the default)"
    )
    ap.add_argument(
        "--surface", help="comma list of c, python, cli, wasm, node (cli = both front-ends)"
    )
    ap.add_argument("--domain", help="comma list of domains")
    ap.add_argument("--match", help="comma list of substrings (ledger old names, violation names)")
    ap.add_argument("--check", help="comma list of check ids")
    ap.add_argument("--skip-check", help="comma list of check ids to leave out of this run")
    ap.add_argument(
        "--regenerate-domains",
        action="store_true",
        help="rewrite the frozen name -> domain file from the inventory before checking",
    )
    ap.add_argument("--domains-file", type=Path, default=DEFAULT_DOMAINS)
    ap.add_argument("--audit-allowlist", action="store_true")
    ap.add_argument(
        "--audit-only",
        action="store_true",
        help="with --audit-allowlist, exit on stale allowlist entries alone, not on violations",
    )
    ap.add_argument("--inventory", action="store_true", help="print the inventory as TSV and exit")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--inventory-file", type=Path)
    ap.add_argument(
        "--baseline-inventory-file",
        type=Path,
        help=f"the {BASELINE_TAG} inventory as JSON, instead of --baseline-file",
    )
    ap.add_argument("--baseline-file", type=Path, default=DEFAULT_BASELINE)
    ap.add_argument(
        "--write-baseline",
        action="store_true",
        help=f"rebuild --baseline-file from the {BASELINE_TAG} tree with today's extractors and exit",
    )
    ap.add_argument("--ledger-dir", type=Path, default=DEFAULT_LEDGER_DIR)
    ap.add_argument("--rules-file", type=Path, default=DEFAULT_RULES)
    ap.add_argument("--mirrors-file", type=Path, default=DEFAULT_MIRRORS)
    ap.add_argument("--allowlist-file", type=Path, default=DEFAULT_ALLOWLIST)
    ap.add_argument("--dimensionless-file", type=Path, default=DEFAULT_DIMENSIONLESS)
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--catalog-file", type=Path, default=DEFAULT_CATALOG)
    ap.add_argument("--document-types-file", type=Path, default=DEFAULT_DOCUMENT_TYPES)
    return ap.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    args.audit_allowlist |= args.audit_only
    code, lines, report = run(args)
    if args.json and not args.inventory:
        json.dump(report, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
    else:
        sys.stdout.write("\n".join(lines) + "\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
