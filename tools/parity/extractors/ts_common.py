"""Shared pragmatic TypeScript parser for the Node and WASM facades.

These facades follow a regular structure, so we parse with regex rather than a
real TS grammar:

    export function name(
      samples: Float32Array,
      sampleRate = 22050,
      nFft = 2048,
      options: SomeOptions = {},
    ): ReturnType {

and class methods inside ``export class Audio { ... }``:

    nameMethod(arg = 1, options: X = {}): Ret {

Trailing ``options`` bags and a ``sampleRate`` leading arg are structural
conventions (the facade folds C scalar params into an options object). We also
parse the generated ``*_gen.ts`` files (re-exported via ``export *``) and pull
enum value-sets out of ``export type X = 'a' | 'b'`` and option-interface fields.

Declarations whose parameter list we cannot balance are recorded as unparsed
rather than silently dropped.
"""

from __future__ import annotations

import re
from pathlib import Path

from model import Extraction, FunctionSig, Param
from normalize import canonical_key, normalize_default, normalize_param_name

# Trailing-bag / callback artifacts the facades fold away. Audio-INPUT roles
# (samples/sample_rate/...) are intentionally NOT here: they are handled by the
# central leading-input-group normalization and the input-naming check so the
# naming signal is preserved rather than silently dropped.
_STRUCTURAL_NAMES = {"options", "validate", "on_progress"}

