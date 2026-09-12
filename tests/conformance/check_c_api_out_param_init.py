#!/usr/bin/env python3
"""Find C-ABI entry points that return before defining an out-parameter.

A ``sonare_*`` entry point that writes an out-parameter on its success path owes
the caller a defined value on every exit path that precedes that write.  A
validation early-return that skips the write hands back an untouched stack slot
while returning an error code, so a defensive C consumer that inspects the
out-parameter after a rejected call reads whatever was there.  No binding reads
these today -- Node zeroes its locals and Python passes zero-initialised ctypes
objects -- which is exactly why the contract erodes without a check.

The scan is over source text and is structured to fail loudly rather than
quietly:

* **Entry points** are definitions whose return type starts at column 0 and
  whose name starts with ``sonare_``.  The body is taken by brace balance from
  the opening brace, never by a fixed character window: a window truncates long
  bodies and undercounts, which is the defect this scan exists to avoid.
* **Out-parameters** are parameters of non-const pointer type.  Function
  pointers and ``const T*`` inputs are excluded structurally, not by name.
* **The first write** is the earliest direct dereference assignment through the
  parameter -- ``*p =``, ``p->field =``, ``p[i] =`` and their compound forms.
* **Early returns** are ``return`` statements before that position at a brace
  depth containing no lambda frame, so a callback's ``return`` is not mistaken
  for an exit from the entry point.
* **A null guard excludes the return.**  A pointer that is NULL cannot be
  written, so a return whose nearest enclosing ``if`` condition null-checks the
  same parameter -- anywhere in a combined condition, not only in its first
  operand -- is not a finding.
* **A single ``*`` to an opaque public type is a receiver, not an
  out-parameter.**  The caller cannot allocate storage for a type the headers
  declare and never define, so such a parameter is the object being operated on;
  a ``sonare_project_*`` entry point writing ``project->field`` is mutating the
  caller's object, not defining a slot.  ``SonareProject**`` stays in the scan.

Known common mode, and the reason the report has an ``unanalysable`` bucket: a
parameter written only through a helper call (``fill_cqt_result(result, out)``)
or through ``memcpy`` has no direct write for the scan to anchor on.  Such a
parameter is reported as unanalysable rather than skipped, because an anchor
that stops matching would otherwise turn every one of its functions clean and
certify a dead scanner.  ``--floor`` pins the number of out-parameters the scan
must resolve at all, separately from the number of findings.

A parameter whose public declaration states it is written only on success is
excluded: that contract belongs in the header, where a caller reads it, and the
phrase is what lets the exclusion be mechanical instead of a list kept here.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE_DIR = ROOT / "src" / "c_api"
DEFAULT_HEADER_DIR = ROOT / "include" / "sonare"

# Comments and literals, longest-context-first at each position, so a `"` inside
# a comment is consumed by the comment alternative and vice versa.  Not a C++
# lexer: it only has to stop code-shaped text inside prose or a message string
# from matching.
_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/
      | R"([^()\\ ]*)\(.*?\)\1"
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)

# A definition's return type begins at column 0 in this tree; the name may sit
# on the same line however long the parameter list runs.
_ENTRY = re.compile(r"^[A-Za-z_][^\n(){};]*?\b(sonare_[A-Za-z0-9_]+)\s*\(", re.MULTILINE)

_IDENTIFIER = re.compile(r"[A-Za-z_]\w*")
_ASSIGN = r"(?:=(?!=)|\+=|-=|\*=|/=|\|=|&=|\^=)"

# A lambda introducer immediately before a brace: a capture list, an optional
# parameter list, and the optional specifier/trailing-return run between them.
_LAMBDA_HEAD = re.compile(
    r"\][^\[\]{};]*$",
)

# Phrases that state an out-parameter is defined only on the success path.  A
# declaration carrying one is excluded from the scan, which is how the
# documented-optional family leaves the report without an entry in this file.
_SUCCESS_ONLY = re.compile(
    r"only on success|on success only|only written on success"
    r"|unmodified on failure|untouched on failure|left untouched unless",
    re.IGNORECASE,
)


def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines."""
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


def strip_comments_and_literals(text: str) -> str:
    """Blank out comments and string/char literals, preserving line structure."""
    return _LEXICAL.sub(_blank, text)


