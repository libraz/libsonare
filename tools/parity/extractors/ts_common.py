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

The public surface of a facade is NOT confined to the index file: the index
re-exports symbols from sibling modules (``export * from './features'``,
``export { mastering } from './effects_mastering'``) and those modules may in
turn re-export from yet deeper modules (``features.ts`` -> ``feature_*``). The
class facades (``Audio``, ``Mixer``, ``RealtimeEngine``, ...) likewise live in
their own files. To capture the true surface we therefore (a) follow
``export ... from './relative'`` re-exports recursively from the index (cycle
safe, deduped) and (b) glob the whole ``bindings/<surface>/src/**/*.ts`` tree.
The two sets are unioned and de-duplicated by canonical key.

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
_METHOD_HEAD = re.compile(
    r"^[ \t]{2,4}(?:static\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.MULTILINE
)
# `export class Foo` / `export abstract class Foo` heads.
_CLASS_HEAD = re.compile(r"export\s+(?:abstract\s+)?class\s+([A-Za-z_][A-Za-z0-9_]*)\b")
_TYPE_UNION = re.compile(r"export\s+type\s+([A-Za-z0-9_]+)\s*=\s*([^;]+);", re.DOTALL)
_STRING_LIT = re.compile(r"""['"]([^'"]+)['"]""")
# A re-export that pulls (some or all) symbols from a sibling module:
#   export * from './features';
#   export { mastering, normalize } from './effects_mastering';
#   export type { Foo } from './types';
# We only care about the module specifier; whatever the module exports is
# captured by parsing that module's text directly (facade modules export exactly
# what they intend to expose). The specifier must be relative (starts with '.').
_REEXPORT_FROM = re.compile(
    r"""export\s+(?:type\s+)?(?:\*|\{[^}]*\})\s*(?:as\s+[A-Za-z_][A-Za-z0-9_]*\s+)?from\s+['"](\.[^'"]+)['"]"""
)

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
    # Optional-param marker may sit on either side: `?options` or `options?`.
    name_part = name_part.strip().strip("?").strip()
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
            ex.unparsed_notes.append(
                f"{file}:{_line_of(text, open_idx)}: {name} (unbalanced args)"
            )
            continue
        inner, _ = bal
        try:
            params = [_parse_ts_param(p, enum_types) for p in _split_ts_args(inner)]
        except Exception:  # noqa: BLE001
            ex.unparsed += 1
            ex.unparsed_notes.append(
                f"{file}:{_line_of(text, open_idx)}: {name} (param parse)"
            )
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
    # Methods of EVERY exported class (Audio, Mixer, RealtimeEngine,
    # StreamAnalyzer, StreamingMasteringChain, RealtimeVoiceChanger, ...). Each
    # class body is brace-matched and its methods recorded as
    # ``<ClassName>.<method>`` so the handle/class coverage matcher can credit
    # the prefix-stripped C handle keys against them.
    for cls in _CLASS_HEAD.finditer(text):
        class_name = cls.group(1)
        start = text.find("{", cls.end())
        if start < 0:
            continue
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
                    raw_name=f"{class_name}.{name}",
                    params=params,
                    file=file,
                    line=base_line + class_body.count("\n", 0, m.start()),
                )
            )


def _resolve_module_specifier(from_file: Path, spec: str) -> Path | None:
    """Resolve a relative ``./module`` import specifier to a ``.ts`` file path.

    TS source uses ESM specifiers that may or may not carry a ``.js`` extension
    (``./features.js`` and ``./features`` both refer to ``features.ts``). We map
    the specifier onto a sibling ``.ts`` file, also trying the ``/index.ts`` form
    for directory specifiers. Returns ``None`` when no source file exists.
    """
    base = (from_file.parent / spec).resolve()
    candidates = []
    if base.suffix == ".js":
        candidates.append(base.with_suffix(".ts"))
    elif base.suffix == ".ts":
        candidates.append(base)
    else:
        candidates.append(base.with_suffix(".ts"))
        candidates.append(base / "index.ts")
    for c in candidates:
        if c.is_file():
            return c
    return None


