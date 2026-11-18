"""Extract the canonical C API surface from sonare_c.h and its generated headers.

The C API is the canonical ABI: it defines the authoritative function NAME,
PARAMETER ORDER and PARAM TYPES, and — for the record-shape unit — the
authoritative FIELD LIST of every public struct. C has no default values.

Parsing strategy: C declarations are not regular, but the libsonare headers are
machine-generated / hand-written in a consistent style: each declaration is a
return-type + ``sonare_<name>(`` + comma-separated params, terminated by ``);``.
We strip comments, join continuation lines, then split on the top-level ``;``.
Record types follow the same discipline — ``typedef struct { ... } SonareXxx;``
(occasionally tag-named) — so the body is brace-matched and split on ``;``.
Both units walk the SAME header set (``_collect_api_headers``), so a header
added to the public umbrella is picked up by both without a second list.
"""

from __future__ import annotations

import re
from pathlib import Path

from model import Extraction, FunctionSig, Param, RecordField, RecordShape
from normalize import canonical_field_name, canonical_key, canonical_record_key

from ._tokens import split_top_level_commas, strip_c_comments

# Return types the tip's public headers spell. extract() adds whatever the read
# tree's own headers spell (a tagged release can return a type the tip dropped),
# so this list is the guard that the loose scan still reaches the tip:
# test_c_return_type_coverage.py fails when a tip return type is missing from it.
_RETURN_TYPES = (
    "SonareError",
    "void",
    "int",
    "float",
    "double",
    "size_t",
    "bool",
    "const char*",
    "const float*",
    "const int*",
    "uint32_t",
    "uint64_t",
    "int32_t",
    "int64_t",
    # A scene loader and the strip accessors return a pointer to an opaque type.
    "SonareMixer*",
    "SonareStrip*",
    # Default seeders return a config struct by value.
    "SonareBoundaryOptions",
    "SonareMeterOptions",
    "SonareMusicAnalyzeOptions",
    "SonareProjectTempoOptions",
    "SonareSectionOptions",
    "SonareTranscribeConfig",
)

# A declaration at column 0, return type first, read loosely so the return types
# of whichever tree is extracted (a tagged release included) join the closed list.
_LOOSE_DECL_RE = re.compile(
    r"^(?P<ret>(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*(?:\s*\*\s*|\s+))sonare_[A-Za-z0-9_]+\s*\(",
    re.MULTILINE,
)
_NON_RETURN_PREFIXES = frozenset({"typedef", "return", "struct", "enum", "union"})


def declared_return_types(text: str) -> set[str]:
    """Return types a header's column-0 sonare_ declarations spell, normalized."""
    found: set[str] = set()
    for m in _LOOSE_DECL_RE.finditer(text):
        ret = " ".join(m.group("ret").split()).replace(" *", "*")
        if ret.split()[0] not in _NON_RETURN_PREFIXES:
            found.add(ret)
    return found


def _decl_re(return_types: set[str]) -> re.Pattern[str]:
    # Longest first, so "const char*" is tried before a shorter alternative.
    alts = sorted(return_types, key=len, reverse=True)
    spelled = [re.escape(t).replace(r"\*", r"\s*\*") for t in alts]
    return re.compile(
        r"\b(?P<ret>" + "|".join(spelled) + r")(?:(?<=\*)\s*|\s+)"
        r"(?P<name>sonare_[A-Za-z0-9_]+)\s*\((?P<args>.*?)\)\s*;",
        re.DOTALL,
    )


# Out-pointer / length / scalar params that the TS/py facades fold away or
# represent differently. These are structural, not positional features.
# Matched with re.search so suffixes like ``_count`` / ``_out`` are caught.
_OUT_NAME_RE = re.compile(r"^out(_|$)|_count$|_out$|^out$")

# Pure C-ABI plumbing param names that have no facade analog (callback bridge,
# user-data cookies, length cookies). Folded away by every facade.
_PLUMBING_NAMES = {
    "callback",
    "user_data",
    "userdata",
    "json_out",
    "out_json",
    "param_count",
    "override_count",
    "mode_count",
}


