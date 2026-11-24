"""Generate the Python table of integer domains the refusal of a non-integer reads.

The core states each member's numeric domain once (``tools/api/numeric-domains.json``,
dumped from ``src/c_api/numeric_domain_rows.h``). Python narrows a caller's integer before
the core sees it, so a fraction, a NaN or a value no C ``int`` holds never reaches the
core check; this table lets that refusal state the member's own domain instead of the C
type's range. A row is keyed by the public entry name (``_entry``) and the caller's
keyword. The C entries an entry reaches are the ``sonare_*`` symbols its body names; a C
subject keeps only its member name and is respelled through ``_field_spelling``, the same
rendering a core refusal goes through. A row exists only for a subject the facade binds: a
member the field-spelling derivation leaves unbound on Python, or a direct C parameter the
entry's signature does not take, is a domain no caller value can reach.

Usage: python3 tools/api/gen_integer_domains.py [--check] [--source <json>]
"""

from __future__ import annotations

import ast
import json
import re
import sys
from collections.abc import Iterator
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/api/numeric-domains.json"
TARGET = ROOT / "bindings/python/src/libsonare/_integer_domains.py"
SPELLING = ROOT / "bindings/python/src/libsonare/_field_spelling.py"
PACKAGE = ROOT / "bindings/python/src/libsonare"
_SYMBOL = re.compile(r"sonare_([a-z0-9_]+)")
Domain = tuple[int, int | None, str | None]


def _text(node: ast.expr) -> str:
    assert isinstance(node, ast.Constant) and isinstance(node.value, str)
    return node.value


def overrides() -> dict[tuple[str, str], str]:
    """The generated respelling table, read from its literal without importing the package."""
    for node in ast.parse(SPELLING.read_text()).body:
        if isinstance(node, ast.AnnAssign) and getattr(node.target, "id", None) == "OVERRIDES":
            assert isinstance(node.value, ast.Dict)
            table: dict[tuple[str, str], str] = {}
            for key, value in zip(node.value.keys, node.value.values, strict=True):
                assert isinstance(key, ast.Tuple) and len(key.elts) == 2
                table[(_text(key.elts[0]), _text(key.elts[1]))] = _text(value)
            return table
    raise SystemExit(f"{SPELLING.relative_to(ROOT)} declares no OVERRIDES")


# Resolvers that map an omitted keyword onto the C ABI's "not given" value and refuse it
# when typed. Such a keyword keeps its row only where the row declares that sentinel.
_SENTINEL_RESOLVERS = frozenset(
    {"_resolve_zero_default", "_resolve_absent_positive", "_optional_nonzero"}
)


# A stricter value naming a kind of integer changes the refusal's wording; any other names a
# relation with another member, refused separately in its own words.
_KIND_STRICTER = frozenset({"power_of_two", "even", "odd"})


def _decorated_entries(
    package: Path,
) -> Iterator[tuple[str, ast.FunctionDef | ast.AsyncFunctionDef]]:
    """(public entry name, function node) for every ``_entry``-decorated function."""
    for path in sorted(package.glob("*.py")):
        for node in ast.walk(ast.parse(path.read_text())):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            for dec in node.decorator_list:
                if (
                    isinstance(dec, ast.Call)
                    and getattr(dec.func, "id", None) == "_entry"
                    and dec.args
                    and isinstance(dec.args[0], ast.Constant)
                ):
                    yield dec.args[0].value, node


def _named_symbols(node: ast.AST) -> tuple[set[str], set[str]]:
    """(C entries a body names, private module-level helpers it calls by bare name)."""
    named: set[str] = set()
    called: set[str] = set()
    for n in ast.walk(node):
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id[:1] == "_":
            called.add(n.func.id)
        if isinstance(n, ast.Attribute):
            m = _SYMBOL.fullmatch(n.attr)
        elif isinstance(n, ast.Constant) and isinstance(n.value, str):
            m = _SYMBOL.fullmatch(n.value)
        else:
            continue
        if m:
            named.add(m.group(1))
    return named, called


def _bodies(package: Path) -> dict[str, list[ast.AST]]:
    """Public entry name -> its body, plus the private helpers it delegates to.

    An entry whose body names no C entry of its own reaches the C entries through the
    private module-level helpers it calls by bare name, followed through such helpers.
    """
    helpers: dict[str, ast.FunctionDef | ast.AsyncFunctionDef] = {}
    for path in sorted(package.glob("*.py")):
        for node in ast.parse(path.read_text()).body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name[:1] == "_":
                helpers[node.name] = node
    out: dict[str, list[ast.AST]] = {}
    for entry, node in _decorated_entries(package):
        bodies: list[ast.AST] = [node]
        named, pending = _named_symbols(node)
        seen: set[str] = set()
        while not named and pending:
            name = pending.pop()
            if name in seen or name not in helpers:
                continue
            seen.add(name)
            bodies.append(helpers[name])
            found, more = _named_symbols(helpers[name])
            named |= found
            pending |= more
        out[entry] = bodies
    return out


