"""Extract the CLI surfaces (best-effort).

Two CLIs exist and neither maps 1:1 onto C function names:

* Python ``cli.py`` — argparse with ``sub.add_parser("bpm", ...)`` subcommands
  and per-subcommand ``add_argument("--hop-length", default=512)`` options. We
  parse it with ``ast`` to recover subcommand names and their option defaults.
* C++ ``tools/sonare_cli.cpp`` — a JSON-path / macro-driven tool that does NOT
  expose discrete subcommands the same way. We do a light token scan for quoted
  command-like literals and record a coverage-only signal.

CLI subcommands use short names (``bpm`` not ``detect_bpm``), so they are kept
as their own keys (kebab->snake) and used for coverage reporting; they are not
positionally diffed against the C ABI (the mapping is intentionally loose).
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

from model import Extraction, FunctionSig, Param
from normalize import kebab_to_snake, normalize_default, normalize_param_name


def _str_const(node: ast.expr | None) -> str | None:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _kw(call: ast.Call, name: str) -> ast.expr | None:
    for k in call.keywords:
        if k.arg == name:
            return k.value
    return None


def _extract_python_cli(root: Path, ex: Extraction) -> None:
    path = root / "bindings" / "python" / "src" / "libsonare" / "cli.py"
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    try:
        tree = ast.parse(text, filename=str(path))
    except SyntaxError as e:
        ex.unparsed += 1
        ex.unparsed_notes.append(f"cli.py: parse error {e}")
        return

    # Map the python variable holding each subparser -> (command, params, line).
    commands: dict[str, FunctionSig] = {}
    # Track which variable name a subparser was assigned to.
    var_to_cmd: dict[str, str] = {}

    for node in ast.walk(tree):
        # `xxx_p = sub.add_parser("name", ...)` or bare `sub.add_parser("name")`
        if isinstance(node, ast.Call) and _is_attr_call(node, "add_parser"):
            cmd = _str_const(node.args[0]) if node.args else None
            if not cmd:
                continue
            key = kebab_to_snake(cmd)
            sig = commands.get(key)
            if sig is None:
                sig = FunctionSig(
                    key=key,
                    surface="cli",
                    raw_name=cmd,
                    params=[],
                    file=str(path.relative_to(root)),
                    line=node.lineno,
                )
                commands[key] = sig

    # Second pass: assignments `var = sub.add_parser("cmd")` to link add_argument.
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call):
            call = node.value
            if _is_attr_call(call, "add_parser") and call.args:
                cmd = _str_const(call.args[0])
                if (
                    cmd
                    and len(node.targets) == 1
                    and isinstance(node.targets[0], ast.Name)
                ):
                    var_to_cmd[node.targets[0].id] = kebab_to_snake(cmd)

    # Third pass: `<var>.add_argument("--opt", default=..., type=..., action=...)`
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and _is_attr_call(node, "add_argument"):
            if not isinstance(node.func, ast.Attribute):
                continue
            recv = node.func.value
            if not isinstance(recv, ast.Name):
                continue
            cmdkey = var_to_cmd.get(recv.id)
            if cmdkey is None or cmdkey not in commands:
                continue
            opt = None
            for a in node.args:
                s = _str_const(a)
                if s and s.startswith("--"):
                    opt = s
                    break
            if opt is None:
                continue
            optname = normalize_param_name(opt.lstrip("-"))
            action = _str_const(_kw(node, "action"))
            default_node = _kw(node, "default")
            if action == "store_true":
                default = "false"
            elif action == "store_false":
                default = "true"
            elif default_node is not None:
                try:
                    default = normalize_default(ast.unparse(default_node))
                except Exception:  # noqa: BLE001
                    default = None
            else:
                default = None
            # choices=[...] -> enum value set
            enum_values: tuple[str, ...] = ()
            ch = _kw(node, "choices")
            if isinstance(ch, (ast.List, ast.Tuple)):
                vals = [_str_const(e) for e in ch.elts]
                enum_values = tuple(sorted(v for v in vals if v))
            commands[cmdkey].params.append(
                Param(
                    name=optname,
                    raw_name=opt,
                    default=default,
                    enum_values=enum_values,
                )
            )

    ex.functions.extend(commands.values())


_CPP_CMD_RE = re.compile(r'==\s*"([a-z][a-z0-9\-\.]+)"')


def _extract_cpp_cli(root: Path, ex: Extraction) -> None:
    path = root / "tools" / "sonare_cli.cpp"
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    # Best-effort: collect command-like quoted literals compared with ==.
    seen = {f.key for f in ex.functions}
    candidates: set[str] = set()
    for m in _CPP_CMD_RE.finditer(text):
        lit = m.group(1)
        # Skip obvious value/enum literals (mode names, profile names) by keeping
        # only multi-segment command paths or known verb-like tokens.
        if "." in lit or "-" in lit:
            candidates.add(lit)
    # Record only as low-confidence coverage notes; do not synthesize fake sigs.
    ex.unparsed_notes.append(
        f"sonare_cli.cpp: JSON-path/macro CLI, {len(candidates)} command-like literals "
        f"(not positionally diffed)"
    )


def _is_attr_call(node: ast.Call, attr: str) -> bool:
    return isinstance(node.func, ast.Attribute) and node.func.attr == attr


def extract(root: Path) -> Extraction:
    ex = Extraction(surface="cli")
    _extract_python_cli(root, ex)
    _extract_cpp_cli(root, ex)
    return ex
