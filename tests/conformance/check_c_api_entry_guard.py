"""Check that every exported C-ABI entry point runs inside the shared guard.

The guard is the macro family in ``src/c_api/sonare_c_internal.h``:
``SONARE_C_API_ENTRY`` clears the thread-local error, and ``SONARE_C_TRY`` with a
``SONARE_C_CATCH*`` turns any C++ exception into a code or sentinel.  Each
exported definition (a ``sonare_*`` name the public headers declare, defined in
``src/c_api/*.cpp``) must, on every preprocessor path, be one of:

* **guarded** -- one top-level ``SONARE_C_TRY`` whose ``SONARE_C_CATCH*`` ends it;
* **runner** -- it ends in ``return f(...)`` with ``f`` a safe function or entry;
* **realtime** -- it opens with ``SONARE_C_RT_API_ENTRY`` (no thread-local access);
* **nothrow** -- nothing in it can throw;
* **exempt** -- listed in ``_EXEMPT`` with the reason, reported once it passes.

What runs before the guard point -- the prefix, or a runner's arguments -- must be
no-throw, by default-deny: no allocating construct, and calls only to names every
declaration under ``src/`` marks ``noexcept``, to ``src/c_api`` functions inferred
safe to a fixed point, to release functions, or to the short lists below.  **The
blind spot is the ``if``-condition allowance**: a validation predicate is trusted.
An entry returning ``SonareError`` or able to record an error must open with
``SONARE_C_API_ENTRY`` or ``SONARE_C_TRY``; the diagnostic accessors must not.
"""

from __future__ import annotations

import argparse
import importlib.util
import itertools
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE_DIR = ROOT / "src" / "c_api"
DEFAULT_HEADER_DIR = ROOT / "include" / "sonare"

_SPEC = importlib.util.spec_from_file_location(
    "libsonare_c_api_entry_guard_parser",
    Path(__file__).resolve().parent / "check_c_api_out_param_init.py",
)
assert _SPEC is not None and _SPEC.loader is not None
_PARSER = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = _PARSER
_SPEC.loader.exec_module(_PARSER)

strip_comments_and_literals = _PARSER.strip_comments_and_literals
match_delimiter = _PARSER.match_delimiter

_TRY = re.compile(r"SONARE_C_TRY\b")
_CATCH = re.compile(r"SONARE_C_CATCH(?:_RETURN|_VOID)?\b")
_CLEAR_ENTRY = re.compile(r"\s*(?:SONARE_C_API_ENTRY\s*;|SONARE_C_TRY\b)")
_RT_ENTRY = re.compile(r"\s*SONARE_C_RT_API_ENTRY\s*;")
_RECORDS_ERROR = re.compile(
    r"\b(?:set_last_error|record_current_exception)\s*\(|SONARE_C_TRY\b|SONARE_C_CATCH"
)
_READS_DIAGNOSTICS = re.compile(r"\blast_\w*_storage\s*\(|\blast_error_message\s*\(")
_CALL = re.compile(r"(\.|->)?\s*\b([A-Za-z_]\w*)\s*(?:<[^;(){}<>]*>)?\s*\(")
_COMPOUND = re.compile(r"(?:if|for|while|switch|else|try|catch|do)\b|\{")
_DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(\w+)")
_DEFINITION = re.compile(r"^[ \t]*(?:[A-Za-z_][^\n;{}()=]*?)\b([A-Za-z_]\w*)\s*\(", re.MULTILINE)

# Constructs that allocate or throw wherever they appear.  A reference or a
# pointer to a container is not a construction, so it is not listed.
_THROWING = re.compile(
    r"\bnew\b|\bthrow\b|\bstatic\b|\bstd::(?:to_string|ostringstream|make_unique|make_shared)\b"
    r"|\bstd::(?:string|vector|map|unordered_map|function)\b"
    r"(?!\s*<[^;{}()=]*?>\s*[&*>,])(?!\s*[&*>,])"
)

_KEYWORDS = frozenset(
    {
        "if",
        "for",
        "while",
        "switch",
        "return",
        "sizeof",
        "alignof",
        "decltype",
        "static_cast",
        "reinterpret_cast",
        "const_cast",
        "static_assert",
        "noexcept",
        "defined",
        "catch",
    }
)

# Members that only read state and cannot throw on any container or handle this
# tree passes across the boundary.
_NOTHROW_MEMBERS = frozenset({"size", "data", "empty", "c_str", "get", "sample_rate", "duration"})