def _reexport_closure(index_path: Path) -> list[Path]:
    """Follow ``export ... from './relative'`` re-exports transitively.

    Starts at ``index_path`` and walks every relative re-export specifier
    (``export *`` / ``export { ... }`` / ``export type { ... }``), recursively,
    cycle-safe and deduplicated. Returns the index plus every reachable module
    in deterministic (sorted-by-path) order.
    """
    seen: set[Path] = set()
    order: list[Path] = []
    stack = [index_path]
    while stack:
        cur = stack.pop()
        if cur in seen or not cur.is_file():
            continue
        seen.add(cur)
        order.append(cur)
        try:
            text = cur.read_text(encoding="utf-8")
        except OSError:
            continue
        for m in _REEXPORT_FROM.finditer(text):
            target = _resolve_module_specifier(cur, m.group(1))
            if target is not None and target not in seen:
                stack.append(target)
    return sorted(order)


def extract_ts(
    root: Path, surface: str, index_rel: str, generated_glob: str
) -> Extraction:
    ex = Extraction(surface=surface)
    index_path = root / index_rel
    gen_dir = root / generated_glob
    gen_files = sorted(gen_dir.glob("*_gen.ts")) if gen_dir.exists() else []

    # The public facade surface is exactly what the index re-exports, followed
    # transitively: ``export * from './features'`` and
    # ``export { mastering } from './effects_mastering'`` pull sibling-module
    # symbols into the surface, and those modules re-export deeper still
    # (``features.ts`` -> ``feature_*``). The class facades (``Audio``, ``Mixer``,
    # ``RealtimeEngine``, ...) reach the index the same way. We deliberately do
    # NOT glob the whole ``src`` tree: that would sweep in internal-only modules
    # the index never re-exports (``worklet.ts`` AudioWorklet entry points,
    # ``codes.ts`` / ``module_state.ts`` enum/state plumbing, the addon loader),
    # which have no C-API counterpart by design and would be pure noise. The
    # re-export closure is the precise definition of the public surface.
    # Work in resolved paths throughout: ``_reexport_closure`` resolves symlinks
    # (e.g. macOS ``/tmp`` -> ``/private/tmp``) so we resolve ``root`` too, and
    # report locations relative to it.
    root_res = root.resolve()
    files: list[Path] = []
    seen_files: set[Path] = set()

    def _add(p: Path) -> None:
        rp = p.resolve()
        if rp in seen_files or not rp.is_file() or rp.name.endswith(".d.ts"):
            return
        seen_files.add(rp)
        files.append(rp)

    if index_path.exists():
        for p in _reexport_closure(index_path.resolve()):
            _add(p)
    for g in gen_files:
        _add(g)

    all_texts = [p.read_text(encoding="utf-8") for p in files]
    enum_types = _collect_enum_types(all_texts)

    def _rel(p: Path) -> str:
        try:
            return str(p.relative_to(root_res))
        except ValueError:
            return str(p)

    # Parse every file, then de-duplicate the recorded signatures: the same
    # symbol can be reached more than once through the re-export closure. The
    # canonical key plus the surface-native raw_name uniquely identify a
    # signature (free function ``cqt`` vs class method ``Audio.cqt``).
    raw_ex = Extraction(surface=surface)
    for p, text in zip(files, all_texts):
        _parse_text(
            text,
            _rel(p),
            surface,
            raw_ex,
            enum_types,
            parse_methods=True,
        )

    seen_sig: set[tuple[str, str]] = set()
    for sig in raw_ex.functions:
        ident = (sig.key, sig.raw_name)
        if ident in seen_sig:
            continue
        seen_sig.add(ident)
        ex.functions.append(sig)
    ex.unparsed = raw_ex.unparsed
    ex.unparsed_notes = raw_ex.unparsed_notes
    return ex
