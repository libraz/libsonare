"""Hold the published surfaces to the interface conventions a reviewer used to check by eye.

Each rule is a predicate over the per-surface inventory (``tools/api/inventory.py``),
so a symbol enters the population the moment it is exported. A rule reports
three numbers: the records it read, the violations it found, and the exemptions
that excused one. Reading zero records fails, because a predicate that stopped
matching anything looks exactly like a clean surface.

Two tables sit beside each rule, both keyed by ``surface:name``:

* ``exempt`` -- a symbol the rule's own text places outside it, with the reason.
* ``pending`` -- a known violation with the fix it awaits.

A rule whose record aggregates several sites (one per C source file) also records
each pending entry's site count, and a count that moved either way fails: a fix
lowers it in the same change, and a new site is a violation rather than a guest.

An entry in either table that matches no violation fails, so each one expires with
the divergence it describes.

Run ``python3 tests/conformance/check_api_conventions.py [--rule NAME]``.
"""

from __future__ import annotations

import argparse
import ast
import dataclasses
import json
import os
import re
import sys
import tomllib
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
    # Counts the sites inside one record; such a rule's pending entries record that count.
    size: Callable[[dict], int] | None = None
    pending_size: dict[str, int] = dataclasses.field(default_factory=dict)


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


_TEARDOWN_SPELLINGS = ("destroy", "close", "dispose", "release", "free")
_FUNCTION_TYPE = re.compile(r"^\(.*\)\s*=>")


_PAIRED_CLASSES = ("RealtimeEngine",)
_SNAKE_PART = re.compile(r"_([a-z0-9])")


def _camel(name: str) -> str:
    return _SNAKE_PART.sub(lambda m: m.group(1).upper(), name)


def _counterpart_records(records: list[dict]) -> list[dict]:
    """One record per public method of a paired Python/Node class, marking a name the other lacks."""
    out: list[dict] = []
    for cls in _PAIRED_CLASSES:
        members = {
            surface: {
                r["member"]
                for r in records
                if r["surface"] == surface
                and r["kind"] == "method"
                and r.get("owner") == cls
                and not r["member"].startswith("_")
            }
            for surface in ("python", "node")
        }
        python_as_js = {_camel(m) for m in members["python"]}
        for m in sorted(members["python"]):
            out.append(
                {
                    "surface": "python",
                    "kind": "counterpart",
                    "name": f"{cls}.{m}",
                    "missing": f"node {cls}.{_camel(m)}"
                    if _camel(m) not in members["node"]
                    else None,
                }
            )
        for m in sorted(members["node"]):
            out.append(
                {
                    "surface": "node",
                    "kind": "counterpart",
                    "name": f"{cls}.{m}",
                    "missing": f"python {cls} method whose camelCase is {m}"
                    if m not in python_as_js
                    else None,
                }
            )
    return out


def _interface_teardown_records(records: list[dict]) -> list[dict]:
    """One record per JS interface or object type that carries destroy() or another teardown member."""
    callable_members: dict[tuple[str, str], set[str]] = {}
    for r in records:
        if not _js(r) or r.get("static"):
            continue
        if r["kind"] == "method" or (
            r["kind"] == "field" and _FUNCTION_TYPE.match(r.get("type", "")) is not None
        ):
            callable_members.setdefault((r["surface"], r["owner"]), set()).add(r["member"])
    out: list[dict] = []
    for r in records:
        if not (_js(r) and r["kind"] == "type" and r.get("ts_kind") in ("interface", "type")):
            continue
        names = callable_members.get((r["surface"], r["name"]), set())
        spelled = sorted(names.intersection(_TEARDOWN_SPELLINGS))
        if spelled:
            out.append(
                {
                    "surface": r["surface"],
                    "kind": "teardown-interface",
                    "name": r["name"],
                    "spelled": spelled,
                }
            )
    return out


def _interface_teardown_violation(record: dict) -> str | None:
    if "destroy" in record["spelled"]:
        return None
    return f"an interface with {'/'.join(record['spelled'])}() releases through destroy()"


_KEYS_OF = re.compile(r"const (\w+) = keysOf<(\w+)>\(\)\(\[([^\]]*)\]\)")
_KEY_SET = re.compile(r"const (\w+) = keySet\(([^)]*)\)")
_QUOTED = re.compile(r"'([^']+)'")


def _document_roots() -> set[str]:
    """Root type names declared in tools/api/document_types.toml."""
    data = tomllib.loads((ROOT / "tools/api/document_types.toml").read_text(encoding="utf-8"))
    return {entry["type"] for entry in data.get("document", [])}


_FUNCTION_HEAD = re.compile(
    r"(?m)^[ \t]*(?:export\s+)?(?:async\s+)?(?:function\s+)?(?:static\s+)?([A-Za-z_]\w*)\s*(?:<[^>()]*>)?\("
)
_NOT_FUNCTIONS = frozenset(
    {"if", "for", "while", "switch", "catch", "return", "super", "constructor"}
)
_KIND_CHECK_MESSAGE = re.compile(
    r"must be (?:an object|a configuration object|an array|a plain object)"
)
_THROW = re.compile(r"throw new \w*Error\(\s*([`'\"][^`'\"]*)")
_KEY_WALK = re.compile(r"Object\.(?:keys|entries|values)\(|for \(const \w+ in |Object\.hasOwn\(")
_PRESENCE_OR_KIND = re.compile(
    r"!?\s*[\w.]+\s*[!=]==?\s*(?:undefined|null)|typeof\s+[\w.]+\s*[!=]==?\s*'\w+'"
    r"|!?\s*Array\.isArray\([\w.]+\)|![\w.]+"
)
_IF_THROW = re.compile(
    r"if \(((?:[^()]|\([^()]*\))*)\)\s*\{?\s*throw new \w*Error\(\s*([`'\"][^`'\"]*)"
)


