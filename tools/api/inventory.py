"""Per-surface inventory of every public symbol libsonare ships.

One record per public symbol and per member (method, field, option, value,
flag), keyed by (surface, kind, qualified name, entry) in exactly the spelling
the migration ledgers under tools/api/migration/ use. Surfaces: wasm, node
(TypeScript, read through the type checker by ts_exports.cjs), python (the
package's ``__all__`` plus the public names of its non-underscore submodules),
c (the public headers, read through tools/parity/extractors/c_api plus enums,
typedefs and ``SONARE_*`` macros), and the two CLI front-ends.

Every record carries its domain (tools/api/domains.py) and the path that
classified it. A surface that scanned no file or produced no record is an
error, so a broken glob cannot read as a clean surface.

Run ``python3 tools/api/inventory.py --json`` to dump the inventory.
"""

from __future__ import annotations

import argparse
import ast
import collections
import io
import json
import re
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "parity"))

import domains
from extractors import c_api, python_pyi
from extractors import cli as parity_cli

SURFACES = ("wasm", "node", "python", "c", "cli-python", "cli-native")
KINDS = (
    "function",
    "method",
    "class",
    "type",
    "field",
    "option",
    "value",
    "export",
    "command",
    "flag",
    "extra",
)
PY_PACKAGE = "bindings/python/src/libsonare"
PY_PUBLIC_SUBMODULES = (
    "analyzer",
    "audio",
    "engine",
    "mixing_assistant",
    "streaming",
    "types",
    "vocal_edit",
    "vocal_project",
)
NODE_ENTRIES = ("index.ts",)
CLI_CONTRACT = "tests/conformance/cli_contract_v2.json"


class InventoryError(RuntimeError):
    """Raised when a surface cannot be read at all."""


def _record(surface: str, kind: str, name: str, file: str, line: int, has_doc: bool, **detail):
    rec = {
        "surface": surface,
        "kind": kind,
        "name": name,
        "entries": [],
        "file": file,
        "line": line,
        "has_doc": has_doc,
        "domain": "",
        "domain_path": "",
        "owner": "",
        "member": "",
    }
    rec.update(detail)
    return rec


# --------------------------------------------------------------------------- TS


def wasm_entries(root: Path) -> dict[str, str]:
    """Map each WASM subpath export with types to its source entry file."""
    pkg = json.loads((root / "bindings/wasm/package.json").read_text(encoding="utf-8"))
    out = {}
    for sub, target in pkg.get("exports", {}).items():
        if isinstance(target, dict) and "types" in target:
            stem = Path(target["types"]).name.removesuffix(".d.ts")
            out[sub] = f"{stem}.ts"
    return out


def _run_ts(root: Path, pkg: str, entries: list[str]) -> list[dict]:
    cmd = ["node", str(HERE / "ts_exports.cjs"), str(root / pkg), *entries]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise InventoryError(f"ts_exports.cjs failed for {pkg}: {proc.stderr.strip()[:500]}")
    return json.loads(proc.stdout)


def _sig(s: dict) -> dict:
    return {
        "params": [
            {
                "name": p["name"],
                "type": p["type"],
                "optional": p["optional"],
                "boolean": p["boolean"],
                "literals": p["literals"],
                "rest": p.get("rest", False),
            }
            for p in s["params"]
        ],
        "returns": s["returns"],
    }


def _ts_options(surface, owner_name, sigs, file, line, out):
    seen = {}
    for s in sigs:
        for p in s["params"]:
            if p["name"] in seen:
                continue
            seen[p["name"]] = True
            out.append(
                _record(
                    surface,
                    "option",
                    f"{owner_name}({p['name']})",
                    file,
                    line,
                    True,
                    owner=owner_name,
                    member=p["name"],
                    type=p["type"],
                    optional=p["optional"],
                    boolean=p["boolean"],
                    literals=p["literals"],
                    inline_literals="'" in p["type"] or '"' in p["type"],
                )
            )


def _ts_nested(surface, owner, members, out):
    """Members of an inline object type, as fields under the dotted owner path.

    A name several union arms declare is one field whose type joins the arms' types.
    """
    merged: dict[str, dict] = {}
    for m in members:
        prev = merged.get(m["name"])
        if prev is None:
            merged[m["name"]] = {**m, "types": [m["type"]], "nested": list(m.get("nested", []))}
            continue
        if m["type"] not in prev["types"]:
            prev["types"].append(m["type"])
        prev["literals"] = sorted(set(prev["literals"]) | set(m["literals"]))
        prev["boolean"] = prev["boolean"] and m["boolean"]
        prev["optional"] = prev["optional"] or m["optional"]
        prev["doc"] = prev["doc"] or m["doc"]
        prev["docText"] = prev.get("docText") or m.get("docText", "")
        prev["nested"] += m.get("nested", [])
    for name, m in merged.items():
        qual = f"{owner}.{name}"
        ty = " | ".join(m["types"])
        out.append(
            _record(
                surface,
                "field",
                qual,
                m["file"],
                m["line"],
                m["doc"],
                owner=owner,
                member=name,
                doc_text=m.get("docText", ""),
                ts_kind=m["kind"],
                static=False,
                optional=m["optional"],
                type=ty,
                boolean=m["boolean"],
                literals=m["literals"],
                inline_literals="'" in ty or '"' in ty,
                sigs=[],
            )
        )
        _ts_nested(surface, qual, m["nested"], out)


def ts_records(surface: str, rows: list[dict], entry_label: dict[str, str]) -> list[dict]:
    """Turn ts_exports.cjs rows into records, merging re-exports of one declaration."""
    merged: dict[tuple, dict] = {}
    for r in rows:
        k = (r["name"], r["file"], r["line"])
        if k in merged:
            merged[k]["_entries"].add(entry_label[r["entry"]])
        else:
            r["_entries"] = {entry_label[r["entry"]]}
            merged[k] = r
    out: list[dict] = []
    for r in merged.values():
        entries = sorted(r["_entries"]) if surface == "wasm" else []
        tk = r["kind"]
        if tk == "function" or (tk == "const" and r["sigs"]):
            kind = "function"
        elif tk == "class":
            kind = "class"
        elif tk in ("interface", "type", "enum"):
            kind = "type"
        else:
            kind = "export"
        top = _record(
            surface,
            kind,
            r["name"],
            r["file"],
            r["line"],
            r["doc"],
            entries=entries,
            doc_text=r.get("docText", ""),
            ts_kind=tk,
            sigs=[_sig(s) for s in r["sigs"]],
            type=r.get("type", ""),
            literals=r.get("literals", []),
            ctor_private=r.get("ctorPrivate", False),
            bases=r.get("bases", []),
        )
        mirror = r.get("platformMirror") or {}
        if mirror:
            top["platform_mirror"] = mirror["interface"]
        group = [top]
        name = r["name"]
        if kind == "function":
            _ts_options(surface, name, r["sigs"], r["file"], r["line"], group)
        if kind == "class" and not r.get("ctorPrivate") and r["sigs"]:
            _ts_options(surface, f"{name}.constructor", r["sigs"], r["file"], r["line"], group)
        for lit in r.get("literals", []) if tk == "type" else []:
            group.append(
                _record(
                    surface,
                    "value",
                    f"{name}='{lit}'",
                    r["file"],
                    r["line"],
                    r["doc"],
                    owner=name,
                    member=lit,
                    token=lit,
                )
            )
        for em in r.get("enumMembers", []):
            group.append(
                _record(
                    surface,
                    "value",
                    f"{name}.{em['name']}",
                    em["file"],
                    em["line"],
                    em["doc"],
                    owner=name,
                    member=em["name"],
                )
            )
        for m in r.get("members", []):
            qual = f"{name}.{m['name']}"
            if m["kind"] in ("method", "accessor"):
                mkind = "method"
            else:
                mkind = "field"
            group.append(
                _record(
                    surface,
                    mkind,
                    qual,
                    m["file"],
                    m["line"],
                    m["doc"],
                    owner=name,
                    member=m["name"],
                    doc_text=m.get("docText", ""),
                    ts_kind=m["kind"],
                    static=m["static"],
                    optional=m["optional"],
                    type=m["type"],
                    boolean=m["boolean"],
                    literals=m["literals"],
                    inline_literals="'" in m["type"] or '"' in m["type"],
                    sigs=[_sig(s) for s in m["sigs"]],
                )
            )
            if m["name"] in mirror.get("members", []):
                group[-1]["platform_mirror"] = mirror["interface"]
            if mkind == "method" and m["kind"] == "method":
                _ts_options(surface, qual, m["sigs"], m["file"], m["line"], group)
            _ts_nested(surface, qual, m.get("nested", []), group)
        for g in group:
            g["entries"] = entries
        out.extend(group)
    return out