def match_delimiter(text: str, open_at: int) -> int:
    """Index just past the delimiter that closes the one at ``open_at``.

    Raises ``ValueError`` when the text runs out, so an unbalanced body is a
    hard error rather than a silently truncated one.
    """
    pairs = {"(": ")", "{": "}", "[": "]"}
    closer = pairs[text[open_at]]
    opener = text[open_at]
    depth = 0
    for index in range(open_at, len(text)):
        if text[index] == opener:
            depth += 1
        elif text[index] == closer:
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError(f"unbalanced {opener} at offset {open_at}")


def split_top_level(text: str, separator: str = ",") -> list[str]:
    """Split on ``separator`` at nesting depth zero."""
    parts: list[str] = []
    depth = 0
    start = 0
    for index, char in enumerate(text):
        if char in "([{<":
            depth += 1
        elif char in ")]}>":
            depth -= 1
        elif char == separator and depth == 0:
            parts.append(text[start:index])
            start = index + 1
    parts.append(text[start:])
    return [part for part in parts if part.strip()]


@dataclass
class Parameter:
    """One parameter of an entry point, as written."""

    text: str
    name: str
    is_pointer: bool
    is_const_pointee: bool
    is_function_pointer: bool
    pointee: str = ""
    stars: int = 0

    @property
    def is_out_candidate(self) -> bool:
        return self.is_pointer and not self.is_const_pointee and not self.is_function_pointer

    def is_receiver_handle(self, opaque: frozenset[str]) -> bool:
        """Whether this is a handle the caller passes in, not one it receives.

        A single ``*`` to a type the public headers declare but never define is
        storage the caller cannot allocate, so it cannot be a slot the callee
        defines: it is the object being operated on.  Deriving this from the
        header's own incomplete types keeps it structural -- a handle type added
        later is covered without editing a list here, and the moment a type
        gains a definition its parameters re-enter the scan.
        """
        return self.stars == 1 and self.pointee in opaque


def parse_parameter(text: str) -> Parameter | None:
    """Classify one parameter declaration; None when it has no name."""
    stripped = text.strip()
    if not stripped or stripped == "void":
        return None
    is_function_pointer = bool(re.search(r"\(\s*\*\s*\w*\s*\)\s*\(", stripped))
    if is_function_pointer:
        match = re.search(r"\(\s*\*\s*(\w+)\s*\)\s*\(", stripped)
        name = match.group(1) if match else ""
        return Parameter(stripped, name, True, False, True)
    # Array parameters decay to pointers; drop the extent before naming.
    declarator = re.sub(r"\[[^\]]*\]", "*", stripped)
    names = _IDENTIFIER.findall(declarator)
    if not names:
        return None
    name = names[-1]
    if name in {"const", "void", "struct", "unsigned", "signed", "long", "short"}:
        return None
    before_name = declarator[: declarator.rindex(name)]
    is_pointer = "*" in before_name or "*" in declarator[declarator.rindex(name) :]
    # `const` binds to the pointee when it precedes the last `*`.
    last_star = before_name.rfind("*")
    is_const_pointee = bool(
        is_pointer and last_star >= 0 and re.search(r"\bconst\b", before_name[:last_star])
    )
    words = [w for w in _IDENTIFIER.findall(before_name) if w != "const"]
    return Parameter(
        stripped,
        name,
        is_pointer,
        is_const_pointee,
        False,
        pointee=words[-1] if words else "",
        stars=declarator.count("*"),
    )


@dataclass
class EntryPoint:
    """A ``sonare_*`` definition, its parameters, and its body text."""

    name: str
    path: Path
    line: int
    parameters: list[Parameter]
    body: str
    body_offset: int
    text: str = field(repr=False, default="")

    def line_of(self, offset_in_body: int) -> int:
        return self.text.count("\n", 0, self.body_offset + offset_in_body) + 1


def parse_entry_points(path: Path, text: str) -> list[EntryPoint]:
    """Every file-scope ``sonare_*`` definition in one translation unit."""
    code = strip_comments_and_literals(text)
    entries: list[EntryPoint] = []
    for match in _ENTRY.finditer(code):
        open_paren = code.index("(", match.end() - 1)
        try:
            after_params = match_delimiter(code, open_paren)
        except ValueError:
            continue
        tail = code[after_params:]
        head = tail.lstrip()
        # A declaration ends in `;`; only a definition opens a body.  Skip any
        # trailing specifier (`noexcept`, a trailing return type) before it.
        brace_at = head.find("{")
        semi_at = head.find(";")
        if brace_at < 0 or (0 <= semi_at < brace_at):
            continue
        if re.search(r"[;})]", head[:brace_at]):
            continue
        body_open = after_params + (len(tail) - len(head)) + brace_at
        try:
            body_end = match_delimiter(code, body_open)
        except ValueError:
            continue
        params = [parse_parameter(part) for part in split_top_level(code[open_paren + 1 : after_params - 1])]
        entries.append(
            EntryPoint(
                name=match.group(1),
                path=path,
                line=code.count("\n", 0, match.start()) + 1,
                parameters=[p for p in params if p is not None],
                body=code[body_open + 1 : body_end - 1],
                body_offset=body_open + 1,
                text=code,
            )
        )
    return entries


