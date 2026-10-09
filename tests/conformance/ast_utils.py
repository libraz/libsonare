import ast
from collections.abc import Iterator


def own_body(function: ast.AST) -> Iterator[ast.AST]:
    for child in ast.iter_child_nodes(function):
        if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            continue
        yield child
        yield from own_body(child)
