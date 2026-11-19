"""Classify every ``return SONARE_ERROR_INVALID_PARAMETER;`` in the C ABI and ratchet the untexted ones.

Each return is read with the guard that reaches it: the condition of a braceless ``if``, or of the
``if`` whose block holds it. The condition is split at top-level ``||`` and each disjunct is filed
by what it tests:

- ``null``: a pointer or handle (``!p``, ``p == nullptr``),
- ``count``: a length, size or count against a constant,
- ``relation``: two caller values against each other,
- ``value``: a value against its domain (``isfinite``, a literal or constant bound),
- ``helper``: the verdict of a called helper or a status it returned,
- ``unclassified``: anything the heuristics do not place; every one is listed, never dropped.

A return's class is its disjuncts' set; more than one is ``mixed``. Its text state is ``conforming``
(a null refusal reading exactly ``<param> must not be NULL`` or ``<param> must not be NULL when
<count> is not 0``, no ``<fn>: `` prefix), ``text`` (any other message set on that path),
``helper`` (the condition calls a C-ABI helper that sets the message itself) or ``none``.

The counts in ``c_abi_bare_refusals.json`` are a line: a count above it fails, and one below it
fails until ``--write`` lowers it in the same change. ``--table <path>`` writes the per-file work
order. A scan that reads no return fails, so a moved tree cannot pass as clean.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path(__file__).resolve().parent / "c_abi_bare_refusals.json"
SOURCE_DIR = "src/c_api"
RETURN = "return SONARE_ERROR_INVALID_PARAMETER;"
CLASSES = ("null", "count", "relation", "value", "helper", "unclassified")
TEXT_STATES = ("conforming", "text", "helper", "none")
# The ratcheted lines: refusals with no message, null refusals in a non-ruled form, guards the
# heuristics cannot place.
RATCHET_KEYS = ("no_text", "null_nonconforming", "unclassified")

MESSAGE_CALL = re.compile(r"\b(set_last_error(?:_static)?|refuse\w*|\w+_or_refuse)\s*\(")
CONFORMING_NULL = re.compile(
    r"^[a-z_][a-z0-9_]*(?:->[a-z0-9_]+)* must not be NULL"
    r"(?: when [a-z_][a-z0-9_]*(?:->[a-z0-9_]+)* is not 0)?$"
)
COUNT_WORD = re.compile(
    r"(?:^|_|\b)(?:len|length|size|count|frames|samples|num|n|channels|capacity|bytes)(?:_|\b|$)",
    re.IGNORECASE,
)
BOOL_WORD = re.compile(r"^(?:ok|valid|found|success|enabled|done|has_\w+|is_\w+|\w+_ok|\w+_valid)$")
CONSTANT = re.compile(
    r"^(?:k[A-Z]\w*|[A-Z][A-Z0-9_]+|(?:std::)?numeric_limits(?:<[^>]*>)?::\w+\(\)|[\w:]*::k[A-Z]\w*|sizeof\(.*\))$"
)
LITERAL = re.compile(r"^-?(?:\d[\d.eE+'xXa-fA-FuUlLf]*|true|false)$")
VALUE_CALL = re.compile(r"\bstd::(?:isfinite|isnan|isinf)\s*\(|\b(?:isfinite|isnan|isinf)\s*\(")
CALL = re.compile(r"\b([A-Za-z_][\w:]*)\s*\(")
COMPARE = re.compile(r"(<=|>=|==|!=|(?<![-<>])<(?![<=])|(?<![-<>])>(?![>=]))")


def blank_comments_and_strings(text: str) -> str:
    """``text`` with comments and string/char literal bodies replaced by spaces, offsets kept."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out[i:j] = " " * (j - i)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out[i:j] = [ch if ch == "\n" else " " for ch in text[i:j]]
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out[i + 1 : j] = " " * (j - i - 1)
            i = j + 1
        else:
            i += 1
    return "".join(out)


def match_back(code: str, close: int) -> int:
    """Index of the bracket opening the one at ``close``, or -1."""
    pair = {")": "(", "}": "{", "]": "["}[code[close]]
    depth = 0
    for i in range(close, -1, -1):
        if code[i] == code[close]:
            depth += 1
        elif code[i] == pair:
            depth -= 1
            if depth == 0:
                return i
    return -1


def match_forward(code: str, open_: int) -> int:
    pair = {"(": ")", "{": "}", "[": "]"}[code[open_]]
    depth = 0
    for i in range(open_, len(code)):
        if code[i] == code[open_]:
            depth += 1
        elif code[i] == pair:
            depth -= 1
            if depth == 0:
                return i
    return -1


