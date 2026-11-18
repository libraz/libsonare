"""Extract the fallback literals the native binding layers substitute for an absent value.

The Node addon reads an omitted argument or key through the shared reader family
(``Optional*Arg(env, info, N, "name", FALLBACK, &out)`` / ``*Property(obj,
"name", FALLBACK)``) and the embind layer through ``v.isUndefined() ? FALLBACK :
...`` / ``optionalX(...).value_or(FALLBACK)``. Each fallback is a second spelling
of a default the C++ core already owns, so this extractor follows the value to the
core field it lands in -- a ``var.field = out;`` assignment, an ``&var.field``
out-pointer or a direct ``var.field = <reader>`` -- and names that field's struct.
``compare._native_default_drift`` then holds the literal against the struct's own
initializer.

A site whose value cannot be followed to a struct field is still returned, with
``anchor`` empty and ``unresolved`` naming why, so the comparison reports it under
``## Comparisons not performed`` rather than dropping it.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from ._tokens import split_top_level_commas
from .cpp_struct import (
    _classify_default,
    _signature_arglists,
    extract_struct_defaults,
    scan_constants,
)

NODE_ADDON_DIR = "bindings/node/src/addon"
WASM_BINDINGS_DIR = "src/wasm"
_STRUCT_ROOTS = ("src", "include")

_READER_RE = re.compile(
    r"\b((?:Optional|Finite|Auto|Midi|Int|Uint|Float|Double|Bool|Word)\w*(?:Arg|Property))\s*\("
)
_TERNARY_RE = re.compile(
    r"\b(\w+)\.isUndefined\(\)(?:\s*\|\|\s*\1\.isNull\(\))?\s*\?\s*([^:;]+?)\s*:"
)
_VALUE_OR_RE = re.compile(r"\.value_or\(\s*([^()]+?)\s*\)")
_STRING_ARG_RE = re.compile(r'^"([^"]*)"$')
_LITERAL_RE = re.compile(
    r"^-?(?:\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)[fFuUlL]*$|^(?:true|false)$"
)
_FUNC_HEAD_RE = re.compile(r"^[A-Za-z_][\w:<>,\s\*&]*\b([\w:~]+)\s*\(")
_STRUCT_HEAD_RE = re.compile(r"\bstruct\s+([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^{;]*)?\{")


@dataclass
class NativeDefault:
    """One fallback literal a native binding substitutes for an absent value."""

    surface: str  # "node-native" | "wasm-native"
    file: str
    line: int
    function: str
    param: str  # the JS-side name the reader is keyed on ("" when not stated)
    literal: str
    anchor: str = ""  # ``Struct.field`` or ``function#position`` the value lands in
    core: str | None = None  # the anchor's own default, when it has a literal one
    unresolved: str = ""  # why no core default could be named


def _strip_comments_keep_lines(text: str) -> str:
    """Blank out comments and string bodies' comment lookalikes, preserving line numbers."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _call_args(text: str, open_paren: int) -> tuple[list[str], int]:
    """Top-level comma-split arguments of the call whose ``(`` is at ``open_paren``."""
    depth = 0
    for j in range(open_paren, len(text)):
        if text[j] in "([{":
            depth += 1
        elif text[j] in ")]}":
            depth -= 1
            if depth == 0:
                inner = text[open_paren + 1 : j]
                return [a.strip() for a in split_top_level_commas(inner, "([{", ")]}")], j
    return [], len(text)


def _functions(text: str) -> list[tuple[str, int, int]]:
    """(name, start, end) of each function definition whose head starts in column 0."""
    out = []
    lines = text.split("\n")
    offsets = []
    pos = 0
    for ln in lines:
        offsets.append(pos)
        pos += len(ln) + 1
    for idx, ln in enumerate(lines):
        if (
            not ln
            or ln[0] in " \t}#/"
            or ln.startswith(("namespace", "using", "template", "struct", "class"))
        ):
            continue
        m = _FUNC_HEAD_RE.match(ln)
        if not m:
            continue
        brace = text.find("{", offsets[idx])
        semi = text.find(";", offsets[idx])
        if brace < 0 or (0 <= semi < brace):
            continue
        depth = 0
        for j in range(brace, len(text)):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    out.append((m.group(1), offsets[idx], j))
                    break
    return out