def writes(body: str, name: str) -> list[int]:
    """Offsets of every direct dereference write through ``name``, in order."""
    escaped = re.escape(name)
    patterns = (
        rf"\*\s*{escaped}\s*{_ASSIGN}",
        rf"\b{escaped}\s*->\s*\w+\s*(?:\[[^\]]*\]\s*)?{_ASSIGN}",
        rf"\b{escaped}\s*\[[^\]]*\]\s*{_ASSIGN}",
    )
    return sorted(m.start() for p in patterns for m in re.finditer(p, body))


def first_read(body: str, name: str) -> int | None:
    """Offset of the earliest dereference of ``name`` that consumes its value.

    A null test is not a read: ``if (!p)`` inspects the pointer, not the object.
    This is what separates an out-parameter from a handle the entry point
    mutates in place -- a handle is read before it is written, an out-parameter
    is written before it is read -- without a list of handle type names.
    """
    escaped = re.escape(name)
    patterns = (
        rf"\*\s*{escaped}\s*(?!{_ASSIGN})",
        rf"\b{escaped}\s*->\s*\w+\s*(?:\[[^\]]*\]\s*)?(?!{_ASSIGN})",
        rf"\b{escaped}\s*\[[^\]]*\]\s*(?!{_ASSIGN})",
    )
    offsets = [m.start() for p in patterns for m in re.finditer(p, body)]
    return min(offsets) if offsets else None


def indirect_write(body: str, name: str) -> bool:
    """Whether ``name`` is passed somewhere that could write through it."""
    escaped = re.escape(name)
    return bool(re.search(rf"\w\s*\([^()]*?\b{escaped}\b", body))


_OPAQUE_TYPEDEF = re.compile(r"typedef\s+struct\s+(\w+)\s+\1\s*;")
# Both definition spellings: the trailing typedef name, and a named struct body.
_CLOSING_TAG = re.compile(r"\}\s*(\w+)\s*;")
_NAMED_STRUCT_BODY = re.compile(r"\bstruct\s+(\w+)\s*\{")


def opaque_handle_types(header_dir: Path) -> frozenset[str]:
    """Public types declared as incomplete and never defined."""
    declared: set[str] = set()
    defined: set[str] = set()
    for path in sorted(header_dir.glob("*.h")):
        text = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
        declared.update(match.group(1) for match in _OPAQUE_TYPEDEF.finditer(text))
        defined.update(match.group(1) for match in _CLOSING_TAG.finditer(text))
        defined.update(match.group(1) for match in _NAMED_STRUCT_BODY.finditer(text))
    return frozenset(declared - defined)


_DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", re.MULTILINE)


def unconditional_return_macros(paths: list[Path]) -> set[str]:
    """Macro names whose expansion always returns from the enclosing function.

    The feature-disabled stub is written as a macro, so a scan that only looks
    for the ``return`` keyword sees a gated entry point as having no exit at all
    in its disabled branch.  A macro qualifies when its body returns and carries
    neither ``catch`` nor ``if``: that keeps the exception-translation macro,
    whose returns are all on catch arms, out of the set.
    """
    found: set[str] = set()
    for path in paths:
        text = strip_comments_and_literals(
            path.read_text(encoding="utf-8", errors="replace")
        )
        lines = text.split("\n")
        index = 0
        while index < len(lines):
            match = _DEFINE.match(lines[index])
            if match is None:
                index += 1
                continue
            body = [lines[index]]
            while body[-1].rstrip().endswith("\\") and index + 1 < len(lines):
                index += 1
                body.append(lines[index])
            joined = "\n".join(body)
            if re.search(r"\breturn\b", joined) and not re.search(
                r"\b(?:catch|if)\b", joined
            ):
                found.add(match.group(1))
            index += 1
    return found


_DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b", re.MULTILINE)