def enclosing_open_brace(code: str, pos: int) -> int:
    depth = 0
    for i in range(pos - 1, -1, -1):
        if code[i] == "}":
            depth += 1
        elif code[i] == "{":
            if depth == 0:
                return i
            depth -= 1
    return -1


def if_condition_before(code: str, end: int) -> tuple[str, int] | None:
    """The ``if (...)`` condition whose ``)`` is the last token before ``end``, with its start."""
    j = end - 1
    while j >= 0 and code[j].isspace():
        j -= 1
    if j < 0 or code[j] != ")":
        return None
    open_ = match_back(code, j)
    if open_ < 0:
        return None
    head = code[:open_].rstrip()
    if not re.search(r"\bif\s*(?:constexpr\s*)?$", head):
        return None
    return code[open_ + 1 : j], open_


def split_top(expr: str, op: str) -> list[str]:
    parts, depth, start, i = [], 0, 0, 0
    while i < len(expr):
        c = expr[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0 and expr.startswith(op, i):
            parts.append(expr[start:i])
            start = i + len(op)
            i += len(op)
            continue
        i += 1
    parts.append(expr[start:])
    return [p.strip() for p in parts if p.strip()]


def strip_parens(expr: str) -> str:
    expr = expr.strip()
    while expr.startswith("(") and match_forward(expr, 0) == len(expr) - 1:
        expr = expr[1:-1].strip()
    return expr


def is_operand_constant(side: str) -> bool:
    side = strip_parens(side)
    side = re.sub(r"^static_cast(?:<[^>]*>)?\((.*)\)$", r"\1", side).strip()
    side = re.sub(r"^\((.*)\)$", r"\1", side).strip()
    if re.fullmatch(r"[\w:]+\s*[/*+-]\s*[\w:()]+", side) and all(
        is_operand_constant(s) for s in re.split(r"\s*[/*+-]\s*", side)
    ):
        return True
    return bool(LITERAL.match(side) or CONSTANT.match(side) or side in ("nullptr", "NULL"))


def classify_disjunct(part: str, refusing: set[str]) -> str:
    part = strip_parens(
        re.sub(r"\b((?:static|reinterpret)_cast|numeric_limits)<[^<>]*>", r"\1", part)
    )
    negated = re.fullmatch(r"!\s*(\(.*\))", part)
    if negated and match_forward(negated.group(1), 0) == len(negated.group(1)) - 1:
        return classify_disjunct(negated.group(1), refusing)
    conjuncts = split_top(part, "&&")
    if len(conjuncts) > 1:
        kinds = {classify_disjunct(c, refusing) for c in conjuncts}
        if len(kinds) > 1:
            kinds.discard("unclassified")
        for preferred in ("helper", "null", "relation", "value", "count"):
            if preferred in kinds:
                return preferred
        return "unclassified"
    if re.search(r"(?:==|!=)\s*(?:nullptr|NULL)\b|\b(?:nullptr|NULL)\s*(?:==|!=)", part):
        return "null"
    if VALUE_CALL.search(part):
        return "value"
    calls = [m.group(1) for m in CALL.finditer(part)]
    calls = [c for c in calls if c not in ("static_cast", "sizeof", "size", "empty", "data")]
    if any(c.split("::")[-1] in refusing for c in calls):
        return "helper"
    if re.search(r"\bSONARE_OK\b|\bSONARE_ERROR_\w+", part):
        return "helper"
    if re.fullmatch(r"!?\s*[\w.\->\[\]]+\.empty\(\)", part):
        return "count"
    if re.fullmatch(r"[A-Za-z_]\w*", part):
        # A predicate computed above the guard.
        return "value"
    neg = re.fullmatch(r"!\s*([A-Za-z_][\w.\->\[\]]*)", part)
    if neg:
        name = neg.group(1).split("->")[-1].split(".")[-1]
        return "helper" if BOOL_WORD.match(name) else "null"
    sides = COMPARE.split(part)
    if len(sides) == 3:
        left, _, right = sides
        lc, rc = is_operand_constant(left), is_operand_constant(right)
        if lc and rc:
            return "unclassified"
        if not lc and not rc:
            return "relation"
        var = right if lc else left
        if COUNT_WORD.search(var) and not re.search(
            r"\b(?:db|hz|sec|ms|ratio|gain)\b", var, re.IGNORECASE
        ):
            return "count"
        return "value"
    if calls:
        # A pure predicate on the value (finite_non_negative, reserved_entity_id).
        return "value"
    return "unclassified"


def message_literals(region: str) -> list[str]:
    """The string literals inside the message calls of ``region`` (raw source)."""
    out = []
    for m in MESSAGE_CALL.finditer(region):
        open_ = region.find("(", m.start())
        close = match_forward(blank_comments_and_strings(region), open_)
        args = region[open_ + 1 : close if close > 0 else len(region)]
        out.append(args)
    return out


def text_state(classes: set[str], region_raw: str | None, cond_helper: bool) -> str:
    if region_raw is not None:
        calls = message_literals(region_raw)
        if calls:
            if classes == {"null"} and len(calls) == 1:
                lits = re.findall(r'"((?:[^"\\]|\\.)*)"', calls[0])
                concat = re.search(r"\+|std::string|\.c_str\(\)", calls[0])
                if len(lits) == 1 and not concat and CONFORMING_NULL.match(lits[0]):
                    return "conforming"
            return "text"
    return "helper" if cond_helper else "none"


@dataclass
class Refusal:
    file: str
    line: int
    classes: set[str]
    state: str
    guard: str

    @property
    def label(self) -> str:
        return next(iter(self.classes)) if len(self.classes) == 1 else "mixed"


@dataclass
class Scan:
    refusals: list[Refusal] = field(default_factory=list)


def refusing_helpers(texts: dict[str, str]) -> set[str]:
    """C-ABI helpers whose body sets the last-error message itself."""
    names: set[str] = set()
    head = re.compile(r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{")
    for raw in texts.values():
        code = blank_comments_and_strings(raw)
        for m in head.finditer(code):
            if m.group(1) in ("if", "for", "while", "switch", "catch", "return"):
                continue
            open_ = code.find("{", m.end() - 1)
            close = match_forward(code, open_)
            body = code[open_:close]
            if MESSAGE_CALL.search(body) or "throw" in body or "record_current_exception" in body:
                names.add(m.group(1))
    return names


def scan_text(path: str, raw: str, refusing: set[str], scan: Scan) -> None:
    code = blank_comments_and_strings(raw)
    start = 0
    while True:
        pos = code.find(RETURN, start)
        if pos < 0:
            break
        start = pos + len(RETURN)
        line = code.count("\n", 0, pos) + 1
        braceless = if_condition_before(code, pos)
        region_raw: str | None = None
        if braceless:
            cond = braceless[0]
            prev = code[max(0, braceless[1] - 400) : braceless[1]]
        else:
            brace = enclosing_open_brace(code, pos)
            found = if_condition_before(code, brace) if brace >= 0 else None
            region_raw = raw[brace + 1 : pos] if brace >= 0 else None
            if found:
                cond = found[0]
                prev = code[max(0, found[1] - 400) : found[1]]
            else:
                cond = ""
                prev = code[max(0, brace - 400) : brace] if brace >= 0 else ""
        guard = " ".join(cond.split())
        if guard:
            classes = {classify_disjunct(d, refusing) for d in split_top(guard, "||")}
        else:
            classes = {"unclassified"}
        names = set(re.findall(r"[A-Za-z_]\w*", guard))
        assigned_from_helper = any(
            re.search(
                rf"\b{re.escape(n)}\s*=\s*[\w:]*?({'|'.join(map(re.escape, refusing)) or 'x^'})\s*\(",
                prev,
            )
            for n in names
        )
        if assigned_from_helper and classes <= {"null", "value", "count", "unclassified"}:
            classes = {"helper"}
        cond_helper = "helper" in classes
        state = text_state(classes, region_raw, cond_helper)
        scan.refusals.append(Refusal(path, line, classes, state, guard or "(no guard)"))


def scan(root: Path) -> Scan:
    files = sorted(p for p in (root / SOURCE_DIR).rglob("*") if p.suffix in (".cpp", ".h"))
    texts = {p.relative_to(root).as_posix(): p.read_text(encoding="utf-8") for p in files}
    refusing = refusing_helpers(texts)
    result = Scan()
    for path, raw in texts.items():
        scan_text(path, raw, refusing, result)
    return result


def counts(result: Scan) -> dict[str, int]:
    return {
        "no_text": sum(r.state == "none" for r in result.refusals),
        "null_nonconforming": sum(
            r.classes == {"null"} and r.state != "conforming" for r in result.refusals
        ),
        "unclassified": sum("unclassified" in r.classes for r in result.refusals),
    }


def table(result: Scan) -> str:
    rows = result.refusals
    by_label = Counter(r.label for r in rows)
    by_class = Counter(c for r in rows for c in r.classes)
    by_state = Counter(r.state for r in rows)
    lines = [
        "# Bare INVALID_PARAMETER returns in src/c_api",
        "",
        "Generated by `python3 tests/conformance/check_c_abi_bare_refusals.py --table <this file>`; do not edit.",
        "",
        f"Returns read: {len(rows)} over {len({r.file for r in rows})} files.",
        "",
        "## Totals",
        "",
        "| class | returns (single class) | returns testing it (mixed included) |",
        "|---|---:|---:|",
    ]
    for c in CLASSES:
        lines.append(f"| {c} | {by_label.get(c, 0)} | {by_class.get(c, 0)} |")
    lines.append(f"| mixed | {by_label.get('mixed', 0)} | |")
    lines += [
        "",
        "| text state | returns |",
        "|---|---:|",
        *[f"| {s} | {by_state.get(s, 0)} |" for s in TEXT_STATES],
        "",
        "Null refusals by text state (single-class null only): "
        + ", ".join(
            f"{s} {sum(r.classes == {'null'} and r.state == s for r in rows)}" for s in TEXT_STATES
        ),
        "",
        "## Per file",
        "",
        (
            "Columns per class are single-class returns; `null` splits into conforming / other "
            "text / helper / no text. `no text` is the file's untexted total across every class."
        ),
        "",
        "| file | total | null conf | null text | null helper | null none | count | relation | value | helper | mixed | unclassified | no text |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    per_file: dict[str, list[Refusal]] = defaultdict(list)
    for r in rows:
        per_file[r.file].append(r)
    for path in sorted(per_file, key=lambda p: (-len(per_file[p]), p)):
        rs = per_file[path]
        single = Counter(r.label for r in rs)
        null_state = Counter(r.state for r in rs if r.classes == {"null"})
        lines.append(
            f"| {path.removeprefix(SOURCE_DIR + '/')} | {len(rs)} | {null_state.get('conforming', 0)} | "
            f"{null_state.get('text', 0)} | {null_state.get('helper', 0)} | {null_state.get('none', 0)} | "
            f"{single.get('count', 0)} | {single.get('relation', 0)} | {single.get('value', 0)} | "
            f"{single.get('helper', 0)} | {single.get('mixed', 0)} | {single.get('unclassified', 0)} | "
            f"{sum(r.state == 'none' for r in rs)} |"
        )
    lines += ["", "## Unclassified guards", ""]
    for r in rows:
        if "unclassified" in r.classes:
            lines.append(f"- `{r.file}:{r.line}` ({r.state}): `{r.guard[:160]}`")
    lines += ["", "## Mixed guards", ""]
    for r in rows:
        if r.label == "mixed":
            lines.append(
                f"- `{r.file}:{r.line}` {'+'.join(sorted(r.classes))} ({r.state}): `{r.guard[:160]}`"
            )
    return "\n".join(lines) + "\n"


def evaluate(found: dict[str, int], baseline: dict[str, int]) -> list[str]:
    errors = []
    for key in RATCHET_KEYS:
        if key not in baseline:
            errors.append(f"{key}: no recorded line; run with --write")
        elif found[key] > baseline[key]:
            errors.append(f"{key}: {found[key]} is above the recorded line {baseline[key]}")
        elif found[key] < baseline[key]:
            errors.append(
                f"{key}: {found[key]} is below the recorded line {baseline[key]}; lower it "
                f"with --write in the same change"
            )
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--write", action="store_true", help="record the current counts as the line"
    )
    parser.add_argument("--table", type=Path, help="write the per-file work order to this path")
    args = parser.parse_args(argv)
    result = scan(ROOT)
    if not result.refusals:
        print(f"no '{RETURN}' read under {SOURCE_DIR}; the scan reads nothing")
        return 1
    found = counts(result)
    by_label = Counter(r.label for r in result.refusals)
    print(f"bare INVALID_PARAMETER returns: {len(result.refusals)}")
    print("by class: " + ", ".join(f"{c} {by_label.get(c, 0)}" for c in (*CLASSES, "mixed")))
    print("ratchet: " + ", ".join(f"{k} {found[k]}" for k in RATCHET_KEYS))
    if args.table:
        args.table.write_text(table(result), encoding="utf-8")
        print(f"wrote {args.table}")
    if args.write:
        BASELINE.write_text(json.dumps(found, indent=2) + "\n", encoding="utf-8")
        print(f"recorded {BASELINE.name}")
        return 0
    baseline = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else {}
    errors = evaluate(found, baseline)
    for e in errors:
        print(e)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
