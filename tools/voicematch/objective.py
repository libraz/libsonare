"""A fingerprint of the fit objective, so a fit can say which objective it answered.

The objective is the code that scores a render against its reference: the
terms, their weights and units, the per-note reducer and the quality report.
Its modules are found by walking the imports of `ROOTS` inside this directory,
so a new metric module joins by being imported rather than by being listed.
Each module contributes its text with comments, docstrings and whitespace
removed, so editing prose or re-wrapping leaves the fingerprint alone while any change
to the code moves it, including a refactor that computes the same thing. The
stimulus (corpus, patterns) and the reference boundary rules are not part of
it: they decide what is rendered, not how a render is scored.
"""

from __future__ import annotations

import ast
import hashlib
import io
import re
import tokenize
from pathlib import Path

HERE = Path(__file__).resolve().parent

#: The modules the objective is entered through.
ROOTS = ("loss", "loss_aggregate", "fit_quality")


def _imports(tree: ast.AST) -> set[str]:
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
            names.add(node.module.split(".")[0])
    return names


def _prose_spans(text: str) -> list[tuple[tuple[int, int], tuple[int, int]]]:
    """(start, end) positions of every comment and docstring in a module."""
    lines = text.splitlines(keepends=True)

    def chars(line: int, byte_col: int) -> int:
        # AST columns count UTF-8 bytes; tokenize columns and slicing count characters.
        return len(lines[line - 1].encode()[:byte_col].decode(errors="ignore"))

    spans = []
    for node in ast.walk(ast.parse(text)):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            first = node.body[0] if node.body else None
            if (
                isinstance(first, ast.Expr)
                and isinstance(first.value, ast.Constant)
                and isinstance(first.value.value, str)
            ):
                start = (first.lineno, chars(first.lineno, first.col_offset))
                end = (first.end_lineno, chars(first.end_lineno, first.end_col_offset))
                spans.append((start, end))
    for tok in tokenize.generate_tokens(io.StringIO(text).readline):
        if tok.type == tokenize.COMMENT:
            spans.append((tok.start, tok.end))
    return spans


def code_text(text: str) -> str:
    """The module with comments, docstrings and all whitespace removed.

    Text rather than an AST dump or a token stream, because both of those are
    spelled differently from one Python release to the next and the fingerprint
    must not be; the positions the prose is cut at are the same in every release.
    """
    lines = text.splitlines(keepends=True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))
    cut = sorted(
        (offsets[a[0] - 1] + a[1], offsets[b[0] - 1] + b[1]) for a, b in _prose_spans(text)
    )
    kept, pos = [], 0
    for lo, hi in cut:
        if lo >= pos:
            kept.append(text[pos:lo])
            pos = hi
    kept.append(text[pos:])
    return re.sub(r"\s+", "", "".join(kept))


def objective_modules(directory: Path = HERE) -> list[str]:
    """Every local module the objective's roots reach, sorted."""
    local = {p.stem for p in directory.glob("*.py")}
    seen: set[str] = set()
    pending = [r for r in ROOTS if r in local]
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        tree = ast.parse((directory / f"{name}.py").read_text())
        pending.extend(sorted((_imports(tree) & local) - seen))
    return sorted(seen)


def objective_digest(directory: Path = HERE) -> str:
    """The fingerprint: sha256 over each objective module's code text."""
    h = hashlib.sha256()
    for name in objective_modules(directory):
        h.update(name.encode() + b"\0" + code_text((directory / f"{name}.py").read_text()).encode())
        h.update(b"\0")
    return h.hexdigest()[:16]