class BranchMap:
    """Which preprocessor branch each offset of a body belongs to.

    Two offsets are comparable only when one's branch path is a prefix of the
    other's.  Without this the scan reads an ``#if`` and its ``#else`` as one
    straight-line body, so a write in the feature-on branch is taken to cover a
    return in the feature-off stub -- which is the exact shape the invariant
    names as a defect, silently absorbed as clean.
    """

    def __init__(self, body: str) -> None:
        self._marks: list[tuple[int, tuple[tuple[int, int], ...]]] = [(0, ())]
        stack: list[list[int]] = []
        next_id = 0
        for match in _DIRECTIVE.finditer(body):
            kind = match.group(1)
            if kind in {"if", "ifdef", "ifndef"}:
                stack.append([next_id, 0])
                next_id += 1
            elif kind in {"elif", "else"}:
                if stack:
                    stack[-1][1] += 1
            elif kind == "endif":
                if stack:
                    stack.pop()
            self._marks.append(
                (match.end(), tuple((cond, branch) for cond, branch in stack))
            )

    def path(self, offset: int) -> tuple[tuple[int, int], ...]:
        found: tuple[tuple[int, int], ...] = ()
        for mark_at, path in self._marks:
            if mark_at > offset:
                break
            found = path
        return found

    def comparable(self, left: int, right: int) -> bool:
        """Whether two offsets are on the same path through the conditionals."""
        a, b = self.path(left), self.path(right)
        return all(x == y for x, y in zip(a, b))

    @property
    def has_conditionals(self) -> bool:
        return len(self._marks) > 1


def statement_end(body: str, start: int) -> int:
    """Offset just past the ``;`` that ends the statement beginning at ``start``.

    Depth-aware, because a tail ``return`` whose expression carries a lambda
    contains semicolons of its own; stopping at the first one would place the
    statement's end inside the lambda and turn a tail return into an apparent
    early return.
    """
    depth = 0
    for index in range(start, len(body)):
        char = body[index]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == ";" and depth == 0:
            return index + 1
    return len(body)


@dataclass
class Frame:
    """One open brace in a body, and whether it introduces a lambda."""

    open_at: int
    is_lambda: bool


def _frames_at(body: str, offset: int) -> list[Frame]:
    """The brace frames enclosing ``offset``, outermost first."""
    stack: list[Frame] = []
    for index, char in enumerate(body[:offset]):
        if char == "{":
            head = body[:index].rstrip()
            stack.append(Frame(index, bool(_LAMBDA_HEAD.search(head))))
        elif char == "}" and stack:
            stack.pop()
    return stack


def _enclosing_condition(body: str, return_at: int) -> str:
    """The nearest ``if`` condition governing the return, or the empty string."""
    frames = _frames_at(body, return_at)
    # `if (cond) { ... return ...; }`
    if frames:
        head = body[: frames[-1].open_at].rstrip()
        if head.endswith(")"):
            open_paren = _matching_open(head, len(head) - 1)
            if open_paren is not None and re.search(r"\bif\s*$", head[:open_paren]):
                return head[open_paren + 1 : -1]
    # `if (cond) return ...;` on one statement.
    head = body[:return_at].rstrip()
    if head.endswith(")"):
        open_paren = _matching_open(head, len(head) - 1)
        if open_paren is not None and re.search(r"\bif\s*$", head[:open_paren]):
            return head[open_paren + 1 : -1]
    return ""


def _matching_open(text: str, close_at: int) -> int | None:
    depth = 0
    for index in range(close_at, -1, -1):
        if text[index] == ")":
            depth += 1
        elif text[index] == "(":
            depth -= 1
            if depth == 0:
                return index
    return None


def null_guards(condition: str, name: str) -> bool:
    """Whether ``condition`` null-checks ``name`` anywhere inside it.

    Anywhere, not only in the first operand: a combined condition that
    null-checks the parameter in its third disjunct guards it just as well, and
    a scan that reads only the leading operand reports every one of those.
    A bare ``if (p)`` counts -- it is the idiom for an optional out-parameter.
    """
    escaped = re.escape(name)
    if any(
        re.search(pattern, condition)
        for pattern in (
            rf"!\s*{escaped}\b",
            rf"\b{escaped}\s*(?:==|!=)\s*(?:nullptr|NULL|0)\b",
            rf"\*\s*{escaped}\s*(?:==|!=)\s*(?:nullptr|NULL|0)\b",
        )
    ):
        return True
    operands = re.split(r"&&|\|\|", condition)
    return any(operand.strip().strip("()").strip() == name for operand in operands)