# The C library, <algorithm> over plain values, and std::numeric_limits: no
# declaration under src/ to carry a noexcept.
_NOTHROW_FREE = frozenset(
    {
        "isfinite",
        "isinf",
        "isnan",
        "memset",
        "memcpy",
        "min",
        "max",
        "abs",
        "fabs",
        "clamp",
        "copy_n",
        "begin",
        "end",
        "find_if",
        "get_if",
        "holds_alternative",
        "offsetof",
        "dynamic_cast",
        "quiet_NaN",
        "infinity",
        "epsilon",
        "lowest",
    }
)

# Entries the default-deny rule cannot clear by itself, each with why it cannot
# throw.  An entry that passes on its own is reported, so the list only shrinks.
_EXEMPT = {
    "sonare_hz_to_mel": "float arithmetic in src/core/convert.cpp; no allocation, no throw",
    "sonare_mel_to_hz": "float arithmetic in src/core/convert.cpp; no allocation, no throw",
    "sonare_hz_to_midi": "float arithmetic in src/core/convert.cpp; no allocation, no throw",
    "sonare_midi_to_hz": "float arithmetic in src/core/convert.cpp; no allocation, no throw",
    "sonare_frames_to_samples": "saturating int arithmetic in src/core/convert.cpp; no throw",
    "sonare_samples_to_frames": "saturating int arithmetic in src/core/convert.cpp; no throw",
}

_RELEASE = re.compile(r"^(?:sonare_)?free_\w+$|^sonare_\w+_destroy$|^sonare_free_\w+$")


def _blank_span(text: str, start: int, end: int) -> str:
    return text[:start] + "".join(c if c == "\n" else " " for c in text[start:end]) + text[end:]


def branch_variants(body: str, limit: int = 32) -> list[str]:
    """Every preprocessor path through ``body``, each as plain code.

    A gated entry has a feature-on body and a feature-off stub, and both ship;
    reading them as one text would let a guard in one branch cover the other.
    """
    lines = body.split("\n")

    def parse(index: int, stop: set[str]) -> tuple[list, int]:
        items: list = []
        while index < len(lines):
            match = _DIRECTIVE.match(lines[index])
            kind = match.group(1) if match else ""
            if kind in stop:
                return items, index
            if kind in {"if", "ifdef", "ifndef"}:
                branches: list[list] = []
                index += 1
                while True:
                    branch, index = parse(index, {"elif", "else", "endif"})
                    branches.append(branch)
                    closer = _DIRECTIVE.match(lines[index]) if index < len(lines) else None
                    index += 1
                    if closer is None or closer.group(1) == "endif":
                        break
                if len(branches) == 1:
                    branches.append([])
                items.append(branches)
                continue
            if not match:
                items.append(lines[index])
            index += 1
        return items, index

    def expand(items: list) -> list[str]:
        variants = [""]
        for item in items:
            if isinstance(item, str):
                variants = [v + item + "\n" for v in variants]
            else:
                options = [o for branch in item for o in expand(branch)]
                variants = [v + o for v, o in itertools.product(variants, options)][:limit]
        return variants

    return expand(parse(0, set())[0])


def _macro_end(code: str, index: int) -> int | None:
    """End of a guard macro statement starting at ``index``, else None."""
    if index > 0 and (code[index - 1].isalnum() or code[index - 1] == "_"):
        return None
    match = _TRY.match(code, index) or _CATCH.match(code, index)
    if not match:
        return None
    end = match.end()
    if match.group(0) == "SONARE_C_CATCH_RETURN":
        rest = code[end:]
        end = match_delimiter(code, end + len(rest) - len(rest.lstrip()))
    tail = code[end:]
    if tail.lstrip().startswith(";"):
        end += len(tail) - len(tail.lstrip()) + 1
    return end


def top_level_statements(code: str) -> list[str]:
    """Each depth-zero statement of a body, the guard macros as their own."""
    spans: list[str] = []
    index, start, length = 0, None, len(code)
    while index < length:
        char = code[index]
        if start is None:
            if char.isspace():
                index += 1
                continue
            start = index
        macro_end = _macro_end(code, index) if char == "S" else None
        if macro_end is not None:
            if code[start:index].strip():
                spans.append(code[start:index])
            spans.append(code[index:macro_end])
            start, index = None, macro_end
            continue
        if char in "([{":
            close = match_delimiter(code, index)
            if char == "{" and _COMPOUND.match(code[start:index].lstrip() or "{"):
                rest = code[close:].lstrip()
                if not re.match(r"(?:else|catch|while)\b", rest):
                    spans.append(code[start:close])
                    start = None
            index = close
            continue
        if char == ";":
            spans.append(code[start : index + 1])
            start = None
        index += 1
    if start is not None and code[start:].strip():
        spans.append(code[start:])
    return spans