def _matching(text: str, start: int, open_: str, close: str) -> int:
    """Index just past the bracket closing the one at ``start``; strings are not skipped."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == open_:
            depth += 1
        elif text[i] == close:
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def _function_bodies(text: str):
    """(name, params, body) for every function or method head with a braced body."""
    for match in _FUNCTION_HEAD.finditer(text):
        if match.group(1) in _NOT_FUNCTIONS:
            continue
        params_end = _matching(text, match.end() - 1, "(", ")")
        tail = text[params_end : params_end + 200]
        brace = tail.find("{")
        if brace < 0 or any(stop in tail[:brace] for stop in (";", "=>", "\n\n")):
            continue
        body_start = params_end + brace
        yield (
            match.group(1),
            text[match.end() : params_end - 1],
            text[body_start : _matching(text, body_start, "{", "}")],
        )


def _document_reader_records(records: list[dict]) -> list[dict]:
    """One record per facade function handed a document, naming how it screens one, if it does.

    A document reaches a function as a parameter typed with a registered root or as a request
    field declared with one. A throw past the argument-kind check, or a walk over the document's
    keys, is a screen. A screen written without a throw or a recognised key walk is not seen.
    """
    roots = _document_roots()
    root_pattern = "|".join(sorted(roots))
    typed_param = re.compile(rf"(\w+)\??\s*:\s*(?:readonly\s+)?(?:{root_pattern})\b")
    document_fields = {
        r["member"]
        for r in records
        if _js(r)
        and r["kind"] == "field"
        and re.search(rf"\b(?:{root_pattern})\b", r.get("type") or "")
    }
    out: list[dict] = []
    for surface in ("node", "wasm"):
        for path in _ts_sources(ROOT, surface):
            text = path.read_text(encoding="utf-8")
            for name, params, body in _function_bodies(text):
                names = typed_param.findall(params)
                fields = [f for f in document_fields if re.search(rf"\w\.{f}\b", body)]
                if not names and not fields:
                    continue
                screens = None
                if names:
                    walked = _KEY_WALK.search(body)
                    refused = [m for m in _THROW.findall(body) if not _KIND_CHECK_MESSAGE.search(m)]
                    if walked:
                        screens = "walks the document's keys"
                    elif refused:
                        screens = f"refuses a value ({refused[0].strip('`')[:60]})"
                for condition, message in _IF_THROW.findall(body):
                    if screens or _KIND_CHECK_MESSAGE.search(message):
                        continue
                    judged = _PRESENCE_OR_KIND.sub("", condition)
                    if any(re.search(rf"\.{f}\b", judged) for f in fields):
                        screens = f"refuses a value ({message.strip('`')[:60]})"
                out.append(
                    {
                        "surface": surface,
                        "kind": "key-set",
                        "name": f"{path.stem}.{name}",
                        "screens": screens,
                        "reader": True,
                    }
                )
    return out


def _key_screen_records(records: list[dict]) -> list[dict]:
    """One record per facade key set, naming the document type it screens, if any."""
    roots = _document_roots()
    fields: dict[tuple[str, str], set[str]] = {}
    for r in records:
        if _js(r) and r["kind"] == "field" and r["owner"] in roots and "." not in r["member"]:
            fields.setdefault((r["surface"], r["owner"]), set()).add(r["member"])
    out: list[dict] = []
    for surface in ("node", "wasm"):
        text = (ROOT / "bindings" / surface / "src/request_keys.ts").read_text(encoding="utf-8")
        for name, typed, body in _KEYS_OF.findall(text):
            screened = typed if typed in roots else None
            out.append({"surface": surface, "kind": "key-set", "name": name, "screens": screened})
        for name, body in _KEY_SET.findall(text):
            keys = set(_QUOTED.findall(body))
            screened = next(
                (doc for (s, doc), names in fields.items() if s == surface and keys == names),
                None,
            )
            out.append({"surface": surface, "kind": "key-set", "name": name, "screens": screened})
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
    (re.compile(r"loudness_?range", re.IGNORECASE), "Lu"),
    (re.compile(r"true_?peak(?!_?oversample)", re.IGNORECASE), "Dbtp"),
    (re.compile(r"integrated|momentary|short_?term", re.IGNORECASE), "Lufs"),
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


_CANCEL_NAMES = ("cancel", "signal")
_TYPE_IDENT = re.compile(r"[A-Za-z_]\w*")


def _cancellation_calls(records: list[dict]) -> list[dict]:
    """One synthetic record per function or method that takes a cancel hook, direct or in a request."""
    hooks: dict[tuple[str, str], set[str]] = {}
    for r in records:
        if r["kind"] == "field" and r.get("member") in _CANCEL_NAMES:
            hooks.setdefault((r["surface"], r["owner"]), set()).add(r["member"])
    out: list[dict] = []
    for r in records:
        if r["kind"] not in ("function", "method") or r["surface"] not in (
            "node",
            "wasm",
            "python",
        ):
            continue
        for sig in r.get("sigs") or []:
            takes: set[str] = set()
            for p in sig.get("params", []):
                if p["name"] in _CANCEL_NAMES:
                    takes.add(p["name"])
                found: set[str] = set()
                for ident in _TYPE_IDENT.findall(p.get("type", "")):
                    found |= hooks.get((r["surface"], ident), set())
                # A worker request type omits the hook that cannot cross a thread.
                if re.search(r"WorkerRequest<|Omit<[^>]*'cancel'", p.get("type", "")):
                    found.discard("cancel")
                takes |= found
            if takes:
                promise = (sig.get("returns") or "").startswith("Promise<")
                out.append(
                    {
                        "surface": r["surface"],
                        "kind": "call",
                        "name": r["name"],
                        "promise": promise,
                        "takes": sorted(takes),
                    }
                )
                break
    return out


def _cancellation_violation(record: dict) -> str | None:
    if record["promise"] and "cancel" in record["takes"]:
        return "a Promise-returning call takes { signal }, not cancel"
    if not record["promise"] and "signal" in record["takes"]:
        return "a synchronous call takes cancel, not signal"
    return None


_BPM_BOUND = re.compile(r"^bpm_?(min|max)$", re.IGNORECASE)
_DEFAULT_BPM = {"min": 60.0, "max": 200.0}
_DOC_DEFAULT = re.compile(r"Default\s+`?(-?\d+(?:\.\d+)?)", re.IGNORECASE)


def _tempo_bound_violation(r: dict) -> str | None:
    side = _BPM_BOUND.match(r["member"]).group(1).lower()  # type: ignore[union-attr]
    if r["surface"] == "python":
        # An omitted bound defers to the core, whose default is the documented one.
        if r.get("type") != "float | None":
            return "a tempo bound is omitted by default (float | None) so the core's 60-200 applies"
        return None
    match = _DOC_DEFAULT.search(r.get("doc_text", ""))
    if not match:
        return "the doc states the default tempo bound"
    if float(match.group(1)) != _DEFAULT_BPM[side]:
        return (
            f"the default tempo bound is {_DEFAULT_BPM[side]:g} BPM, the doc says {match.group(1)}"
        )
    return None


_MIDI_NAME = re.compile(r"^(midi2?|gm)[A-Z]|^perNoteControllerName$")
_ABSENT_RETURN = re.compile(r"\|\s*null\b|\|\s*-1\b|\bnull\s*\||-1\s*\|")


def _midi_function_violation(r: dict) -> str | None:
    if r["kind"] == "method":
        return "a MIDI/GM helper is a module function, not a Project static"
    sig = (r.get("sigs") or [{}])[0]
    returns = sig.get("returns") or ""
    if _ABSENT_RETURN.search(returns):
        return "a lookup returns its value and throws RangeError instead of returning null or -1"
    params = sig.get("params", [])
    if returns.startswith("ProjectMidiEvent") and [p["name"] for p in params] != ["request"]:
        return "a message packer takes one request object"
    return None


_PY_CONFIG_PARAMS = ("options", "config", "overrides")
_PY_PACKAGE = ROOT / "bindings/python/src/libsonare"
_PY_DOCUMENT_FORMS = {"dict[str, Any]", "Mapping[str, Any]"}


def _python_document_aliases() -> set[str]:
    """Module-level aliases the package declares for a free-form JSON document."""
    aliases: set[str] = set()
    for path in sorted(_PY_PACKAGE.glob("*.py")):
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"))
        except (OSError, SyntaxError):
            continue
        for node in tree.body:
            target, value = None, None
            if isinstance(node, ast.Assign) and len(node.targets) == 1:
                target, value = node.targets[0], node.value
            elif isinstance(node, ast.AnnAssign):
                target, value = node.target, node.value
            if (
                isinstance(target, ast.Name)
                and value is not None
                and ast.unparse(value) in _PY_DOCUMENT_FORMS
            ):
                aliases.add(target.id)
    return aliases


def _python_is_document(annotation: ast.expr | None, aliases: set[str]) -> bool:
    """True when the annotation names a document alias, optionally beside None.

    A bare dict/Mapping annotation is not a document: it is the untyped option bag the
    construction rule refuses, so only a declared package alias exempts. JSON text is not
    accepted beside it; text enters only through an entry named for it (``*_json``).
    """
    if annotation is None:
        return False
    members: list[ast.expr] = []
    stack = [annotation]
    while stack:
        node = stack.pop()
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr):
            stack += [node.left, node.right]
        else:
            members.append(node)
    forms = [ast.unparse(m) for m in members]
    documents = [f for f in forms if f in aliases]
    return bool(documents) and all(f in documents or f == "None" for f in forms)


def _python_init_params(path: Path, cls: str) -> tuple[list[str], list[str]] | None:
    """Parameter names of ``cls.__init__`` and those typed as a library document, or None."""
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"))
    except (OSError, SyntaxError):
        return None
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == cls:
            for item in node.body:
                if isinstance(item, ast.FunctionDef) and item.name == "__init__":
                    a = item.args
                    args = [*a.posonlyargs, *a.args, *a.kwonlyargs]
                    names = [
                        *(x.arg for x in args),
                        *([a.vararg.arg] if a.vararg else []),
                        *([a.kwarg.arg] if a.kwarg else []),
                    ]
                    aliases = _python_document_aliases()
                    documents = [x.arg for x in args if _python_is_document(x.annotation, aliases)]
                    return names, documents
    return None


def _construction_records(records: list[dict]) -> list[dict]:
    out: list[dict] = []
    for r in records:
        if r["kind"] == "class" and _js(r) and not r.get("ctor_private"):
            # An Error subclass keeps the platform Error constructor shape.
            if "Error" in (r.get("bases") or []):
                continue
            params = [
                p["name"] for sig in (r.get("sigs") or [{}])[:1] for p in sig.get("params", [])
            ]
            out.append(
                {"surface": r["surface"], "kind": "js-ctor", "name": r["name"], "params": params}
            )
        elif r["kind"] == "method" and _js(r) and r.get("static") and r.get("member") == "create":
            out.append(
                {
                    "surface": r["surface"],
                    "kind": "js-create",
                    "name": r["name"],
                    "returns": _returns(r) or "",
                }
            )
        elif r["kind"] == "class" and r["surface"] == "python":
            found = _python_init_params(ROOT / r["file"], r["name"])
            if found is not None:
                params, documents = found
                out.append(
                    {
                        "surface": "python",
                        "kind": "py-ctor",
                        "name": r["name"],
                        "params": params,
                        "documents": documents,
                    }
                )
    return out


def _construction_violation(r: dict) -> str | None:
    if r["kind"] == "js-ctor" and len(r["params"]) > 1:
        return "a synchronous constructor takes one config object"
    if r["kind"] == "js-create" and not r["returns"].startswith("Promise<"):
        return "static create() exists only for constructions that return a Promise"
    # A library document (mixer scene, playback config, mastering chain) is the value the
    # object is built from, not an option bag, so a `config` typed by a document alias is exempt.
    documents = [p for p in r.get("documents", []) if p == "config"]
    if r["kind"] == "py-ctor" and any(
        p in _PY_CONFIG_PARAMS and p not in documents for p in r["params"]
    ):
        return "a Python constructor takes keyword arguments, not an options or config object"
    return None


_EX_SUFFIX = re.compile(r"(Ex|_ex)$")


_TUPLE = re.compile(r"(?:readonly\s+)?\[\s*[A-Za-z_]")
_ARRAYISH = re.compile(r"\[\]|Array<|^\s*(?:readonly\s+)?\[")
_INPUT_OWNER = re.compile(r"(Request|Options|Config|Input|Params)$")
_BUILTIN_ARRAYS = {
    "Array",
    "ReadonlyArray",
    "Float32Array",
    "Float64Array",
    "Uint8Array",
    "Int16Array",
    "Int32Array",
    "Uint32Array",
    "Uint16Array",
}


_PY_LIST = re.compile(r"\b(Sequence|list|Iterable)\[")
_PY_PLAIN = {"Sequence", "Iterable", "Mapping", "Any", "None"}


def _list_input_population(r: dict) -> bool:
    if r["surface"] == "python":
        return r["kind"] in ("field", "option") and _PY_LIST.search(r.get("type", "")) is not None
    if not _js(r) or r["kind"] not in ("field", "option"):
        return False
    if r["kind"] == "field" and not _INPUT_OWNER.search(r.get("owner", "")):
        return False
    return _ARRAYISH.search(r.get("type", "")) is not None


def _list_input_violation(r: dict) -> str | None:
    t = r.get("type", "")
    if r["surface"] == "python":
        records = [i for i in re.findall(r"[A-Z]\w+", t) if i not in _PY_PLAIN]
        if "Mapping[" in t and records:
            return "a list input takes its record class only, no mapping beside it"
        return None
    if not _TUPLE.search(t):
        return None
    items = [i for i in re.findall(r"[A-Z]\w+", _TUPLE.sub("", t)) if i not in _BUILTIN_ARRAYS]
    if items:
        return "a list input takes its record form only, no tuple form beside it"
    return None


_PROBE_NAME = re.compile(r"(Version|_version|Support|_support|Available|_available)$")
_ABI_OK = re.compile(r"^version$|[Aa]biVersion$|abi_version$")


def _probe_violation(r: dict) -> str | None:
    name = r["name"]
    if name.endswith(("Version", "_version")):
        if _ABI_OK.search(name):
            return None
        return "a version probe is *AbiVersion(); features come from capabilities().features"
    return "a feature probe is capabilities().features.<name>, not has*Support / *Available"


_STEREO_PARAM = re.compile(r"^(source|reference)?_?(left|right)(_?channels)?$")
_TWO_CHANNEL_ALGORITHM = re.compile(r"mid|side|width|pan|correlation|scope|goniometer")
_STEREO_SUFFIX = re.compile(r"stereo(into)?$")
_ALL_SURFACES = ("node", "wasm", "python")


def _fold(name: str) -> str:
    """Case- and underscore-insensitive name, without a trailing Into/Async variant marker."""
    return re.sub(r"(into|async)$", "", name.replace("_", "").lower())


def _stereo_calls(records: list[dict]) -> list[dict]:
    """One record per callable that takes a left/right pair, or is a *Stereo member."""
    direct: dict[tuple[str, str], set[str]] = {}
    nested: dict[tuple[str, str], set[str]] = {}
    for r in records:
        # A flag pair (polarity invert) is not an audio input.
        if (
            r["kind"] == "field"
            and r["surface"] in ("node", "wasm")
            and not r.get("type", "").startswith("boolean")
        ):
            direct.setdefault((r["surface"], r["owner"]), set()).add(r["member"])
            nested.setdefault((r["surface"], r["owner"]), set()).update(
                _TYPE_IDENT.findall(r.get("type", ""))
            )
    request_fields = {
        key: members.union(*(direct.get((key[0], t), set()) for t in nested.get(key, ())))
        for key, members in direct.items()
    }
    calls: list[dict] = []
    for r in records:
        if r["kind"] not in ("function", "method") or r["surface"] not in _ALL_SURFACES:
            continue
        sigs = r.get("sigs") or []
        if not sigs:
            continue
        names = {p["name"] for p in sigs[0].get("params", [])}
        for p in sigs[0].get("params", []):
            for ident in _TYPE_IDENT.findall(p.get("type", "")):
                names |= request_fields.get((r["surface"], ident), set())
        takes_pair = any(_STEREO_PARAM.match(_fold(n)) for n in names)
        member = r.get("member") or r["name"]
        if takes_pair or _STEREO_SUFFIX.search(_fold(member)):
            calls.append(
                {
                    "surface": r["surface"],
                    "kind": "stereo-call",
                    "name": r["name"],
                    "family": ("", _fold(member)),
                    "pair": takes_pair,
                    "channels": "channels" in names,
                    "stereo_member": _STEREO_SUFFIX.search(_fold(member)) is not None,
                }
            )
    families: dict[tuple[str, str], set[str]] = {}
    for c in calls:
        if c["stereo_member"]:
            families.setdefault(c["family"], set()).add(c["surface"])
    return [{**c, "family_surfaces": sorted(families.get(c["family"], ()))} for c in calls]


def _stereo_pair_violation(c: dict) -> str | None:
    if _TWO_CHANNEL_ALGORITHM.search(c["family"][1]):
        return None
    if not c["stereo_member"]:
        return "only a two-channel algorithm takes left and right; other multichannel inputs take channels"
    missing = [s for s in _ALL_SURFACES if s not in c["family_surfaces"]]
    if missing:
        return f"a *Stereo member exists on every surface; absent on {', '.join(missing)}"
    if c["channels"]:
        return "a *Stereo member takes left and right, never channels"
    return None


_REF_PARAM = re.compile(r"^(strip|bus)(_?[Ii]d)?$")


def _non_null(type_: str) -> str:
    """The type with its null / undefined / None arm removed: a null bus id names the master mix."""
    return re.sub(r"\s*\|\s*(null|undefined|None)\b", "", type_).strip()


def _ref_kind(type_: str) -> str:
    base = _non_null(type_)
    if base in ("number", "int", "float"):
        return "number"
    if base in ("string", "str"):
        return "string"
    return base


def _strip_bus_records(records: list[dict]) -> list[dict]:
    params = [
        r
        for r in records
        if r["surface"] in ("node", "wasm", "python")
        and _REF_PARAM.match(r.get("member", ""))
        and (
            r["kind"] == "option"
            or (r["kind"] == "field" and _INPUT_OWNER.search(r.get("owner", "")))
        )
    ]
    first_param = {
        (r["surface"], r["name"]): params[0]["name"]
        for r in records
        if r["kind"] == "method"
        for params in [(r.get("sigs") or [{}])[0].get("params", [])]
        if params
    }
    out: list[dict] = []
    by_class: dict[tuple[str, str], list[str]] = {}
    by_method: dict[tuple[str, str, str], dict[str, set[str]]] = {}
    for r in params:
        group = _REF_PARAM.match(r["member"]).group(1)  # type: ignore[union-attr]
        cls, _, method = r["owner"].partition(".")
        by_class.setdefault((r["surface"], cls + ":" + group), []).append(_non_null(r["type"]))
        by_method.setdefault((cls, _fold(method), group), {}).setdefault(r["surface"], set()).add(
            _ref_kind(r["type"])
        )
    for r in params:
        group = _REF_PARAM.match(r["member"]).group(1)  # type: ignore[union-attr]
        cls, _, method = r["owner"].partition(".")
        siblings = by_class[(r["surface"], cls + ":" + group)]
        common = max(set(siblings), key=siblings.count)
        others = {
            s: kinds
            for s, kinds in by_method[(cls, _fold(method), group)].items()
            if s != r["surface"]
        }
        out.append(
            {
                "surface": r["surface"],
                "kind": "ref-param",
                "name": r["name"],
                "group": group,
                "type": r["type"],
                "family_type": common,
                "other_kinds": {s: sorted(k) for s, k in others.items()},
                "creates": _fold(method) == f"add{group}"
                and first_param.get((r["surface"], r["owner"])) == r["member"],
            }
        )
    return out


def _ref_param_violation(r: dict) -> str | None:
    # The first parameter of add_strip / addStrip is the id of a strip being created, not a
    # reference to an existing one.
    if r.get("creates"):
        return None
    if r["group"] == "strip":
        if not r["type"].startswith("StripRef"):
            return "a strip parameter is a StripRef"
        return None
    if _non_null(r["type"]) != r["family_type"]:
        return f"a bus parameter carries one type across its family ({r['family_type']})"
    kind = _ref_kind(r["type"])
    for surface, kinds in r["other_kinds"].items():
        if kind not in kinds:
            return f"a bus id is the same kind on every surface ({surface}: {', '.join(kinds)})"
    return None


_CAPACITY_PREFIXES = ("max", "min", "in", "out", "input", "output")


def _payload_prefix(member: str) -> str | None:
    """The payload word before ``Channels`` / ``_channels``, or None when the name is not qualified."""
    if member in ("channels", "numChannels", "num_channels"):
        return None
    camel = re.fullmatch(r"([a-z][A-Za-z0-9]*?)Channels", member)
    snake = re.fullmatch(r"([a-z0-9_]+?)_channels", member)
    match = camel or snake
    return match.group(1) if match else None


def _payload_channel_records(records: list[dict]) -> list[dict]:
    siblings: dict[tuple[str, str], set[str]] = {}
    for r in records:
        if r["kind"] in ("option", "field") and r["surface"] in ("node", "wasm", "python"):
            siblings.setdefault((r["surface"], r["owner"]), set()).add(r.get("member", ""))
    out: list[dict] = []
    for r in records:
        if r["kind"] not in ("option", "field") or r["surface"] not in ("node", "wasm", "python"):
            continue
        prefix = _payload_prefix(r.get("member", ""))
        if prefix is None:
            continue
        snake = r["surface"] == "python"
        rate = f"{prefix}_sample_rate" if snake else f"{prefix}SampleRate"
        out.append(
            {
                "surface": r["surface"],
                "kind": "payload-channels",
                "name": r["name"],
                "member": r["member"],
                "prefix": prefix,
                "has_rate": rate in siblings[(r["surface"], r["owner"])],
            }
        )
    return out


def _payload_channel_violation(r: dict) -> str | None:
    first = re.split(r"(?=[A-Z])|_", r["prefix"])[0].lower()
    if (
        first in _CAPACITY_PREFIXES
        or r["member"].lower().endswith("numchannels")
        or r["member"].endswith("_num_channels")
    ):
        return None
    if not r["has_rate"]:
        return f"a payload-qualified channel count sits beside its {r['prefix']} sample rate"
    return None


_ERROR_CLASS_REASON = "the error class keeps its Sonare prefix"


_REGISTERED_CLASS_REASON = "registered with Web Audio or as a global"
_REGISTERED_CLASSES = (
    "wasm:SonareEngine",
    "wasm:SonarePlaybackNode",
    "wasm:SonarePlaybackWorkletProcessor",
    "wasm:SonareRealtimeEngineNode",
    "wasm:SonareRealtimeEngineWorkletProcessor",
    "wasm:SonareRealtimeVoiceChangerWorkletProcessor",
    "wasm:SonareStreamAnalyzerNode",
    "wasm:SonareStreamAnalyzerWorkletProcessor",
    "wasm:SonareWorkletProcessor",
)


def _type_name_violation(r: dict) -> str | None:
    """Flag a Sonare-prefixed type unless it is a registered class or named for one."""
    name = r["name"]
    if not name.startswith("Sonare"):
        return None
    owners = [
        key.split(":", 1)[1] for key in _REGISTERED_CLASSES if key.split(":", 1)[0] == r["surface"]
    ]
    if any(
        name.startswith(owner) and name[len(owner) : len(owner) + 1].isupper() for owner in owners
    ):
        return None
    return "a public type drops the Sonare prefix"


_Q8 = re.compile(r"q8", re.IGNORECASE)


_RESULT_OWNER = re.compile(r"Result$")
_SUMMARY_NAME = re.compile(
    r"^(num[A-Z]\w*|num_\w+|\w+Count|\w+_count|has[A-Z]\w*|has_\w+|isEmpty|is_empty)$"
)
_ARRAY_FIELD = re.compile(r"\[\]|Array|ndarray|Sequence|^list\[|^tuple\[")


def _summary_stems(member: str) -> list[str]:
    """Names of the field a derived summary member would summarise."""
    if member in ("isEmpty", "is_empty"):
        return []
    words = re.split(r"_", member) if "_" in member else re.findall(r"[A-Z]?[a-z0-9]+", member)
    words = [w.lower() for w in words]
    if words[0] in ("num", "has"):
        stem = words[1:]
    elif words[-1] == "count":
        stem = words[:-1]
    else:
        return []
    base = (
        "_".join(stem)
        if "_" in member
        else "".join(w if i == 0 else w.capitalize() for i, w in enumerate(stem))
    )
    return [base, base + "s", base.removesuffix("s")]


def _derived_summary_records(records: list[dict]) -> list[dict]:
    arrays: dict[tuple[str, str], set[str]] = {}
    for r in records:
        if (
            r["kind"] == "field"
            and r["surface"] in ("node", "wasm", "python")
            and _ARRAY_FIELD.search(r.get("type", ""))
        ):
            arrays.setdefault((r["surface"], r["owner"]), set()).add(r["member"])
    out: list[dict] = []
    for r in records:
        if r["kind"] != "field" or r["surface"] not in ("node", "wasm", "python"):
            continue
        if not _RESULT_OWNER.search(r["owner"]) or not _SUMMARY_NAME.match(r["member"]):
            continue
        beside = arrays.get((r["surface"], r["owner"]), set())
        stems = _summary_stems(r["member"])
        out.append(
            {
                "surface": r["surface"],
                "kind": "summary-field",
                "name": r["name"],
                "summarises": bool(beside) if not stems else bool(beside & set(stems)),
            }
        )
    return out


_COUNT_MEMBER = re.compile(r"^(?P<stem>\w+?)(?:Count|_count)$")


def _collection_records(records: list[dict]) -> list[dict]:
    members: dict[tuple[str, str], set[str]] = {}
    for r in records:
        if (
            r["kind"] in ("method", "field")
            and r["surface"] in ("node", "wasm", "python")
            and r["owner"]
        ):
            members.setdefault((r["surface"], r["owner"]), set()).add(r["member"])
    out: list[dict] = []
    for (surface, owner), names in members.items():
        for name in sorted(names):
            match = _COUNT_MEMBER.match(name)
            if not match or "." in owner:
                continue
            stem = match.group("stem")
            candidates = (f"{stem}ByIndex", f"{stem}_by_index", f"{stem}At", f"{stem}_at")
            index = [c for c in candidates if c in names]
            out.append(
                {
                    "surface": surface,
                    "kind": "count-member",
                    "name": f"{owner}.{name}",
                    "index": index,
                }
            )
    return out


_NEW_ERROR = re.compile(r"\bnew\s+(\w*Error)\(")
_COMMENT_LINE = re.compile(r"^\s*(//|/\*|\*)")


def _ts_sources(root: Path, surface: str) -> list[Path]:
    return sorted(
        p
        for p in (root / "bindings" / surface / "src").rglob("*.ts")
        if not p.name.endswith(".d.ts")
    )


def _error_construction_sites(root: Path) -> list[dict]:
    """One record per `new *Error(` site in the Node and WASM TypeScript sources."""
    out: list[dict] = []
    for surface in ("node", "wasm"):
        for path in _ts_sources(root, surface):
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if _COMMENT_LINE.match(line) or "cause:" in line:
                    continue
                for match in _NEW_ERROR.finditer(line):
                    out.append(
                        {
                            "surface": surface,
                            "kind": "error-site",
                            "name": f"{path.relative_to(root).as_posix()}:{number}",
                            "class": match.group(1),
                        }
                    )
    return out


_QUANTUM_LITERAL = re.compile(r"(?<![\w.])128(?![\w.])")
_QUANTUM_DEFINITION = re.compile(r"AUDIO_WORKLET_RENDER_QUANTUM\s*=\s*128")
_RING_CONTEXT = re.compile(r"capacity|RingOptions|resolveRing", re.IGNORECASE)
_MIDI_CONTEXT = re.compile(r"midi|velocity|program|controller", re.IGNORECASE)


def _worklet_quantum_sites(root: Path) -> list[dict]:
    """One record per bare 128 in the worklet sources, with the context it sits in."""
    src = root / "bindings" / "wasm" / "src"
    files = [*sorted((src / "worklet").rglob("*.ts")), src / "public_types_worklet.ts"]
    out: list[dict] = []
    for path in files:
        if not path.is_file() or path.name.endswith(".d.ts"):
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        for number, line in enumerate(lines, 1):
            if _COMMENT_LINE.match(line) or not _QUANTUM_LITERAL.search(line):
                continue
            if _QUANTUM_DEFINITION.search(line):
                continue
            window = " ".join(lines[max(0, number - 5) : number])
            context = (
                "ring-capacity"
                if _RING_CONTEXT.search(window)
                else ("midi" if _MIDI_CONTEXT.search(window) else None)
            )
            out.append(
                {
                    "surface": "wasm",
                    "kind": "quantum-literal",
                    "name": f"{path.relative_to(root).as_posix()}:{number}",
                    "context": context,
                }
            )
    return out


_DIMENSION_FIELD = re.compile(
    r"^n(Bins|Frames|Mels|Rows|Cols|Coeffs|Mfcc|Bands|Chroma)$|^num(Rows|Cols|Bins|Frames)$"
)
_ROW_COL_FIELD = re.compile(r"^(rows|cols|nRows|nCols|numRows|numCols)$")


def _matrix_records(records: list[dict]) -> list[dict]:
    members: dict[tuple[str, str], set[str]] = {}
    out: list[dict] = []
    for r in records:
        if r["kind"] == "field" and _js(r):
            members.setdefault((r["surface"], r["owner"]), set()).add(r["member"])
            if r["owner"].endswith("Result"):
                out.append(r)
    for (surface, owner), names in members.items():
        if owner != "Matrix2D" and "data" in names and any(_ROW_COL_FIELD.match(n) for n in names):
            out.append(
                {"surface": surface, "kind": "matrix-shape", "name": owner, "member": "data"}
            )
    return out


def _matrix_violation(r: dict) -> str | None:
    if r["kind"] == "matrix-shape":
        return "a two-dimensional value is Matrix2D { data, rows, cols }"
    if _DIMENSION_FIELD.match(r["member"]):
        return (
            "a Matrix2D carries its dimensions; a result has no per-result nBins / nFrames / nMels"
        )
    return None


_C_LEXICAL = re.compile(
    r"R\"(\w*)\(.*?\)\1\"|//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.DOTALL
)
_C_ENTRY = re.compile(r"^[A-Za-z_][^\n(){};]*?\b(sonare_\w+)\s*\(", re.MULTILINE)
_C_DEFINITION = re.compile(
    r"^[ \t]*(?:[A-Za-z_][^\n;{}()=]*?)\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{",
    re.MULTILINE | re.DOTALL,
)
_C_BASE_SETTERS = ("set_last_error", "set_last_error_static", "record_current_exception")
_C_NOT_A_FUNCTION = frozenset(
    {"if", "for", "while", "switch", "catch", "return", "constexpr", "get", "operator"}
)
_C_ERROR_RETURN = re.compile(r"\breturn\s+(SONARE_ERROR_(?!OK\b)\w+)\s*;")


def _c_blank(text: str) -> str:
    """Source text with comments and literals blanked, newlines kept."""
    return _C_LEXICAL.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def _c_match(text: str, open_at: int) -> int:
    """Index just past the delimiter closing the one at ``open_at``."""
    pairs = {"(": ")", "{": "}"}
    opener, closer, depth = text[open_at], pairs[text[open_at]], 0
    for i in range(open_at, len(text)):
        if text[i] == opener:
            depth += 1
        elif text[i] == closer:
            depth -= 1
            if depth == 0:
                return i + 1
    raise ValueError("unbalanced delimiter")


def _c_entry_bodies(code: str) -> list[tuple[str, int, str]]:
    """(name, body offset, body) of each file-scope ``sonare_*`` definition."""
    out = []
    for match in _C_ENTRY.finditer(code):
        try:
            after = _c_match(code, code.index("(", match.end() - 1))
        except ValueError:
            continue
        head = code[after:].lstrip()
        brace, semi = head.find("{"), head.find(";")
        if brace < 0 or 0 <= semi < brace or re.search(r"[;})]", head[:brace]):
            continue
        start = after + (len(code) - after - len(head)) + brace
        try:
            end = _c_match(code, start)
        except ValueError:
            continue
        out.append((match.group(1), start + 1, code[start + 1 : end - 1]))
    return out


def _c_message_setters(codes: list[str]) -> set[str]:
    """Names of functions, directly or through another, that record the error message."""
    bodies = []
    for code in codes:
        for match in _C_DEFINITION.finditer(code):
            if match.group(1) in _C_NOT_A_FUNCTION:
                continue
            try:
                bodies.append(
                    (match.group(1), code[match.end() - 1 : _c_match(code, match.end() - 1)])
                )
            except ValueError:
                continue
    setters = set(_C_BASE_SETTERS)
    grew = True
    while grew:
        grew = False
        for name, body in bodies:
            if name not in setters and any(re.search(rf"\b{s}\s*\(", body) for s in setters):
                setters.add(name)
                grew = True
    return setters


def _c_unset_returns(body: str, setters: set[str]) -> list[tuple[int, str]]:
    """Offsets of non-OK literal returns with no message-setting call on their path."""
    token = re.compile(
        r"[{}]|" + _C_ERROR_RETURN.pattern + r"|\b(?:" + "|".join(sorted(setters)) + r")\s*\("
    )
    stack, bad = [False], []
    for match in token.finditer(body):
        text = match.group(0)
        if text == "{":
            stack.append(False)
        elif text == "}":
            if len(stack) > 1:
                stack.pop()
        elif text.startswith("return"):
            if not any(stack):
                bad.append((match.start(), match.group(1)))
        else:
            stack[-1] = True
    return bad


def _c_error_path_files(root: Path) -> list[dict]:
    """One record per C-ABI source file, counting error returns that set no message."""
    paths = sorted((root / "src" / "c_api").glob("*.cpp"))
    sources = {
        p: _c_blank(p.read_text(encoding="utf-8"))
        for p in [*paths, *sorted((root / "src" / "c_api").glob("*.h"))]
    }
    setters = _c_message_setters(list(sources.values()))
    out: list[dict] = []
    for path in paths:
        code = sources[path]
        unset: list[str] = []
        entries = _c_entry_bodies(code)
        for name, offset, body in entries:
            for at, _ in _c_unset_returns(body, setters):
                unset.append(f"{name}:{code.count(chr(10), 0, offset + at) + 1}")
        if entries:
            out.append(
                {
                    "surface": "c",
                    "kind": "c-error-path",
                    "name": path.relative_to(root).as_posix(),
                    "unset": unset,
                }
            )
    return out


def _c_error_path_violation(r: dict) -> str | None:
    if r["unset"]:
        return f"{len(r['unset'])} non-OK returns set no error message, first {r['unset'][0]}"
    return None


_C_ERROR_PATH_PENDING: dict[str, int] = {
    "src/c_api/core_analysis.cpp": 21,
    "src/c_api/core_audio.cpp": 21,
    "src/c_api/core_cqt.cpp": 2,
    "src/c_api/core_quick.cpp": 19,
    "src/c_api/features_inverse.cpp": 48,
    "src/c_api/features_music.cpp": 7,
    "src/c_api/features_segment.cpp": 14,
    "src/c_api/features_spectral_pitch.cpp": 44,
    "src/c_api/features_spectrogram_mel.cpp": 6,
    "src/c_api/features_streaming.cpp": 24,
    "src/c_api/mixing_assistant.cpp": 2,
    "src/c_api/mixing_graph.cpp": 6,
    "src/c_api/mixing_mixer.cpp": 29,
    "src/c_api/mixing_strip.cpp": 49,
    "src/c_api/project_bounce.cpp": 11,
    "src/c_api/project_core.cpp": 41,
    "src/c_api/project_edit_clip.cpp": 76,
    "src/c_api/project_edit_track.cpp": 84,
    "src/c_api/project_external_stems.cpp": 6,
    "src/c_api/project_midi.cpp": 39,
    "src/c_api/project_mir.cpp": 25,
    "src/c_api/project_part_rig.cpp": 11,
    "src/c_api/project_soundfont.cpp": 5,
    "src/c_api/project_timeline.cpp": 1,
    "src/c_api/project_vocal_edit.cpp": 7,
    "src/c_api/sonare_c_acoustic.cpp": 7,
    "src/c_api/sonare_c_assist.cpp": 2,
    "src/c_api/sonare_c_compat.cpp": 35,
    "src/c_api/sonare_c_daw.cpp": 44,
    "src/c_api/sonare_c_editing.cpp": 41,
    "src/c_api/sonare_c_effects.cpp": 40,
    "src/c_api/sonare_c_engine.cpp": 64,
    "src/c_api/sonare_c_engine_capture.cpp": 21,
    "src/c_api/sonare_c_engine_clips.cpp": 15,
    "src/c_api/sonare_c_engine_controller.cpp": 19,
    "src/c_api/sonare_c_engine_midi.cpp": 99,
    "src/c_api/sonare_c_engine_mixing.cpp": 108,
    "src/c_api/sonare_c_engine_render.cpp": 37,
    "src/c_api/sonare_c_engine_timeline.cpp": 1,
    "src/c_api/sonare_c_mastering_apply.cpp": 10,
    "src/c_api/sonare_c_mastering_chain.cpp": 4,
    "src/c_api/sonare_c_mastering_dynamics.cpp": 3,
    "src/c_api/sonare_c_mastering_eq.cpp": 13,
    "src/c_api/sonare_c_mastering_preset.cpp": 7,
    "src/c_api/sonare_c_mastering_repair.cpp": 12,
    "src/c_api/sonare_c_mastering_streaming.cpp": 25,
    "src/c_api/sonare_c_memory.cpp": 1,
    "src/c_api/sonare_c_playback.cpp": 20,
    "src/c_api/sonare_c_polyphony.cpp": 15,
    "src/c_api/sonare_c_sample_bank.cpp": 9,
    "src/c_api/sonare_c_streaming_retune.cpp": 10,
    "src/c_api/sonare_c_transcribe.cpp": 2,
    "src/c_api/sonare_c_voice_changer.cpp": 40,
    "src/c_api/synth_catalog.cpp": 1,
}


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
    ),
    Rule(
        name="document-key-screen",
        summary=(
            "a document the core parses has one refuser, the core: no facade key set screens it, "
            "and a facade function handed one refuses nothing past its kind; a screen written "
            "without a throw or a recognised key walk is not seen"
        ),
        population=lambda r: r["kind"] == "key-set",
        violation=lambda r: (
            None
            if not r["screens"]
            else f"{r['screens']}; the core is the one refuser of a document"
            if r.get("reader")
            else f"{r['screens']} is a document; the core refuses its keys at every depth as InvalidFormat"
        ),
        derive=lambda records: [*_key_screen_records(records), *_document_reader_records(records)],
        pending={
            **{
                key: "goes when the chain document reaches the core as written"
                for key in (
                    "node:CHAIN_CONFIG_SECTIONS",
                    "wasm:MasteringChainConfigKeys",
                    "node:chain_config.flattenChainConfig",
                    "wasm:chain_config.flattenChainConfig",
                )
            },
        },
    ),
    Rule(
        name="interface-teardown",
        summary="a JS interface that can be released carries destroy(), not only close/dispose/release/free",
        population=lambda r: r["kind"] == "teardown-interface",
        violation=_interface_teardown_violation,
        derive=_interface_teardown_records,
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
    Rule(
        name="cancellation",
        summary="Promise-returning calls take signal; synchronous calls take cancel",
        population=lambda r: r["kind"] == "call",
        violation=_cancellation_violation,
        derive=_cancellation_calls,
    ),
    Rule(
        name="tempo-range",
        summary="every public tempo search defaults to 60-200 BPM, on the C struct, the JS requests and Python",
        population=lambda r: (
            r["surface"] in ("node", "wasm", "python", "c")
            and r["kind"] in ("field", "option")
            and _BPM_BOUND.match(r.get("member", "")) is not None
            and not (r["surface"] == "c" and r["kind"] == "option")
        ),
        violation=_tempo_bound_violation,
        exempt={
            "python:cyclic_tempogram(bpm_min)": "a fold base, not a search range",
        },
    ),
    Rule(
        name="midi-module-functions",
        summary="midi*/midi2*/gm* helpers are module functions: request-object packers, throwing lookups",
        population=lambda r: (
            _js(r)
            and (
                (r["kind"] == "function" and _MIDI_NAME.match(r["name"]) is not None)
                or (
                    r["kind"] == "method"
                    and bool(r.get("static"))
                    and r["owner"] == "Project"
                    and _MIDI_NAME.match(r["member"]) is not None
                )
            )
        ),
        violation=_midi_function_violation,
        # These map a valid input through a binding table that need not cover it, so null means
        # "maps to nothing" (the C ABI's INVALID_STATE, and only that); an invalid argument throws.
        exempt={
            f"{s}:{f}": "null is a well-formed input the bindings do not map; invalid arguments throw"
            for s in ("node", "wasm")
            for f in ("midiCcLearn", "midiCcToBreakpoint", "midiParamToCc")
        },
    ),
    Rule(
        name="construction",
        summary="sync construction is new X(config) or keyword arguments; create() is async",
        population=lambda r: r["kind"] in ("js-ctor", "js-create", "py-ctor"),
        violation=_construction_violation,
        derive=_construction_records,
    ),
    Rule(
        name="input-types",
        summary="no public function or method carries an Ex / _ex suffix variant",
        population=lambda r: (
            r["surface"] in ("node", "wasm", "python") and r["kind"] in ("function", "method")
        ),
        violation=lambda r: (
            "an operation has one entry point, not an Ex variant"
            if _EX_SUFFIX.search(r.get("member") or r["name"])
            else None
        ),
    ),
    Rule(
        name="one-input-form",
        summary="a list-of-items input takes one item form: no tuple beside the record",
        population=_list_input_population,
        violation=_list_input_violation,
    ),
    Rule(
        name="abi-probes",
        summary="versions are *AbiVersion(); feature presence comes from capabilities().features",
        population=lambda r: (
            r["surface"] in ("node", "wasm", "python")
            and r["kind"] == "function"
            and _PROBE_NAME.search(r["name"]) is not None
        ),
        violation=_probe_violation,
        exempt={"wasm:isWebMidiAvailable": "probes the host, not the library"},
    ),
    Rule(
        name="stereo-pair",
        summary="left/right only for two-channel algorithms and *Stereo families on every surface",
        population=lambda r: r["kind"] == "stereo-call",
        violation=_stereo_pair_violation,
        derive=_stereo_calls,
    ),
    Rule(
        name="strip-ref",
        summary="strip params are StripRef; bus params keep one type per family and one kind per surface",
        population=lambda r: r["kind"] == "ref-param",
        violation=_ref_param_violation,
        derive=_strip_bus_records,
    ),
    Rule(
        name="payload-channels",
        summary="a <payload>Channels count sits beside <payload>SampleRate; capacities are exempt by prefix",
        population=lambda r: r["kind"] == "payload-channels",
        violation=_payload_channel_violation,
        derive=_payload_channel_records,
    ),
    Rule(
        name="type-names",
        summary=(
            "public JS/Python type names carry no Sonare prefix, except the error class, the "
            "registered classes and the companion types named for them"
        ),
        population=lambda r: (
            r["surface"] in ("node", "wasm", "python") and r["kind"] in ("type", "class")
        ),
        violation=_type_name_violation,
        exempt={
            "node:SonareError": _ERROR_CLASS_REASON,
            "wasm:SonareError": _ERROR_CLASS_REASON,
            "python:SonareError": _ERROR_CLASS_REASON,
            "python:SonareValueError": _ERROR_CLASS_REASON,
            **{key: _REGISTERED_CLASS_REASON for key in _REGISTERED_CLASSES},
        },
    ),
    Rule(
        name="engine-counterparts",
        summary=(
            "every public method of a paired Python/Node class has a counterpart of the same name "
            "after snake/camel mapping; it cannot see a pair whose names agree but whose arguments "
            "or behaviour differ, a class outside the paired list, or WASM, which parity holds to Node"
        ),
        population=lambda r: r["kind"] == "counterpart",
        violation=lambda r: f"no counterpart: {r['missing']}" if r["missing"] else None,
        derive=_counterpart_records,
        exempt={
            "python:RealtimeEngine.close": "Python releases a handle through close()",
            "node:RealtimeEngine.destroy": "JS releases a handle through destroy()",
            "node:RealtimeEngine.[Symbol.dispose]": "the JS explicit-resource-management protocol",
        },
    ),
    Rule(
        name="engine-vocabulary",
        summary="Q8 fixed-point stays in C: no q8 name on the JS or Python surface",
        population=lambda r: (
            r["surface"] in ("node", "wasm", "python")
            and r["kind"] in ("field", "option", "method", "function", "type", "class")
        ),
        violation=lambda r: (
            "a Q8 fixed-point value is a plain fractional number outside C"
            if _Q8.search(r["name"])
            else None
        ),
    ),
    Rule(
        name="derived-summary",
        summary="a result record carries no count, flag or emptiness summary of a list it already holds",
        population=lambda r: r["kind"] == "summary-field",
        violation=lambda r: (
            "a result record carries no derived summary beside the field it derives from"
            if r["summarises"]
            else None
        ),
        derive=_derived_summary_records,
    ),
    Rule(
        name="collections",
        summary="a collection is one method returning an array, not a *Count beside an index getter",
        population=lambda r: r["kind"] == "count-member",
        violation=lambda r: (
            f"a count and {r['index'][0]} pair is replaced by one collection method"
            if r["index"]
            else None
        ),
        derive=_collection_records,
    ),
    Rule(
        name="error-class-by-cause",
        summary="a Node/WASM facade raises SonareError, TypeError or RangeError, never a plain Error",
        population=lambda r: r["kind"] == "error-site",
        violation=lambda r: (
            "a failure is a coded SonareError (or TypeError / RangeError for an argument), not a plain Error"
            if r["class"] == "Error"
            else None
        ),
        scan=_error_construction_sites,
    ),
    Rule(
        name="worklet-render-quantum",
        summary="the 128-frame render quantum is spelled AUDIO_WORKLET_RENDER_QUANTUM in the worklet sources",
        population=lambda r: r["kind"] == "quantum-literal",
        violation=lambda r: (
            "a bare 128 in the worklet sources goes through AUDIO_WORKLET_RENDER_QUANTUM"
            if r["context"] is None
            else None
        ),
        scan=_worklet_quantum_sites,
    ),
    Rule(
        name="matrix-2d",
        summary="2-D JS results are Matrix2D; no per-result dimension fields on a *Result record",
        population=lambda r: r["kind"] == "matrix-shape" or r["kind"] == "field",
        violation=_matrix_violation,
        derive=_matrix_records,
        exempt={
            f"{s}:MfccResult.nMels": "echo of the request parameter nMels; not a dimension of any array in the result"
            for s in ("node", "wasm")
        },
    ),
    Rule(
        name="c-error-path",
        summary="every non-OK return of a C-ABI entry point sets sonare_last_error_message()",
        population=lambda r: r["kind"] == "c-error-path",
        violation=_c_error_path_violation,
        scan=_c_error_path_files,
        size=lambda r: len(r["unset"]),
        pending={
            f"c:{path}": "set the error message on each validation return, which records none today"
            for path in _C_ERROR_PATH_PENDING
        },
        pending_size={f"c:{path}": n for path, n in _C_ERROR_PATH_PENDING.items()},
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
            if rule.size is not None:
                held, measured = rule.pending_size.get(key), rule.size(record)
                if held != measured:
                    violations.append(
                        Finding(
                            key,
                            f"pending entry holds {held} sites and the source has {measured}: "
                            "a fix lowers the count, a new site is a violation",
                        )
                    )
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