def _enclosing(funcs, pos):
    best = None
    for name, s, e in funcs:
        if s <= pos <= e and (best is None or s > best[1]):
            best = (name, s, e)
    return best


def _statement_lhs(text: str, func_start: int, pos: int) -> str:
    """The text between the previous statement boundary and ``pos``."""
    k = max(
        text.rfind(";", func_start, pos),
        text.rfind("{", func_start, pos),
        text.rfind("}", func_start, pos),
    )
    return text[k + 1 : pos]


_DECL_RE = r"(?:const\s+)?(?:[\w:]+::)?(\w+)(?:<[^<>;]*>)?(?:\s*[*&]\s*|\s+){var}\s*(?:[;{{(=,)])"


def _var_type(body: str, var: str) -> str | None:
    m = re.search(_DECL_RE.format(var=re.escape(var)), body)
    if not m or m.group(1) in {"auto", "return", "const", "else", "case", "new", "delete"}:
        return None
    return m.group(1)


def _param_type(head: str, name: str) -> str | None:
    """Type of parameter ``name`` in a function head ``(... const T& name ...)``."""
    m = re.search(rf"(?:[\w:]+::)?(\w+)\s*(?:const\s*)?[&*]?\s*{re.escape(name)}\s*[,)]", head)
    return m.group(1) if m else None


_CAST = r"(?:static_cast<[^>]+>\(\s*)?"


def _field_uses(body: str, value: str) -> set[tuple[str, str, bool]]:
    """``var.field = value;`` assignments (a ``!= 0`` or ``? 1 : 0`` bool spelling allowed), each flagged when an ``if (value != 0)`` /
    ``if (value > 0)`` test guards it -- a zero fallback there means "keep the core's own"."""
    out = set()
    for m in re.finditer(
        rf"\b(\w+)(?:\.|->)(\w+)\s*=\s*{_CAST}{value}\s*\)?\s*(?:!=\s*0\s*|\?\s*1\s*:\s*0\s*)?;",
        body,
    ):
        before = body[max(0, m.start() - 160) : m.start()]
        guarded = bool(
            re.search(rf"if\s*\(\s*{value}\s*(?:!=|>)\s*0(?:\.0)?f?\s*\)\s*\{{?\s*$", before)
        )
        out.add((m.group(1), m.group(2), guarded))
    return out


GUARDED_SENTINEL = (
    "a zero fallback is only assigned when non-zero, so it keeps the core's own default"
)
# Declines where the native literal is itself the only default the tree spells.
ONLY_DEFAULT_PREFIX = "the native literal is the only default found"
ONLY_DEFAULT_C_FIELD = f"{ONLY_DEFAULT_PREFIX}: the C struct field is consumed as-is"
ONLY_DEFAULT_ARG = f"{ONLY_DEFAULT_PREFIX}: the core argument has no default"
C_FIELD_SWITCHED = (
    "the C ABI maps the C struct field through a switch, which the walker does not follow"
)
PRESENCE_GUARDED = "read only when the key is present, so the fallback never applies"


def _presence_guarded(text: str, func_start: int, pos: int, key: str) -> bool:
    """True when the read at ``pos`` sits in a block entered only when ``key`` is present.

    The guard is ``if (!v.IsUndefined() ...)`` over ``v = obj.Get("key")``, or
    ``if (obj.Has("key"))``, on the innermost block holding the read.
    """
    depth = 0
    brace = -1
    for i in range(pos - 1, func_start - 1, -1):
        if text[i] == "}":
            depth += 1
        elif text[i] == "{":
            if depth == 0:
                brace = i
                break
            depth -= 1
    if brace < 0:
        return False
    head = re.search(r"\bif\s*\((.*)\)\s*$", text[func_start:brace], re.DOTALL)
    if head is None:
        return False
    cond = head.group(1)[-400:]
    if re.search(rf'\.Has\(\s*"{re.escape(key)}"\s*\)', cond):
        return True
    before = text[func_start:brace]
    return any(
        re.search(rf'\b{re.escape(v)}\s*=\s*[\w.>-]+\.Get\(\s*"{re.escape(key)}"\s*\)', before)
        for v in re.findall(r"!\s*(\w+)\.IsUndefined\(\)", cond)
    )


