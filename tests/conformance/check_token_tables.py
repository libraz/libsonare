"""Check every facade's token table for a closed set against the core's own tokens.

A closed set the core writes by name (``to_string``) and reads back
(``<set>_from_string``) is spelled three more times on each surface: a runtime
table mapping token to the C ABI ordinal the scalar entry points take, sometimes
a token list indexed by that ordinal, and the declared union a type checker sees.
Each row below names those sources for one set. The written tokens are the set;
``from_string`` must accept exactly those plus the row's declared earlier forms,
so a third spelling fails. The C header's enumerators give each token's ordinal,
matched by name. A source that adds, drops or reorders a token fails with the
token named.

Document keys an earlier file form wrote follow the same rule (``KEY_FORMS``):
the core reader takes each beside its current key, nothing writes it, and the
only other places the old spelling may appear are the declared readers that
mirror the core's.

Run: python3 tests/conformance/check_token_tables.py
Self-tests: python3 -m unittest tests/conformance/test_token_tables.py
"""

from __future__ import annotations

import re
import sys
import types
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Source kinds: ordinal tables map token -> ordinal; set sources only list tokens.
ORDINAL_KINDS = ("ts_object", "ts_array", "py_tuple")
SET_KINDS = ("ts_union", "py_literal")


class Source(typing.NamedTuple):
    surface: str
    path: str
    kind: str
    symbol: str


class CoreTable(typing.NamedTuple):
    path: str
    enum: str  # C++ type whose ``to_string`` overload writes the tokens
    from_string: str


class TokenSet(typing.NamedTuple):
    name: str
    core: CoreTable
    c_enum: tuple[str, str]  # (header, enumerator prefix)
    sources: tuple[Source, ...]
    # Token -> enumerator suffix, only where the names do not normalize alike.
    c_aliases: typing.Mapping[str, str] = types.MappingProxyType({})
    # Spellings an earlier file form wrote, still read and never written.
    earlier_forms: tuple[str, ...] = ()


def _src(surface: str, path: str, kind: str, symbol: str) -> Source:
    return Source(surface, path, kind, symbol)


ROWS: tuple[TokenSet, ...] = (
    TokenSet(
        name="pan law",
        core=CoreTable("src/mixing/api/scene_json.cpp", "PanLaw", "pan_law_from_string"),
        c_enum=("include/sonare/sonare_c_mixing.h", "SONARE_PAN_LAW_"),
        c_aliases={"const4.5Db": "CONST_4P5DB"},
        sources=(
            _src("node", "bindings/node/src/value_coercion.ts", "ts_object", "PAN_LAW_VALUES"),
            _src("node", "bindings/node/src/types_mixing.ts", "ts_union", "PanLaw"),
            _src("wasm", "bindings/wasm/src/codes.ts", "ts_object", "PAN_LAW_VALUES"),
            _src("wasm", "bindings/wasm/src/public_types_mixing.ts", "ts_union", "PanLaw"),
            _src("python", "bindings/python/src/libsonare/_types_enums.py", "py_literal", "PanLaw"),
            _src(
                "python", "bindings/python/src/libsonare/_types_enums.py", "py_tuple", "_PAN_LAWS"
            ),
        ),
    ),
    TokenSet(
        name="pan mode",
        core=CoreTable("src/mixing/api/scene_json.cpp", "PanMode", "pan_mode_from_string"),
        c_enum=("include/sonare/sonare_c_mixing.h", "SONARE_PAN_MODE_"),
        sources=(
            _src("node", "bindings/node/src/value_coercion.ts", "ts_object", "PAN_MODE_VALUES"),
            _src("node", "bindings/node/src/types_mixing.ts", "ts_union", "PanMode"),
            _src("wasm", "bindings/wasm/src/codes.ts", "ts_object", "PAN_MODE_VALUES"),
            _src("wasm", "bindings/wasm/src/public_types_mixing.ts", "ts_union", "PanMode"),
            _src(
                "python", "bindings/python/src/libsonare/_types_enums.py", "py_literal", "PanMode"
            ),
            _src(
                "python", "bindings/python/src/libsonare/_types_enums.py", "py_tuple", "_PAN_MODES"
            ),
        ),
    ),
    TokenSet(
        name="send timing",
        core=CoreTable("src/mixing/api/scene_json.cpp", "SendTiming", "send_timing_from_string"),
        earlier_forms=("pre", "post"),
        c_enum=("include/sonare/sonare_c_mixing.h", "SONARE_SEND_TIMING_"),
        sources=(
            _src("node", "bindings/node/src/value_coercion.ts", "ts_object", "SEND_TIMING_VALUES"),
            _src("node", "bindings/node/src/types_mixing.ts", "ts_union", "SendTiming"),
            _src("wasm", "bindings/wasm/src/codes.ts", "ts_object", "SEND_TIMING_VALUES"),
            _src("wasm", "bindings/wasm/src/public_types_mixing.ts", "ts_union", "SendTiming"),
            _src(
                "python",
                "bindings/python/src/libsonare/_types_enums.py",
                "py_literal",
                "SendTiming",
            ),
            _src(
                "python",
                "bindings/python/src/libsonare/_types_enums.py",
                "py_tuple",
                "_SEND_TIMINGS",
            ),
        ),
    ),
)