def _strip_lambda_bodies(text: str) -> str:
    """Blank the bodies of lambdas: a callee runs them inside its own guard."""
    lambda_head = re.compile(r"\]\s*(?:\([^{};]*\))?[^\[\]{};()]*\{")
    index = 0
    while True:
        match = lambda_head.search(text, index)
        if not match:
            return text
        open_at = match.end() - 1
        try:
            close = match_delimiter(text, open_at)
        except ValueError:
            return text
        text = _blank_span(text, open_at + 1, close - 1)
        index = close


def _if_conditions(text: str) -> list[tuple[int, int]]:
    spans = []
    for match in re.finditer(r"\bif\s*\(", text):
        try:
            spans.append((match.end() - 1, match_delimiter(text, match.end() - 1)))
        except ValueError:
            continue
    return spans


def throwing_constructs(text: str, safe: frozenset[str]) -> list[str]:
    """What in ``text`` could throw, by the default-deny rule."""
    text = _strip_lambda_bodies(text)
    found = [m.group(0).strip() for m in _THROWING.finditer(text)]
    conditions = _if_conditions(text)
    for match in _CALL.finditer(text):
        member, name = match.group(1), match.group(2)
        if name in _KEYWORDS or name.startswith("SONARE_C_"):
            continue
        if any(start <= match.start(2) < end for start, end in conditions):
            continue
        if member:
            if name in _NOTHROW_MEMBERS or name in safe:
                continue
        elif name in _NOTHROW_FREE or name in safe or _RELEASE.match(name):
            continue
        found.append(f"{name}()")
    return found


def _raw_guard(statement: str) -> bool:
    """A ``try`` statement with a catch-all arm that does not rethrow."""
    if not statement.lstrip().startswith("try"):
        return False
    for match in re.finditer(r"catch\s*\(\s*\.\.\.\s*\)\s*\{", statement):
        body = statement[match.end() - 1 : match_delimiter(statement, match.end() - 1)]
        if not re.search(r"\bthrow\s*;", body):
            return True
    return False


def classify(
    code: str, safe: frozenset[str], allow_raw_try: bool, macros: frozenset[str] = frozenset()
) -> tuple[str, list[str]]:
    """Class of one preprocessor path through a body, and its problems.

    A function's arguments are evaluated before its guard runs, so they count
    as prefix; a guard macro's arguments are the body of its lambda, so they do
    not.
    """
    if not code.strip():
        return "empty", []
    if _RT_ENTRY.match(code):
        return "realtime", []
    statements = top_level_statements(code)
    for index, statement in enumerate(statements):
        stripped = statement.strip()
        if _TRY.match(stripped) or (allow_raw_try and _raw_guard(stripped)):
            problems = []
            if _TRY.match(stripped):
                tail = statements[index + 1 :]
                catches = [i for i, s in enumerate(tail) if _CATCH.match(s.strip())]
                if catches != [len(tail) - 1]:
                    problems.append(
                        "SONARE_C_TRY is not closed by a SONARE_C_CATCH ending the body"
                    )
            else:
                tail = statements[index + 1 :]
                problems += [f"{c} after the try" for c in throwing_constructs("".join(tail), safe)]
            prefix = "".join(statements[:index])
            problems += [f"{c} before SONARE_C_TRY" for c in throwing_constructs(prefix, safe)]
            return "guarded", problems
    last = statements[-1].strip() if statements else ""
    call = re.match(r"(?:return\s+)?([A-Za-z_]\w*)\s*(?:<[^;(){}]*>)?\s*\(", last)
    if call and call.group(1) in safe and not last.startswith(("if", "for", "while", "switch")):
        arguments = "" if call.group(1) in macros else last[call.end() - 1 :]
        before = "".join(statements[:-1]) + arguments
        problems = [f"{c} before the guarded call" for c in throwing_constructs(before, safe)]
        return "runner", problems
    return "nothrow", [f"{c} with no guard" for c in throwing_constructs(code, safe)]


