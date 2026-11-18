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
import ast
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


_BPM_BOUND = re.compile(r"^bpm_?(min|max)$", re.I)
_DEFAULT_BPM = {"min": 60.0, "max": 200.0}
_DOC_DEFAULT = re.compile(r"Default\s+`?(-?\d+(?:\.\d+)?)", re.I)


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


def _python_init_params(path: Path, cls: str) -> list[str] | None:
    """Parameter names of ``cls.__init__`` in a Python source file, or None when it has none."""
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"))
    except (OSError, SyntaxError):
        return None
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == cls:
            for item in node.body:
                if isinstance(item, ast.FunctionDef) and item.name == "__init__":
                    a = item.args
                    names = [x.arg for x in (*a.posonlyargs, *a.args, *a.kwonlyargs)]
                    return [
                        *names,
                        *([a.vararg.arg] if a.vararg else []),
                        *([a.kwarg.arg] if a.kwarg else []),
                    ]
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
            params = _python_init_params(ROOT / r["file"], r["name"])
            if params is not None:
                out.append(
                    {"surface": "python", "kind": "py-ctor", "name": r["name"], "params": params}
                )
    return out


def _construction_violation(r: dict) -> str | None:
    if r["kind"] == "js-ctor" and len(r["params"]) > 1:
        return "a synchronous constructor takes one config object"
    if r["kind"] == "js-create" and not r["returns"].startswith("Promise<"):
        return "static create() exists only for constructions that return a Promise"
    if r["kind"] == "py-ctor" and any(p in _PY_CONFIG_PARAMS for p in r["params"]):
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


def _ref_kind(type_: str) -> str:
    base = re.sub(r"\s*\|\s*(null|undefined|None)\b", "", type_).strip()
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
    out: list[dict] = []
    by_class: dict[tuple[str, str], list[str]] = {}
    by_method: dict[tuple[str, str, str], dict[str, set[str]]] = {}
    for r in params:
        group = _REF_PARAM.match(r["member"]).group(1)  # type: ignore[union-attr]
        cls, _, method = r["owner"].partition(".")
        by_class.setdefault((r["surface"], cls + ":" + group), []).append(r["type"])
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
            }
        )
    return out