class KeyForm(typing.NamedTuple):
    """A document key an earlier file form wrote, still read and never written."""

    name: str
    core: str  # reader and writer of the document
    current: str
    earlier: str
    mirrors: tuple[str, ...] = ()  # TS readers that must take both spellings as the core does


_SCENE = "src/mixing/api/scene_json.cpp"
_WORKLET_MIRROR = ("bindings/wasm/src/worklet/engine-mixer-facade.ts",)

KEY_FORMS: tuple[KeyForm, ...] = (
    KeyForm("scene insert processor", _SCENE, "processor", "processor_name", _WORKLET_MIRROR),
    KeyForm("scene insert params", _SCENE, "params", "params_json", _WORKLET_MIRROR),
    KeyForm("scene insert sidechain", _SCENE, "sidechainKey", "sidechain_key", _WORKLET_MIRROR),
)

# Trees an earlier key may be written from.
KEY_SWEEP_ROOTS = ("src", "include", "bindings/node/src", "bindings/wasm/src", "bindings/python/src")
KEY_SWEEP_SUFFIXES = (".cpp", ".h", ".ts", ".py", ".pyi")


def _core_reads(form: KeyForm) -> re.Pattern[str]:
    return re.compile(
        rf'_or_legacy\([^;]*?"{re.escape(form.current)}"\s*,\s*"{re.escape(form.earlier)}"'
    )


def _mirror_reads(form: KeyForm) -> re.Pattern[str]:
    return re.compile(
        rf"aliasedValue\(\s*\w+\s*,\s*'{re.escape(form.current)}'\s*,\s*'{re.escape(form.earlier)}'\s*\)"
    )


def _writes(key: str) -> re.Pattern[str]:
    return re.compile(rf'\bput\(\s*\w+\s*,\s*"{re.escape(key)}"')


def evaluate_key_forms(
    root: Path, forms: tuple[KeyForm, ...] = KEY_FORMS
) -> tuple[list[str], int, int]:
    """Problems, forms checked and reader sites matched."""
    problems: list[str] = []
    matched = 0
    sweep = [
        f
        for base in KEY_SWEEP_ROOTS
        if (root / base).is_dir()
        for f in sorted((root / base).rglob("*"))
        if f.suffix in KEY_SWEEP_SUFFIXES and not f.name.endswith(".d.ts")
    ]
    texts = {f: f.read_text(encoding="utf-8", errors="replace") for f in sweep}
    for form in forms:
        core_path = root / form.core
        core = core_path.read_text(encoding="utf-8") if core_path.is_file() else ""
        if not _core_reads(form).search(core):
            problems.append(f"{form.name}: {form.core} does not read {form.earlier!r} beside {form.current!r}")
        else:
            matched += 1
        if not re.search(rf'known_keys\([^;]*"{re.escape(form.earlier)}"', core):
            problems.append(f"{form.name}: {form.core} does not declare {form.earlier!r} a known key")
        if not _writes(form.current).search(core):
            problems.append(f"{form.name}: {form.core} writes no {form.current!r}")
        for mirror in form.mirrors:
            path = root / mirror
            text = path.read_text(encoding="utf-8") if path.is_file() else ""
            if _mirror_reads(form).search(text):
                matched += 1
            else:
                problems.append(f"{form.name}: {mirror} does not read {form.earlier!r} beside {form.current!r}")
        quoted = re.compile(rf"""(['"]){re.escape(form.earlier)}\1""")
        readers = (_core_reads(form), _mirror_reads(form), re.compile(r"known_keys\("))
        for f, text in texts.items():
            for i, line in enumerate(text.splitlines(), 1):
                if quoted.search(line) and not any(r.search(line) for r in readers):
                    rel = f.relative_to(root)
                    problems.append(f"{form.name}: {form.earlier!r} appears outside a reader at {rel}:{i}")
    return problems, len(forms), matched


