"""Parity facts for tools that need the extractors' pairing without the report.

``field_bindings`` exposes how each C-ABI parameter and struct field is spelled
on each facade, using the same pairing the comparisons use: canonical snake_case
name equality, never position. A C subject whose canonical name no facade
parameter or field shares has no binding on that surface (``surface_spelling``
is ``None``), and a facade name with no C counterpart on the same key is its own
row with ``c_subject`` ``None``. A rename therefore shows as one of each on the
same key; nothing here guesses that the two are the same subject.

Only keys a surface declares are reported for it: a function or record the
facade omits entirely is the coverage check's business, not a binding.
A C config struct (``config``, ``options``, ...) is not a subject: its interior
is reached through the record rows, so request-object keys appear only where the
request type mirrors a C struct and is extracted as one. A facade bag-named
parameter (``config``, ``request``, ...) that C does not also take under that
name is reported as an unpaired facade name, since it may be where a C
parameter was folded (a C ``preset`` taken as keyword ``config``).
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Literal

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import check_parity
import compare
from model import FunctionSig, RecordShape

# Canonical spellings of the sample rate, which C marks structural.
_RATE_NAMES = frozenset({"sample_rate", "sr"})
# Entry suffixes the C layer folds away when it keys a refusal, in its match order.
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


def _param_rows(c_sig: FunctionSig, sig: FunctionSig, surface: Surface) -> list[FieldBinding]:
    rows: list[FieldBinding] = []
    facade = {p.name: p for p in sig.core_params() if p.name not in compare._CALLBACK_NAMES}
    c_names = set()
    for p in c_sig.core_params():
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
        if name not in c_names:
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
    """Every (C subject, facade) pairing the parity extractors make, plus unpaired facade names."""
    extract = check_parity._EXTRACTORS
    c_ex = extract["c"](root)
    c_funcs = compare._index(c_ex)
    c_recs = compare._index_records(c_ex)
    variants = _folded_variants(c_ex.functions, c_funcs)
    rows: list[FieldBinding] = []
    for surface in surfaces:
        ex = extract[surface](root)
        funcs = compare._index(ex)
        for key in sorted(c_funcs.keys() & funcs.keys()):
            base = _param_rows(c_funcs[key], funcs[key], surface)
            rows.extend(base)
            seen = {r.c_subject for r in base}
            for variant in variants.get(key, []):
                for row in _param_rows(replace(variant, key=key), funcs[key], surface):
                    if row.c_subject is not None and row.c_subject not in seen:
                        seen.add(row.c_subject)
                        rows.append(row)
        recs = compare._index_records(ex)
        for key in sorted(c_recs.keys() & recs.keys()):
            rows.extend(_record_rows(c_recs[key], recs[key], surface))
    return rows