@dataclass
class Function:
    name: str
    path: Path
    line: int
    signature: str
    body: str


def parse_functions(path: Path) -> list[Function]:
    """Every column-0 function definition in one file."""
    code = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
    found: list[Function] = []
    for match in _DEFINITION.finditer(code):
        name = match.group(1)
        if name in _KEYWORDS or name.isupper():
            continue
        open_paren = match.end() - 1
        try:
            after = match_delimiter(code, open_paren)
        except ValueError:
            continue
        head = code[after:]
        brace = head.find("{")
        if brace < 0 or re.search(r"[;})(=]", head[:brace]):
            continue
        try:
            body_end = match_delimiter(code, after + brace)
        except ValueError:
            continue
        found.append(
            Function(
                name=name,
                path=path,
                line=code.count("\n", 0, match.start()) + 1,
                signature=code[match.start() : open_paren],
                body=code[after + brace + 1 : body_end - 1],
            )
        )
    return found


def noexcept_names(source_root: Path) -> frozenset[str]:
    """Names every declaration of which under ``src/`` is ``noexcept``.

    The promise is the declaration's, and the compiler holds the body to it.  A
    name with one declaration that does not make it -- an overload, or a method
    of the same name on another class -- is not taken, since a call site cannot
    be told apart by name.
    """
    declaration = re.compile(
        r"^[ \t]*(?!return\b|else\b|case\b|delete\b|new\b)"
        r"(?:(?:virtual|static|inline|constexpr|explicit|friend)\s+)*"
        r"[A-Za-z_][\w:<>,\s\*&]*?[\s\*&]([A-Za-z_]\w*)\s*\(",
        re.MULTILINE,
    )
    promised: set[str] = set()
    plain: set[str] = set()
    for path in sorted(source_root.rglob("*.h")) + sorted(source_root.rglob("*.cpp")):
        code = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
        for match in declaration.finditer(code):
            open_paren = match.end() - 1
            try:
                after = match_delimiter(code, open_paren)
            except ValueError:
                continue
            tail = re.match(r"[\s\w]*?(?=[;{=:]|$)", code[after : after + 120])
            qualifiers = tail.group(0) if tail else ""
            (promised if re.search(r"\bnoexcept\b", qualifiers) else plain).add(match.group(1))
    return frozenset(promised - plain - _KEYWORDS)


def exported_names(header_dir: Path) -> set[str]:
    names: set[str] = set()
    for path in sorted(header_dir.glob("*.h")):
        text = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
        names.update(re.findall(r"\b(sonare_\w+)\s*\(", text))
    return names


def macro_runners(source_dir: Path, safe: frozenset[str]) -> set[str]:
    """Macros expanding to ``return f([...] {...})`` with ``f`` safe.

    Their arguments are the lambda's body, so they run inside the guard.
    """
    found: set[str] = set()
    for path in sorted(source_dir.glob("*.h")) + sorted(source_dir.glob("*.cpp")):
        raw = path.read_text(encoding="utf-8", errors="replace").replace("\\\n", " ")
        for match in re.finditer(
            r"^[ \t]*#[ \t]*define[ \t]+(\w+)\([^)]*\)(.*)$", raw, re.MULTILINE
        ):
            target = re.match(r"\s*return\s+(\w+)\s*\(\s*\[", match.group(2))
            if target and target.group(1) in safe:
                found.add(match.group(1))
    return found


@dataclass
class Finding:
    function: str
    file: str
    line: int
    problem: str

    def as_line(self) -> str:
        return f"{self.file}:{self.line}  {self.function}: {self.problem}"


