"""Parity facts for tools that need the extractors' pairing without the report.

``field_bindings`` exposes how each C-ABI parameter and struct field is spelled
on each facade, using the same pairing the comparisons use: canonical snake_case
name equality, never position. A C subject whose canonical name no facade
parameter or field shares has no binding on that surface (``surface_spelling``
is ``None``), and a facade name with no C counterpart on the same key is its own
row with ``c_subject`` ``None``. A rename therefore shows as one of each on the
same key; nothing here guesses that the two are the same subject.

Only entries a surface exposes are reported for it: by the same key, as the
class method a handle op strips to, or under a coverage alias. A function or
record the facade omits entirely is the coverage check's business. Rows carry
the key the C layer reports a refusal under, a second form folded into its base.
A caller-filled ``const T*`` struct is a subject through its members unless the
facade takes the record mirroring it, whose interior the record rows cover; the
members bind to keyword arguments or to fields of an object-typed parameter, as
does a plain C argument a facade folds into such a request. A facade bag-named
parameter (``config``, ``request``, ...) nothing binds into is reported as an
unpaired facade name, since it may be where a C parameter was folded (a C
``preset`` taken as keyword ``config``).
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Literal

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import check_parity
import compare
import normalize
from extractors import c_api
from model import Extraction, FunctionSig, Param, RecordShape

# Canonical spellings of the sample rate, which C marks structural.
_RATE_NAMES = frozenset({"sample_rate", "sr"})
# Entry suffixes the C layer folds away when it keys a refusal, in its match order.
# A ``const T*`` parameter whose ``T`` is a C record: a struct the caller fills in.
_CALLER_FILLED = re.compile(r"const\s+(\w+)\s*\*")
# A pointer to an opaque ``Sonare`` type: the handle a class method is called on.
_HANDLE_ARG = re.compile(r"(?:const\s+)?(Sonare\w+)\s*\*")
_FOLDED_SUFFIXES = ("_with_progress", "_with_options", "_checked", "_ex2", "_ex", "_v2")

Surface = Literal["node", "wasm", "python"]
FACADES: tuple[Surface, ...] = ("node", "wasm", "python")


@dataclass(frozen=True)
class FieldBinding:
    """One C subject on one facade, or one facade name with no C subject.

    Attributes:
        c_key: Canonical key of the C function or record.
        c_subject: C spelling of the parameter or struct field; ``None`` for a
            facade name that pairs with no C subject on this key.
        kind: Whether the subject is a function parameter or a record field.
        surface: The facade.
        surface_spelling: The facade's spelling; ``None`` when the C subject has
            no facade binding.
        file: Facade location, or the C location when unbound.
        line: Line of ``file``.
    """

    c_key: str
    c_subject: str | None
    kind: Literal["param", "record_field"]
    surface: Surface
    surface_spelling: str | None
    file: str
    line: int


def _record_of(type_hint: str, records: dict[str, RecordShape]) -> RecordShape | None:
    """The record a parameter's type names, by any identifier in the hint (``readonly X[]``).

    Matched by canonical record key, so Python's public name finds its ctypes mirror.
    """
    by_key = {r.key: r for r in records.values()}
    for token in re.findall(r"[A-Za-z_]\w*", type_hint):
        rec = records.get(token) or by_key.get(normalize.canonical_record_key(token, "c"))
        if rec is not None:
            return rec
    return None


def _caller_filled(p: Param, c_records: dict[str, RecordShape]) -> RecordShape | None:
    """The C record a ``const T*`` parameter points at, or None when it is not one."""
    m = _CALLER_FILLED.fullmatch(p.type.strip())
    return c_records.get(m.group(1)) if m else None


def _names_record(type_hint: str, records: dict[str, RecordShape], key: str) -> bool:
    """Whether any identifier in ``type_hint`` (a union, say) names the record ``key``."""
    return any(
        (rec := _record_of(token, records)) is not None and rec.key == key
        for token in re.findall(r"[A-Za-z_]\w*", type_hint)
    )


def _takes(sig: FunctionSig, c_rec: RecordShape, records: dict[str, RecordShape]) -> str | None:
    """The facade parameter typed as the record mirroring ``c_rec``, if any."""
    return next(
        (
            p.name
            for p in sig.params
            if (rec := _record_of(p.type, records)) is not None and rec.key == c_rec.key
        ),
        None,
    )


def _member_rows(
    c_sig: FunctionSig,
    c_rec: RecordShape,
    sig: FunctionSig,
    facade: dict[str, Param],
    records: dict[str, RecordShape],
    surface: Surface,
) -> list[FieldBinding]:
    """A caller-filled struct's members as subjects of the entry that takes it.

    A facade spells them as keywords of the call or as fields of an object-typed
    parameter; whichever it uses is consumed and so not reported as unpaired.
    """
    # A trailing options object is structural to the comparisons but still holds members.
    bags = {p.name: rec for p in sig.params if (rec := _record_of(p.type, records)) is not None}
    rows: list[FieldBinding] = []
    for f in filter(c_api.is_member_a_caller_sets, c_rec.fields):
        spelling = None
        if f.name in facade:
            spelling = facade.pop(f.name)
            spelling = spelling.raw_name or spelling.name
        else:
            for rec in bags.values():
                field_ = next((x for x in rec.fields if x.name == f.name), None)
                if field_ is not None:
                    spelling = field_.raw_name or field_.name
                    break
        rows.append(
            FieldBinding(
                c_sig.key,
                f.raw_name or f.name,
                "param",
                surface,
                spelling,
                sig.file if spelling else c_sig.file,
                sig.line if spelling else c_sig.line,
            )
        )
    for name in bags:
        facade.pop(name, None)
    return rows


def _param_rows(
    c_sig: FunctionSig,
    sig: FunctionSig,
    surface: Surface,
    c_records: dict[str, RecordShape] | None = None,
    records: dict[str, RecordShape] | None = None,
) -> list[FieldBinding]:
    c_records = c_records or {}
    records = records or {}
    rows: list[FieldBinding] = []
    facade = {p.name: p for p in sig.core_params() if p.name not in compare._CALLBACK_NAMES}
    # Fields of object-typed facade parameters, where a facade folds C arguments into a request.
    folded = {
        f.name: (p.name, f)
        for p in sig.params
        if (rec := _record_of(p.type, records)) is not None
        for f in rec.core_fields()
    }
    used_bags: set[str] = set()
    c_names = set()
    for p in c_sig.core_params():
        c_rec = _caller_filled(p, c_records)
        same = facade.get(p.name)
        if same is None and p.name in folded:
            bag, field_ = folded[p.name]
            used_bags.add(bag)
            c_names.add(p.name)
            rows.append(
                FieldBinding(
                    c_sig.key,
                    p.raw_name or p.name,
                    "param",
                    surface,
                    field_.raw_name or field_.name,
                    sig.file,
                    sig.line,
                )
            )
            continue
        renamed = (
            same is not None
            and c_rec is not None
            and _record_of(same.type, records) is not None
            and not _names_record(same.type, records, c_rec.key)
        )
        if c_rec is not None and (same is None or renamed):
            if (mirror := _takes(sig, c_rec, records)) is not None:
                # The facade takes the mirrored struct; its interior is the record rows' job.
                facade.pop(mirror, None)
                continue
            rows.extend(_member_rows(c_sig, c_rec, sig, facade, records, surface))
            continue
        if p.name in compare._STRUCT_BAG_NAMES:
            # A C struct argument is reached through the record rows; a facade bag of
            # the same name is that struct, not an unpaired name.
            facade.pop(p.name, None)
            continue
        c_names.add(p.name)
        bound = facade.get(p.name)
        rows.append(
            FieldBinding(
                c_sig.key,
                p.raw_name or p.name,
                "param",
                surface,
                (bound.raw_name or bound.name) if bound else None,
                sig.file if bound else c_sig.file,
                sig.line if bound else c_sig.line,
            )
        )
    # The sample rate is structural on the C side but a named argument on every facade
    # that takes one, so it is paired whenever the facade has it and never reported unbound.
    c_rate = next((p for p in c_sig.params if p.structural and p.name in _RATE_NAMES), None)
    rate = next((p for p in sig.params if p.name in _RATE_NAMES), None)
    if c_rate is not None and rate is not None:
        facade.pop(rate.name, None)
        rows.append(
            FieldBinding(
                c_sig.key,
                c_rate.raw_name or c_rate.name,
                "param",
                surface,
                rate.raw_name or rate.name,
                sig.file,
                sig.line,
            )
        )
    for name, p in facade.items():
        if name not in c_names and name not in used_bags:
            rows.append(
                FieldBinding(
                    c_sig.key, None, "param", surface, p.raw_name or p.name, sig.file, sig.line
                )
            )
    return rows


def _record_rows(c_rec: RecordShape, rec: RecordShape, surface: Surface) -> list[FieldBinding]:
    rows: list[FieldBinding] = []
    facade = {f.name: f for f in rec.core_fields()}
    c_names = set()
    for f in c_rec.core_fields():
        c_names.add(f.name)
        bound = facade.get(f.name)
        rows.append(
            FieldBinding(
                c_rec.key,
                f.raw_name or f.name,
                "record_field",
                surface,
                (bound.raw_name or bound.name) if bound else None,
                rec.file if bound else c_rec.file,
                rec.line if bound else c_rec.line,
            )
        )
    for name, f in facade.items():
        if name not in c_names and name not in compare._FACADE_ONLY_FIELDS:
            rows.append(
                FieldBinding(
                    c_rec.key,
                    None,
                    "record_field",
                    surface,
                    f.raw_name or f.name,
                    rec.file,
                    rec.line,
                )
            )
    return rows


def _handle_method(key: str, ex: Extraction) -> FunctionSig | None:
    """The facade class method a C handle op is, by the prefix the coverage check strips."""
    prefix = compare._handle_prefix(key)
    if prefix is None:
        return None
    cls = dict(compare._HANDLE_FULL_PREFIXES)[prefix]
    names = (key[len(prefix) :], *compare._ALIAS_COVERAGE.get(key, ()))
    owner = compare._HANDLE_ALIAS_OWNERS.get(key, cls)
    for name in names:
        for f in ex.functions:
            if f.key == name and f.raw_name.split(".", 1)[0] in (cls, owner) and "." in f.raw_name:
                return f
    return None


def _as_method(c_sig: FunctionSig, method: bool, c_records: dict[str, RecordShape]) -> FunctionSig:
    """``c_sig`` without its leading handle argument when it pairs with a class method.

    The handle is an opaque ``Sonare*`` pointer, const or not, never a record.
    """
    if not method or not c_sig.params:
        return c_sig
    m = _HANDLE_ARG.fullmatch(c_sig.params[0].type.strip())
    if m is None or m.group(1) in c_records:
        return c_sig
    return replace(c_sig, params=c_sig.params[1:])


def _paired(
    c_funcs: dict[str, FunctionSig], ex: Extraction, c_records: dict[str, RecordShape]
) -> list[tuple[str, FunctionSig, FunctionSig]]:
    """(key, C signature, facade signature) for every C entry the surface exposes.

    A method forwarding ``**kwargs`` to its class constructor also takes the constructor's.
    """
    funcs = compare._index(ex)
    out = []
    for key in sorted(c_funcs):
        if key in funcs:
            out.append((key, c_funcs[key], funcs[key]))
        elif (method := _handle_method(key, ex)) is not None:
            out.append((key, _as_method(c_funcs[key], True, c_records), method))
        elif compare._handle_prefix(key) is None:
            alias = next(
                (funcs[a] for a in compare._ALIAS_COVERAGE.get(key, ()) if a in funcs), None
            )
            if alias is not None:
                out.append((key, c_funcs[key], alias))
    return [(key, c_sig, _with_constructor(sig, ex)) for key, c_sig, sig in out]


def _with_constructor(sig: FunctionSig, ex: Extraction) -> FunctionSig:
    """``sig`` plus its class constructor's parameters when it forwards ``**kwargs`` there."""
    ctor = ex.constructors.get(sig.raw_name.split(".", 1)[0]) if "." in sig.raw_name else None
    if not sig.var_keyword or ctor is None:
        return sig
    have = {p.name for p in sig.params}
    extra = [p for p in ctor.params if p.name not in have and not p.structural]
    return replace(sig, params=[*sig.params, *extra])


