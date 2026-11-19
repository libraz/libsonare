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
A config struct or options bag (``config``, ``request``, ...) is not a subject
on either side: its interior is reached through the record rows, so
request-object keys appear only where the request type mirrors a C struct and
is extracted as one.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
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
    skip = compare._CALLBACK_NAMES | compare._STRUCT_BAG_NAMES
    facade = {p.name: p for p in sig.core_params() if p.name not in skip}
    c_names = set()
    for p in c_sig.core_params():
        if p.name in compare._STRUCT_BAG_NAMES:
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


def field_bindings(root: Path, surfaces: tuple[Surface, ...] = FACADES) -> list[FieldBinding]:
    """Every (C subject, facade) pairing the parity extractors make, plus unpaired facade names."""
    extract = check_parity._EXTRACTORS
    c_ex = extract["c"](root)
    c_funcs = compare._index(c_ex)
    c_recs = compare._index_records(c_ex)
    rows: list[FieldBinding] = []
    for surface in surfaces:
        ex = extract[surface](root)
        funcs = compare._index(ex)
        for key in sorted(c_funcs.keys() & funcs.keys()):
            rows.extend(_param_rows(c_funcs[key], funcs[key], surface))
        recs = compare._index_records(ex)
        for key in sorted(c_recs.keys() & recs.keys()):
            rows.extend(_record_rows(c_recs[key], recs[key], surface))
    return rows