@dataclass
class Finding:
    """One return that reaches the caller with an out-parameter undefined."""

    kind: str
    optional: bool
    function: str
    parameter: str
    file: str
    return_line: int
    write_line: int | None
    returned: str
    condition: str

    def as_line(self) -> str:
        where = f"{self.file}:{self.return_line}"
        tail = (
            f"before the write at line {self.write_line}"
            if self.write_line is not None
            else "in a preprocessor branch that never writes it"
        )
        mark = "/optional" if self.optional else ""
        return (
            f"[{self.kind}{mark}] {where}  {self.function}({self.parameter})  "
            f"returns {self.returned!r} {tail}"
        )


@dataclass
class ScanResult:
    """What one entry point contributed to the report."""

    findings: list[Finding]
    unanalysable: list[str]
    resolved: int = 0
    in_place: int = 0
    receivers: int = 0


def scan_entry_point(
    entry: EntryPoint,
    success_only: set[tuple[str, str]],
    return_macros: frozenset[str] = frozenset(),
    opaque: frozenset[str] = frozenset(),
) -> ScanResult:
    """Classify every out-parameter candidate of one entry point."""
    result = ScanResult(findings=[], unanalysable=[])
    rel = _display(entry.path)
    branches = BranchMap(entry.body)
    exits = r"\breturn\b"
    if return_macros:
        exits += r"|\b(?:" + "|".join(sorted(return_macros)) + r")\b"
    returns = [
        match.start()
        for match in re.finditer(exits, entry.body)
        if not any(frame.is_lambda for frame in _frames_at(entry.body, match.start()))
    ]
    for param in entry.parameters:
        if not param.is_out_candidate:
            continue
        if param.is_receiver_handle(opaque):
            result.receivers += 1
            continue
        if (entry.name, param.name) in success_only:
            continue
        write_offsets = writes(entry.body, param.name)
        if not write_offsets:
            if indirect_write(entry.body, param.name):
                result.unanalysable.append(
                    f"{rel}:{entry.line}  {entry.name}({param.name}) "
                    "is only ever written through a call, so its exit paths are unchecked"
                )
            continue
        read_at = first_read(entry.body, param.name)
        if read_at is not None and read_at < write_offsets[0]:
            # Read before written: the entry point mutates an object the caller
            # already owns, so there is nothing to define on an early return.
            result.in_place += 1
            continue
        result.resolved += 1
        # An out-parameter the body itself guards with a null check before
        # writing is one the caller may omit, so its success-only contract is
        # a documentation question rather than a missing zero-write.  Marked
        # rather than dropped: the judgement is the header's to make.
        write_condition = _enclosing_condition(entry.body, write_offsets[0])
        optional = any(
            null_guards(write_condition, other.name)
            for other in entry.parameters
            if other.is_out_candidate
        )
        for return_at in returns:
            reachable = [w for w in write_offsets if branches.comparable(return_at, w)]
            if not reachable:
                # The whole branch this return sits in never writes the
                # parameter, while another branch does: the feature-disabled
                # stub shape.
                kind, write_at = "feature-disabled", None
            elif return_at < reachable[0]:
                kind, write_at = "validation", reachable[0]
            else:
                continue
            end = statement_end(entry.body, return_at)
            if write_at is not None and end > write_at:
                # The write happens inside this return's own expression -- a
                # lambda it constructs -- so the return is the success path.
                continue
            condition = _enclosing_condition(entry.body, return_at)
            if null_guards(condition, param.name):
                continue
            result.findings.append(
                Finding(
                    kind=kind,
                    optional=optional,
                    function=entry.name,
                    parameter=param.name,
                    file=rel,
                    return_line=entry.line_of(return_at),
                    write_line=entry.line_of(write_at) if write_at is not None else None,
                    returned=" ".join(entry.body[return_at : end - 1].split())[:120],
                    condition=" ".join(condition.split()),
                )
            )
    return result


# A ``@param`` body runs to the next block command, not to the next ``@``: an
# inline ``@p`` reference is part of the prose, and splitting on it truncates
# the body before the sentence that carries the contract.
_BLOCK_COMMAND = re.compile(
    r"@(?:param|return|retval|brief|details|note|see|warning|throws|pre|post)\b"
)
_PARAM_TAG = re.compile(r"@param(?:\s*\[[^\]]*\])?\s+(\w+)")


def _param_bodies(doc: str) -> list[tuple[str, str]]:
    """Each ``@param`` name paired with its prose, inline ``@p`` refs included."""
    found: list[tuple[str, str]] = []
    for match in _PARAM_TAG.finditer(doc):
        rest = doc[match.end() :]
        stop = _BLOCK_COMMAND.search(rest)
        found.append((match.group(1), rest[: stop.start()] if stop else rest))
    return found