def extract_ts(root: Path) -> tuple[list[dict], dict[str, int]]:
    labels = wasm_entries(root)
    by_file = {v: k for k, v in labels.items()}
    rows = _run_ts(root, "bindings/wasm", sorted(by_file))
    recs = ts_records("wasm", rows, by_file)
    nrows = _run_ts(root, "bindings/node", list(NODE_ENTRIES))
    recs += ts_records("node", nrows, {e: "." for e in NODE_ENTRIES})
    files = {
        "wasm": len({r["file"] for r in recs if r["surface"] == "wasm"}),
        "node": len({r["file"] for r in recs if r["surface"] == "node"}),
    }
    return recs, files


# ----------------------------------------------------------------------- Python


class _PyModule:
    """Top-level bindings of one package module (.py or .pyi)."""

    def __init__(self, path: Path, rel: str):
        self.path = path
        self.rel = rel
        self.tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        self.defs: dict[str, ast.AST] = {}
        self.imports: dict[str, tuple[str, str]] = {}
        self.external: set[str] = set()
        self.star: list[str] = []
        self.all: list[str] | None = None
        for node in self.tree.body:
            self._bind(node)

    def _bind(self, node: ast.AST) -> None:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            self.defs.setdefault(node.name, node)
        elif isinstance(node, ast.Assign):
            for t in node.targets:
                if isinstance(t, ast.Name):
                    if t.id == "__all__":
                        self.all = _str_list(node.value)
                    else:
                        self.defs.setdefault(t.id, node)
        elif isinstance(node, ast.AugAssign) and getattr(node.target, "id", "") == "__all__":
            self.all = (self.all or []) + _str_list(node.value)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            self.defs.setdefault(node.target.id, node)
        elif hasattr(ast, "TypeAlias") and isinstance(node, ast.TypeAlias):
            self.defs.setdefault(node.name.id, node)
        elif isinstance(node, ast.ImportFrom):
            for a in node.names:
                local = a.asname or a.name
                if a.name == "*":
                    if node.level >= 1 and node.module:
                        self.star.append(node.module.split(".")[0])
                    continue
                if node.level >= 1 and node.module:
                    self.imports[local] = (node.module.split(".")[0], a.name)
                else:
                    self.external.add(local)
        elif isinstance(node, ast.Import):
            for a in node.names:
                self.external.add(a.asname or a.name.split(".")[0])
        elif isinstance(node, ast.If):
            for child in node.body:
                self._bind(child)


def _str_list(node: ast.AST) -> list[str]:
    if isinstance(node, (ast.List, ast.Tuple)):
        return [
            e.value for e in node.elts if isinstance(e, ast.Constant) and isinstance(e.value, str)
        ]
    return []


def _docstring(node: ast.AST | None) -> bool:
    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
        return bool(ast.get_docstring(node))
    return False


def _docstring_text(*nodes: ast.AST | None) -> str:
    """The first docstring among ``nodes`` (implementation, then stub)."""
    for node in nodes:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            text = ast.get_docstring(node)
            if text:
                return text
    return ""


_PY_ARGS_HEADERS = {"Args:", "Arguments:", "Keyword Args:", "Keyword Arguments:"}
_PY_ARG_ENTRY = re.compile(r"\*{0,2}(\w+)(?:\s*\([^)]*\))?:\s*(.*)")


def _py_param_docs(doc: str) -> dict[str, str]:
    """Each parameter's text from the Google-style ``Args:`` sections of a docstring."""
    out: dict[str, str] = {}
    header = entry = None
    cur = ""
    for ln in doc.split("\n"):
        text = ln.strip()
        indent = len(ln) - len(ln.lstrip())
        if text in _PY_ARGS_HEADERS:
            header, entry, cur = indent, None, ""
            continue
        if header is None or not text:
            continue
        if indent <= header:
            header = None
            continue
        entry = indent if entry is None else entry
        m = _PY_ARG_ENTRY.fullmatch(text)
        if indent == entry and m:
            cur = m[1]
            out[cur] = m[2]
        elif cur:
            out[cur] = f"{out[cur]} {text}".strip()
    return out


def _attr_doc(tree: ast.AST | None, node: ast.AST | None) -> bool:
    """A module attribute is documented by a string literal statement right after it."""
    if tree is None or node is None:
        return False
    for holder in ast.walk(tree):
        for field in ("body", "orelse", "finalbody"):
            block = getattr(holder, field, None)
            if not isinstance(block, list):
                continue
            for i, stmt in enumerate(block[:-1]):
                if stmt is node:
                    nxt = block[i + 1]
                    return (
                        isinstance(nxt, ast.Expr)
                        and isinstance(nxt.value, ast.Constant)
                        and isinstance(nxt.value.value, str)
                    )
    return False


def _unparse(node: ast.AST | None) -> str:
    return ast.unparse(node) if node is not None else ""


def _literal_tokens(text: str) -> list[str]:
    if "Literal[" not in text:
        return []
    return sorted(set(re.findall(r"""['"]([^'"]*)['"]""", text)))


class _PyPackage:
    def __init__(self, root: Path):
        base = root / PY_PACKAGE
        self.py: dict[str, _PyModule] = {}
        self.pyi: dict[str, _PyModule] = {}
        for p in sorted(base.iterdir()):
            if p.suffix == ".py":
                self.py[p.stem] = _PyModule(p, f"{PY_PACKAGE}/{p.name}")
            elif p.suffix == ".pyi":
                self.pyi[p.stem] = _PyModule(p, f"{PY_PACKAGE}/{p.name}")
        self.files = len(self.py) + len(self.pyi)
        # Function nodes by line, per file of this tree; a tagged tree reuses the relative paths.
        self.node_index: dict[str, dict[int, ast.AST]] = {}

    def resolve(self, stem: str, name: str, depth: int = 0):
        """Follow `from .x import name` chains to (module stem, node, file)."""
        if depth > 20:
            return None
        for table in (self.py, self.pyi):
            mod = table.get(stem)
            if mod is None:
                continue
            if name in mod.defs:
                return stem, mod.defs[name], mod.rel
            if name in mod.imports:
                sub, orig = mod.imports[name]
                hit = self.resolve(sub, orig, depth + 1)
                if hit:
                    return hit
            for sub in mod.star:
                if name.startswith("_"):
                    break
                target = self.py.get(sub) or self.pyi.get(sub)
                if target is not None and target.all is not None and name not in target.all:
                    continue
                hit = self.resolve(sub, name, depth + 1)
                if hit:
                    return hit
        return None

    def stub_node(self, stem: str, name: str):
        mod = self.pyi.get(stem)
        return mod.defs.get(name) if mod else None

    def module_public(self, stem: str) -> list[str]:
        mod = self.py.get(stem) or self.pyi.get(stem)
        if mod is None:
            return []
        if mod.all is not None:
            return list(mod.all)
        names = [n for n in mod.defs if not n.startswith("_")]
        names += [n for n in mod.imports if not n.startswith("_")]
        return sorted(set(names))

    def class_node(self, name: str, stem_hint: str | None = None):
        """Find a class definition by name, preferring the hinted module."""
        order = ([stem_hint] if stem_hint else []) + sorted(self.py) + sorted(self.pyi)
        for st in order:
            for table in (self.py, self.pyi):
                mod = table.get(st)
                if mod and isinstance(mod.defs.get(name), ast.ClassDef):
                    return mod.defs[name], mod.rel
        return None, None


_ENUM_BASES = {"Enum", "IntEnum", "StrEnum", "IntFlag", "Flag"}
_TYPE_BASES = {"TypedDict", "NamedTuple", "Protocol"}


def _decorators(node: ast.ClassDef) -> list[str]:
    return [_unparse(d) for d in node.decorator_list]


def _class_methods(pkg: _PyPackage, node: ast.ClassDef, seen: set[str] | None = None) -> set[str]:
    """Method names of a class including its in-package bases (dunders included)."""
    seen = seen or set()
    names = {c.name for c in node.body if isinstance(c, (ast.FunctionDef, ast.AsyncFunctionDef))}
    for b in node.bases:
        bn = _unparse(b)
        if bn in seen:
            continue
        seen.add(bn)
        bnode, _ = pkg.class_node(bn)
        if bnode is not None:
            names |= _class_methods(pkg, bnode, seen)
    return names


def _inherited_method(pkg: _PyPackage, node: ast.ClassDef, meth: str, seen: set[str] | None = None):
    """The definition of `meth` on the nearest in-package base of `node`."""
    seen = seen or set()
    for b in node.bases:
        bn = _unparse(b)
        if bn in seen:
            continue
        seen.add(bn)
        bnode, _ = pkg.class_node(bn)
        if bnode is None:
            continue
        for c in bnode.body:
            if isinstance(c, (ast.FunctionDef, ast.AsyncFunctionDef)) and c.name == meth:
                return c
        hit = _inherited_method(pkg, bnode, meth, seen)
        if hit is not None:
            return hit
    return None