_NOT_CALLS = {"if", "while", "for", "switch", "return", "sizeof", "catch"}


def _call_uses(body: str, var: str) -> set[tuple[str, int]]:
    """(callee, position) of each call that takes ``var`` as a whole top-level argument."""
    out = set()
    for m in re.finditer(r"(?<![\w.>])([A-Za-z_][\w:]*)\s*\(", body):
        callee = m.group(1).split("::")[-1]
        if callee in _NOT_CALLS or callee.endswith(("_cast", "Arg", "Property")):
            continue
        args, _ = _call_args(body, m.end() - 1)
        for i, a in enumerate(args):
            if re.fullmatch(rf"{_CAST}{re.escape(var)}\s*\)?", a):
                out.add((callee, i))
    return out


class Resolver:
    """Follows a native value to the C++ core field or default argument it lands in.

    The value is followed through at most two C-ABI hops: a C struct field to the
    C++ field its C-ABI implementation copies it into, and a C-ABI argument to the
    call its implementation passes it on to.
    """

    def __init__(self, root: Path) -> None:
        self.root = root
        self.headers: dict[Path, str] = {}
        for base in _STRUCT_ROOTS:
            for path in sorted((root / base).rglob("*.h")):
                raw = path.read_text(encoding="utf-8", errors="replace")
                self.headers[path] = _strip_comments_keep_lines(raw)
        self.structs: dict[str, Path] = {}
        for path, text in self.headers.items():
            for m in _STRUCT_HEAD_RE.finditer(text):
                self.structs.setdefault(m.group(1), path)
        self.constants = scan_constants([root / "src/util/constants.h", *self.headers])
        self.c_defs: dict[str, tuple[str, str]] = {}
        for path in sorted((root / "src/c_api").rglob("*.cpp")):
            text = _strip_comments_keep_lines(path.read_text(encoding="utf-8"))
            for name, s, e in _functions(text):
                brace = text.find("{", s)
                key, body = name.split("::")[-1], text[brace:e]
                # A feature-off stub shares the name; the real definition wins.
                if key not in self.c_defs or "_STUB_" in self.c_defs[key][1]:
                    self.c_defs[key] = (text[s:brace], body)
        self._struct_cache: dict[str, dict[str, str]] = {}
        self._func_cache: dict[str, list[str | None] | None] = {}

    def struct_default(self, struct: str, field: str) -> tuple[str | None, str]:
        if struct not in self.structs:
            return None, "the value lands in a binding-local struct, not a core one"
        if struct not in self._struct_cache:
            self._struct_cache[struct] = extract_struct_defaults(
                self.structs[struct], struct, self.constants
            )
        core = self._struct_cache[struct].get(field)
        return core, "" if core is not None else "the core struct field has no literal initializer"

    def func_default(self, func: str, pos: int) -> tuple[str | None, str]:
        """Default argument at ``pos`` of C++ function ``func``; every overload must agree."""
        if func not in self._func_cache:
            per: list[set] = []
            for text in self.headers.values():
                if func not in text:
                    continue
                for arglist in _signature_arglists(text, func):
                    for i, part in enumerate(split_top_level_commas(arglist)):
                        while len(per) <= i:
                            per.append(set())
                        _lhs, eq, rhs = part.partition("=")
                        per[i].add(_classify_default(rhs, self.constants) if eq else None)
            self._func_cache[func] = [next(iter(v)) if len(v) == 1 else None for v in per] or None
        defaults = self._func_cache[func]
        if defaults is None:
            return None, "the value is passed to a function with no C++ declaration in a header"
        if pos >= len(defaults) or defaults[pos] is None:
            return None, ONLY_DEFAULT_ARG
        return defaults[pos], ""

    def resolve(
        self, body: str, target: str, head: str = "", depth: int = 0
    ) -> tuple[str, str | None, str]:
        """(anchor, core default, unresolved reason) for ``target`` inside ``body``."""
        target = target.strip().lstrip("&").strip()
        m = re.fullmatch(r"(\w+)(?:\.|->)(\w+)", target)
        if m:
            return self._field_of(body, head, m.group(1), m.group(2), depth)
        if not re.fullmatch(r"\w+", target):
            return "", None, "value is used in an expression, not stored"
        fields = _field_uses(body, re.escape(target))
        if len(fields) == 1:
            var, field, guarded = fields.pop()
            if guarded:
                return f"{var}.{field}", None, GUARDED_SENTINEL
            return self._field_of(body, head, var, field, depth)
        if len(fields) > 1:
            return "", None, "value assigned to more than one field"
        calls = _call_uses(body, target)
        if not calls:
            return "", None, "value neither stored in a field nor passed to a call"
        results = [self._call_default(callee, pos, depth) for callee, pos in sorted(calls)]
        if len(results) == 1:
            return results[0]
        cores = {core for _a, core, _w in results}
        if len(cores) == 1 and None not in cores:
            return " & ".join(dict.fromkeys(a for a, _c, _w in results)), cores.pop(), ""
        return "", None, "value passed to several calls whose defaults differ or are missing"

    def _call_default(self, callee: str, pos: int, depth: int) -> tuple[str, str | None, str]:
        if callee.startswith("sonare_"):
            return self._through_c_function(callee, pos, depth)
        core, why = self.func_default(callee, pos)
        return f"{callee}#{pos}", core, why

    def _field_of(self, body, head, var, field, depth):
        ty = _var_type(body, var) or (_param_type(head, var) if head else None)
        if not ty:
            return "", None, "the struct the value is stored in is not declared in the function"
        if ty.startswith("Sonare"):
            return self._through_c_struct(ty, field, depth)
        core, why = self.struct_default(ty, field)
        return f"{ty}.{field}", core, why

    def _through_c_struct(self, struct: str, field: str, depth: int):
        """Follow a C ABI struct field to the C++ field the C ABI copies it into."""
        anchor = f"{struct}.{field}"
        if depth >= 2:
            return anchor, None, "the C ABI hop limit was reached"
        hits = set()
        switched = False
        for head, body in self.c_defs.values():
            pm = re.search(rf"\b{struct}\s*(?:const\s*)?[&*]\s*(\w+)", head)
            if not pm:
                continue
            switched = switched or bool(
                re.search(rf"\bswitch\s*\(\s*{pm.group(1)}(?:\.|->){field}\s*\)", body)
            )
            for var, cpp_field, guarded in _field_uses(body, rf"{pm.group(1)}(?:\.|->){field}\b"):
                ty = _var_type(body, var) or _param_type(head, var)
                if ty and not ty.startswith("Sonare"):
                    hits.add((ty, cpp_field, guarded))
        if not hits:
            return anchor, None, C_FIELD_SWITCHED if switched else ONLY_DEFAULT_C_FIELD
        if len(hits) > 1:
            return anchor, None, "the C ABI struct field is copied into several C++ fields"
        ty, cpp_field, guarded = hits.pop()
        if guarded:
            return f"{ty}.{cpp_field}", None, GUARDED_SENTINEL
        core, why = self.struct_default(ty, cpp_field)
        return f"{ty}.{cpp_field}", core, why

    def _through_c_function(self, func: str, pos: int, depth: int):
        """Follow a C ABI argument into its implementation."""
        anchor = f"{func}#{pos}"
        if func not in self.c_defs or depth >= 2:
            return (
                anchor,
                None,
                "the C ABI argument is not followed (no definition, or the hop limit)",
            )
        head, body = self.c_defs[func]
        inner = head[head.find("(") + 1 : head.rfind(")")]
        params = split_top_level_commas(inner, "([{", ")]}")
        pname = re.search(r"(\w+)\s*$", params[pos].strip()) if pos < len(params) else None
        if not pname:
            return anchor, None, "the C ABI argument is not a named parameter"
        return self.resolve(body, pname.group(1), head, depth + 1)


