"""Extract the Node / WASM record shapes from their TypeScript type declarations.

Neither JS facade mirrors a C struct byte-for-byte: the Node addon marshals
field by field (``obj.Set("nBins", ...)``) and WASM embind does the same
(``val.set("nBins", ...)``), with no ``value_object`` and no shared layout. What
both surfaces DO declare is the resulting record as a TypeScript ``interface``
(or an object-literal ``type`` alias) in their public type files, and that
declaration is what a caller compiles against. It is therefore the record shape
this extractor compares — a field the C ABI grew and the facade never declared
is unreachable from TypeScript whether or not the marshalling code sets it.

``extends`` is resolved within the surface, so an interface that inherits half
its fields is compared on its effective field set rather than on the half it
spells out locally. Inheritance from an unresolved name (an imported type from
outside the parsed set) is recorded as unparsed: the record would otherwise look
artificially small, and a small record reads as missing fields.

Parsed with the same pragmatic regex/brace-matching approach as
``ts_common.py``; the facades are hand-written in a regular style. Methods,
index signatures and call signatures inside an interface are not fields and are
skipped.
"""

from __future__ import annotations

import re
from pathlib import Path

from model import Extraction, RecordField, RecordShape
from normalize import canonical_field_name, canonical_record_key

from ._tokens import strip_c_comments