def extract_python(root: Path) -> tuple[list[dict], dict[str, int], dict]:
    pkg = _PyPackage(root)
    init = pkg.py["__init__"]
    py_all = list(init.all or [])
    sigs: dict[str, list] = collections.defaultdict(list)
    for f in python_pyi.extract(root).functions:
        sigs[f.raw_name].append(f)

    def pick_sig(raw: str, files: tuple[str, ...]):
        cands = sigs.get(raw, [])
        for want in files:
            for f in cands:
                if f.file == want:
                    return f
        return cands[0] if cands else None

    names: dict[str, list[str]] = {}
    for n in py_all:
        names.setdefault(n, []).append("__init__")
    for stem in PY_PUBLIC_SUBMODULES:
        for n in pkg.module_public(stem):
            names.setdefault(n, []).append(stem)

    out: list[dict] = []
    unresolved = []
    for name in sorted(names):
        via = names[name][0]
        hit = pkg.resolve(via, name)
        in_all = name in py_all
        modules = sorted(set(names[name]))
        if hit is None:
            unresolved.append(name)
            out.append(
                _record(
                    "python",
                    "export",
                    name,
                    "",
                    0,
                    False,
                    in_all=in_all,
                    modules=modules,
                    unresolved=True,
                )
            )
            continue
        stem, node, file = hit
        line = getattr(node, "lineno", 0)
        # A name defined in a private module is stubbed in the public module re-exporting it.
        stub = pkg.stub_node(stem, name) or next(
            (pkg.stub_node(m, name) for m in modules if pkg.stub_node(m, name) is not None), None
        )
        files_pref = (f"{PY_PACKAGE}/{stem}.pyi", file)
        common = {"in_all": in_all, "modules": modules}
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            f = pick_sig(name, files_pref)
            kw = _py_kwargs(pkg, stem, None, node, {p["name"] for p in _py_params(f)})
            out.extend(
                _py_function("python", name, f, node, stub, file, line, common, kw_params=kw)
            )
        elif isinstance(node, ast.ClassDef):
            out.extend(_py_class(pkg, name, node, stub, stem, file, pick_sig, sigs, common))
        else:
            text = _unparse(node)
            value = node.value if isinstance(node, (ast.Assign, ast.AnnAssign)) else None
            vtext = _unparse(value)
            ann = _unparse(getattr(node, "annotation", None))
            is_alias = (
                "TypeAlias" in ann
                or vtext.startswith(("Literal[", "Union["))
                or (
                    isinstance(value, (ast.Subscript, ast.BinOp))
                    and re.match(r"^[A-Z]", name or "")
                    and not re.match(r"^[A-Z0-9_]+$", name)
                )
            )
            toks = _literal_tokens(vtext)
            stub_mod = pkg.pyi.get(stem)
            documented = _attr_doc(pkg.py[stem].tree if stem in pkg.py else None, node) or (
                _attr_doc(stub_mod.tree, stub) if stub_mod is not None else False
            )
            rec = _record(
                "python",
                "type" if is_alias else "export",
                name,
                file,
                line,
                documented,
                py_kind="alias" if is_alias else "constant",
                type=text,
                literals=toks,
                **common,
            )
            out.append(rec)
            for t in toks:
                out.append(
                    _record(
                        "python",
                        "value",
                        f"{name}='{t}'",
                        file,
                        line,
                        False,
                        owner=name,
                        member=t,
                        token=t,
                    )
                )
    out += _python_extras(root)
    meta = {"python_all": sorted(py_all), "python_unresolved": unresolved}
    return out, {"python": pkg.files}, meta


def _python_extras(root: Path) -> list[dict]:
    """The package's optional-dependency extras, each one a name `pip install libsonare[x]` takes."""
    rel = "bindings/python/pyproject.toml"
    text = (root / rel).read_text(encoding="utf-8")
    extras = tomllib.loads(text).get("project", {}).get("optional-dependencies", {})
    out = []
    for name in sorted(extras):
        m = re.search(rf"^{re.escape(name)}\s*=", text, flags=re.MULTILINE)
        line = _line_of(text, m.start()) if m else 0
        # Packaging, not a module: the manifest's basename would read as the project domain.
        out.append(_record("python", "extra", name, rel, line, False))
        out[-1].update(domain="core", domain_path="override")
    return out


def _param_dicts(params, optional: bool = False) -> list[dict]:
    return [
        {
            "name": p.raw_name,
            "type": p.type,
            "optional": optional or p.default is not None,
            "default": p.default,
            "boolean": p.type.replace(" ", "") in ("bool", "bool|None"),
            "literals": _literal_tokens(p.type),
        }
        for p in params
        if p.raw_name not in ("self", "cls")
    ]


def _py_params(f) -> list[dict]:
    return [] if f is None else _param_dicts(f.params)


def _own_init(pkg: _PyPackage, node: ast.ClassDef):
    """The `__init__` a class defines or inherits from an in-package base, or None."""
    for c in node.body:
        if isinstance(c, (ast.FunctionDef, ast.AsyncFunctionDef)) and c.name == "__init__":
            return c
    return _inherited_method(pkg, node, "__init__")


def _local_import(fn: ast.AST, name: str) -> tuple[str, str] | None:
    """(module stem, original name) of a relative `from .x import name` inside a function body."""
    for n in ast.walk(fn):
        if isinstance(n, ast.ImportFrom) and n.level >= 1 and n.module:
            for a in n.names:
                if (a.asname or a.name) == name:
                    return n.module.split(".")[0], a.name
    return None


def _field_params(pkg: _PyPackage, node: ast.ClassDef, seen: set[str] | None = None) -> list:
    """A dataclass's generated constructor parameters: its annotated fields, bases first."""
    seen = seen or set()
    out = []
    for b in node.bases:
        bn = _unparse(b)
        bnode, _ = pkg.class_node(bn) if bn not in seen else (None, None)
        if bnode is not None:
            seen.add(bn)
            out += _field_params(pkg, bnode, seen)
    for c in node.body:
        if isinstance(c, ast.AnnAssign) and isinstance(c.target, ast.Name):
            ann = _unparse(c.annotation)
            if ann.startswith("ClassVar") or c.target.id.startswith("_"):
                continue
            out.append(
                types.SimpleNamespace(
                    raw_name=c.target.id,
                    type=ann,
                    default=_unparse(c.value) if c.value is not None else None,
                    keyword_only=False,
                )
            )
    return out


def _class_target(pkg: _PyPackage, node: ast.ClassDef):
    init = _own_init(pkg, node)
    if init is not None:
        return python_pyi._params_from_args(init.args), init, node, True
    if any(d.split("(")[0].split(".")[-1] == "dataclass" for d in _decorators(node)):
        return _field_params(pkg, node), None, node, False
    return None


def _forward_target(pkg: _PyPackage, stem: str, owner: ast.ClassDef | None, fn, func: ast.expr):
    """(parameters, definition, owning class, is_method) of the callable a call names, or None."""
    if isinstance(func, ast.Name):
        if func.id == "cls" and owner is not None:
            return _class_target(pkg, owner)
        local = _local_import(fn, func.id)
        hit = pkg.resolve(*local) if local else pkg.resolve(stem, func.id)
        node = hit[1] if hit else None
        if isinstance(node, ast.ClassDef):
            return _class_target(pkg, node)
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            return python_pyi._params_from_args(node.args), node, None, False
        return None
    if (
        isinstance(func, ast.Attribute)
        and isinstance(func.value, ast.Name)
        and func.value.id in ("self", "cls")
        and owner is not None
    ):
        found = next(
            (
                c
                for c in owner.body
                if isinstance(c, (ast.FunctionDef, ast.AsyncFunctionDef)) and c.name == func.attr
            ),
            None,
        ) or _inherited_method(pkg, owner, func.attr)
        if found is None:
            return None
        return python_pyi._params_from_args(found.args), found, owner, True
    return None