def _split_args(args: str) -> list[str]:
    """Split a C arg list on top-level commas (no nested templates here)."""
    args = args.strip()
    if not args or args == "void":
        return []
    # C declarations here never nest braces; keep the historical narrower
    # ``([<`` / ``)]>`` bracket set so behavior is byte-for-byte unchanged.
    return split_top_level_commas(args, "([<", ")]>")


def _parse_param(decl: str) -> Param:
    """Parse a single C parameter declaration into a Param."""
    decl = decl.strip()
    # The identifier is the last token (handles ``const SonareConfig* config``,
    # ``float** out``, ``size_t* out_count``). Pointers/qualifiers belong to type.
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", decl)
    name = m.group(1) if m else ""
    ctype = decl[: m.start()].strip() if m else decl
    # Reattach pointer stars that sit before the name.
    ptr = decl[len(ctype) : m.start()].strip() if m else ""
    full_type = (ctype + ptr).strip()

    structural = False
    is_ptr = "*" in decl
    lname = name.lower()
    if _OUT_NAME_RE.search(lname):
        structural = True  # out_*, out, *_count, *_out
    elif lname in _PLUMBING_NAMES:
        structural = True  # callback/user_data/json_out/... no facade analog
    elif lname in ("samples", "data", "length", "sample_rate", "size", "len"):
        structural = True  # input-buffer plumbing folded by facades
    elif name == "" or (
        is_ptr and lname in ("audio", "self", "handle", "engine", "out_key", "out_bpm")
    ):
        structural = True

    return Param(name=name, raw_name=name, default=None, type=full_type, structural=structural)


# A count right after its array is that array's length, which the facades derive:
# ``ops, n_ops`` / ``channels, num_channels``, the ``frames, channels`` of an interleaved buffer, and the run of
# dimensions after a matrix (``features, n_features, n_frames``), which every facade
# takes from the array's shape.
_INTEGER_TYPES = {"int", "int32_t", "size_t", "uint32_t", "int64_t", "uint64_t"}
_MATRIX_DIMENSION_RE = re.compile(
    r"^(n|rows|cols|input_length|n_(bins|frames|features|rows|cols|mfcc|chroma|lags)|\w+_(rows|cols))$"
)


def _is_integer(param: Param) -> bool:
    return param.type.replace("const", "").strip() in _INTEGER_TYPES


def matrix_dimensions(params: list[Param]) -> set[str]:
    """Names of the dimension-named integers that directly follow a pointer."""
    names: set[str] = set()
    for i, prev in enumerate(params):
        if "*" not in prev.type:
            continue
        for cur in params[i + 1 :]:
            if not _is_integer(cur) or not _MATRIX_DIMENSION_RE.match(cur.name):
                break
            names.add(cur.name)
    return names


def _mark_derived_counts(params: list[Param]) -> list[Param]:
    """Mark an integer count that directly follows the array it measures as structural."""
    padded = [*params, Param(name="")]
    for prev, cur, nxt in zip(padded, padded[1:], padded[2:]):
        if "*" not in prev.type or not _is_integer(cur):
            continue
        if (
            cur.name in (f"n_{prev.name}", f"num_{prev.name}")
            and not nxt.name.startswith("n_")
            or cur.name == "frames"
            and nxt.name == "channels"
        ):
            cur.structural = True
    dims = matrix_dimensions(params)
    for p in params:
        if p.name in dims:
            p.structural = True
    return params


# Local public-API includes: ``#include <sonare/sonare_c_effects.h>`` etc.
# ``sonare_c.h`` is an umbrella over per-domain headers, so the canonical C
# surface is spread across the headers it pulls in. We follow those local
# includes transitively to reconstruct the full surface. Internal / helper
# headers are excluded -- they hold private plumbing, not the public ABI.
_LOCAL_INCLUDE_RE = re.compile(r'#include\s+"(sonare_c[A-Za-z0-9_]*\.h)"')


def _is_internal_header(name: str) -> bool:
    return name.endswith("_internal.h") or "_helpers" in name