@dataclass
class Report:
    findings: list[Finding]
    entry_points: int
    classes: dict[str, int]
    safe_helpers: int


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def audit(source_dir: Path, header_dir: Path, noexcept_root: Path | None = None) -> Report:
    """Classify every exported entry under ``source_dir``.

    ``noexcept_root`` is the tree whose declarations supply the ``noexcept``
    names; it defaults to the parent of ``source_dir``.
    """
    exported = exported_names(header_dir) if header_dir.is_dir() else None
    functions: dict[str, list[Function]] = {}
    for path in sorted(source_dir.glob("*.h")) + sorted(source_dir.glob("*.cpp")):
        for function in parse_functions(path):
            functions.setdefault(function.name, []).append(function)

    def is_entry(function: Function) -> bool:
        return (
            function.name.startswith("sonare_")
            and function.path.suffix == ".cpp"
            and (exported is None or function.name in exported)
        )

    root = noexcept_root if noexcept_root is not None else source_dir.parent
    base = noexcept_names(root) if root.is_dir() else frozenset()
    safe: frozenset[str] = base
    results: dict[int, tuple[set[str], list[str]]] = {}
    # Safety is inferred to a fixed point: a function is safe once every path
    # through every definition of its name is, given what is already safe.
    for _ in range(12):
        runners = frozenset(macro_runners(source_dir, safe))
        current = safe | runners
        grown = set(current)
        for name, definitions in functions.items():
            clean = True
            for function in definitions:
                kinds, problems = set(), []
                for variant in branch_variants(function.body):
                    kind, variant_problems = classify(
                        variant, current, not is_entry(function), runners
                    )
                    kinds.add(kind)
                    problems += variant_problems
                results[id(function)] = (kinds, problems)
                clean = clean and not problems
            if clean:
                grown.add(name)
        if grown == set(current):
            break
        safe = frozenset(grown)

    # Which functions can leave something in the error slot, transitively: an
    # entry reaching one must clear the slot first.
    recording = {
        name
        for name, definitions in functions.items()
        if any(_RECORDS_ERROR.search(f.body) for f in definitions)
    }
    while True:
        grown = {
            name
            for name, definitions in functions.items()
            if name not in recording
            and any(
                re.search(r"\b(?:" + "|".join(map(re.escape, recording)) + r")\s*\(", f.body)
                for f in definitions
            )
        }
        if not grown:
            break
        recording |= grown

    report = Report([], 0, {}, len({n for n in safe if n in functions}))
    for definitions in functions.values():
        for function in definitions:
            if not is_entry(function):
                continue
            report.entry_points += 1
            kinds, problems = results[id(function)]
            if function.name in _EXEMPT:
                report.classes["exempt"] = report.classes.get("exempt", 0) + 1
                if not problems:
                    report.findings.append(
                        Finding(
                            function.name,
                            _display(function.path),
                            function.line,
                            "passes on its own; remove it from _EXEMPT",
                        )
                    )
                continue
            for kind in kinds - {"empty"}:
                report.classes[kind] = report.classes.get(kind, 0) + 1
            returns_error = bool(re.match(r"\s*SonareError\b", function.signature))
            records = function.name in recording
            # The diagnostic accessors read the slot this rule clears, so they
            # are the one class that must not clear it.
            accessor = bool(_READS_DIAGNOSTICS.search(function.body)) and not records
            needs_clear = (returns_error or records) and not accessor
            opens_uncleared = any(
                v.strip() and not _CLEAR_ENTRY.match(v) for v in branch_variants(function.body)
            )
            if "realtime" not in kinds and needs_clear and opens_uncleared:
                problems = problems + ["does not open with SONARE_C_API_ENTRY or SONARE_C_TRY"]
            for problem in dict.fromkeys(problems):
                report.findings.append(
                    Finding(function.name, _display(function.path), function.line, problem)
                )
    report.findings.sort(key=lambda f: (f.file, f.line, f.problem))
    return report


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE_DIR)
    parser.add_argument("--header-dir", type=Path, default=DEFAULT_HEADER_DIR)
    parser.add_argument(
        "--floor", type=int, default=0, help="minimum entry points the scan must classify"
    )
    parser.add_argument("--json", action="store_true", help="emit the report as JSON")
    args = parser.parse_args()
    report = audit(args.source_dir, args.header_dir)
    if args.json:
        print(
            json.dumps(
                {
                    "entry_points": report.entry_points,
                    "classes": report.classes,
                    "safe_helpers": report.safe_helpers,
                    "findings": [f.__dict__ for f in report.findings],
                },
                indent=2,
            )
        )
    else:
        print(f"entry points scanned: {report.entry_points}")
        for kind in sorted(report.classes):
            print(f"  {kind}: {report.classes[kind]}")
        print(f"  src/c_api helpers inferred safe: {report.safe_helpers}")
        print(f"  findings: {len(report.findings)}")
        for finding in report.findings:
            print(f"  {finding.as_line()}", file=sys.stderr)
    if report.entry_points < args.floor:
        print(
            f"\nclassified {report.entry_points} entry points, below the floor of "
            f"{args.floor}: the definition pattern stopped matching",
            file=sys.stderr,
        )
        return 2
    return 1 if report.findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
