"""Extract the Python surface from .pyi stubs via the stdlib ``ast`` module.

We parse the type stubs (``analyzer.pyi``, ``audio.pyi``, ``engine.pyi``) which
carry the real signatures (names, annotations, defaults). The ``__init__.pyi``
is just re-exports, so it is parsed only to know the public symbol set.

The headless-arrangement ``Project`` surface lives in ``_project.py`` (an inline
type-annotated module with no separate ``.pyi`` stub), so that module is parsed
too. ``ast`` reads signatures only — function bodies are ignored — so reading the
implementation ``.py`` yields the same surface a stub would.

Methods on classes (``Audio``, ``Mixer``, ``Project``, ...) are emitted too;
their ``self`` parameter is marked structural.
"""

from __future__ import annotations

import ast
from pathlib import Path

from model import Extraction, FunctionSig, Param
from normalize import canonical_key, normalize_default, normalize_param_name


def _ann_to_str(node: ast.expr | None) -> str:
    if node is None:
        return ""
    try:
        return ast.unparse(node)
    except Exception:  # noqa: BLE001
        return ""


def _default_to_str(node: ast.expr | None) -> str | None:
    if node is None:
        return None
    try:
        return ast.unparse(node)
    except Exception:  # noqa: BLE001
        return None


def _enum_values_from_ann(ann: str) -> tuple[str, ...]:
    """Pull string-literal members out of a ``Literal[...]`` / union annotation."""
    import re

    vals = re.findall(r"""['"]([^'"]+)['"]""", ann)
    return tuple(sorted(set(vals)))


def _params_from_args(args: ast.arguments) -> list[Param]:
    params: list[Param] = []
    positional = list(args.posonlyargs) + list(args.args)
    # Align defaults to the tail of positional args.
    n_def = len(args.defaults)
    pad = [None] * (len(positional) - n_def)
    pos_defaults = pad + list(args.defaults)
    for a, d in zip(positional, pos_defaults):
        ann = _ann_to_str(a.annotation)
        structural = a.arg in ("self", "cls")
        params.append(
            Param(
                name=normalize_param_name(a.arg),
                raw_name=a.arg,
                default=normalize_default(_default_to_str(d)),
                type=ann,
                enum_values=_enum_values_from_ann(ann),
                structural=structural,
            )
        )
    # Keyword-only args (after ``*``): e.g. ``*, validate: bool = True``.
    for a, d in zip(args.kwonlyargs, args.kw_defaults):
        ann = _ann_to_str(a.annotation)
        structural = a.arg in ("validate",)
        params.append(
            Param(
                name=normalize_param_name(a.arg),
                raw_name=a.arg,
                default=normalize_default(_default_to_str(d)),
                type=ann,
                enum_values=_enum_values_from_ann(ann),
                structural=structural,
            )
        )
    return params


def _walk(node: ast.AST, ex: Extraction, file: str, class_prefix: str = "") -> None:
    for child in ast.iter_child_nodes(node):
        if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
            name = child.name
            if name.startswith("_"):
                continue
            key = canonical_key(name, "python")
            ex.functions.append(
                FunctionSig(
                    key=key,
                    surface="python",
                    raw_name=(class_prefix + "." + name) if class_prefix else name,
                    params=_params_from_args(child.args),
                    returns=_ann_to_str(child.returns),
                    file=file,
                    line=child.lineno,
                )
            )
        elif isinstance(child, ast.ClassDef):
            _walk(child, ex, file, class_prefix=child.name)


def extract(root: Path) -> Extraction:
    ex = Extraction(surface="python")
    base = root / "bindings" / "python" / "src" / "libsonare"
    for fname in ("analyzer.pyi", "audio.pyi", "engine.pyi", "_project.py"):
        path = base / fname
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        try:
            tree = ast.parse(text, filename=str(path))
        except SyntaxError as e:
            ex.unparsed += 1
            ex.unparsed_notes.append(f"{fname}: parse error {e}")
            continue
        _walk(tree, ex, str(path.relative_to(root)))
    return ex