def _py_kwargs(pkg, stem, owner, fn, bound: set[str], depth: int = 0) -> list[dict] | None:
    """The keywords a `**name` parameter accepts, or None when they cannot be resolved.

    They are the keys of an `Unpack[TypedDict]` annotation, or the keyword parameters of
    the callable the body forwards `**name` to, less what the call already binds.
    """
    kw = fn.args.kwarg if fn is not None else None
    if kw is None:
        return []
    if depth > 4:
        return None
    ann = kw.annotation
    if isinstance(ann, ast.Subscript) and _unparse(ann.value).split(".")[-1] == "Unpack":
        td, _ = pkg.class_node(_unparse(ann.slice).split(".")[-1], stem)
        if td is None:
            return None
        fields = [p for p in _field_params(pkg, td) if p.raw_name not in bound]
        return _param_dicts(fields, optional=True)
    for call in ast.walk(fn):
        if not isinstance(call, ast.Call) or not any(
            k.arg is None and isinstance(k.value, ast.Name) and k.value.id == kw.arg
            for k in call.keywords
        ):
            continue
        target = _forward_target(pkg, stem, owner, fn, call.func)
        if target is None:
            return None
        params, tnode, towner, is_method = target
        positional = [p for p in params if not p.keyword_only]
        if is_method and positional and positional[0].raw_name in ("self", "cls"):
            positional = positional[1:]
        taken = {p.raw_name for p in positional[: len(call.args)]}
        taken |= {k.arg for k in call.keywords if k.arg is not None}
        rest = [
            p
            for p in params
            if p.raw_name not in taken
            and p.raw_name not in bound
            and p.raw_name not in ("self", "cls")
            and not p.raw_name.startswith("_")
        ]
        names = bound | {p.raw_name for p in rest}
        deeper = _py_kwargs(pkg, stem, towner, tnode, names, depth + 1)
        if deeper is None:
            return None
        return _param_dicts(rest) + deeper
    return None


def _py_function(
    surface, qual, f, node, stub, file, line, extra, kind="function", owner="", kw_params=()
):
    params = _py_params(f)
    returns = f.returns if f is not None else _unparse(getattr(node, "returns", None))
    has_doc = _docstring(node) or _docstring(stub)
    rec = _record(
        surface,
        kind,
        qual,
        f.file if f else file,
        f.line if f else line,
        has_doc,
        sigs=[{"params": params, "returns": returns}],
        doc_text=_docstring_text(node, stub),
        owner=owner,
        member=qual.split(".")[-1] if owner else "",
        **extra,
    )
    if kw_params is None:
        rec["kwargs_unresolved"] = True
    group = [rec]
    param_docs = _py_param_docs(rec["doc_text"])
    for p in params + list(kw_params or ()):
        group.append(
            _record(
                surface,
                "option",
                f"{qual}({p['name']})",
                rec["file"],
                rec["line"],
                True,
                owner=qual,
                member=p["name"],
                type=p["type"],
                optional=p["optional"],
                boolean=p["boolean"],
                literals=p["literals"],
                inline_literals=bool(p["literals"]),
                doc_text=param_docs.get(p["name"], ""),
            )
        )
    return group


def _py_class(pkg, name, node, stub, stem, file, pick_sig, sig_names, common):
    bases = [_unparse(b) for b in node.bases]
    decos = _decorators(node)
    base_names = {b.split(".")[-1].split("[")[0] for b in bases}
    is_enum = bool(base_names & _ENUM_BASES)
    is_dataclass = any(d.split("(")[0].split(".")[-1] == "dataclass" for d in decos)
    frozen = any("frozen=True" in d.replace(" ", "") for d in decos)
    is_typed = bool(base_names & _TYPE_BASES)
    kind = "type" if (is_enum or is_dataclass or is_typed) else "class"
    py_kind = (
        "enum" if is_enum else "dataclass" if is_dataclass else "typed" if is_typed else "class"
    )
    has_doc = _docstring(node) or _docstring(stub)
    top = _record(
        "python",
        kind,
        name,
        file,
        node.lineno,
        has_doc,
        doc_text=_docstring_text(node, stub),
        py_kind=py_kind,
        bases=bases,
        decorators=decos,
        dataclass=is_dataclass,
        frozen=frozen,
        **common,
    )
    out = [top]
    body_nodes = list(node.body) + (list(stub.body) if isinstance(stub, ast.ClassDef) else [])
    seen_members: set[str] = set()
    idx = 0
    for c in body_nodes:
        if isinstance(c, ast.AnnAssign) and isinstance(c.target, ast.Name):
            fname = c.target.id
            if fname.startswith("_") or fname in seen_members:
                continue
            seen_members.add(fname)
            ann = _unparse(c.annotation)
            if ann.startswith("ClassVar"):
                continue
            out.append(
                _record(
                    "python",
                    "field",
                    f"{name}.{fname}",
                    file,
                    c.lineno,
                    _field_doc(node, c),
                    owner=name,
                    member=fname,
                    type=ann,
                    optional="None" in ann or c.value is not None,
                    index=idx,
                    literals=_literal_tokens(ann),
                    inline_literals=bool(_literal_tokens(ann)),
                )
            )
            idx += 1
        elif is_enum and isinstance(c, ast.Assign):
            for t in c.targets:
                if (
                    isinstance(t, ast.Name)
                    and not t.id.startswith("_")
                    and t.id not in seen_members
                ):
                    seen_members.add(t.id)
                    out.append(
                        _record(
                            "python",
                            "value",
                            f"{name}.{t.id}",
                            file,
                            c.lineno,
                            _field_doc(node, c),
                            owner=name,
                            member=t.id,
                        )
                    )
    methods = sorted(_class_methods(pkg, node))
    prefix = f"{name}."
    from_sigs = sorted({raw[len(prefix) :] for raw in sig_names if raw.startswith(prefix)})
    for meth in sorted(
        set(from_sigs)
        | {m for m in methods if m in ("__enter__", "__exit__")}
        | {m for m in methods if not m.startswith("_")}
    ):
        if meth.startswith("_") and meth not in ("__enter__", "__exit__"):
            continue
        f = pick_sig(f"{name}.{meth}", (f"{PY_PACKAGE}/{stem}.pyi", file))
        mnode = next(
            (
                c
                for c in node.body
                if isinstance(c, (ast.FunctionDef, ast.AsyncFunctionDef)) and c.name == meth
            ),
            None,
        )
        snode = None
        if isinstance(stub, ast.ClassDef):
            snode = next(
                (
                    c
                    for c in stub.body
                    if isinstance(c, (ast.FunctionDef, ast.AsyncFunctionDef)) and c.name == meth
                ),
                None,
            )
        if mnode is None and f is not None:
            mnode = _node_at(pkg, f.file, f.line)
        if mnode is None:
            mnode = _inherited_method(pkg, node, meth)
        line = f.line if f else (mnode.lineno if mnode else node.lineno)
        extra = {"decorators": [_unparse(d) for d in getattr(mnode, "decorator_list", [])]}
        kw = _py_kwargs(pkg, stem, node, mnode, {p["name"] for p in _py_params(f)})
        out.extend(
            _py_function(
                "python",
                f"{name}.{meth}",
                f,
                mnode,
                snode,
                file,
                line,
                extra,
                kind="method",
                owner=name,
                kw_params=kw,
            )
        )
    if kind == "class":
        options, resolved = _py_constructor(pkg, name, node, stub, stem, file)
        out.extend(options)
        if not resolved:
            top["kwargs_unresolved"] = True
    return out


def _py_constructor(pkg, name, node, stub, stem, file) -> tuple[list[dict], bool]:
    """The constructor's keyword surface as `Name.constructor(param)` options, as on the JS side,
    and whether a `**name` parameter of it resolved.

    The constructor itself is keyed through its class; a parameter whose name starts with an
    underscore is internal and not listed.
    """
    init = _own_init(pkg, node)
    if init is None:
        return [], True
    sinit = None
    if isinstance(stub, ast.ClassDef):
        sinit = next(
            (
                c
                for c in stub.body
                if isinstance(c, (ast.FunctionDef, ast.AsyncFunctionDef)) and c.name == "__init__"
            ),
            None,
        )
    sig = python_pyi._params_from_args((sinit or init).args)
    params = [p for p in sig if not p.raw_name.startswith("_")]
    bound = {p.raw_name for p in params}
    kw = _py_kwargs(pkg, stem, node, init, bound)
    qual = f"{name}.constructor"
    group = _py_function(
        "python",
        qual,
        types.SimpleNamespace(params=params, returns="None", file=file, line=init.lineno),
        init,
        sinit,
        file,
        init.lineno,
        {},
        kind="method",
        owner=name,
        kw_params=kw,
    )
    return group[1:], kw is not None


def _node_at(pkg: _PyPackage, rel: str, line: int):
    if rel not in pkg.node_index:
        stem = Path(rel).stem
        table = pkg.pyi if rel.endswith(".pyi") else pkg.py
        mod = table.get(stem)
        idx = {}
        if mod:
            for n in ast.walk(mod.tree):
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    idx[n.lineno] = n
        pkg.node_index[rel] = idx
    return pkg.node_index[rel].get(line)