def _collect_api_headers(root: Path) -> list[Path]:
    """All public C-API headers, starting from sonare_c.h and following its
    local ``sonare_c*.h`` includes transitively (excluding internal/helpers)."""
    # Public C-API headers live under include/sonare/; sonare_c.h pulls in its
    # domain siblings by bare name (resolved relative to that directory).
    pub = root / "include" / "sonare"
    src = root / "src"
    out: list[Path] = []
    seen: set[Path] = set()
    queue: list[Path] = [pub / "sonare_c.h"]
    while queue:
        path = queue.pop(0)
        if path in seen or not path.exists():
            continue
        seen.add(path)
        out.append(path)
        raw = path.read_text(encoding="utf-8")
        for inc in _LOCAL_INCLUDE_RE.findall(raw):
            if _is_internal_header(inc):
                continue
            child = pub / inc
            if child not in seen:
                queue.append(child)
    # Generated headers (when codegen is active) as a backstop.
    for gen in sorted((src / "generated").glob("*_gen.h")):
        if gen not in seen:
            out.append(gen)
    return out


# ``typedef struct [Tag] {`` — the opening of a record definition. An opaque
# handle typedef (``typedef struct SonareProject SonareProject;``) has no body
# and is deliberately not matched: it declares no fields to compare.
_STRUCT_OPEN_RE = re.compile(r"\btypedef\s+struct(?:\s+[A-Za-z_][A-Za-z0-9_]*)?\s*\{")

# The trailing ``} SonareXxx;`` that names the typedef.
_STRUCT_CLOSE_RE = re.compile(r"^\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*;")

# A function-pointer member: ``void (*render)(float* const*, int, int)``. The
# member name sits inside the ``(*name)`` group, not at the end of the decl.
_FUNC_PTR_RE = re.compile(r"\(\s*\*\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)")

# Explicit padding / reserved bytes. Present purely to pin the byte layout, so
# no facade mirrors them (Python ctypes does, to keep offsets right, but the
# field carries no semantic meaning to compare).
_PADDING_FIELD_RE = re.compile(r"^(reserved|_?pad(ding)?)([0-9_].*)?$")

# Length companions of a pointer / array member: a trailing ``_count`` /
# ``_length`` / ``_len``, a leading ``num_``, or the bare noun on its own
# (``SonareHpssResult.length`` next to ``harmonic`` / ``percussive``). The
# facades derive the count from the array they hand back, so a record that owns
# at least one pointer or array member may legitimately drop these (the
# allowlist's "array-length derivation" category, expressed as a rule rather
# than as per-record entries).
_LENGTH_FIELD_RE = re.compile(r"^(count|length|len|size)$|.*_(count|length|len)$|^num_.+")

# ABI plumbing members that describe the STRUCT rather than the data in it: the
# version tag that selects which field generation is populated, and the presence
# bitmask that says which optional fields the caller filled in. The JS facades
# express both through the type system (a versioned union, an optional field),
# so no facade mirrors them as data.
_ABI_PLUMBING_FIELDS = {
    "struct_version",
    "struct_size",
    "abi_version",
    "present_fields",
    "present_fields2",
}

# Integer spellings a length companion is written in. A ``float* ..._length``
# would not be a count, so the type is checked as well as the name.
_INT_TYPE_TOKENS = (
    "int",
    "size_t",
    "uint8_t",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "int8_t",
    "int16_t",
    "int32_t",
    "int64_t",
    "unsigned",
    "long",
    "short",
)


def _is_int_type(ctype: str) -> bool:
    if "*" in ctype:
        return False
    tokens = ctype.replace("const", " ").split()
    return any(t in _INT_TYPE_TOKENS for t in tokens)


def _parse_record_field(decl: str) -> RecordField | None:
    """Parse one struct member declaration into a RecordField.

    Handles the three shapes the public headers use: a plain scalar / pointer
    member (``float* magnitude``), a fixed array member (``char name[64]``,
    ``float points[A * 2]``) and a function-pointer member
    (``void (*render)(float* const*, int, int)``).
    """
    decl = " ".join(decl.split())
    if not decl:
        return None
    fp = _FUNC_PTR_RE.search(decl)
    if fp is not None:
        name = fp.group("name")
        return RecordField(
            name=canonical_field_name(name),
            raw_name=name,
            type=decl[: fp.start()].strip() + " (*)()",
            structural=False,
        )
    # Drop any array extents before locating the trailing identifier.
    base = re.sub(r"\[[^\]]*\]", "", decl).strip()
    is_array = base != decl
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", base)
    if m is None:
        return None
    name = m.group(1)
    ctype = base[: m.start()].strip()
    if not ctype:
        return None  # a bare identifier is not a member declaration
    if is_array:
        ctype += "[]"
    return RecordField(name=canonical_field_name(name), raw_name=name, type=ctype)