def _folded_variants(
    functions: list[FunctionSig], c_funcs: dict[str, FunctionSig]
) -> dict[str, list[FunctionSig]]:
    """Second forms (``_ex``, ``_with_progress``, ...) by the base key they fold into.

    The C layer reports a refusal from a second form under its base entry's key
    (``c_entry_key`` in ``src/c_api/sonare_c_internal.cpp``), so a parameter only the
    second form declares is a subject of the base key too.
    """
    out: dict[str, list[FunctionSig]] = {}
    for f in functions:
        for suffix in _FOLDED_SUFFIXES:
            base = f.key[: -len(suffix)]
            if f.key.endswith(suffix) and base and base in c_funcs:
                out.setdefault(base, []).append(f)
                break
    return out


def field_bindings(root: Path, surfaces: tuple[Surface, ...] = FACADES) -> list[FieldBinding]:
    """Every (C subject, facade) pairing the parity extractors make, plus unpaired facade names.

    Rows carry the key a refusal is reported under, which folds a second form into its base.
    """
    extract = check_parity._EXTRACTORS
    c_ex = extract["c"](root)
    c_funcs = compare._index(c_ex)
    c_recs = compare._index_records(c_ex)
    c_by_name = {r.raw_name: r for r in c_ex.records}
    variants = _folded_variants(c_ex.functions, c_funcs)
    rows: list[FieldBinding] = []
    for surface in surfaces:
        ex = extract[surface](root)
        by_name = {r.raw_name: r for r in ex.records}
        direct = compare._index(ex).keys()
        seen: dict[str, set[tuple[str | None, str | None]]] = {}
        # A base entry ahead of its second forms, so its own pairing wins a shared subject.
        paired = sorted(_paired(c_funcs, ex, c_by_name), key=lambda t: (entry_key(t[0]), t[0]))
        for key, c_sig, sig in paired:
            emit = entry_key(key)
            forms = [replace(c_sig, key=emit)] + [
                _as_method(replace(v, key=emit), key not in direct, c_by_name)
                for v in variants.get(key, [])
            ]
            done = seen.setdefault(emit, set())
            for form in forms:
                for row in _param_rows(form, sig, surface, c_by_name, by_name):
                    tag = (row.c_subject, None if row.c_subject else row.surface_spelling)
                    if tag not in done:
                        done.add(tag)
                        rows.append(row)
        recs = compare._index_records(ex)
        for key in sorted(c_recs.keys() & recs.keys()):
            rows.extend(_record_rows(c_recs[key], recs[key], surface))
    return rows