def _reader_sites(text: str):
    """(position, param, fallback, target) of each Node reader call carrying a fallback."""
    funcs = _functions(text)
    for m in _READER_RE.finditer(text):
        name = m.group(1)
        args, _end = _call_args(text, m.end() - 1)
        key_idx = next((i for i, a in enumerate(args) if _STRING_ARG_RE.match(a)), None)
        if key_idx is None or key_idx + 1 >= len(args):
            continue
        fn = _enclosing(funcs, m.start())
        if fn is None:
            continue
        if name.endswith("Arg"):
            target = next((a for a in reversed(args) if a.startswith("&")), "")
        else:
            lhs = _statement_lhs(text, fn[1], m.start())
            am = re.search(r"([\w.>-]+)\s*=\s*$", lhs)
            target = am.group(1) if am else ""
        yield fn, m.start(), _STRING_ARG_RE.match(args[key_idx]).group(1), args[key_idx + 1], target


def _embind_sites(text: str):
    """(position, param, fallback, target) of each embind ternary / value_or fallback."""
    funcs = _functions(text)
    found = [(m.start(), m.group(2).strip(), m.group(1)) for m in _TERNARY_RE.finditer(text)]
    found += [(m.start(), m.group(1).strip(), "") for m in _VALUE_OR_RE.finditer(text)]
    for start, fallback, var in found:
        fn = _enclosing(funcs, start)
        if fn is None:
            continue
        lhs = _statement_lhs(text, fn[1], start)
        am = re.search(r"([\w.>-]+)\s*=\s*[^=]*$", lhs)
        yield fn, start, _key_from_val(text, start, var), fallback, am.group(1) if am else ""