_STR = re.compile(r"""(['"])((?:(?!\1).)+)\1""")


def _norm(name: str) -> str:
    return re.sub(r"[^0-9a-z]", "", name.lower())


def _balanced(text: str, start: int, open_ch: str, close_ch: str) -> str | None:
    """The text between the bracket at or after ``start`` and its match."""
    i = text.find(open_ch, start)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == open_ch:
            depth += 1
        elif text[j] == close_ch:
            depth -= 1
            if depth == 0:
                return text[i + 1 : j]
    return None


def _function_body(text: str, head: re.Pattern[str]) -> str | None:
    m = head.search(text)
    if m is None:
        return None
    return _balanced(text, text.find("{", m.end()), "{", "}")


def read_tokens(text: str, function: str) -> list[str] | None:
    """Tokens a ``from_string`` function accepts, in source order."""
    body = _function_body(text, re.compile(rf"\b{re.escape(function)}\s*\("))
    return (re.findall(r'==\s*"([^"]+)"', body) or None) if body else None


def written_tokens(text: str, enum: str) -> list[str] | None:
    """Tokens the ``to_string`` overload for ``enum`` returns, in source order."""
    head = re.compile(rf"\bto_string\s*\(\s*(?:const\s+)?{re.escape(enum)}\b")
    body = _function_body(text, head)
    if not body:
        return None
    tokens = [t for r in re.findall(r"\breturn\b([^;]*);", body) for _q, t in _STR.findall(r)]
    return tokens or None


def c_enumerators(text: str, prefix: str) -> dict[str, int]:
    """Enumerator suffix -> value for every ``<prefix>NAME = n``."""
    return {
        m.group(1): int(m.group(2))
        for m in re.finditer(rf"\b{re.escape(prefix)}(\w+)\s*=\s*(-?\d+)", text)
    }


def read_source(text: str, src: Source) -> dict[str, int] | set[str] | None:
    """An ordinal kind as token -> ordinal, a set kind as a token set; None when absent."""
    sym = re.escape(src.symbol)
    if src.kind == "ts_object":
        m = re.search(rf"\bconst\s+{sym}\b[^=]*=", text)
        body = m and _balanced(text, m.end(), "{", "}")
        if not body:
            return None
        return {
            e.group(2) or e.group(3): int(e.group(4))
            for e in re.finditer(r"""(?:(['"])([^'"]+)\1|([\w$]+))\s*:\s*(-?\d+)""", body)
        }
    if src.kind == "ts_array":
        m = re.search(rf"\bconst\s+{sym}\b[^=]*=", text)
        body = m and _balanced(text, m.end(), "[", "]")
        if body is None or body is False:
            return None
        return {tok: i for i, (_q, tok) in enumerate(_STR.findall(body))}
    if src.kind == "ts_union":
        m = re.search(rf"\btype\s+{sym}\s*=([^;]*);", text)
        return {tok for _q, tok in _STR.findall(m.group(1))} if m else None
    if src.kind == "py_literal":
        m = re.search(rf"^{sym}\s*=\s*Literal\[", text, re.MULTILINE)
        body = m and _balanced(text, m.end() - 1, "[", "]")
        return {tok for _q, tok in _STR.findall(body)} if body else None
    if src.kind == "py_tuple":
        m = re.search(rf"^{sym}\s*(?::[^=]*)?=\s*(.*)$", text, re.MULTILINE)
        if m is None:
            return None
        rhs = m.group(1)
        args = re.match(r"get_args\((\w+)\)", rhs)
        if args:
            lit = re.search(rf"^{args.group(1)}\s*=\s*Literal\[", text, re.MULTILINE)
            body = lit and _balanced(text, lit.end() - 1, "[", "]")
        else:
            body = _balanced(text, m.start(1), "(", ")")
        if not body:
            return None
        return {tok: i for i, (_q, tok) in enumerate(_STR.findall(body))}
    raise ValueError(f"unknown source kind {src.kind!r}")