# export function foo(  ... up to the closing paren before the return type
_FUNC_HEAD = re.compile(r"export\s+(?:async\s+)?function\s+([A-Za-z0-9_]+)\s*\(")
# class method head: indented `name(` that is not a keyword/control statement.
_METHOD_HEAD = re.compile(r"^[ \t]{2,4}(?:static\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.MULTILINE)
_TYPE_UNION = re.compile(r"export\s+type\s+([A-Za-z0-9_]+)\s*=\s*([^;]+);", re.DOTALL)
_STRING_LIT = re.compile(r"""['"]([^'"]+)['"]""")

_NON_METHOD = {
    "if",
    "for",
    "while",
    "switch",
    "catch",
    "return",
    "function",
    "constructor",
    "get",
    "set",
}


def _balanced_arglist(text: str, open_paren_idx: int) -> tuple[str, int] | None:
    """Return (inner-arg-text, index-after-close) for a ``(`` at open_paren_idx."""
    depth = 0
    i = open_paren_idx
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
            if depth == 0:
                return text[open_paren_idx + 1 : i], i + 1
        i += 1
    return None


def _split_ts_args(inner: str) -> list[str]:
    parts = []
    depth = 0
    cur = []
    for ch in inner:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def _parse_ts_param(decl: str, enum_types: dict[str, tuple[str, ...]]) -> Param:
    """Parse ``nFft = 2048`` / ``options: DeclickOptions = {}`` / ``samples: Float32Array``."""
    decl = decl.strip().rstrip(",")
    default = None
    if "=" in decl:
        lhs, rhs = decl.split("=", 1)
        default = rhs.strip()
    else:
        lhs = decl
    lhs = lhs.strip()
    type_str = ""
    if ":" in lhs:
        name_part, type_str = lhs.split(":", 1)
        name_part = name_part.strip()
        type_str = type_str.strip()
    else:
        name_part = lhs
    name_part = name_part.lstrip("?").strip()
    norm = normalize_param_name(name_part)

    enum_values: tuple[str, ...] = ()
    if type_str:
        if type_str in enum_types:
            enum_values = enum_types[type_str]
        elif "|" in type_str and "'" in type_str or '"' in type_str:
            enum_values = tuple(sorted(set(_STRING_LIT.findall(type_str))))

    structural = norm in _STRUCTURAL_NAMES or default == "{}"
    # An options bag with a literal default of {} is structural.
    if default == "{}":
        default = None
    return Param(
        name=norm,
        raw_name=name_part,
        default=normalize_default(default),
        type=type_str,
        enum_values=enum_values,
        structural=structural,
    )


def _collect_enum_types(texts: list[str]) -> dict[str, tuple[str, ...]]:
    enums: dict[str, tuple[str, ...]] = {}
    for t in texts:
        for m in _TYPE_UNION.finditer(t):
            rhs = m.group(2)
            if "'" in rhs or '"' in rhs:
                enums[m.group(1)] = tuple(sorted(set(_STRING_LIT.findall(rhs))))
    return enums


def _line_of(text: str, idx: int) -> int:
    return text.count("\n", 0, idx) + 1


def _parse_text(
    text: str,
    file: str,
    surface: str,
    ex: Extraction,
    enum_types: dict[str, tuple[str, ...]],
    parse_methods: bool,
) -> None:
    seen_here: set[str] = set()
    # Top-level export functions.
    for m in _FUNC_HEAD.finditer(text):
        name = m.group(1)
        open_idx = m.end() - 1
        bal = _balanced_arglist(text, open_idx)
        if bal is None:
            ex.unparsed += 1
            ex.unparsed_notes.append(f"{file}:{_line_of(text, open_idx)}: {name} (unbalanced args)")
            continue
        inner, _ = bal
        try:
            params = [_parse_ts_param(p, enum_types) for p in _split_ts_args(inner)]
        except Exception:  # noqa: BLE001
            ex.unparsed += 1
            ex.unparsed_notes.append(f"{file}:{_line_of(text, open_idx)}: {name} (param parse)")
            continue
        key = canonical_key(name, surface)
        seen_here.add(name)
        ex.functions.append(
            FunctionSig(
                key=key,
                surface=surface,
                raw_name=name,
                params=params,
                file=file,
                line=_line_of(text, m.start()),
            )
        )

    if not parse_methods:
        return
    # Audio class methods. Limit to the body of `export class Audio { ... }`.
    cls = re.search(r"export\s+class\s+Audio\b", text)
    if not cls:
        return
    body = _balanced_arglist  # not used; manual brace scan below
    start = text.find("{", cls.end())
    if start < 0:
        return
    depth = 0
    end = start
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    class_body = text[start : end + 1]
    base_line = _line_of(text, start)
    for m in _METHOD_HEAD.finditer(class_body):
        name = m.group(1)
        if name in _NON_METHOD or name.startswith("_"):
            continue
        open_idx = m.end() - 1
        bal = _balanced_arglist(class_body, open_idx)
        if bal is None:
            continue
        inner, after = bal
        # Must be a method: a `{` should follow the (optional) return type.
        tail = class_body[after : after + 80]
        if "{" not in tail:
            continue
        try:
            params = [_parse_ts_param(p, enum_types) for p in _split_ts_args(inner)]
        except Exception:  # noqa: BLE001
            continue
        key = canonical_key(name, surface)
        ex.functions.append(
            FunctionSig(
                key=key,
                surface=surface,
                raw_name="Audio." + name,
                params=params,
                file=file,
                line=base_line + class_body.count("\n", 0, m.start()),
            )
        )


def extract_ts(root: Path, surface: str, index_rel: str, generated_glob: str) -> Extraction:
    ex = Extraction(surface=surface)
    index_path = root / index_rel
    gen_dir = root / generated_glob
    gen_files = sorted(gen_dir.glob("*_gen.ts")) if gen_dir.exists() else []

    all_texts = []
    if index_path.exists():
        all_texts.append(index_path.read_text(encoding="utf-8"))
    for g in gen_files:
        all_texts.append(g.read_text(encoding="utf-8"))
    enum_types = _collect_enum_types(all_texts)

    if index_path.exists():
        _parse_text(
            index_path.read_text(encoding="utf-8"),
            str(index_path.relative_to(root)),
            surface,
            ex,
            enum_types,
            parse_methods=True,
        )
    for g in gen_files:
        _parse_text(
            g.read_text(encoding="utf-8"),
            str(g.relative_to(root)),
            surface,
            ex,
            enum_types,
            parse_methods=False,
        )
    return ex