def _field_doc(cls: ast.ClassDef, node: ast.AST) -> bool:
    """A field is documented by a string literal right after it or by the class docstring naming it."""
    body = cls.body
    for i, c in enumerate(body):
        if c is node and i + 1 < len(body):
            nxt = body[i + 1]
            if (
                isinstance(nxt, ast.Expr)
                and isinstance(getattr(nxt, "value", None), ast.Constant)
                and isinstance(nxt.value.value, str)
            ):
                return True
    doc = ast.get_docstring(cls) or ""
    name = node.target.id if isinstance(node, ast.AnnAssign) else getattr(node.targets[0], "id", "")
    return (
        bool(name) and re.search(rf"(^|\s){re.escape(name)}\s*[:(]", doc, re.MULTILINE) is not None
    )


# ---------------------------------------------------------------------------- C


_C_TYPEDEF_ENUM = re.compile(
    r"typedef\s+enum\s*\w*\s*\{(?P<body>.*?)\}\s*(?P<name>\w+)\s*;", re.DOTALL
)
_C_OPAQUE = re.compile(r"typedef\s+struct\s+(\w+)\s+(?P<name>\w+)\s*;")
_C_FNPTR = re.compile(r"typedef\s+[^;{}()]*\(\s*\*\s*(?P<name>\w+)\s*\)\s*\([^;]*\)\s*;", re.DOTALL)
_C_SCALAR_TYPEDEF = re.compile(
    r"typedef\s+(?!struct\b|enum\b|union\b)[\w\s\*]+?\b(?P<name>Sonare\w+)\s*;"
)
_C_DEFINE = re.compile(
    r"^\s*#\s*define\s+(?P<name>SONARE_\w+)(?P<fn>\()?(?P<rest>.*)$", re.MULTILINE
)


def _line_of(raw: str, pos: int) -> int:
    return raw.count("\n", 0, pos) + 1


def _c_doc_before(lines: list[str], idx: int) -> bool:
    """Whether a Doxygen comment ends on the line right above declaration line idx (0-based)."""
    cur = lines[idx]
    if "///<" in cur or "/**<" in cur or "//!<" in cur:
        return True
    j = idx - 1
    # Walk up over the earlier lines of the same declaration (return type, macros).
    while j >= 0:
        s = lines[j].strip()
        if not s:
            return False
        if s.startswith(("///", "//!")):
            return True
        if s.endswith("*/"):
            k = j
            while k >= 0 and "/*" not in lines[k]:
                k -= 1
            return k >= 0 and ("/**" in lines[k] or "/*!" in lines[k])
        if s.startswith(("//", "#")) or s.endswith((";", "{", "}")):
            return False
        j -= 1
    return False


def _c_doc_text(lines: list[str], idx: int) -> str:
    """Text of the Doxygen comment ending right above declaration line idx (0-based), or ""."""
    cur = lines[idx]
    for mark in ("///<", "/**<", "//!<"):
        if mark in cur:
            return cur.split(mark, 1)[1].strip().removesuffix("*/").strip()
    j = idx - 1
    while j >= 0 and lines[j].strip().startswith(("///", "//!")):
        j -= 1
    if j < idx - 1:
        return " ".join(s.strip()[3:].strip() for s in lines[j + 1 : idx])
    if j >= 0 and lines[j].strip().endswith("*/"):
        k = j
        while k >= 0 and "/*" not in lines[k]:
            k -= 1
        if k >= 0 and ("/**" in lines[k] or "/*!" in lines[k]):
            body = " ".join(x.strip() for x in lines[k : j + 1])
            return re.sub(r"^/\*[*!]\s*|\s*\*/$", "", body).replace(" * ", " ").strip()
    return ""


_C_PARAM_DOC = re.compile(
    r"[@\\]param(?:\[[a-z, ]*\])?\s+(\w+)\s+(.*?)"
    r"(?=\s[@\\](?:param|return|returns|retval|brief|details|note|see|pre|post|warning)\b|$)"
)


def _c_param_docs(doc: str) -> dict[str, str]:
    """Each parameter's text from the ``@param`` blocks of a Doxygen comment."""
    return {m[1]: m[2].strip() for m in _C_PARAM_DOC.finditer(doc)}


def _c_decl_line(raw: str, pattern: str) -> int:
    """First line matching pattern that is not inside a comment, or 0."""
    stripped = _blank_comments(raw)
    m = re.search(pattern, stripped)
    return _line_of(raw, m.start()) if m else 0


def _blank_comments(raw: str) -> str:
    """Replace comments with spaces, keeping offsets and newlines."""

    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))

    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, raw, flags=re.DOTALL)


def extract_c(root: Path) -> tuple[list[dict], dict[str, int]]:
    ex = c_api.extract(root)
    headers = c_api._collect_api_headers(root)
    header_set = {str(h.relative_to(root)) for h in headers if h.exists()}
    header_set |= {str(p.relative_to(root)) for p in sorted((root / "include/sonare").glob("*.h"))}
    raw_by_file = {f: (root / f).read_text(encoding="utf-8") for f in sorted(header_set)}
    lines_by_file = {f: t.split("\n") for f, t in raw_by_file.items()}
    out: list[dict] = []

    def doc_at(file, line):
        return line > 0 and _c_doc_before(lines_by_file[file], line - 1)

    for f in ex.functions:
        raw = raw_by_file.get(f.file, "")
        line = _c_decl_line(raw, rf"\b{re.escape(f.raw_name)}\s*\(") or f.line
        rec = _record(
            "c",
            "function",
            f.raw_name,
            f.file,
            line,
            doc_at(f.file, line),
            sigs=[
                {
                    "params": [
                        {
                            "name": p.raw_name,
                            "type": p.type,
                            "optional": False,
                            "boolean": p.type.replace("const", "").strip() == "bool",
                            "literals": [],
                        }
                        for p in f.params
                    ],
                    "returns": f.returns,
                }
            ],
        )
        out.append(rec)
        param_docs = (
            _c_param_docs(_c_doc_text(lines_by_file[f.file], line - 1))
            if line and f.file in lines_by_file
            else {}
        )
        for p in f.params:
            if not p.raw_name:
                continue
            out.append(
                _record(
                    "c",
                    "option",
                    f"{f.raw_name}({p.raw_name})",
                    f.file,
                    line,
                    True,
                    owner=f.raw_name,
                    member=p.raw_name,
                    type=p.type,
                    optional=False,
                    boolean=False,
                    literals=[],
                    doc_text=param_docs.get(p.raw_name, ""),
                )
            )
    declared_types: set[str] = set()
    for r in ex.records:
        raw = raw_by_file.get(r.file, "")
        stripped = _blank_comments(raw)
        m = re.search(rf"\}}\s*{re.escape(r.raw_name)}\s*;", stripped)
        start = stripped.rfind("typedef", 0, m.start()) if m else -1
        line = _line_of(raw, start) if start >= 0 else r.line
        declared_types.add(r.raw_name)
        out.append(
            _record(
                "c",
                "type",
                r.raw_name,
                r.file,
                line,
                doc_at(r.file, line),
                c_kind="struct",
                fields=[x.raw_name for x in r.fields],
            )
        )
        body_start = _line_of(raw, start) if start >= 0 else 0
        body_end = _line_of(raw, m.start()) if m else 0
        for i, fld in enumerate(r.fields):
            fline = 0
            if body_start and body_end:
                pat = re.compile(
                    rf"\b{re.escape(fld.raw_name)}\s*(\[[^\]]*\])?\s*;|\(\s*\*\s*{re.escape(fld.raw_name)}\s*\)"
                )
                for ln in range(body_start, body_end + 1):
                    if pat.search(_blank_comments(lines_by_file[r.file][ln - 1])):
                        fline = ln
                        break
            out.append(
                _record(
                    "c",
                    "field",
                    f"{r.raw_name}.{fld.raw_name}",
                    r.file,
                    fline or line,
                    bool(fline) and doc_at(r.file, fline),
                    owner=r.raw_name,
                    member=fld.raw_name,
                    type=fld.type,
                    index=i,
                    optional=False,
                    literals=[],
                    doc_text=_c_doc_text(lines_by_file[r.file], fline - 1) if fline else "",
                )
            )
    for file, raw in raw_by_file.items():
        stripped = _blank_comments(raw)
        for m in _C_TYPEDEF_ENUM.finditer(stripped):
            name = m.group("name")
            if name in declared_types:
                continue
            declared_types.add(name)
            line = _line_of(raw, m.start())
            out.append(_record("c", "type", name, file, line, doc_at(file, line), c_kind="enum"))
            body_off = m.start("body")
            for vm in re.finditer(r"\b(SONARE_\w+)\b\s*(=|,|$)", m.group("body"), re.MULTILINE):
                vline = _line_of(raw, body_off + vm.start())
                out.append(
                    _record(
                        "c",
                        "value",
                        f"{name}.{vm.group(1)}",
                        file,
                        vline,
                        doc_at(file, vline),
                        owner=name,
                        member=vm.group(1),
                    )
                )
        for rx, ckind in (
            (_C_OPAQUE, "opaque"),
            (_C_FNPTR, "callback"),
            (_C_SCALAR_TYPEDEF, "typedef"),
        ):
            for m in rx.finditer(stripped):
                name = m.group("name")
                if name in declared_types:
                    continue
                declared_types.add(name)
                line = _line_of(raw, m.start())
                out.append(_record("c", "type", name, file, line, doc_at(file, line), c_kind=ckind))
        for m in _C_DEFINE.finditer(stripped):
            name = m.group("name")
            if not m.group("rest").strip() and not m.group("fn"):
                continue
            line = _line_of(raw, m.start("name"))
            out.append(
                _record(
                    "c",
                    "export",
                    name,
                    file,
                    line,
                    doc_at(file, line),
                    c_kind="macro",
                    type=m.group("rest").strip(),
                )
            )
    seen = set()
    deduped = []
    for r in out:
        k = (r["kind"], r["name"])
        if k in seen:
            continue
        seen.add(k)
        deduped.append(r)
    return deduped, {"c": len(raw_by_file)}


