"""Extract the Python record shapes from the ctypes structure mirrors.

Python is the only surface with RAW BYTE LAYOUT: every public C struct is
mirrored by a ``ctypes.Structure`` subclass whose ``_fields_`` list must match
the header field-for-field, in order. The mirrors do NOT live in ``_ffi.py``
(that module holds only the shared-library loader and the aggregate ABI
constant) — they are split across ``_ffi_types_<domain>.py`` modules, which are
discovered by glob rather than listed so a new domain module is picked up
automatically.

Parsing is stdlib ``ast`` on the module source: no import, no built shared
library, no ctypes evaluation. A ``_fields_`` entry is a 2- or 3-tuple whose
first element is the field-name string literal; anything else in the class body
is ignored. A class assembled dynamically (``_fields_`` built by a loop or a
comprehension rather than a literal list) is recorded as unparsed rather than
silently reported as an empty record — an empty record would read as "every C
field is missing", which is the noisiest possible failure mode.

Field-layout equality is separately and mechanically guarded by
``tools/abi/abi-layout.json`` plus ``bindings/python/tests/test_abi_layout.py``
(``sizeof`` / ``alignof`` / ``offsetof`` straight from the headers). That guard
sees byte offsets, not names: a field renamed on both sides of a same-width swap
keeps the layout valid. This extractor is the naming half of the pair, and it is
the only half that also covers Node and WASM.
"""

from __future__ import annotations

import ast
from pathlib import Path

from model import Extraction, RecordField, RecordShape
from normalize import canonical_field_name, canonical_record_key

# The ctypes mirrors, by glob. ``_ffi.py`` deliberately does not match: it holds
# the loader and the aggregate ABI constant, no Structure subclasses.
_FFI_TYPES_GLOB = "bindings/python/src/libsonare/_ffi_types_*.py"

# ctypes spellings that mean "explicit padding", used as a secondary signal to
# the shared name-based padding rule in the comparison layer.
_PADDING_NAMES = ("reserved", "_pad", "pad", "padding")


def _is_ctypes_structure(node: ast.ClassDef) -> bool:
    """True when ``node`` derives from ``ctypes.Structure`` / ``Structure``."""
    for base in node.bases:
        if isinstance(base, ast.Attribute) and base.attr in ("Structure", "Union"):
            return True
        if isinstance(base, ast.Name) and base.id in ("Structure", "Union"):
            return True
    return False


def _fields_literal(node: ast.ClassDef) -> ast.List | None:
    """Return the class body's ``_fields_ = [...]`` list literal, if it is one."""
    for stmt in node.body:
        targets: list[ast.expr] = []
        if isinstance(stmt, ast.Assign):
            targets = list(stmt.targets)
            value = stmt.value
        elif isinstance(stmt, ast.AnnAssign) and stmt.value is not None:
            targets = [stmt.target]
            value = stmt.value
        else:
            continue
        for t in targets:
            if isinstance(t, ast.Name) and t.id == "_fields_":
                return value if isinstance(value, ast.List) else None
    return None


def _type_of(entry: ast.expr) -> str:
    try:
        return ast.unparse(entry)
    except Exception:  # noqa: BLE001
        return ""


def _parse_class(node: ast.ClassDef, file: str, ex: Extraction) -> None:
    literal = _fields_literal(node)
    if literal is None:
        ex.unparsed += 1
        ex.unparsed_notes.append(
            f"{file}:{node.lineno}: {node.name} (no literal _fields_ list)"
        )
        return
    fields: list[RecordField] = []
    for item in literal.elts:
        if not isinstance(item, ast.Tuple) or not item.elts:
            continue
        head = item.elts[0]
        if not isinstance(head, ast.Constant) or not isinstance(head.value, str):
            continue
        raw = head.value
        ctype = _type_of(item.elts[1]) if len(item.elts) > 1 else ""
        fields.append(
            RecordField(
                name=canonical_field_name(raw),
                raw_name=raw,
                type=ctype,
                structural=raw.lower().startswith(_PADDING_NAMES),
            )
        )
    if not fields:
        return
    ex.records.append(
        RecordShape(
            key=canonical_record_key(node.name, "python"),
            surface="python",
            raw_name=node.name,
            fields=fields,
            file=file,
            line=node.lineno,
        )
    )


def extract_records(root: Path, ex: Extraction) -> None:
    """Append every ``ctypes.Structure`` mirror under ``root`` to ``ex``."""
    seen: set[str] = set()
    for path in sorted(root.glob(_FFI_TYPES_GLOB)):
        rel = str(path.relative_to(root))
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"))
        except SyntaxError as e:  # noqa: PERF203 - record, do not crash
            ex.unparsed += 1
            ex.unparsed_notes.append(f"{rel}: parse error ({e})")
            continue
        for node in ast.walk(tree):
            if not isinstance(node, ast.ClassDef) or not _is_ctypes_structure(node):
                continue
            if node.name in seen:
                continue
            seen.add(node.name)
            _parse_class(node, rel, ex)