def entries(package: Path = PACKAGE) -> dict[str, frozenset[str]]:
    """Public entry name -> the C entries (without ``sonare_``) its bodies name."""
    return {
        entry: frozenset(set().union(*(_named_symbols(b)[0] for b in bodies)))
        for entry, bodies in _bodies(package).items()
    }


def sentinel_keywords(package: Path = PACKAGE) -> set[tuple[str, str]]:
    """(entry, keyword) pairs whose keyword an entry resolves through a sentinel resolver."""
    out: set[tuple[str, str]] = set()
    for entry, bodies in _bodies(package).items():
        for body in bodies:
            for n in ast.walk(body):
                if (
                    isinstance(n, ast.Call)
                    and getattr(n.func, "id", None) in _SENTINEL_RESOLVERS
                    and n.args
                    and isinstance(n.args[0], ast.Name)
                ):
                    out.add((entry, n.args[0].id))
    return out


def named_keywords(package: Path = PACKAGE) -> set[tuple[str, str]]:
    """(entry, keyword) pairs annotated with no ``int``: an enumeration a caller names."""
    out: set[tuple[str, str]] = set()
    for entry, node in _decorated_entries(package):
        a = node.args
        for arg in [*a.posonlyargs, *a.args, *a.kwonlyargs]:
            if arg.annotation is not None and "int" not in ast.unparse(arg.annotation):
                out.add((entry, arg.arg))
    return out


def signatures(package: Path = PACKAGE) -> dict[str, frozenset[str]]:
    """Public entry name -> the parameter names its signature takes.

    An entry taking ``**kwargs`` is left out, since any keyword may reach it.
    """
    out: dict[str, set[str]] = {}
    for entry, node in _decorated_entries(package):
        a = node.args
        if a.kwarg is not None:
            continue
        out.setdefault(entry, set()).update(x.arg for x in [*a.posonlyargs, *a.args, *a.kwonlyargs])
    return {entry: frozenset(names) for entry, names in out.items()}


def python_unbound() -> set[tuple[str, str]]:
    """(C entry, C subject) pairs the field-spelling derivation finds no Python binding for."""
    import tomllib

    sys.path[:0] = [str(ROOT / "tools/api"), str(ROOT / "tools/parity")]
    import gen_field_spelling as spelling

    renames = tomllib.loads(spelling.RENAMES.read_text(encoding="utf-8"))
    bindings = spelling.load_bindings()(ROOT)
    _, unbound, _ = spelling.derive(bindings, {}, renames, spelling.load_c_params(ROOT))
    return {(u.c_key, u.c_subject) for u in unbound if u.surface == "python"}


def _reachable(
    entry: str,
    c_entry: str,
    param: str,
    keyword: str,
    taken: dict[str, frozenset[str]] | None,
    unbound: set[tuple[str, str]],
) -> bool:
    """Whether a caller's value can reach a row: the facade binds its subject.

    A member of an array element keeps its path and is judged by neither test.
    """
    if "[]" in param:
        return True
    path = param.split(".")
    if (c_entry, path[-1]) in unbound:
        return False
    return len(path) > 1 or taken is None or entry not in taken or keyword in taken[entry]


def _keyword(param: str, entry: str, spelled: dict[tuple[str, str], str]) -> str:
    """The caller's spelling of a row's subject.

    A member of a struct argument is a keyword of its own; an array element or a member of
    one keeps its path from the first indexed container (``levels[]``,
    ``originals[].source_id``), the spelling a facade refusal takes once its index is
    written empty.
    """
    path = param.split(".")
    first = next((i for i, part in enumerate(path) if part.endswith("[]")), None)
    if first is None:
        return spelled.get((entry, path[-1]), path[-1])
    if first == len(path) - 1:
        subject = path[-1].removesuffix("[]")
        return spelled.get((entry, subject), subject) + "[]"
    return ".".join(path[first:])