# -------------------------------------------------------------------------- CLI


_NATIVE_BUILDERS = (
    "flag|int_value|number_value|string_value|path_value|required_path|nullable_string_value"
    "|repeatable_path|optional_string|global_int|global_number|required_int|required_string"
    "|mode_mask_value|make_option"
)
_NATIVE_OPTION = re.compile(rf"\b(?:{_NATIVE_BUILDERS})\s*\(\s*\"([^\"]+)\"")
_NATIVE_SWITCH = re.compile(r"\bflag\s*\(\s*\"([^\"]+)\"")
_NATIVE_FIXED = {
    "output_value": "output",
    "required_output": "output",
    "bits_value": "bits",
    "true_peak_oversample_value": "true-peak-oversample",
    "target_platform_value": "target-platform",
}
_NATIVE_CALL = re.compile(r"\b(add_command|add_project_command)\s*\(")
_NATIVE_BUILDER = re.compile(rf"\b(?:{_NATIVE_BUILDERS})\s*\(")
# The only string-literal brace list a builder takes is its alias list.
_NATIVE_ALIASES = re.compile(r'\{\s*("[^"]+"(?:\s*,\s*"[^"]+")*)\s*\}')
_NATIVE_LOOP = re.compile(r"for\s*\(\s*const\s+char\s*\*\s*(\w+)\s*:\s*\{([^}]*)\}\s*\)\s*$")