def _key_from_val(text: str, start: int, var: str) -> str:
    """The JS key a ``val`` came from: ``val x = obj["key"]`` or a ``"key"`` in the statement."""
    if var:
        dm = re.search(rf"\b{re.escape(var)}\s*=\s*\w+\[\s*\"(\w+)\"\s*\]", text[:start])
        if dm:
            return dm.group(1)
    seg = text[text.rfind(";", 0, start) + 1 : text.find(";", start)]
    km = re.findall(r'"(\w+)"', seg)
    return km[-1] if km else ""


def extract(root: Path) -> tuple[list[NativeDefault], dict[str, str]]:
    """Every fallback-literal site in the Node addon and the embind layer, resolved,
    with the named-constant table the resolver read (for a fallback spelled as one)."""
    resolver = Resolver(root)
    out: list[NativeDefault] = []
    sources = [
        ("node-native", p, _reader_sites)
        for p in sorted((root / NODE_ADDON_DIR).rglob("*.cpp"))
        + sorted((root / NODE_ADDON_DIR).rglob("*.h"))
        if not p.name.startswith("sonare_wrap_options")
    ]
    sources += [
        ("wasm-native", p, _embind_sites) for p in sorted((root / WASM_BINDINGS_DIR).rglob("*.cpp"))
    ]
    for surface, path, finder in sources:
        rel = path.relative_to(root).as_posix()
        text = _strip_comments_keep_lines(path.read_text(encoding="utf-8"))
        for fn, pos, param, fallback, target in finder(text):
            rec = NativeDefault(
                surface=surface,
                file=rel,
                line=text.count("\n", 0, pos) + 1,
                function=fn[0],
                param=param,
                literal=fallback,
            )
            if fallback.startswith("ZeroIsSentinel"):
                rec.unresolved = "zero is the library-default sentinel, not a default value"
            elif param and _presence_guarded(text, fn[1], pos, param):
                rec.unresolved = PRESENCE_GUARDED
            elif not target:
                rec.unresolved = "reader result is not stored"
            else:
                rec.anchor, rec.core, rec.unresolved = resolver.resolve(text[fn[1] : fn[2]], target)
            out.append(rec)
    return out, resolver.constants


def literal_value(value: str, constants: dict[str, str]) -> str | None:
    """The fallback as a comparable literal, resolving a named constant; None otherwise."""
    value = value.strip()
    if _LITERAL_RE.match(value):
        return value
    return constants.get(value.split("::")[-1])
