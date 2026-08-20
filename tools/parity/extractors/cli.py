"""Extract the CLI surfaces (best-effort).

Two CLIs exist and neither maps 1:1 onto C function names:

* Python ``cli.py`` — argparse with ``sub.add_parser("bpm", ...)`` subcommands
  and per-subcommand ``add_argument("--hop-length", default=512)`` options. We
  parse it with ``ast`` to recover subcommand names and their option defaults.
* The native C++ CLI under ``tools/`` — commands are declared as
  ``add_command(registry, "name", ...)`` records, which we read directly.
  Command names only: their option specs are built by helper calls whose
  defaults are not recovered here, and the CLI is not positionally diffed.

Both front-ends contribute to the one ``cli`` surface, which answers "is this
capability reachable from a command line" rather than "from which binary" -- the
two share most of their vocabulary but each ships commands the other does not.
Entries parsed from the Python CLI win on a name collision because they carry
per-option defaults.

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


# Directory holding the native CLI translation units. Walked rather than named,
# so a registry that moves to a new file in here keeps being read.
_NATIVE_CLI_DIR = "tools"

# A registration in the native CLI's command registry:
# ``add_command(commands, "chroma", true, {...})``. Keyed on the call and its
# command-name literal only -- never on line breaks, indentation or the option
# list, so reformatting and option churn in the registry do not change what is
# collected here.
_ADD_COMMAND_RE = re.compile(r'\badd_command\s*\(\s*\w+\s*,\s*"([^"]+)"')

# Below this the walk is treated as broken rather than as a small CLI. The
# native registry holds most of the shipped commands, so a handful means the
# pattern or the location stopped matching -- which is exactly how this leg
# spent a long time reporting on a fraction of the surface it claims to cover.
_MIN_NATIVE_COMMANDS = 40


class CliScopeError(RuntimeError):
    """Raised when the native CLI registry scan collapsed, so no result is trustworthy."""


def _extract_cpp_cli(root: Path, ex: Extraction) -> None:
    """Collect the native CLI's commands from its registration calls.

    The native CLI used to dispatch on ``argv[1] == "name"`` comparisons, and
    this reader matched those. The registry has since moved into
    ``add_command(...)`` records, leaving the old pattern resolving against a
    handful of leftover literals and silently reporting a near-empty surface.
    Reading the registrations is reading the source of truth.
    """
    cpp_dir = root / _NATIVE_CLI_DIR
    if not cpp_dir.is_dir():
        raise CliScopeError(f"native CLI directory '{_NATIVE_CLI_DIR}' is missing")

    commands: dict[str, tuple[str, str, int]] = {}
    for path in sorted(cpp_dir.glob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        rel = str(path.relative_to(root))
        for m in _ADD_COMMAND_RE.finditer(text):
            name = m.group(1)
            commands.setdefault(
                kebab_to_snake(name), (name, rel, text.count("\n", 0, m.start()) + 1)
            )

    if len(commands) < _MIN_NATIVE_COMMANDS:
        raise CliScopeError(
            f"found {len(commands)} native CLI command registration(s) under "
            f"'{_NATIVE_CLI_DIR}', expected at least {_MIN_NATIVE_COMMANDS}. The registry "
            "moved or the registration spelling changed; refusing to report a CLI "
            "surface this small rather than silently under-reporting coverage."
        )

    # The Python CLI is parsed first and its entries win: it carries per-option
    # defaults, which the native registry's option specs are not parsed for.
    for key, (raw_name, rel, line) in sorted(commands.items()):
        if any(f.key == key for f in ex.functions):
            continue
        ex.functions.append(
            FunctionSig(
                key=key,
                surface="cli",
                raw_name=raw_name,
                params=[],
                file=rel,
                line=line,
            )
        )


def _note_cross_cli_spellings(ex: Extraction) -> None:
    """Record commands the two CLIs spell differently, as a note only.

    The native CLI namespaces some commands (``project.bounce``) that the Python
    CLI spells bare (``bounce``). Each spelling is its own key, so the namespaced
    one reports as a CLI-only symbol with no C counterpart -- true of the name,
    misleading about the surface, because the command is reachable under the
    other spelling too.

    This is a note rather than a normalization on purpose. Collapsing
    ``project.x`` onto ``x`` would assert the two are the same command; the
    naming difference between the front-ends is deliberate, and a bare command
    of the same name could exist independently. Stating the pairing lets a
    reader inherit that judgement instead of rediscovering it, without the tool
    deciding it. The pairs are derived from the collected commands on every run,
    so a renamed or added command cannot leave a stale list behind.
    """
    keys = {f.key for f in ex.functions}
    pairs = sorted(
        (key, key.split(".", 1)[1])
        for key in keys
        if "." in key and key.split(".", 1)[1] in keys
    )
    if not pairs:
        return
    spelled = ", ".join(f"{dotted} = {bare}" for dotted, bare in pairs)
    ex.unparsed_notes.append(
        f"{len(pairs)} command(s) are spelled two ways across the two CLIs and so "
        f"appear under both keys ({spelled}). Not normalized: the naming difference is "
        "deliberate and the tool does not assert the spellings denote one command."
    )


def _is_attr_call(node: ast.Call, attr: str) -> bool:
    return isinstance(node.func, ast.Attribute) and node.func.attr == attr


def extract(root: Path) -> Extraction:
    ex = Extraction(surface="cli")
    _extract_python_cli(root, ex)
    _extract_cpp_cli(root, ex)
    _note_cross_cli_spellings(ex)
    return ex