def _mark_structural_fields(fields: list[RecordField]) -> None:
    """Flag members with no facade counterpart by design (in place).

    Padding / ``reserved`` members and ABI plumbing (the version tag, the
    presence bitmask) are always structural. A length companion is structural
    only when the record actually owns a pointer or array member for the facade
    to derive the count from — otherwise the ``_count`` name is carrying real
    information and must be compared.
    """
    has_indirect = any(("*" in f.type or f.type.endswith("[]")) for f in fields)
    for f in fields:
        if (
            _PADDING_FIELD_RE.match(f.name)
            or f.name in _ABI_PLUMBING_FIELDS
            or (has_indirect and _LENGTH_FIELD_RE.match(f.name) and _is_int_type(f.type))
        ):
            f.structural = True


def _extract_records(text: str, raw: str, path_rel: str, ex: Extraction) -> None:
    """Append every ``typedef struct { ... } SonareXxx;`` in ``text`` to ``ex``.

    ``text`` is the comment-stripped header; ``raw`` is the original, used only
    to report a line number a reader can jump to.
    """
    for m in _STRUCT_OPEN_RE.finditer(text):
        open_idx = text.index("{", m.end() - 1)
        depth = 0
        close_idx = -1
        for i in range(open_idx, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    close_idx = i
                    break
        if close_idx < 0:
            ex.unparsed += 1
            ex.unparsed_notes.append(f"{path_rel}: unbalanced struct body")
            continue
        tail = _STRUCT_CLOSE_RE.match(text[close_idx + 1 :])
        if tail is None:
            ex.unparsed += 1
            ex.unparsed_notes.append(f"{path_rel}: unnamed typedef struct")
            continue
        name = tail.group("name")
        body = text[open_idx + 1 : close_idx]
        fields: list[RecordField] = []
        for part in body.split(";"):
            fld = _parse_record_field(part)
            if fld is not None:
                fields.append(fld)
        if not fields:
            continue
        _mark_structural_fields(fields)
        decl = f"}} {name};"
        line = raw.count("\n", 0, raw.find(decl)) + 1 if decl in raw else 0
        ex.records.append(
            RecordShape(
                key=canonical_record_key(name, "c"),
                surface="c",
                raw_name=name,
                fields=fields,
                file=path_rel,
                line=line,
            )
        )


def extract(root: Path) -> Extraction:
    ex = Extraction(surface="c")
    files = [p for p in _collect_api_headers(root) if p.exists()]
    texts = {p: strip_c_comments(p.read_text(encoding="utf-8")) for p in files}
    return_types = set(_RETURN_TYPES)
    for text in texts.values():
        return_types |= declared_return_types(text)
    decl_re = _decl_re(return_types)
    seen: set[str] = set()
    for path in files:
        raw = path.read_text(encoding="utf-8")
        text = texts[path]
        _extract_records(text, raw, str(path.relative_to(root)), ex)
        for m in decl_re.finditer(text):
            name = m.group("name")
            key = canonical_key(name, "c")
            if key in seen:
                continue
            args = m.group("args")
            try:
                parts = _split_args(args)
                params = _mark_derived_counts([_parse_param(p) for p in parts])
            except Exception:  # noqa: BLE001 - record, do not crash
                ex.unparsed += 1
                ex.unparsed_notes.append(f"{path.name}: {name} (arg parse failed)")
                continue
            line = raw.count("\n", 0, raw.find(name)) + 1 if name in raw else 0
            seen.add(key)
            ex.functions.append(
                FunctionSig(
                    key=key,
                    surface="c",
                    raw_name=name,
                    params=params,
                    returns=" ".join(m.group("ret").split()).replace(" *", "*"),
                    file=str(path.relative_to(root)),
                    line=line,
                )
            )
    return ex