def _balanced(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    while i < len(text):
        ch = text[i]
        if ch == '"':
            i = text.index('"', i + 1)
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _native_aliases(call: str) -> dict[str, list[str]]:
    """Long alias spellings of each option a command's builder calls declare, by primary name."""
    out: dict[str, list[str]] = {}
    for b in _NATIVE_BUILDER.finditer(call):
        close = _balanced(call, b.end() - 1)
        body = call[b.end() : close]
        head = re.match(r'\s*"([^"]+)"', body)
        if not head:
            continue
        spellings = [
            s
            for group in _NATIVE_ALIASES.findall(body[head.end() :])
            for s in re.findall(r'"([^"]+)"', group)
            if len(s) > 1
        ]
        if spellings:
            out.setdefault(head.group(1), []).extend(spellings)
    return out


def _top_level_split(text: str) -> list[str]:
    """`text` cut at the commas outside any bracket or string literal, each part stripped."""
    parts, depth, start, i = [], 0, 0, 0
    while i < len(text):
        ch = text[i]
        if ch == '"':
            i = text.index('"', i + 1)
        elif ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
        i += 1
    parts.append(text[start:].strip())
    return parts


def _native_unbuilt_options(call: str, kind: str) -> list[str]:
    """Entries of a registration's option list that build no option inside the call itself."""
    args = _top_level_split(call)
    at = 3 if kind == "add_command" else 1
    if len(args) <= at or not args[at].startswith("{"):
        return []
    fixed = re.compile(rf"\b(?:{'|'.join(_NATIVE_FIXED)})\s*\(")
    return [
        entry
        for entry in _top_level_split(args[at][1:-1])
        if entry and not (_NATIVE_BUILDER.search(entry) or fixed.search(entry))
    ]


def _native_forwarded(text: str, at: int, name: str) -> bool:
    """Whether `name` at `at` is a `const char*` parameter of the helper the call sits in."""
    return (
        re.search(rf"const\s+char\s*\*\s*{re.escape(name)}\b", text[max(0, at - 400) : at])
        is not None
    )


def _native_cli(root: Path) -> tuple[dict[str, dict], int, int, list[str]]:
    """Commands and flags of the native CLI registry, its flag registrations and its problems.

    A command name is a string literal, a `for (const char* x : {...})` loop variable, or a
    helper's own parameter forwarded; a flag name is a string literal. Anything else is a
    problem, since the registry would hold a name this scan cannot read.
    """
    commands: dict[str, dict] = {}
    registrations = 0
    problems: list[str] = []
    files = sorted((root / "tools/cli").rglob("*.cpp"))
    for path in files:
        raw = path.read_text(encoding="utf-8")
        text = _blank_comments(raw)
        rel = str(path.relative_to(root))
        for m in _NATIVE_CALL.finditer(text):
            if re.search(r"\bvoid\s*$", text[: m.start()]):
                continue
            close = _balanced(text, m.end() - 1)
            if close < 0:
                problems.append(f"{rel}:{_line_of(raw, m.start())}: unbalanced {m.group(1)} call")
                continue
            call = text[m.end() : close]
            for b in _NATIVE_BUILDER.finditer(call):
                registrations += 1
                if not call[b.end() :].lstrip().startswith('"'):
                    problems.append(
                        f"{rel}:{_line_of(raw, m.end() + b.start())}: flag name is not a literal"
                    )
            args = call.split(",", 2)
            if m.group(1) == "add_command":
                if len(args) < 2:
                    continue
                first = args[1].strip()
            else:
                first = args[0].strip()
            names: list[str] = []
            if first.startswith('"'):
                names = [first.strip('"')]
            else:
                line_start = text.rfind("\n", 0, m.start())
                prev = text[text.rfind("\n", 0, line_start - 1) + 1 : line_start].strip()
                lm = _NATIVE_LOOP.search(prev)
                if lm and lm.group(1) == first:
                    names = re.findall(r'"([^"]+)"', lm.group(2))
                elif re.fullmatch(r"\w+", first) and _native_forwarded(text, m.start(), first):
                    continue
                else:
                    problems.append(
                        f"{rel}:{_line_of(raw, m.start())}: unknown registration shape for "
                        f"{m.group(1)} name {first[:40]!r}"
                    )
            if not names:
                continue
            # An option built elsewhere and passed in is invisible to this scan.
            for entry in _native_unbuilt_options(call, m.group(1)):
                problems.append(
                    f"{rel}:{_line_of(raw, m.start())}: option {entry} is not built in the call"
                )
            flags = ["json"] + _NATIVE_OPTION.findall(call)
            flags += [v for k, v in _NATIVE_FIXED.items() if re.search(rf"\b{k}\s*\(", call)]
            switches = {"json", *_NATIVE_SWITCH.findall(call)}
            aliases = _native_aliases(call)
            for primary, spellings in aliases.items():
                flags += spellings
                if primary in switches:
                    switches |= set(spellings)
            for n in names:
                entry = commands.setdefault(
                    n,
                    {
                        "file": rel,
                        "line": _line_of(raw, m.start()),
                        "flags": [],
                        "switches": set(),
                        "aliases": {},
                    },
                )
                entry["switches"] |= switches
                for primary, spellings in aliases.items():
                    for a in spellings:
                        entry["aliases"][a] = primary
                for fl in flags:
                    if fl not in entry["flags"]:
                        entry["flags"].append(fl)
    return commands, len(files), registrations, problems


_PY_SWITCH_ACTIONS = {"store_true", "store_false", "store_const", "count", "BooleanOptionalAction"}


class _PyCliScan:
    """Commands and long flags of the Python argparse CLI, parents and helpers resolved."""

    def __init__(self, root: Path):
        self.base = root / PY_PACKAGE
        self.root = root
        self.files = [self.base / "cli.py", *sorted(self.base.glob("_cli_*.py"))]
        self.parents: dict[str, list[str]] = {}
        self.parent_flags: dict[str, list[str]] = collections.defaultdict(list)
        self.helpers: dict[str, list[str]] = {}
        self.commands: dict[str, dict] = {}
        self.switches: set[str] = set()
        self.help: dict[str, list[str]] = collections.defaultdict(list)
        # Every later long spelling of one add_argument, mapped to its first.
        self.alias_of: dict[str, str] = {}
        self.registrations = 0
        self.problems: list[str] = []

    @staticmethod
    def _loops(tree: ast.AST) -> dict[str, list[str]]:
        """Loop variables bound to string constants: `for x in ("a", "b")` and tuple unpacking."""
        out: dict[str, list[str]] = {}
        for node in ast.walk(tree):
            if not isinstance(node, ast.For) or not isinstance(node.iter, (ast.Tuple, ast.List)):
                continue
            targets = node.target.elts if isinstance(node.target, ast.Tuple) else [node.target]
            for i, t in enumerate(targets):
                if not isinstance(t, ast.Name):
                    continue
                vals = []
                for e in node.iter.elts:
                    item = e.elts[i] if isinstance(e, ast.Tuple) and i < len(e.elts) else e
                    if isinstance(item, ast.Constant) and isinstance(item.value, str):
                        vals.append(item.value)
                if vals:
                    out[t.id] = vals
        return out

    @staticmethod
    def _long_flags(call: ast.Call, loops: dict[str, list[str]]) -> list[str]:
        if any(k.arg == "help" and _unparse(k.value) == "argparse.SUPPRESS" for k in call.keywords):
            return []
        if call.args and isinstance(call.args[0], ast.Name) and call.args[0].id in loops:
            return [o[2:] for o in loops[call.args[0].id] if o.startswith("--")]
        opts = [
            a.value for a in call.args if isinstance(a, ast.Constant) and isinstance(a.value, str)
        ]
        return [o[2:] for o in opts if o.startswith("--")]

    @staticmethod
    def _parent_key(fname: str, node: ast.AST) -> str:
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            return f"_cli_options.py:{node.attr}"
        return f"{fname}:{_unparse(node)}"

    def scan(self) -> None:
        trees = [(p, ast.parse(p.read_text(encoding="utf-8"))) for p in self.files]
        for path, tree in trees:
            loops = self._loops(tree)
            self._scan_parents(tree, path.name, loops)
            self._scan_helpers(tree, loops)
        for path, tree in trees:
            self._scan_commands(tree, path, self._loops(tree))
        self._scan_switches(trees)

    def _scan_switches(self, trees) -> None:
        """Long flags every declaration of which takes no value, and each flag's help texts."""
        switched, valued = set(), set()
        for path, tree in trees:
            loops = self._loops(tree)
            for c in ast.walk(tree):
                if not (isinstance(c, ast.Call) and getattr(c.func, "attr", "") == "add_argument"):
                    continue
                # The parser subclass forwards to argparse; that is not a registration.
                if _unparse(c.func.value) == "super()":
                    continue
                self.registrations += 1
                head = c.args[0] if c.args else None
                if not (isinstance(head, ast.Constant) and isinstance(head.value, str)):
                    rel = path.relative_to(self.root)
                    self.problems.append(f"{rel}:{c.lineno}: flag name is not a literal")
                action = next((_unparse(k.value) for k in c.keywords if k.arg == "action"), "")
                bucket = (
                    switched if action.strip("'\"").split(".")[-1] in _PY_SWITCH_ACTIONS else valued
                )
                names = self._long_flags(c, loops)
                bucket.update(names)
                if not (c.args and isinstance(c.args[0], ast.Name)):
                    for alias in names[1:]:
                        self.alias_of[alias] = names[0]
                text = next((k.value for k in c.keywords if k.arg == "help"), None)
                if isinstance(text, ast.Constant) and isinstance(text.value, str):
                    for n in names:
                        if text.value not in self.help[n]:
                            self.help[n].append(text.value)
        self.switches = switched - valued

    def _scan_parents(self, tree: ast.AST, fname: str, loops) -> None:
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.Assign)
                and isinstance(node.value, ast.Call)
                and len(node.targets) == 1
            ):
                tgt = node.targets[0]
                if isinstance(tgt, ast.Name) and _unparse(node.value.func).endswith(
                    "ArgumentParser"
                ):
                    self.parents[f"{fname}:{tgt.id}"] = [
                        self._parent_key(fname, e)
                        for k in node.value.keywords
                        if k.arg == "parents"
                        for e in getattr(k.value, "elts", [])
                    ]
            if isinstance(node, ast.Call) and getattr(node.func, "attr", "") == "add_argument":
                recv = node.func.value
                if isinstance(recv, ast.Name):
                    self.parent_flags[f"{fname}:{recv.id}"].extend(self._long_flags(node, loops))

    def _scan_helpers(self, tree: ast.AST, loops) -> None:
        """Functions that add arguments to the parser passed as their first parameter."""
        calls: dict[str, list[str]] = {}
        for node in ast.walk(tree):
            if not isinstance(node, ast.FunctionDef) or not node.args.args:
                continue
            first = node.args.args[0].arg
            flags, nested = [], []
            for c in ast.walk(node):
                if not isinstance(c, ast.Call):
                    continue
                if (
                    getattr(c.func, "attr", "") == "add_argument"
                    and _unparse(c.func.value) == first
                ):
                    flags += self._long_flags(c, loops)
                elif isinstance(c.func, ast.Name) and c.args and _unparse(c.args[0]) == first:
                    nested.append(c.func.id)
            if flags or nested:
                self.helpers[node.name] = flags
                calls[node.name] = nested
        for name, nested in calls.items():
            for n in nested:
                self.helpers[name] = self.helpers[name] + self.helpers.get(n, [])

    def _flags_of_parent(self, key: str, seen=None) -> list[str]:
        seen = seen or set()
        if key in seen:
            return []
        seen.add(key)
        out = []
        for p in self.parents.get(key, []):
            out += self._flags_of_parent(p, seen)
        return out + self.parent_flags.get(key, [])

    @staticmethod
    def _branch_names(
        call: ast.Call, names: list[str], parents: dict, loopvar: str | None
    ) -> list[str]:
        """Narrow a loop-expanded command name by an enclosing `if var == "x"` branch."""
        if loopvar is None:
            return names
        child: ast.AST = call
        node = parents.get(call)
        while node is not None:
            if (
                isinstance(node, ast.If)
                and isinstance(node.test, ast.Compare)
                and _unparse(node.test.left) == loopvar
                and isinstance(node.test.ops[0], ast.Eq)
                and isinstance(node.test.comparators[0], ast.Constant)
            ):
                val = node.test.comparators[0].value
                in_body = any(child is b for b in node.body)
                return [val] if in_body else [n for n in names if n != val]
            child, node = node, parents.get(node)
        return names

    def _scan_commands(self, tree: ast.AST, path: Path, loops) -> None:
        fname = path.name
        parents_of = {c: p for p in ast.walk(tree) for c in ast.iter_child_nodes(p)}
        alias: dict[str, str] = {}
        subparser_of: dict[str, str] = {}
        assigned: dict[str, list[tuple[int, list[str]]]] = collections.defaultdict(list)
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.Assign)
                and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
            ):
                v = node.value
                if isinstance(v, ast.Attribute) and isinstance(v.value, ast.Name):
                    alias[node.targets[0].id] = f"_cli_options.py:{v.attr}"
                if (
                    isinstance(v, ast.Call)
                    and getattr(v.func, "attr", "") == "add_subparsers"
                    and isinstance(v.func.value, ast.Name)
                ):
                    subparser_of[node.targets[0].id] = v.func.value.id
        calls = sorted(
            (
                n
                for n in ast.walk(tree)
                if isinstance(n, ast.Call)
                and getattr(n.func, "attr", "") == "add_parser"
                and n.args
            ),
            key=lambda n: (n.lineno, n.col_offset),
        )
        for call in calls:
            arg = call.args[0]
            loopvar = None
            if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                names = [arg.value]
            elif isinstance(arg, ast.Name) and arg.id in loops:
                loopvar = arg.id
                names = self._branch_names(call, loops[arg.id], parents_of, loopvar)
            else:
                continue
            recv = call.func.value
            parent_cmds = [None]
            if isinstance(recv, ast.Name) and recv.id in subparser_of:
                parent_cmds = self._latest(assigned, subparser_of[recv.id], call.lineno) or [None]
            parents = [
                alias.get(_unparse(e), self._parent_key(fname, e))
                for k in call.keywords
                if k.arg == "parents"
                for e in getattr(k.value, "elts", [])
            ]
            full_names = [f"{pc}.{n}" if pc else n for pc in parent_cmds for n in names]
            for full in full_names:
                entry = self.commands.setdefault(
                    full,
                    {"file": str(path.relative_to(self.root)), "line": call.lineno, "flags": []},
                )
                for p in parents:
                    self._add(entry, self._flags_of_parent(p))
            holder = parents_of.get(call)
            if isinstance(holder, ast.Assign) and isinstance(holder.targets[0], ast.Name):
                assigned[holder.targets[0].id].append((call.lineno, full_names))
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            found: list[str] = []
            recv = None
            if getattr(node.func, "attr", "") == "add_argument":
                recv = node.func.value
                found = self._long_flags(node, loops)
            elif isinstance(node.func, ast.Name) and node.func.id in self.helpers and node.args:
                recv = node.args[0]
                found = self.helpers[node.func.id]
            if isinstance(recv, ast.Name):
                for full in self._latest(assigned, recv.id, node.lineno) or []:
                    self._add(self.commands[full], found)

    @staticmethod
    def _latest(assigned, var: str, line: int) -> list[str] | None:
        """Command names of the last assignment to var above line."""
        best = None
        for ln, names in assigned.get(var, []):
            if ln <= line and (best is None or ln >= best[0]):
                best = (ln, names)
        return best[1] if best else None

    @staticmethod
    def _add(entry: dict, flags: list[str]) -> None:
        for fl in flags:
            if fl not in entry["flags"]:
                entry["flags"].append(fl)