# ``export interface Foo extends Bar, Baz {`` / ``interface Foo {``
_IFACE_HEAD = re.compile(
    r"(?:export\s+)?interface\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"(?:\s*<[^{]*?>)?"
    r"(?:\s+extends\s+(?P<bases>[^{]+?))?\s*\{"
)

# ``export type Foo = ...;`` — the other spelling of a record. Only the
# object-literal and intersection forms (``{ ... }``, ``A & B``, ``A & { ... }``)
# are records; a union / mapped / conditional / function alias is not, and is
# skipped by :func:`_parse_intersection`.
_TYPE_ALIAS_HEAD = re.compile(
    r"(?:export\s+)?type\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"(?:\s*<[^=]*?>)?\s*=\s*"
)

# A bare type reference inside an intersection: ``ValidateOptions``,
# ``MelodyOptions``. Generic references are not resolved (no substitution), so
# they make the alias unparseable rather than silently under-reported.
_BARE_TYPE_REF = re.compile(r"^[A-Za-z_$][A-Za-z0-9_$.]*$")

# ``readonly foo?: Type`` — a property. ``foo(``/``[k: string]``/``new (`` are
# not properties.
_PROP_RE = re.compile(
    r"^(?:readonly\s+)?(?P<name>[A-Za-z_$][A-Za-z0-9_$]*)\s*(?P<opt>\?)?\s*:\s*(?P<type>.+)$",
    re.DOTALL,
)

# Type names that are structural on the JS surfaces: a facade record may carry a
# progress / cancellation hook or an options bag that no C struct declares.
# Recorded but not treated as a C-field counterpart.
_STRUCTURAL_FIELD_NAMES = {"on_progress", "cancel", "signal"}

# Path fragments whose records are INTERNAL to the binding and are not mirrors of
# a C struct. ``ts_common.py`` already keeps the AudioWorklet entry points out of
# the function surface (they have no C-API counterpart by design); the same holds
# for the records they declare. The worklet's SharedArrayBuffer ring payloads are
# named after the C structs they carry messages ABOUT — ``SonareClipPageRequest``
# there is ``{clipId, pageIndex}``, the wire form, not the C
# ``{clip_id, channel, sample}`` — so comparing them against the C ABI reports a
# difference that is not drift.
#
# These files are still PARSED, because a record outside the worklet may extend a
# type declared inside it; they are only excluded from being EMITTED as records.
_INTERNAL_PATH_PARTS = ("/worklet/",)


def _is_internal_path(rel: str) -> bool:
    norm = "/" + rel.replace("\\", "/")
    return any(part in norm for part in _INTERNAL_PATH_PARTS)


def _match_brace(text: str, open_idx: int) -> int:
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


# Tokens that continue a member declaration across a line break. A wide string
# union is routinely written one alternative per line:
#
#     quality:
#       | 'major'
#       | 'minor';
#
# so a newline only terminates a member when neither side of it is a
# continuation. Without this the member splits into fragments and the property —
# ``quality`` — reads as absent, which the comparison would report as a missing
# C field.
_CONTINUES_BEFORE = ("|", "&", "<", "(", ",", ":", "=", "extends")
_CONTINUES_AFTER = ("|", "&", ")", "]", ">", "extends")


def _split_members(body: str) -> list[str]:
    """Split an interface body into member declarations.

    Members are terminated by ``;``, ``,`` or a newline; nested object / union /
    generic punctuation must not split a member, so the split is depth-aware,
    and a newline inside a wrapped union / generic does not split either.
    """
    parts: list[str] = []
    cur: list[str] = []
    depth = 0
    for i, ch in enumerate(body):
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if depth != 0:
            cur.append(ch)
            continue
        if ch in ";,":
            parts.append("".join(cur))
            cur = []
            continue
        if ch == "\n":
            before = "".join(cur).rstrip()
            after = body[i + 1 :].lstrip()
            if before and not before.endswith(_CONTINUES_BEFORE):
                if not after.startswith(_CONTINUES_AFTER):
                    parts.append(before)
                    cur = []
                    continue
            cur.append(" ")
            continue
        cur.append(ch)
    parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def _parse_members(body: str) -> list[RecordField]:
    fields: list[RecordField] = []
    for member in _split_members(body):
        m = _PROP_RE.match(member)
        if m is None:
            continue  # method / index signature / call signature: not a field
        raw = m.group("name")
        name = canonical_field_name(raw)
        fields.append(
            RecordField(
                name=name,
                raw_name=raw,
                type=" ".join(m.group("type").split()),
                structural=name in _STRUCTURAL_FIELD_NAMES,
                optional=m.group("opt") is not None,
            )
        )
    return fields


def _line_of(text: str, idx: int) -> int:
    return text.count("\n", 0, idx) + 1


def _base_names(raw: str | None) -> list[str]:
    """Split an ``extends A, B<C>`` clause into its base type names."""
    if not raw:
        return []
    out: list[str] = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        out.append(re.split(r"[<\s]", part, maxsplit=1)[0])
    return out


def _alias_rhs(text: str, start: int) -> tuple[str, int] | None:
    """Return the right-hand side of a ``type X = <rhs>;`` and its end index.

    Scans to the terminating top-level ``;``. A brace / bracket / paren block is
    skipped over so an object-literal alias is captured whole.
    """
    depth = 0
    for i in range(start, len(text)):
        ch = text[i]
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        elif ch == ";" and depth == 0:
            return text[start:i], i
    return None


def _parse_intersection(rhs: str) -> tuple[list[str], list[RecordField]] | None:
    """Split a type-alias RHS into ``(base_names, own_fields)``.

    Accepts the record spellings only: ``{ ... }``, ``A``, ``A & B``,
    ``A & { ... }``. Returns ``None`` for anything else (a union, a function
    type, a generic reference, a mapped type) so a non-record alias is never
    mistaken for an empty record.
    """
    terms: list[str] = []
    cur: list[str] = []
    depth = 0
    for ch in rhs:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "&" and depth == 0:
            terms.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    terms.append("".join(cur))

    bases: list[str] = []
    fields: list[RecordField] = []
    for term in terms:
        term = term.strip()
        if not term:
            return None
        if term.startswith("{") and term.endswith("}"):
            fields.extend(_parse_members(term[1:-1]))
        elif _BARE_TYPE_REF.match(term):
            bases.append(term)
        else:
            return None
    if not bases and not fields:
        return None
    return bases, fields


def _collect_declarations(
    files: list[tuple[str, str]],
) -> tuple[dict[str, tuple[str, list[str], list[RecordField], str, int]], list[str]]:
    """Parse every interface / record-shaped type alias in ``files``.

    Returns ``(declarations, notes)`` where a declaration maps the TS type name
    to ``(name, base_names, local_fields, file, line)``. The first declaration of
    a name wins; a duplicate (the same public type re-declared in a second
    module) is skipped rather than merged, because merging would invent a field
    set neither module declares.
    """
    decls: dict[str, tuple[str, list[str], list[RecordField], str, int]] = {}
    notes: list[str] = []
    for rel, raw in files:
        text = strip_c_comments(raw)
        for m in _IFACE_HEAD.finditer(text):
            name = m.group("name")
            open_idx = text.index("{", m.end() - 1)
            close_idx = _match_brace(text, open_idx)
            if close_idx < 0:
                notes.append(f"{rel}:{_line_of(text, open_idx)}: {name} (unbalanced)")
                continue
            if name in decls:
                continue
            bases = _base_names(m.group("bases"))
            fields = _parse_members(text[open_idx + 1 : close_idx])
            decls[name] = (name, bases, fields, rel, _line_of(text, m.start()))
        for m in _TYPE_ALIAS_HEAD.finditer(text):
            name = m.group("name")
            if name in decls:
                continue
            rhs = _alias_rhs(text, m.end())
            if rhs is None:
                continue
            parsed = _parse_intersection(rhs[0])
            if parsed is None:
                continue  # not a record-shaped alias (union / function / mapped)
            bases, fields = parsed
            decls[name] = (name, bases, fields, rel, _line_of(text, m.start()))
    return decls, notes


def _effective_fields(
    name: str,
    decls: dict[str, tuple[str, list[str], list[RecordField], str, int]],
    seen: set[str],
    ex: Extraction,
) -> tuple[list[RecordField], bool]:
    """Flatten ``name``'s own fields plus every resolvable base's.

    Returns ``(fields, complete)``; ``complete`` is False when a base could not
    be resolved, in which case the record is dropped rather than compared
    against a truncated field set.
    """
    if name in seen:
        return [], True  # cyclic extends: treat as already contributed
    seen.add(name)
    entry = decls.get(name)
    if entry is None:
        return [], False
    _, bases, own, _, _ = entry
    out: list[RecordField] = []
    complete = True
    for base in bases:
        inherited, ok = _effective_fields(base, decls, seen, ex)
        complete = complete and ok
        out.extend(inherited)
    by_name = {f.name for f in out}
    for f in own:
        if f.name in by_name:
            out = [x for x in out if x.name != f.name]  # own declaration overrides
        out.append(f)
    return out, complete


def extract_records(root: Path, surface: str, src_rel: str, ex: Extraction) -> None:
    """Append every TypeScript record declared under ``src_rel`` to ``ex``.

    The whole ``src`` tree is walked rather than the ``export *`` closure the
    function extractor uses: a type file can be pulled in by a plain
    ``import type`` from a facade module without being re-exported from the
    index, and missing it would shrink the record set silently. Generated
    ``.d.ts`` module-shape declarations are excluded — they type the raw
    embind/N-API module, not the public records.
    """
    src = root / src_rel
    if not src.is_dir():
        ex.unparsed += 1
        ex.unparsed_notes.append(f"{src_rel}: source tree missing")
        return
    files: list[tuple[str, str]] = []
    for path in sorted(src.rglob("*.ts")):
        if path.name.endswith(".d.ts"):
            continue
        files.append((str(path.relative_to(root)), path.read_text(encoding="utf-8")))
    if not files:
        ex.unparsed += 1
        ex.unparsed_notes.append(f"{src_rel}: no .ts sources found")
        return

    decls, notes = _collect_declarations(files)
    ex.unparsed_notes.extend(notes)
    ex.unparsed += len(notes)

    for name, (_, _, _, rel, line) in sorted(decls.items()):
        if _is_internal_path(rel):
            continue  # binding-internal wire type, not a C-struct mirror
        fields, complete = _effective_fields(name, decls, set(), ex)
        if not fields:
            continue
        if not complete:
            ex.unparsed += 1
            ex.unparsed_notes.append(
                f"{rel}:{line}: {name} (extends an unresolved type; record skipped)"
            )
            continue
        ex.records.append(
            RecordShape(
                key=canonical_record_key(name, surface),
                surface=surface,
                raw_name=name,
                fields=fields,
                file=rel,
                line=line,
            )
        )
