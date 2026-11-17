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
tools/api/surface_allowlist.toml (four categories) or, for the Python surface of a
librosa-named function, by tools/api/librosa_mirrors.toml. ``--audit-allowlist``
fails on any allowlist entry that excused nothing in a full run.

Exit 0 only when nothing failed.
"""

from __future__ import annotations

import argparse
import collections
import fnmatch
import json
import re
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import domains

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
    "non-finite-is-absent",
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
    "python-public-names",
    "individual",
)
ALLOW_CATEGORIES = ("librosa-mirror", "platform", "wire-format", "hot-path")
EXAMPLE_SURFACES = ("wasm", "node", "python", "c", "cli")

DEFAULT_LEDGER_DIR = HERE / "migration"
DEFAULT_RULES = HERE / "migration" / "rules.toml"
DEFAULT_MIRRORS = HERE / "librosa_mirrors.toml"
DEFAULT_ALLOWLIST = HERE / "surface_allowlist.toml"
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
        if "removed" in e and e["removed"] is not True:
            bad.append("removed must be true")
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
    return problems


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
        new_key = _ledger_key(e["new"]) if "new" in e else None
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


def check_ledger(entries, lookup, selected):
    """Return (failures, changed) for valid entries on the selected surfaces."""
    failures, changed = [], []
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
            fail = None
            if "removed" in e:
                if here:
                    fail = "removed name still present"
            elif "new" in e:
                msgs = []
                if here:
                    msgs.append("old name still present")
                if (s, kind, e["new"], entry) not in lookup:
                    msgs.append(f"new name {e['new']} not present")
                fail = "; ".join(msgs) or None
            else:
                changed.append(
                    {
                        "surface": s,
                        "kind": kind,
                        "name": old + tag,
                        "domain": e["domain"],
                        "text": e["changed"],
                    }
                )
            if fail:
                failures.append(
                    {
                        "surface": s,
                        "kind": kind,
                        "name": old + tag,
                        "domain": e["domain"],
                        "message": fail,
                    }
                )
    return failures, changed


# ------------------------------------------------------------------ checks


@dataclass
class Ctx:
    records: list[dict]
    mirrors: dict[str, str]
    python_all: set[str]
    cli_only: dict[str, list[str]]
    by_owner: dict = field(default_factory=dict)
    all_records: list[dict] = field(default_factory=list)

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
    "db_per_oct",
    "samples",
    "frames",
    "sample",
    "frame",
    "lufs",
    "lu",
    "dbtp",
)


def _unit_split(name: str):
    """(stem, unit) when the snake name ends with stem or stem_unit, else None."""
    s = snake(name)
    for stem in _STEMS:
        if s == stem or s.endswith("_" + stem):
            return stem, None
        for unit in _UNITS:
            if s == f"{stem}_{unit}" or s.endswith(f"_{stem}_{unit}"):
                return stem, unit
    return None


def chk_unit_in_name(ctx):
    cands, bad = 0, []
    for r in recs(ctx, ALL, ("option", "field", "flag")):
        if ctx.mirror(r):
            continue
        hit = _unit_split(bare(r))
        if hit is None:
            continue
        cands += 1
        if hit[1] is None:
            bad.append((r, f"'{hit[0]}' names a physical quantity; add its unit suffix"))
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


def chk_sample_rate_required(ctx):
    cands, bad = 0, []
    handle_types = _js_handle_method_types(ctx)
    for r in recs(ctx, JS, ("type",)):
        if not re.search(r"(Request|Options)$", r["name"]):
            continue
        if (r["surface"], r["name"]) in handle_types:
            continue
        fields = {m["member"]: m for m in ctx.members(r) if m["kind"] == "field"}
        if "samples" not in fields and "channels" not in fields:
            continue
        cands += 1
        sr = fields.get("sampleRate")
        if sr is None or sr.get("optional"):
            bad.append((r, "takes samples but sampleRate is not a required field"))
        if "validate" not in fields:
            bad.append((r, "takes samples but has no validate field"))
    for r in recs(ctx, ("python",), ("function", "method")):
        if ctx.mirror(r) or not _py_rate_scoped(r):
            continue
        params = {p["name"]: p for s in r.get("sigs", []) for p in s["params"]}
        audio = bool(set(params) & _PY_AUDIO_PARAMS)
        sr = params.get("sample_rate")
        if not audio and sr is None:
            continue
        cands += 1
        if sr is not None and sr["optional"]:
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


def chk_options_suffix(ctx):
    cands = recs(ctx, ("c",), ("function",))
    bad = [
        (r, f"suffix {_C_SUFFIX.search(r['name']).group(0)} marks a second form of one operation")
        for r in cands
        if _C_SUFFIX.search(r["name"])
    ]
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
    {"sonare_free_string", "sonare_free_floats", "sonare_free_ints"}
)


def chk_c_free(ctx):
    cands, bad = 0, []
    types = recs(ctx, ("c",), ("type",))
    structs = {_c_snake(t["name"]) for t in types if t.get("c_kind") == "struct"}
    handles = {_c_snake(t["name"]) for t in types if t.get("c_kind") == "opaque"}
    for r in recs(ctx, ("c",), ("function",)):
        toks = r["name"].split("_")
        if not set(toks) & {"free", "destroy", "release"}:
            continue
        cands += 1
        n = r["name"]
        if n in _C_PLAIN_BUFFER_RELEASERS:
            continue
        m = re.fullmatch(r"sonare_free_(\w+)", n)
        if m and m.group(1) in structs:
            continue
        m = re.fullmatch(r"sonare_(\w+)_destroy", n)
        if m and m.group(1) in handles:
            continue
        bad.append(
            (
                r,
                "release is sonare_free_<result struct>, sonare_<handle>_destroy or a plain-buffer releaser",
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


def chk_c_count_types(ctx):
    cands, bad = 0, []
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
        cands += 1
        if t != want:
            bad.append((r, f"{n} is {want}, not {t}"))
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


def chk_array_results(ctx):
    cands, bad = 0, []
    for r in recs(ctx, ("python",), ("function", "method")):
        ret = " ".join(s.get("returns", "") for s in r.get("sigs", []))
        if not ret:
            continue
        cands += 1
        if re.search(r"\blist\[(float|int)\]", ret):
            bad.append((r, "returns a list of numbers; return an ndarray"))
        if re.search(r"(?<![\w.])tuple\[", ret) and not ctx.mirror(r):
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
    cands = recs(ctx, ("python",), ("function", "method", "option", "field"))
    bad = [(r, "camelCase name") for r in cands if re.search(r"[a-z][A-Z]", bare(r))]
    return len(cands), bad


def _positional_counts(r):
    return [len([p for p in s["params"] if not p.get("rest")]) for s in r.get("sigs", [])]


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


def chk_high_pass(ctx):
    cands = ctx.records
    bad = [
        (r, "spell high-pass as highPass / high_pass")
        for r in cands
        if re.search(r"hpf|Hpf|HPF|[hH]ighpass", bare(r))
    ]
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
    cands = [r for r in ctx.records if re.search(r"cent(re|er)", bare(r), re.IGNORECASE)]
    bad = [
        (r, "spell centre as center") for r in cands if re.search(r"centre", bare(r), re.IGNORECASE)
    ]
    return len(cands), bad


def chk_doc_comments(ctx):
    cands = recs(ctx, LANG, ("function", "method", "class", "type", "field", "export"))
    bad = [(r, "no doc comment") for r in cands if not r.get("has_doc")]
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
        for m in re.finditer(r"(?<![\w.])([A-Z]\w*)", text):
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
    cands, bad = 0, []
    cmds = {
        s: {r["name"]: r for r in recs(ctx, (s,), ("command",)) if not r.get("group")} for s in CLI
    }
    flags = {s: {r["name"]: r for r in recs(ctx, (s,), ("flag",))} for s in CLI}
    skip = set(ctx.cli_only.get("python_only", [])) | set(ctx.cli_only.get("native_only", []))
    for s, other in (("cli-python", "cli-native"), ("cli-native", "cli-python")):
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
    Check("python-public-names", ("python",), chk_python_public_names),
    Check("cli-front-end-parity", CLI, chk_cli_front_end_parity),
    Check("cli-positional-input", CLI, chk_cli_positional_input),
)
CHECK_IDS = {c.id for c in CHECKS}


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
    lookup = set()
    for r in records:
        if r["surface"] == "wasm":
            for e in r["entries"] or ["."]:
                lookup.add((r["surface"], r["kind"], r["name"], e))
        else:
            lookup.add((r["surface"], r["kind"], r["name"], None))
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


def run(args) -> tuple[int, list[str], dict]:
    root = args.root
    out: list[str] = []
    failed = False
    if args.inventory_file:
        inv = json.loads(Path(args.inventory_file).read_text(encoding="utf-8"))
    else:
        import inventory

        inv = inventory.build(root)
    records = inv["records"]
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
    slugs = {r.get("slug") for r in rules}
    mirrors = load_mirrors(args.mirrors_file, problems)
    allow = load_allowlist(args.allowlist_file, problems, CHECK_IDS)
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
    failed |= bool(problems)

    def keep_entry(le):
        if want_domains and le.data.get("domain") not in want_domains:
            return False
        return not matches or any(m in str(le.data.get("old", "")) for m in matches)

    lookup = build_lookup(records)
    lfail, changed = check_ledger([e for e in entries if keep_entry(e)], lookup, selected)
    out.append(f"Ledger failures: {len(lfail)}")
    out += [f"  {line_of(f)}" for f in sorted(lfail, key=line_of)]
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
        if n == 0 and narrowed:
            out.append(f"    not compared: {chk.id} (no candidates under this filter)")
        elif n == 0:
            out.append(
                f"    FAIL {chk.id}: no candidates on the selected surfaces; its extractor found nothing"
            )
            failed = True
        out += [f"    {line_of(v)}" for v in viol]
        failed |= bool(viol)
        for v in viol:
            table[(v["domain"], v["surface"])] += 1
        report["checks"].append(
            {"id": chk.id, "checked": n, "violations": viol, "excused": excused}
        )

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
    ap.add_argument("--inventory", action="store_true", help="print the inventory as TSV and exit")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--inventory-file", type=Path)
    ap.add_argument("--ledger-dir", type=Path, default=DEFAULT_LEDGER_DIR)
    ap.add_argument("--rules-file", type=Path, default=DEFAULT_RULES)
    ap.add_argument("--mirrors-file", type=Path, default=DEFAULT_MIRRORS)
    ap.add_argument("--allowlist-file", type=Path, default=DEFAULT_ALLOWLIST)
    ap.add_argument("--root", type=Path, default=ROOT)
    return ap.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    code, lines, report = run(args)
    if args.json and not args.inventory:
        json.dump(report, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
    else:
        sys.stdout.write("\n".join(lines) + "\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