def _ref_param_violation(r: dict) -> str | None:
    if r["group"] == "strip":
        if not r["type"].startswith("StripRef"):
            return "a strip parameter is a StripRef"
        return None
    if r["type"] != r["family_type"]:
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
    Rule(
        name="cancellation",
        summary="Promise-returning calls take signal; synchronous calls take cancel",
        population=lambda r: r["kind"] == "call",
        violation=_cancellation_violation,
        derive=_cancellation_calls,
        pending={
            f"{s}:VocalRenderSnapshot.{m}": "a synchronous render request takes cancel: () => boolean, not signal"
            for s in ("node", "wasm")
            for m in ("render", "beginRenderJob")
        },
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
        pending={
            f"{s}:{f}": "throw RangeError for an unmappable input, or state in the rule why the absent result stays"
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
        pending={
            "python:PlaybackRenderer": "take the renderer config as keyword arguments",
            "python:StreamAnalyzer": "take the stream config as keyword arguments",
            "python:StreamingMasteringChain": "take the chain config as keyword arguments",
            "python:VocalEditSession": "drop options= and **option_overrides, take keyword arguments",
            "wasm:ClipPageStreamer": "take one config object holding the engine",
            "wasm:SonarePlaybackWorkletProcessor": "fold port into the options object",
            "wasm:SonareRealtimeEngineWorkletProcessor": "fold transport into the options object",
            "wasm:SonareStreamAnalyzerWorkletProcessor": "fold port into the options object",
            "wasm:SonareWorkletProcessor": "fold transport into the options object",
            "wasm:VocalEditDraft": "declare the handle constructor private, as Node does",
            "wasm:VocalEditSession": "declare the handle constructor private, as Node does",
            "wasm:VocalRenderSnapshot": "declare the handle constructor private, as Node does",
            "wasm:VocalEditWorkerDraft": "declare the guarded constructor private",
            "wasm:VocalEditWorkerSession": "declare the guarded constructor private",
        },
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
        pending={
            "python:mastering_chain_stereo": "no Node/WASM counterpart; masterAudioStereo is the entry there, drop the Python-only twin or add the family",
            "python:mastering_process_stereo": "rename to mastering_apply_processor_stereo (Node/WASM masteringApplyProcessorStereo)",
            "python:mastering_pair_process_stereo": "rename to mastering_apply_pair_processor_stereo (Node/WASM masteringApplyPairProcessorStereo)",
            "python:normalize_rms_stereo": "Node/WASM normalizeStereo takes left/right; no normalizeRmsStereo exists there",
            "python:RealtimeVoiceChanger.process_planar_stereo": "Node/WASM RealtimeVoiceChanger.processPlanar takes channels",
            "python:RealtimeVoiceChanger.process_planar_stereo_with_carrier": "Node/WASM RealtimeVoiceChanger.processPlanarWithCarrier takes channels",
            "node:masteringApplyProcessorStereo": "Python spells it mastering_process_stereo",
            "wasm:masteringApplyProcessorStereo": "Python spells it mastering_process_stereo",
            "node:masteringApplyPairProcessorStereo": "Python spells it mastering_pair_process_stereo",
            "wasm:masteringApplyPairProcessorStereo": "Python spells it mastering_pair_process_stereo",
            "node:mixingAssistantSuggest": "MixAssistantTrack carries left/right; a mixing track takes channels",
            "wasm:mixingAssistantSuggest": "MixAssistantTrack carries left/right; a mixing track takes channels",
        },
    ),
    Rule(
        name="strip-ref",
        summary="strip params are StripRef; bus params keep one type per family and one kind per surface",
        population=lambda r: r["kind"] == "ref-param",
        violation=_ref_param_violation,
        derive=_strip_bus_records,
        pending={
            **{
                f"{s}:{c}.setBusStrip(bus)": "the definition object is not a bus reference; name the parameter for what it is"
                for s, c in (
                    ("node", "RealtimeEngine"),
                    ("wasm", "RealtimeEngine"),
                    ("wasm", "SonareEngine"),
                )
            },
            **{
                f"{s}:{c}.setStrip(strip)": "the definition object is not a strip reference; name the parameter for what it is"
                for s, c in (
                    ("node", "RealtimeEngine"),
                    ("wasm", "RealtimeEngine"),
                    ("wasm", "SonareEngine"),
                )
            },
            **{
                f"{s}:{c}.setTrackOutputBus(busId)": "busId is number | null here and number on every sibling; pick one form for 'no bus'"
                for s, c in (
                    ("node", "RealtimeEngine"),
                    ("wasm", "RealtimeEngine"),
                    ("wasm", "SonareEngine"),
                )
            },
            "python:Mixer.add_strip(strip_id)": "type the strip parameter StripRef, or rename it for a new strip's id",
            "python:Mixer.strip_by_id(strip_id)": "type the strip parameter StripRef",
        },
    ),
    Rule(
        name="payload-channels",
        summary="a <payload>Channels count sits beside <payload>SampleRate; capacities are exempt by prefix",
        population=lambda r: r["kind"] == "payload-channels",
        violation=_payload_channel_violation,
        derive=_payload_channel_records,
        pending={
            **{
                f"{s}:Mixer.{m}({side}Channels)": "send the declarations and what they count"
                for s in ("node", "wasm")
                for m in ("processStereo",)
                for side in ("left", "right")
            },
            **{
                f"wasm:Mixer.processStereoInto({side}Channels)": "send the declarations and what they count"
                for side in ("left", "right")
            },
            **{
                f"python:Mixer.process_stereo({side}_channels)": "send the declarations and what they count"
                for side in ("left", "right")
            },
        },
    ),
    Rule(
        name="type-names",
        summary="public JS/Python type names carry no Sonare prefix, except the error class",
        population=lambda r: (
            r["surface"] in ("node", "wasm", "python") and r["kind"] in ("type", "class")
        ),
        violation=lambda r: (
            "a public type drops the Sonare prefix" if r["name"].startswith("Sonare") else None
        ),
        exempt={
            "node:SonareError": _ERROR_CLASS_REASON,
            "wasm:SonareError": _ERROR_CLASS_REASON,
            "python:SonareError": _ERROR_CLASS_REASON,
            "python:SonareValueError": _ERROR_CLASS_REASON,
        },
        pending={
            key: "drop the Sonare prefix from the worklet types"
            for key in (
                "wasm:SonareClipPageRequestRingBuffer",
                "wasm:SonareClipPageRequestRingReadResult",
                "wasm:SonareEngine",
                "wasm:SonareEngineClipPageRequestMessage",
                "wasm:SonareEngineOptions",
                "wasm:SonareEngineSyncErrorMessage",
                "wasm:SonareEngineTelemetryError",
                "wasm:SonareEngineTelemetryRecord",
                "wasm:SonareEngineTelemetryRingBuffer",
                "wasm:SonareEngineTelemetryRingReadResult",
                "wasm:SonareEngineTelemetryType",
                "wasm:SonareEngineTransportFacade",
                "wasm:SonareExternalMidiRingBuffer",
                "wasm:SonareExternalMidiRingReadResult",
                "wasm:SonareMeterRingBuffer",
                "wasm:SonareMeterRingReadResult",
                "wasm:SonareModuleHandle",
                "wasm:SonarePlaybackNodeOptions",
                "wasm:SonarePlaybackWorkletDiagnostics",
                "wasm:SonarePlaybackWorkletProcessor",
                "wasm:SonarePlaybackWorkletProcessorOptions",
                "wasm:SonareRealtimeEngineNode",
                "wasm:SonareRealtimeEngineNodeCapabilities",
                "wasm:SonareRealtimeEngineNodeOptions",
                "wasm:SonareRealtimeEngineWorkletProcessor",
                "wasm:SonareRealtimeEngineWorkletProcessorOptions",
                "wasm:SonareRealtimeVoiceChangerWorkletProcessor",
                "wasm:SonareRealtimeVoiceChangerWorkletProcessorOptions",
                "wasm:SonareRenderOfflineRequest",
                "wasm:SonareRingCapacityOptions",
                "wasm:SonareScopeRingBuffer",
                "wasm:SonareScopeRingOptions",
                "wasm:SonareScopeRingReadResult",
                "wasm:SonareSpectrumRingBuffer",
                "wasm:SonareSpectrumRingOptions",
                "wasm:SonareSpectrumRingReadResult",
                "wasm:SonareStreamAnalyzerNode",
                "wasm:SonareStreamAnalyzerNodeOptions",
                "wasm:SonareStreamAnalyzerWorkletProcessor",
                "wasm:SonareStreamAnalyzerWorkletProcessorOptions",
                "wasm:SonareWorkletMeterSnapshot",
                "wasm:SonareWorkletProcessor",
                "wasm:SonareWorkletProcessorOptions",
                "wasm:SonareWorkletScopeSnapshot",
                "wasm:SonareWorkletSpectrumSnapshot",
            )
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