def _explicit_bounds(row: dict) -> tuple[int, int | None]:
    """The bounds an explicitly given value must meet: the row's, less its sentinel.

    Absence selects the sentinel, so a typed sentinel is refused; it may only sit on an
    endpoint, since a hole inside the range has no ``[lo, hi]`` spelling.
    """
    lo, hi = int(row["lo"]), None if row["hi"] is None else int(row["hi"])
    sentinel = row.get("sentinel")
    if sentinel is None or sentinel < lo or (hi is not None and sentinel > hi):
        return lo, hi
    if sentinel == lo:
        return lo + 1, hi
    if sentinel == hi:
        return lo, hi - 1
    raise SystemExit(f"{row['entry']} {row['param']}: sentinel {sentinel} inside [{lo}, {hi}]")


def rows(
    domains: list[dict],
    spelled: dict[tuple[str, str], str],
    reached: dict[str, frozenset[str]],
    excluded: set[tuple[str, str]] = frozenset(),
    sentinels: set[tuple[str, str]] = frozenset(),
    taken: dict[str, frozenset[str]] | None = None,
    unbound: set[tuple[str, str]] = frozenset(),
) -> dict[tuple[str, str], Domain]:
    """(entry, keyword) -> (lo, hi, stricter) for every integer domain.

    ``hi`` is None when open above; ``stricter`` is the row's power-of-two / even / odd
    restriction, or None.

    A keyword two reached C entries bound differently gets no row, since the caller's
    value would be refused in either domain's words; nor does a keyword in ``excluded``,
    nor one in ``sentinels`` whose row declares no sentinel, since Python reads a value
    the core does not as "not given". ``taken`` (entry -> signature parameters) and
    ``unbound`` (Python-unbound C subjects) drop the rows no caller value reaches.
    """
    by_c: dict[str, list[tuple[str, Domain, bool, str]]] = {}
    for row in domains:
        if row["kind"] != "integer":
            continue
        keyword = _keyword(row["param"], row["entry"], spelled)
        stricter = row["stricter"] if row["stricter"] in _KIND_STRICTER else None
        value = (*_explicit_bounds(row), stricter)
        declared = row.get("sentinel") is not None
        by_c.setdefault(row["entry"], []).append((keyword, value, declared, row["param"]))
    out: dict[tuple[str, str], Domain] = {}
    split: set[tuple[str, str]] = set()
    undeclared: set[tuple[str, str]] = set()
    for entry, c_entries in reached.items():
        for c_entry in sorted(c_entries):
            for keyword, value, declared, param in by_c.get(c_entry, []):
                if not _reachable(entry, c_entry, param, keyword, taken, unbound):
                    continue
                key = (entry, keyword)
                if out.get(key, value) != value:
                    split.add(key)
                if key in sentinels and not declared:
                    undeclared.add(key)
                out.setdefault(key, value)
    dropped = split | undeclared | excluded
    return dict(sorted((k, v) for k, v in out.items() if k not in dropped))


def _quoted(stricter: str | None) -> str:
    return "None" if stricter is None else f'"{stricter}"'


def _row(entry: str, keyword: str, values: list[str]) -> str:
    """One table line, split the way the formatter splits a line past 100 columns."""
    key = f'("{entry}", "{keyword}")'
    line = f"    {key}: ({', '.join(values)}),\n"
    if len(line) <= 101:
        return line
    if len(f"    {key}: (") > 100:
        return f'    (\n        "{entry}",\n        "{keyword}",\n    ): ({", ".join(values)}),\n'
    inner = "".join(f"        {v},\n" for v in values)
    return f"    {key}: (\n{inner}    ),\n"


def render(table: dict[tuple[str, str], Domain]) -> str:
    body = "".join(
        _row(entry, keyword, [str(lo), str(hi), _quoted(stricter)])
        for (entry, keyword), (lo, hi, stricter) in table.items()
    )
    return (
        '"""Generated by tools/api/gen_integer_domains.py from numeric-domains.json; do not edit."""\n\n'
        "# (public entry, Python keyword) -> the integer domain's (lo, hi, stricter); hi is None\n"
        "# when open above, stricter is 'power_of_two', 'even', 'odd' or None.\n"
        f"DOMAINS: dict[tuple[str, str], tuple[int, int | None, str | None]] = {{\n{body}}}\n"
    )


def main(argv: list[str]) -> int:
    source = Path(argv[argv.index("--source") + 1]) if "--source" in argv else SOURCE
    domains = json.loads(source.read_text())["rows"]
    table = rows(
        domains,
        overrides(),
        entries(),
        named_keywords(),
        sentinel_keywords(),
        signatures(),
        python_unbound(),
    )
    text = render(table)
    if "--check" in argv:
        if not TARGET.exists() or TARGET.read_text() != text:
            print(f"{TARGET.relative_to(ROOT)} is stale; run tools/api/gen_integer_domains.py")
            return 1
        return 0
    TARGET.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