def entry_key(key: str) -> str:
    """The key the C layer reports a refusal under: a second-form suffix folded away."""
    for suffix in _FOLDED_SUFFIXES:
        if key.endswith(suffix) and len(key) > len(suffix):
            return key[: -len(suffix)]
    return key


def _exposed(root: Path, c_ex: Extraction) -> dict[str, set[str]]:
    """C keys some facade has a member for, each with the record keys that member takes.

    A member answers for an entry by the same key, or by the handle-stripped tail or an alias.
    """
    taken: dict[str, set[str]] = {}
    for surface in FACADES:
        ex = check_parity._EXTRACTORS[surface](root)
        by_name = {r.raw_name: r for r in ex.records}
        for f in ex.functions:
            owner = f.raw_name.split(".", 1)[0] if "." in f.raw_name else ""
            recs = taken.setdefault(f"{owner}.{f.key}", set())
            for p in f.params:
                if (rec := _record_of(p.type, by_name)) is not None:
                    recs.add(rec.key)
    members = {name.split(".", 1)[1] for name in taken}
    out: dict[str, set[str]] = {}
    for f in c_ex.functions:
        prefix = compare._handle_prefix(f.key)
        names = {f.key, *compare._ALIAS_COVERAGE.get(f.key, ())}
        tails = {n for n in names if n in members}
        if prefix is not None:
            # A handle op answers only on its own class, never on a same-named member elsewhere.
            cls = dict(compare._HANDLE_FULL_PREFIXES)[prefix]
            owners = {cls, compare._HANDLE_ALIAS_OWNERS.get(f.key, cls)}
            tails = {f"{o}.{n}" for o in owners for n in names | {f.key[len(prefix) :]}}
        else:
            tails = {t for name in taken if name.split(".", 1)[1] in tails for t in (name,)}
        if tails & taken.keys():
            out[f.key] = set().union(*(taken[t] for t in tails & taken.keys()))
    return out