def zero_registrations(registrations: dict[str, int]) -> list[str]:
    """A front-end with no flag registration read, which a broken scan and an empty CLI share."""
    return [f"{s}: 0 flag registrations" for s, n in registrations.items() if n == 0]


def extract_cli(root: Path) -> tuple[list[dict], dict[str, int], dict]:
    scan = _PyCliScan(root)
    scan.scan()
    py_cmds = scan.commands
    parity_names = {f.raw_name for f in parity_cli.extract(root, "python").functions}
    missing = sorted(n for n in parity_names if n not in py_cmds and f"project.{n}" not in py_cmds)
    if missing:
        raise InventoryError(
            f"python CLI scan missed commands the parity extractor sees: {missing}"
        )
    nat_cmds, nat_files, nat_regs, nat_problems = _native_cli(root)
    out = []
    for surface, cmds in (("cli-python", py_cmds), ("cli-native", nat_cmds)):
        for name in sorted(cmds):
            c = cmds[name]
            switches = c["switches"] if surface == "cli-native" else scan.switches
            aliases = (
                c["aliases"]
                if surface == "cli-native"
                else {a: p for a, p in scan.alias_of.items() if p in c["flags"]}
            )
            group = any(other.startswith(name + ".") for other in cmds)
            out.append(_record(surface, "command", name, c["file"], c["line"], True, group=group))
            for fl in sorted(c["flags"]):
                out.append(
                    _record(
                        surface,
                        "flag",
                        f"{name} --{fl}",
                        c["file"],
                        c["line"],
                        True,
                        owner=name,
                        member=fl,
                        boolean=fl in switches,
                        alias_of=aliases.get(fl, ""),
                        doc_text="\n".join(scan.help.get(fl, []))
                        if surface == "cli-python"
                        else "",
                    )
                )
    contract = json.loads((root / CLI_CONTRACT).read_text(encoding="utf-8"))
    only = collections.defaultdict(list)
    for cname, spec in contract.get("commands", {}).items():
        only[spec.get("classification", "")].append(cname)
    registrations = {"cli-python": scan.registrations, "cli-native": nat_regs}
    problems = scan.problems + nat_problems
    problems += zero_registrations(registrations)
    meta = {
        "cli_only": {k: sorted(only.get(k, [])) for k in ("python_only", "native_only")},
        "cli_registrations": registrations,
        "cli_problems": problems,
    }
    return out, {"cli-python": len(scan.files), "cli-native": nat_files}, meta


# ----------------------------------------------------------------------- domain


def assign_domains(records: list[dict]) -> tuple[collections.Counter, list[str]]:
    """Set domain and domain_path on every record: classify per surface, then inherit.

    The cross-surface name vote is not applied here; the checker applies the frozen
    assignment in domains.json.
    """
    errors: list[str] = []
    for r in records:
        if r["owner"] or r.get("domain"):
            continue
        try:
            d, p = domains.classify(r["surface"], r["name"], r["file"])
        except domains.UnknownCommandError:
            errors.append(
                f"{r['surface']} command {r['name']}: not in the CLI command domain table"
            )
            d, p = "core", "default"
        r["domain"], r["domain_path"] = d, p
    domains.inherit(records)
    return collections.Counter(r["domain_path"] for r in records), errors


# ---------------------------------------------------------------------- build


def build(root: Path = ROOT) -> dict:
    """Build the full inventory.

    @throws InventoryError when a surface cannot be read.
    """
    records: list[dict] = []
    files: dict[str, int] = {}
    meta: dict = {}
    ts, f = extract_ts(root)
    records += ts
    files.update(f)
    py, f, m = extract_python(root)
    records += py
    files.update(f)
    meta.update(m)
    c, f = extract_c(root)
    records += c
    files.update(f)
    cl, f, m = extract_cli(root)
    records += cl
    files.update(f)
    meta.update(m)
    paths, errors = assign_domains(records)
    errors += meta.pop("cli_problems", [])
    records.sort(key=lambda r: (r["surface"], r["kind"], r["name"], ",".join(r["entries"])))
    counts = collections.Counter(r["surface"] for r in records)
    return {
        "records": records,
        "files": {s: files.get(s, 0) for s in SURFACES},
        "counts": {s: counts.get(s, 0) for s in SURFACES},
        "domain_paths": {p: paths.get(p, 0) for p in domains.PATHS},
        "errors": errors,
        **meta,
    }


def _tag_python_scope(tree: Path) -> dict[str, object]:
    """The python extractor's module lists as the tagged tree itself declared them."""
    src = (tree / "tools/parity/extractors/python_pyi.py").read_text(encoding="utf-8")
    scope = {}
    for node in ast.parse(src).body:
        target = node.targets[0] if isinstance(node, ast.Assign) else getattr(node, "target", None)
        if isinstance(target, ast.Name) and target.id in ("_SCANNED", "_NOT_SCANNED"):
            scope[target.id] = ast.literal_eval(node.value)
    return scope


def build_at_tag(root: Path, tag: str) -> tuple[dict | None, str]:
    """Build the inventory of a tagged release with today's extractors.

    Returns (inventory, "") or (None, reason) when the tag is not in this clone
    or its tree cannot be read. The python module lists come from the tagged tree,
    since today's lists name modules the release did not have.
    """
    probe = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "-q", "--verify", f"refs/tags/{tag}"],
        capture_output=True,
        check=False,
    )
    if probe.returncode != 0:
        return (
            None,
            f"refs/tags/{tag} is not in this clone; fetch it with: git fetch origin tag {tag}",
        )
    with tempfile.TemporaryDirectory(prefix="sonare-api-tag-") as d:
        tree = Path(d)
        arch = subprocess.run(
            ["git", "-C", str(root), "archive", tag, "--", "bindings", "include", "tools", "tests"],
            capture_output=True,
            check=False,
        )
        if arch.returncode != 0:
            return None, f"git archive {tag} failed: {arch.stderr.decode()[:200]}"
        with tarfile.open(fileobj=io.BytesIO(arch.stdout)) as tar:
            tar.extractall(tree, filter="data")
        for pkg in ("bindings/wasm", "bindings/node"):
            (tree / pkg / "node_modules").symlink_to(root / pkg / "node_modules")
        saved = {k: getattr(python_pyi, k) for k in ("_SCANNED", "_NOT_SCANNED")}
        try:
            for k, v in _tag_python_scope(tree).items():
                setattr(python_pyi, k, v)
            return build(tree), ""
        except (InventoryError, python_pyi.PythonScopeError, OSError) as exc:
            return None, f"{tag} tree unreadable: {exc}"
        finally:
            for k, v in saved.items():
                setattr(python_pyi, k, v)


def scan_failures(inv: dict) -> list[str]:
    """Surfaces that scanned nothing or produced nothing."""
    out = []
    for s in SURFACES:
        if inv["files"].get(s, 0) == 0:
            out.append(f"{s}: scanned 0 files")
        if inv["counts"].get(s, 0) == 0:
            out.append(f"{s}: produced 0 records")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--json", action="store_true", help="dump the inventory as JSON")
    ap.add_argument("--root", type=Path, default=ROOT)
    args = ap.parse_args(argv)
    inv = build(args.root)
    if args.json:
        json.dump(inv, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write("\n")
    else:
        for s in SURFACES:
            print(f"{s}: scanned {inv['files'][s]} files, {inv['counts'][s]} records")
        for s, n in inv["cli_registrations"].items():
            print(f"{s}: {n} flag registrations")
        print("domain paths: " + ", ".join(f"{k}={v}" for k, v in inv["domain_paths"].items()))
    failures = scan_failures(inv) + inv["errors"]
    for line in failures:
        print(line, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