def success_only_parameters(header_dir: Path) -> set[tuple[str, str]]:
    """``(function, parameter)`` pairs the public headers declare success-only."""
    declared: set[tuple[str, str]] = set()
    for path in sorted(header_dir.glob("*.h")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"\bsonare_[A-Za-z0-9_]+\s*\(", text):
            name = match.group(0).split("(")[0].strip()
            doc = _doc_block_before(text, match.start())
            if not doc:
                continue
            for param_name, body in _param_bodies(doc):
                if _SUCCESS_ONLY.search(body):
                    declared.add((name, param_name))
    return declared


def _doc_block_before(text: str, offset: int) -> str:
    """The contiguous comment block immediately above a declaration."""
    lines = text[:offset].split("\n")
    collected: list[str] = []
    for line in reversed(lines[:-1]):
        stripped = line.strip()
        if stripped.startswith(("///", "//", "*", "/*", "*/")) or stripped.endswith("*/"):
            collected.append(stripped)
            continue
        if not stripped and collected:
            continue
        break
    return "\n".join(reversed(collected))


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


@dataclass
class Report:
    """The whole scan, findings and population counts together."""

    findings: list[Finding]
    unanalysable: list[str]
    entry_points: int
    resolved: int
    in_place: int
    receivers: int = 0


def audit(source_dir: Path, header_dir: Path) -> Report:
    """Scan every translation unit under ``source_dir``."""
    success_only = success_only_parameters(header_dir) if header_dir.is_dir() else set()
    opaque = opaque_handle_types(header_dir) if header_dir.is_dir() else frozenset()
    return_macros = frozenset(
        unconditional_return_macros(
            sorted(source_dir.glob("*.h")) + sorted(header_dir.glob("*.h"))
            if header_dir.is_dir()
            else sorted(source_dir.glob("*.h"))
        )
    )
    report = Report([], [], 0, 0, 0)
    for path in sorted(source_dir.glob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for entry in parse_entry_points(path, text):
            report.entry_points += 1
            result = scan_entry_point(entry, success_only, return_macros, opaque)
            report.findings.extend(result.findings)
            report.unanalysable.extend(result.unanalysable)
            report.resolved += result.resolved
            report.in_place += result.in_place
            report.receivers += result.receivers
    report.findings.sort(key=lambda f: (f.kind, f.file, f.return_line, f.parameter))
    report.unanalysable.sort()
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE_DIR)
    parser.add_argument("--header-dir", type=Path, default=DEFAULT_HEADER_DIR)
    parser.add_argument(
        "--floor",
        type=int,
        default=0,
        help="minimum out-parameters the scan must resolve; guards against a "
        "pattern that stopped matching reporting a clean tree",
    )
    parser.add_argument("--json", action="store_true", help="emit findings as JSON")
    parser.add_argument(
        "--quiet-findings",
        action="store_true",
        help="report counts only, for use while the population is being triaged",
    )
    args = parser.parse_args()

    report = audit(args.source_dir, args.header_dir)

    if args.json:
        print(
            json.dumps(
                {
                    "entry_points": report.entry_points,
                    "resolved_out_parameters": report.resolved,
                    "mutated_in_place": report.in_place,
                    "receiver_handles": report.receivers,
                    "unanalysable": report.unanalysable,
                    "findings": [f.__dict__ for f in report.findings],
                },
                indent=2,
            )
        )
    else:
        print(f"entry points scanned: {report.entry_points}")
        print(f"  out-parameters with a resolvable first write: {report.resolved}")
        print(f"  pointers read before written, so mutated in place: {report.in_place}")
        print(f"  opaque handles the caller passes in, not slots it receives: {report.receivers}")
        print(f"  out-parameters written only through a call: {len(report.unanalysable)}")
        print(f"  returns preceding a first write: {len(report.findings)}")
        if report.findings and not args.quiet_findings:
            print(
                "\nThese returns reach the caller before the out-parameter is defined:",
                *(f"  {finding.as_line()}" for finding in report.findings),
                sep="\n",
                file=sys.stderr,
            )

    if report.resolved < args.floor:
        print(
            f"\nresolved {report.resolved} out-parameters, below the floor of "
            f"{args.floor}: the write or parameter pattern stopped matching, so a "
            "clean report here would certify nothing",
            file=sys.stderr,
        )
        return 2
    return 1 if report.findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