def struct_entries_without_bindings(
    root: Path, rows: list[FieldBinding] | None = None
) -> list[str]:
    """C entries a facade reaches that take a caller-filled struct none of whose members bind.

    Reached: some facade has a member named for the entry, or for the base it folds into,
    on any class once a handle prefix is stripped -- deliberately looser than the pairing,
    so an entry the pairing misses is still held to the check. Bound: a paired row on that
    key, or, where the facade member takes the struct's mirrored record itself, a paired
    row of that record. An entry failing this names a struct the facades fill in under
    spellings no extractor connects to the C members, so no refusal from it is respelled.
    """
    rows = field_bindings(root) if rows is None else rows
    c_ex = check_parity._EXTRACTORS["c"](root)
    c_by_name = {r.raw_name: r for r in c_ex.records}
    variants = _folded_variants(c_ex.functions, compare._index(c_ex))
    base_of = {v.key: base for base, forms in variants.items() for v in forms}
    exposed = _exposed(root, c_ex)
    bound = {r.c_key for r in rows if r.c_subject is not None and r.surface_spelling is not None}
    out = set()
    for f in c_ex.functions:
        structs = {rec.key for p in f.params if (rec := _caller_filled(p, c_by_name)) is not None}
        keys = {f.key, base_of.get(f.key, f.key)}
        reached = [exposed[k] for k in keys if k in exposed]
        if not structs or not reached:
            continue
        taken = set().union(*reached)
        if not {entry_key(k) for k in keys} & bound and not structs & taken & bound:
            out.add(f.key)
    return sorted(out)