def evaluate(root: Path, rows: tuple[TokenSet, ...] = ROWS) -> tuple[list[str], int, int]:
    """Problems, rows checked and sources compared."""
    problems: list[str] = []
    compared = 0

    def read(path: str) -> str | None:
        p = root / path
        return p.read_text(encoding="utf-8") if p.is_file() else None

    for row in rows:
        core_text = read(row.core.path) or ""
        tokens = written_tokens(core_text, row.core.enum)
        accepted = read_tokens(core_text, row.core.from_string)
        if not tokens or not accepted:
            missing = f"to_string({row.core.enum})" if not tokens else row.core.from_string
            problems.append(f"{row.name}: no tokens found in {missing} ({row.core.path})")
            continue
        for tok in sorted(set(row.earlier_forms) & set(tokens)):
            problems.append(f"{row.name}: earlier form {tok!r} is still written")
        for tok in sorted(set(accepted) - set(tokens) - set(row.earlier_forms)):
            problems.append(f"{row.name}: {row.core.from_string} accepts undeclared {tok!r}")
        for tok in sorted((set(tokens) | set(row.earlier_forms)) - set(accepted)):
            problems.append(f"{row.name}: {row.core.from_string} does not accept {tok!r}")
        header = read(row.c_enum[0]) or ""
        enumerators = c_enumerators(header, row.c_enum[1])
        by_norm = {_norm(k): v for k, v in enumerators.items()}
        expected: dict[str, int] = {}
        for tok in tokens:
            key = _norm(row.c_aliases.get(tok, tok))
            if key not in by_norm:
                problems.append(
                    f"{row.name}: core token {tok!r} matches no {row.c_enum[1]}* enumerator"
                )
                continue
            expected[tok] = by_norm[key]
        unmatched = sorted(set(enumerators.values()) - set(expected.values()))
        if unmatched:
            problems.append(
                f"{row.name}: {row.c_enum[1]}* ordinals {unmatched} carry no core token"
            )
        for src in row.sources:
            text = read(src.path)
            got = read_source(text, src) if text is not None else None
            where = f"{row.name}: {src.surface} {src.symbol} ({src.path})"
            if got is None:
                problems.append(f"{where}: not found")
                continue
            compared += 1
            names = set(got)
            for tok in sorted(names - set(tokens)):
                problems.append(f"{where}: extra token {tok!r}")
            for tok in sorted(set(tokens) - names):
                problems.append(f"{where}: missing token {tok!r}")
            if isinstance(got, dict):
                for tok in sorted(names & set(expected)):
                    if got[tok] != expected[tok]:
                        problems.append(
                            f"{where}: token {tok!r} has ordinal {got[tok]}, the C ABI's is {expected[tok]}"
                        )
    return problems, len(rows), compared


def main() -> int:
    problems, rows, compared = evaluate(ROOT)
    print(f"token tables: {rows} rows, {compared} sources compared")
    if rows == 0 or compared == 0:
        print("FAIL: nothing was compared")
        return 1
    key_problems, forms, readers = evaluate_key_forms(ROOT)
    print(f"earlier document keys: {forms} keys, {readers} reader sites matched")
    if forms == 0 or readers == 0:
        print("FAIL: no earlier document key was compared")
        return 1
    problems += key_problems
    for p in problems:
        print(f"FAIL: {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
