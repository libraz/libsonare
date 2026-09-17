"""Keep every JS-to-C++ numeric narrowing in ``src/wasm`` accounted for.

Three populations, all enforced here, all drifting for the same reason -- a new
translation unit can convert a JS value however it likes and nothing notices:

* **File-local readers.**  A function taking an ``emscripten::val`` and returning
  a numeric type is a reader.  Readers belong in ``bindings/common/``; a second
  copy elsewhere means a fix to the conversion contract reaches some call sites
  and not others.  The Node addon has this rule already, enforced by its own
  test; this is the WASM half.
* **Inline narrowings.**  ``val::as<int>()`` SATURATES out of range, so every
  value past the maximum arrives as ``INT_MAX`` -- a plausible in-domain number
  that any positivity guard waves through.  A narrowing therefore has to go
  through the shared range-checked reader or be recorded with the mechanism that
  makes it harmless.
* **Positional embind parameters.**  A parameter DECLARED as a narrow integer is
  converted by embind's own glue, not by any code in this tree, and that glue
  WRAPS: ``__embind_register_integer`` installs ``toWireType: value => value``,
  so the JS number reaches the wasm i32 parameter through ToInt32 and 2^32 + 5
  arrives as 5.  The two paths therefore disagree about the same C++ type --
  ``val::as<uint32_t>()`` on an object field saturates to ``UINT32_MAX``, the
  positional spelling wraps -- and the wrapping half is the one no downstream
  guard can see, because a wrapped value is in the guard's domain by
  construction.  A site here is recorded with its status: ``benign`` states a
  mechanism, ``open`` states that a wrapped value is accepted and selects
  something different and that nobody has fixed it yet.

TWO CONVERSIONS DELIBERATELY OUTSIDE THE POSITIONAL POPULATION
--------------------------------------------------------------
Both were read out of the shipped glue in ``bindings/wasm/dist/sonare.js``
rather than assumed, because each is a different registration function with a
different contract:

* ``bool`` -- ``toWireType: o => o ? trueValue : falseValue``.  Neither wraps nor
  saturates; every JS value is a legal argument, so there is no number the
  conversion silently changes.
* ``float`` -- ``toWireType: value => value``, and the f32 demotion turns an
  overflow into Infinity rather than into a plausible in-domain number.  That is
  the saturating defect, not the wrapping one, so the float parameters are their
  own population with their own section below, not a silent exclusion.

WHAT THE FLOAT SECTION DOES AND DOES NOT ASSERT
-----------------------------------------------
Read this before reading the float section's count as coverage.  A float
positional parameter cannot be routed through a checked reader while it is still
DECLARED ``float``: embind's glue converts it before any code in this tree runs,
so routing means changing the parameter to a ``val`` -- which removes it from
this population entirely.  What remains here is therefore the ungoverned set by
construction, and whether a member of it is safe is decided almost everywhere
OUTSIDE ``src/wasm``: by a core validator or by the C ABI's ``finite()``, several
call hops away and through a config struct.  A text scan over this tree sees
``config.fmin = fmin;`` and can conclude nothing from it.

So the float section asserts two things and no third:

* the collection still matches -- a pinned floor, plus an exact per-file
  ``distribution``.  A floor can only see the population DRAINING; it cannot see
  a parameter land.  The distribution can, in either direction, and it fails
  naming the file, so a float parameter cannot arrive without someone triaging
  it in the same change.  Most ledgers in this tree carry only the floor.
* every parameter that is knowingly NOT refused carries a ``passthrough`` record
  with a reason and a mechanism citation this file reads back.

It does NOT assert that the unrecorded remainder is refused.  Nothing here can:
that verdict comes from driving each entry point with the value the f32 parameter
actually holds, which is ``tests/conformance/wasm_float_saturation_test.cpp``
natively and the WASM test suite for the guards that live in this tree.  A green
float section is silent about all of it.

The records live in ``wasm_narrowing_records.json`` and are read as data.  A
record that matches nothing is itself an error: a stale one keeps asserting a
reviewed decision about a site that no longer exists, and the next narrowing to
land on that spelling inherits the blessing unexamined.

TWO SCANS, BY DIFFERENT ROUTES, WHOSE DISAGREEMENT IS AN ASSERTION
------------------------------------------------------------------
A blind spot in a regex is invisible from inside the regex, so re-reading it
proves nothing; only a second measurement by another route disagrees.  The
population is therefore counted twice and the disagreement is an error, not a
log line:

* **Scan A, top-down over declarations.**  Every parameter list mentioning
  ``val`` that opens a body, named function and lambda alike.  The body's extent
  comes from brace balance, never from a fixed character window -- a window stops
  inside the first long entry point and everything past it goes uncounted.  Each
  container's own count is taken by re-reading its body text with directly
  nested container bodies blanked out.
* **Scan B, bottom-up over expressions.**  Every cast match by absolute offset,
  assigned to the innermost container span that contains it.

Neither route can produce the other's answer by construction, so:

* every cast has a container -- an orphan means the declaration pattern is too
  narrow to see the function the cast sits in;
* the two per-container counts reconcile -- a disagreement means a body span was
  mis-measured;
* the scan finds a known-nonzero population.  Two empty sets agree perfectly:
  with the cast pattern made to match nothing, both checks above go green and
  certify a scanner that has stopped working, so a floor is pinned.

A floor sized to a population the work is meant to REMOVE stops being a check on
the scanner and becomes a check on the work not happening.  The positional
narrow-integer population was converted to ``val`` reads, so ``parameters`` /
``functions`` / ``files`` now count only what remains by design: the 64-bit
family, which refuses rather than wraps, and the constructors, whose parameter
types are spelled in the registration.  The evidence that the scan still matches
therefore rests on ``registrations`` and ``declarations``, which are two orders
of magnitude larger and are untouched by any conversion, and on the injection
ablation -- one added embind function taking one narrow integer parameter is
reported as ungraded no matter how small the graded population has become.

The positional population is counted twice on the same principle, by the two
directions of one relation:

* **Route R, registration-first.**  Each ``function("js", &sym)`` is resolved to
  the declaration of ``sym`` and its parameter list is read.
* **Route D, declaration-first.**  Every declaration in the tree carrying a
  narrow integer parameter is enumerated, then kept if its name is registered.

The two produce the same set only when resolution is a bijection, so their
disagreement names exactly the failure that matters: a symbol whose declarations
do not agree on a signature -- an ``_ex`` overload family, a header and a
definition that have drifted -- resolves under route R to one of them while
route D yields both.  A registration whose symbol resolves to nothing is an
orphan and is reported on its own, because route D cannot see it at all.

KNOWN COMMON MODE
-----------------
Both scans classify a cast through the one type list below.  A narrowing whose
type is spelled outside that list -- an alias, a typedef, a template parameter --
is invisible to both scans AND to their agreement, so the reconciliation cannot
detect it.  Widening the list is the only remedy; nothing here will report it.
The same is true of the declaration side for a parameter type that is an alias
for ``val``, and of the positional routes for a parameter whose declared type is
an alias for a narrow integer: both routes read the same spelling, so they agree
on missing it.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WASM_TREE = ROOT / "src" / "wasm"
RECORDS = Path(__file__).resolve().parent / "wasm_narrowing_records.json"

# Everything keyed on a path is keyed relative to the tree actually scanned, so
# that --tree reaches a copy. Anchoring on WASM_TREE instead made the flag a
# crash rather than a seam, and a scan whose sources sit elsewhere cannot tell a
# shared reader from a binding.
SHARED_READER_SUBDIR = Path("bindings") / "common"

_SOURCE_SUFFIXES = (".cpp", ".cc", ".h", ".hpp")

# The common mode above lives here. Spelled out rather than matched loosely,
# because a loose pattern would sweep `as<float>` and `as<std::string>` into a
# population that is about integer saturation.
INTEGER_TYPES = (
    "int",
    "unsigned",
    "unsigned int",
    "short",
    "unsigned short",
    "long",
    "unsigned long",
    "long long",
    "unsigned long long",
    "size_t",
    "std::size_t",
    "ssize_t",
    "ptrdiff_t",
    "int8_t",
    "uint8_t",
    "int16_t",
    "uint16_t",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
    "std::int8_t",
    "std::uint8_t",
    "std::int16_t",
    "std::uint16_t",
    "std::int32_t",
    "std::uint32_t",
    "std::int64_t",
    "std::uint64_t",
)

# Return types that make a val-taking function a reader. `bool` is deliberately
# out: a boolean read cannot saturate, so a file-local one duplicates a helper
# without duplicating the defect this exists for.
NUMERIC_RETURNS = INTEGER_TYPES + ("float", "double", "long double")

# An embind registration with a JS name. A function reached this way is an entry
# point the module exports, not a reader -- structurally the two are identical
# (both take a val and return a number) and only the registration tells them
# apart.
_EXPORTED = re.compile(r"\bfunction\s*\(\s*\"[^\"]*\"\s*,\s*&\s*([\w:]+)")

# The same registrations read for their JS name and their C++ symbol together,
# across every form embind offers a name and an address.
_REGISTRATION = re.compile(
    r"\.?\s*\b(function|class_function|property)\s*\(\s*\"([^\"]+)\"\s*,\s*&\s*([\w:]+)"
)

# `constructor<T...>()` names its parameter types in place rather than pointing
# at a declaration, so it is matched separately and carries no symbol.
_CONSTRUCTOR = re.compile(r"\.?\s*\bconstructor\s*<([^>]*)>\s*\(\s*\)")

# Types converted positionally by an integer rule. `float` and `bool` are outside
# this population by mechanism, for the reasons in the module docstring.
POSITIONAL_TYPES = frozenset(INTEGER_TYPES) | {"char", "unsigned char", "signed char"}

# The subset embind registers through the bigint glue rather than the integer
# glue, which is a different conversion and therefore a different defect.
_BIGINT_TYPES = frozenset(
    {
        "int64_t",
        "uint64_t",
        "std::int64_t",
        "std::uint64_t",
        "long long",
        "unsigned long long",
    }
)

# A float parameter shaped like a count or an index is the one float the WRAPPING
# question reaches, and there is none today -- so the pattern is a trip-wire
# rather than a classifier.
_COUNT_SHAPED = re.compile(r"^(?:n|num|count)_|_(?:count|index|frames|samples)$|^n$|_n$")

_TYPE_ALTERNATION = "|".join(
    re.escape(name) for name in sorted(INTEGER_TYPES, key=len, reverse=True)
)
_CAST = re.compile(rf"\.\s*as\s*<\s*({_TYPE_ALTERNATION})\s*>\s*\(\s*\)")

# The qualified-name alternative is load-bearing: three headers spell the
# parameter `emscripten::val`, and without it every container in them disappears
# and their casts read as orphans.
_VAL_PARAM = re.compile(r"\b(?:emscripten\s*::\s*)?val\b")

# Trailing specifiers between a parameter list and the body it opens.
_AFTER_PARAMS = re.compile(
    r"(?:\s|const\b|noexcept\b|override\b|final\b|mutable\b|->|[\w:<>,&*\[\]()])*"
)

_LEXICAL = re.compile(
    r"""//[^\n]*
      | /\*.*?\*/""",
    re.DOTALL | re.VERBOSE,
)
_LITERAL = re.compile(
    r"""R"([^()\\ ]*)\(.*?\)\1"
      | "(?:[^"\\\n]|\\.)*"
      | '(?:[^'\\\n]|\\.)*'""",
    re.DOTALL | re.VERBOSE,
)


def _blank(match: re.Match[str]) -> str:
    """Replace a matched span with spaces, keeping its newlines and its length."""
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


def prepare(text: str) -> tuple[str, str]:
    """Return (text without comments, that text with literals blanked too).

    Both are the same length as the input, so an offset means the same position
    in either.  Structure -- brace balance, receiver extraction -- is read from
    the second, since a brace or a bracket inside a string literal would break
    it; the readable receiver text comes from the first.
    """
    readable = _LEXICAL.sub(_blank, text)
    return readable, _LITERAL.sub(_blank, readable)


def _relative(path: Path, tree: Path) -> str:
    """A record key: the path under the scanned tree, falling back to the repo."""
    try:
        return str(path.relative_to(tree))
    except ValueError:
        return _display(path)


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _match_forward(code: str, start: int, opener: str, closer: str) -> int:
    """Index just past the delimiter matching the one at ``start``, or -1."""
    depth = 0
    for i in range(start, len(code)):
        if code[i] == opener:
            depth += 1
        elif code[i] == closer:
            depth -= 1
            if depth == 0:
                return i + 1
    return -1


def _match_backward(code: str, end: int, opener: str, closer: str) -> int:
    """Index of the delimiter matching the closer at ``end``, or -1."""
    depth = 0
    for i in range(end, -1, -1):
        if code[i] == closer:
            depth += 1
        elif code[i] == opener:
            depth -= 1
            if depth == 0:
                return i
    return -1


def receiver_of(readable: str, code: str, dot: int) -> str:
    """The expression text a cast at ``dot`` is taken on.

    Walked backwards over subscripts, calls and member accesses so the key a
    field is read under survives into the record -- ``options["nFft"]`` rather
    than ``options``.
    """
    i = dot
    while i > 0:
        j = i - 1
        while j >= 0 and code[j] in " \t\n":
            j -= 1
        if j < 0:
            break
        if code[j] in "])":
            opener = "[" if code[j] == "]" else "("
            closer = code[j]
            start = _match_backward(code, j, opener, closer)
            if start < 0:
                break
            i = start
            continue
        if code[j].isalnum() or code[j] in "_":
            while j >= 0 and (code[j].isalnum() or code[j] == "_"):
                j -= 1
            i = j + 1
            # A member access or a qualification continues the expression.
            k = i - 1
            while k >= 0 and code[k] in " \t\n":
                k -= 1
            if k >= 0 and (code[k] == "." or (k > 0 and code[k - 1 : k + 1] in ("::", "->"))):
                i = k + 1 if code[k] == "." else k
                continue
            break
        break
    return " ".join(readable[i:dot].split())


class Container:
    """One val-taking function or lambda body, located by brace balance."""

    def __init__(self, path: Path, name: str, returns: str, body: tuple[int, int]) -> None:
        self.path = path
        self.name = name
        self.returns = returns
        self.start, self.end = body

    @property
    def is_lambda(self) -> bool:
        return self.name == "<lambda>"

    def contains(self, offset: int) -> bool:
        return self.start <= offset < self.end


def _declaration_before(readable: str, code: str, open_paren: int) -> tuple[str, str]:
    """The (name, return type) preceding a parameter list, or a lambda marker."""
    j = open_paren - 1
    while j >= 0 and code[j] in " \t\n":
        j -= 1
    if j >= 0 and code[j] == "]":
        return "<lambda>", ""
    end = j + 1
    while j >= 0 and (code[j].isalnum() or code[j] == "_"):
        j -= 1
    name = readable[j + 1 : end].strip()
    if not name:
        return "<lambda>", ""
    line_start = code.rfind("\n", 0, j + 1) + 1
    prefix = readable[line_start : j + 1]
    # `inline`, `static` and the like are not the return type; the last type-ish
    # token before the name is.
    tokens = [t for t in re.split(r"[\s*&]+", prefix.strip()) if t]
    skip = {"inline", "static", "constexpr", "extern", "virtual", "explicit", "template"}
    returns = next((t for t in reversed(tokens) if t not in skip), "")
    return name, returns


def containers_of(path: Path, readable: str, code: str) -> list[Container]:
    """Scan A: every val-taking body in one file, by brace balance."""
    found: list[Container] = []
    for open_paren in (i for i, ch in enumerate(code) if ch == "("):
        close = _match_forward(code, open_paren, "(", ")")
        if close < 0:
            continue
        if not _VAL_PARAM.search(code[open_paren:close]):
            continue
        tail = _AFTER_PARAMS.match(code, close)
        brace = tail.end() if tail else close
        if brace >= len(code) or code[brace] != "{":
            continue
        body_end = _match_forward(code, brace, "{", "}")
        if body_end < 0:
            continue
        name, returns = _declaration_before(readable, code, open_paren)
        found.append(Container(path, name, returns, (brace, body_end)))
    return found


class Site:
    """One narrowing, with everything a record is keyed on."""

    def __init__(
        self, path: Path, offset: int, line: int, cast_type: str, receiver: str, tree: Path
    ) -> None:
        self.path = path
        self.tree = tree
        self.offset = offset
        self.line = line
        self.type = cast_type
        self.receiver = receiver
        self.container: Container | None = None

    @property
    def display(self) -> str:
        return f"{_display(self.path)}:{self.line}"

    @property
    def key(self) -> tuple[str, str, str]:
        return (_relative(self.path, self.tree), self.receiver, self.type)


class Scan:
    """Both scans over the whole tree, plus what their disagreement says."""

    def __init__(self, tree: Path = WASM_TREE, *, body_window: int | None = None) -> None:
        self.tree = tree
        # A fixed window instead of brace balance -- kept as an option so the
        # cross-check's power can be demonstrated rather than argued.
        self.body_window = body_window
        self.containers: list[Container] = []
        self.sites: list[Site] = []
        self.readers: list[Container] = []
        self.exported: set[str] = set()
        self.declared_counts: dict[int, int] = {}
        self._run()

    def _files(self) -> list[Path]:
        return sorted(
            p
            for p in self.tree.rglob("*")
            if p.suffix in _SOURCE_SUFFIXES and p.is_file()
        )

    def _run(self) -> None:
        prepared = {}
        for path in self._files():
            prepared[path] = prepare(path.read_text(encoding="utf-8", errors="replace"))
            self.exported.update(
                name.rsplit("::", 1)[-1] for name in _EXPORTED.findall(prepared[path][0])
            )
        for path, (readable, code) in prepared.items():
            containers = containers_of(path, readable, code)
            self.containers.extend(containers)
            for container in containers:
                if (
                    not container.is_lambda
                    and container.returns in NUMERIC_RETURNS
                    and container.name not in self.exported
                ):
                    self.readers.append(container)

            # Scan A: each container's own count, taken by re-reading its body
            # text with directly nested container bodies blanked.
            for container in containers:
                read_to = container.end
                if self.body_window is not None:
                    read_to = min(container.start + self.body_window, container.end)
                body = list(code[container.start : read_to])
                for other in containers:
                    if other is container or not container.contains(other.start):
                        continue
                    if any(
                        mid is not container
                        and mid is not other
                        and mid.contains(other.start)
                        and container.contains(mid.start)
                        for mid in containers
                    ):
                        continue
                    lo = other.start - container.start
                    hi = min(other.end, read_to) - container.start
                    for i in range(max(lo, 0), max(hi, 0)):
                        if body[i] != "\n":
                            body[i] = " "
                self.declared_counts[id(container)] = len(
                    _CAST.findall("".join(body))
                )

            # Scan B: each cast by absolute offset, then innermost container.
            for match in _CAST.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                site = Site(
                    path,
                    match.start(),
                    line,
                    match.group(1),
                    receiver_of(readable, code, match.start()),
                    self.tree,
                )
                enclosing = [c for c in containers if c.contains(site.offset)]
                if enclosing:
                    site.container = max(enclosing, key=lambda c: c.start)
                self.sites.append(site)

    @property
    def orphans(self) -> list[Site]:
        return [site for site in self.sites if site.container is None]

    def reconcile(self) -> list[str]:
        """Containers whose two counts disagree, named with both numbers."""
        observed: dict[int, int] = {}
        for site in self.sites:
            if site.container is not None:
                observed[id(site.container)] = observed.get(id(site.container), 0) + 1
        disagreements = []
        for container in self.containers:
            declared = self.declared_counts.get(id(container), 0)
            counted = observed.get(id(container), 0)
            if declared != counted:
                disagreements.append(
                    f"{_display(container.path)} {container.name}: "
                    f"scan A saw {declared}, scan B assigned {counted}"
                )
        return sorted(disagreements)


# ---------------------------------------------------------------------------
# The positional population
# ---------------------------------------------------------------------------

# How far back a declaration's name and return type can sit from its parameter
# list. Wide enough for the longest qualified name plus template arguments in
# this tree; a lookback is what keeps the walk linear in the file's size.
_LOOKBACK = 200

_DECL_NAME = re.compile(r"([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)\s*$")
_NOT_A_RETURN_TYPE = re.compile(r"\b(?:return|new|delete|throw|case|else|co_return)$")
_STATEMENT_HEADS = frozenset(
    {"if", "for", "while", "switch", "return", "catch", "sizeof", "else", "do",
     "case", "new", "delete", "throw"}
)


def bracket_map(code: str, opener: str, closer: str) -> dict[int, int]:
    """Every matched delimiter pair in one file, as {open index: index past close}.

    One stacked pass rather than a forward scan per opener: the declaration walk
    below asks about every `(` in the tree, and a per-opener scan that runs to
    end-of-file on an unbalanced one costs the whole file each time.
    """
    pairs: dict[int, int] = {}
    stack: list[int] = []
    for i, ch in enumerate(code):
        if ch == opener:
            stack.append(i)
        elif ch == closer and stack:
            pairs[stack.pop()] = i + 1
    return pairs


def split_top_level(text: str) -> list[str]:
    """Split on commas that are not inside a bracket of any kind."""
    parts, depth, current = [], 0, ""
    for ch in text:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += ch
    if current.strip():
        parts.append(current)
    return [" ".join(part.split()) for part in parts if part.strip()]


def declarator_like(parameter: str) -> bool:
    """Whether one comma-separated item reads as a parameter, not an argument.

    Negative rather than whitelisted, because a whitelist of type names is the
    same common mode as the cast type list and would silently drop every
    parameter spelled with a type it had not heard of.  What an argument carries
    and a declarator cannot: a call, a literal, an operator, a member access --
    and where an argument is a bare identifier, a named parameter is two tokens.
    """
    body = parameter.split("=")[0].strip()
    if not body:
        return False
    if re.search(r"[().\"'+\-/%!?]", body.replace("->", "")):
        return False
    body = re.sub(r"\b(?:const|volatile)\b", " ", body)
    if any(ch in body for ch in "&*<["):
        return True
    tokens = body.split()
    if len(tokens) >= 2:
        return True
    # A single token is a type only when it is unnamed: a builtin, or a name
    # written in the tree's type case.
    return tokens[0] in POSITIONAL_TYPES or bool(re.match(r"^[A-Z]", tokens[0]))


def parameter_type(parameter: str) -> str:
    """The declared type of one parameter, with its name and default removed."""
    body = re.sub(r"\bconst\b", " ", parameter.split("=")[0])
    tokens = " ".join(body.split()).replace("&", " & ").replace("*", " * ").split()
    if len(tokens) > 1 and re.fullmatch(r"[A-Za-z_]\w*", tokens[-1]):
        tokens = tokens[:-1]
    text = " ".join(tokens).replace(" &", "&").replace(" *", "*")
    # One spelling per type: three headers qualify `val`, so a declaration and
    # its definition would otherwise read as two different signatures.
    return text.replace("emscripten::val", "val")


def parameter_name(parameter: str) -> str:
    """The declared name of one parameter, or the empty string if unnamed."""
    body = parameter.split("=")[0].strip()
    match = re.search(r"([A-Za-z_]\w*)\s*$", body)
    if match is None or " ".join(body.split()) == match.group(1):
        return ""
    return match.group(1)


class Declaration:
    """One function or method declaration, wherever it was written."""

    def __init__(self, path: Path, line: int, cls: str | None, name: str,
                 params: list[str]) -> None:
        self.path, self.line, self.cls, self.name = path, line, cls, name
        self.params = params
        self.types = tuple(parameter_type(p) for p in params)

    @property
    def qualified(self) -> str:
        return f"{self.cls}::{self.name}" if self.cls else self.name

    @property
    def display(self) -> str:
        return f"{_display(self.path)}:{self.line}"

    @property
    def signature(self) -> str:
        return f"{self.qualified}({', '.join(self.types)})"

    def narrow_indices(self) -> list[int]:
        return [i for i, t in enumerate(self.types) if t.rstrip("&* ") in POSITIONAL_TYPES]


class Registration:
    """One embind registration: a JS name bound to a C++ address."""

    def __init__(self, path: Path, line: int, kind: str, js_name: str, symbol: str) -> None:
        self.path, self.line, self.kind = path, line, kind
        self.js_name, self.symbol = js_name, symbol
        self.declaration: Declaration | None = None

    @property
    def display(self) -> str:
        return f"{_display(self.path)}:{self.line}"


class Positional:
    """One narrow integer parameter of one registered function."""

    def __init__(self, registration: Registration, declaration: Declaration, index: int) -> None:
        self.registration, self.declaration, self.index = registration, declaration, index
        self.name = parameter_name(declaration.params[index])
        self.type = declaration.types[index].rstrip("&* ")

    @property
    def key(self) -> tuple[str, str, int]:
        return (self.registration.js_name, self.declaration.qualified, self.index)

    @property
    def display(self) -> str:
        label = self.name or f"#{self.index}"
        return f"{self.registration.display}  {self.registration.js_name}({label}: {self.type})"


def class_spans(code: str, braces: dict[int, int] | None = None) -> list[tuple[str, int, int]]:
    """Every `class X { … }` / `struct X { … }` body, by brace balance."""
    if braces is None:
        braces = bracket_map(code, "{", "}")
    spans = []
    for match in re.finditer(r"\b(?:class|struct)\s+([A-Za-z_]\w*)\s*(?::[^{;]*)?\{", code):
        brace = code.index("{", match.end() - 1)
        end = braces.get(brace, -1)
        if end > 0:
            spans.append((match.group(1), brace, end))
    return spans


def declarations_of(path: Path, readable: str, code: str) -> list[Declaration]:
    """Every declaration in one file, whether it opens a body or ends in `;`."""
    parens = bracket_map(code, "(", ")")
    spans = class_spans(code)
    found: list[Declaration] = []
    for open_paren, close in sorted(parens.items()):
        # A bounded lookback, not the whole prefix: the name is anchored at the
        # paren, and slicing every prefix in the tree costs more than the scan.
        window = max(0, open_paren - _LOOKBACK)
        match = _DECL_NAME.search(code[window:open_paren].rstrip())
        if match is None:
            continue
        name = " ".join(match.group(1).split())
        if name.split("::")[-1] in _STATEMENT_HEADS:
            continue
        tail = _AFTER_PARAMS.match(code, close)
        stop = tail.end() if tail else close
        if stop >= len(code) or code[stop] not in "{;:":
            continue
        name_at = window + match.start()
        before = readable[max(0, name_at - _LOOKBACK):name_at].rstrip()
        # A declaration has a return type; a call has a statement head or an
        # operator where the return type would be.
        if not re.search(r"[\w>\]&*]$", before) or _NOT_A_RETURN_TYPE.search(before):
            continue
        params = split_top_level(readable[open_paren + 1:close - 1])
        if params and not all(declarator_like(p) for p in params):
            continue
        cls = None
        if "::" in name:
            cls, name = name.rsplit("::", 1)
            cls = cls.split("::")[-1]
        else:
            enclosing = [c for c, begin, end in spans if begin <= open_paren < end]
            if enclosing:
                cls = enclosing[-1]
        found.append(
            Declaration(path, readable.count("\n", 0, name_at) + 1, cls, name, params)
        )
    return found


class PositionalScan:
    """Both directions of the registration-to-declaration relation."""

    def __init__(self, tree: Path = WASM_TREE, *, short_name_only: bool = False) -> None:
        self.tree = tree
        # Resolving on the short name alone -- kept as an option so the routes'
        # disagreement can be demonstrated rather than argued.
        self.short_name_only = short_name_only
        self.registrations: list[Registration] = []
        self.declarations: list[Declaration] = []
        self.constructors: list[tuple[Path, int, tuple[str, ...]]] = []
        self.float_parameters: list[Positional] = []
        self.route_r: list[Positional] = []
        self.route_d: list[Positional] = []
        self.unresolved: list[Registration] = []
        self.conflicting: list[tuple[Registration, list[Declaration]]] = []
        self._run()

    def _files(self) -> list[Path]:
        return sorted(
            p for p in self.tree.rglob("*") if p.suffix in _SOURCE_SUFFIXES and p.is_file()
        )

    def _run(self) -> None:
        for path in self._files():
            readable, code = prepare(path.read_text(encoding="utf-8", errors="replace"))
            for match in _REGISTRATION.finditer(readable):
                self.registrations.append(
                    Registration(
                        path,
                        readable.count("\n", 0, match.start()) + 1,
                        match.group(1),
                        match.group(2),
                        match.group(3),
                    )
                )
            for match in _CONSTRUCTOR.finditer(code):
                self.constructors.append(
                    (
                        path,
                        code.count("\n", 0, match.start()) + 1,
                        tuple(split_top_level(match.group(1))),
                    )
                )
            self.declarations.extend(declarations_of(path, readable, code))

        index: dict[str, list[Declaration]] = {}
        for declaration in self.declarations:
            index.setdefault(declaration.qualified, []).append(declaration)
            if declaration.cls:
                index.setdefault(declaration.name, []).append(declaration)

        registered: dict[str, list[Registration]] = {}
        for registration in self.registrations:
            candidates = self._candidates(index, registration.symbol)
            if not candidates:
                self.unresolved.append(registration)
                continue
            if len({d.types for d in candidates}) > 1:
                self.conflicting.append((registration, candidates))
                continue
            registration.declaration = candidates[0]
            registered.setdefault(candidates[0].qualified, []).append(registration)
            # Route R: the registration's own declaration, read forwards.
            for i in candidates[0].narrow_indices():
                self.route_r.append(Positional(registration, candidates[0], i))
            for i, t in enumerate(candidates[0].types):
                if t.rstrip("&* ") == "float":
                    self.float_parameters.append(Positional(registration, candidates[0], i))

        # Route D: every declaration carrying a narrow parameter, kept if
        # something registered it. Seen declarations are deduplicated by
        # signature, since a header and its definition are one function.
        seen: set[tuple[str, int, str]] = set()
        for declaration in self.declarations:
            for registration in registered.get(declaration.qualified, []):
                for i in declaration.narrow_indices():
                    key = (registration.js_name, declaration.qualified, i)
                    if key in seen:
                        continue
                    seen.add(key)
                    self.route_d.append(Positional(registration, declaration, i))

    def _candidates(self, index: dict[str, list[Declaration]], symbol: str) -> list[Declaration]:
        if self.short_name_only:
            return index.get(symbol.split("::")[-1], [])
        for key in (symbol, symbol.split("::", 1)[-1], symbol.split("::")[-1]):
            if key in index:
                return index[key]
        return []

    def reconcile(self) -> list[str]:
        """Parameters one route found and the other did not, named with both."""
        by_r = {p.key for p in self.route_r}
        by_d = {p.key for p in self.route_d}
        lines = [
            f"{js}  {qualified} parameter #{i}: route R only"
            for js, qualified, i in sorted(by_r - by_d)
        ]
        lines += [
            f"{js}  {qualified} parameter #{i}: route D only"
            for js, qualified, i in sorted(by_d - by_r)
        ]
        return sorted(lines)

    def constructor_parameters(self) -> list[tuple[Path, int, int, list[int], list[str]]]:
        """Each `constructor<T...>()` that takes a narrow integer, and where."""
        found = []
        for path, line, types in self.constructors:
            narrow = [(i, t.rstrip("&* ")) for i, t in enumerate(types)]
            indices = [i for i, t in narrow if t in POSITIONAL_TYPES]
            if indices:
                found.append(
                    (
                        path,
                        line,
                        len(types),
                        indices,
                        [t for i, t in narrow if t in POSITIONAL_TYPES],
                    )
                )
        return found


def load_records(path: Path = RECORDS) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


class Records:
    """The recorded-benign data, and which of its entries matched anything."""

    def __init__(self, data: dict, tree: Path | None = None) -> None:
        self.tree = tree if tree is not None else WASM_TREE
        self.shapes = data.get("shapes", [])
        self.pending_types = {e["type"]: e for e in data.get("pending_types", [])}
        self.narrowings = data.get("narrowings", [])
        self.readers = data.get("readers", [])
        self.used: set[str] = set()
        self._shape_patterns = {
            shape["name"]: re.compile(shape["receiver_pattern"]) for shape in self.shapes
        }

    def covers(self, site: Site) -> bool:
        for shape in self.shapes:
            if self._shape_patterns[shape["name"]].search(site.receiver):
                self.used.add(f"shape:{shape['name']}")
                return True
        if site.type in self.pending_types:
            self.used.add(f"pending_type:{site.type}")
            return True
        for entry in self.narrowings:
            if (entry["file"], entry["receiver"], entry["type"]) == site.key:
                self.used.add(f"narrowing:{entry['file']}:{entry['receiver']}")
                return True
        return False

    def covers_reader(self, container: Container) -> bool:
        relative = _relative(container.path, self.tree)
        for entry in self.readers:
            if entry["file"] == relative and entry["symbol"] == container.name:
                self.used.add(f"reader:{relative}:{container.name}")
                return True
        return False

    def pending_count(self) -> int:
        """Records that enumerate a site without grading what it does."""
        return sum(
            1
            for entry in self.narrowings + self.readers
            if entry.get("triage") == "pending"
        )

    def unused(self) -> list[str]:
        every = (
            [f"shape:{s['name']}" for s in self.shapes]
            + [f"pending_type:{t}" for t in self.pending_types]
            + [f"narrowing:{e['file']}:{e['receiver']}" for e in self.narrowings]
            + [f"reader:{e['file']}:{e['symbol']}" for e in self.readers]
        )
        return sorted(name for name in every if name not in self.used)


def derive_defect(types: set[str]) -> str | None:
    """The conversion rule a set of declared types implies, or None if mixed.

    embind picks its glue by the C++ type, so the rule is not a matter of
    opinion and a record does not get to state it.  Deriving it is what stops a
    record from being re-labelled: no word in the file can make a ``uint32_t``
    stop wrapping.  A record covering two rules at once is refused rather than
    resolved, because one verdict cannot describe both halves.
    """
    kinds = set()
    for name in types:
        if name in _BIGINT_TYPES:
            kinds.add("refuses")
        elif name == "float":
            kinds.add("saturates")
        else:
            kinds.add("wraps")
    if len(kinds) != 1:
        return None
    return kinds.pop()


def read_citation(root: Path, citation: dict) -> str | None:
    """Check one mechanism citation, returning the failure or None.

    A citation is a path plus the text that must be readable at it, so a claim
    that something is harmless points at where that can be seen rather than
    asserting it.  ``built`` marks a file the build produces rather than the
    repo tracking -- the embind glue is the case that matters, since the
    conversion rules live in emscripten's output and in no committed file -- and
    only excuses the file being absent, never a quote that is there and wrong.
    """
    path = root / citation["file"]
    if not path.is_file():
        if citation.get("built"):
            return None
        return f"{citation['file']}: no such file, and it is not marked built"
    text = path.read_text(encoding="utf-8", errors="replace")
    if citation["contains"] not in text:
        return f"{citation['file']}: does not contain {citation['contains'][:60]!r}"
    return None


class PositionalRecords:
    """The recorded positional parameters, graded by what a wrapped value does.

    A shape carries a count, not just a pattern.  A pattern alone would absorb
    the next parameter to take a covered name -- the very case worth reporting --
    so a shape whose population has moved is a failure in either direction, and
    a fix has to delete or decrement its record in the same change.

    ``status`` is what the inline records cannot express, and the two values are
    deliberately not the same price:

    * ``open`` says the value wraps, is accepted, and selects something
      different.  It costs a ``selects`` line naming what gets picked.
    * ``benign`` says no such value exists.  It costs a ``mechanism`` citation --
      a path and the text that must be readable at it -- AND it is only legal
      where the defect DERIVED FROM THE DECLARED TYPES is ``refuses``.

    That second condition is the one that matters.  A status flipped by hand
    would otherwise turn 709 wrapping parameters into reviewed-and-fine on no
    evidence; deriving the rule from the type means no edit to this file can
    make a ``uint32_t`` stop wrapping, so ``benign`` is unreachable for them
    without changing the C++ declaration itself.  A genuinely harmless wrapping
    parameter -- one pinned by an earlier argument, say -- is not expressible
    here on purpose: it needs its own status with its own evidence rule, and
    that friction is the point.

    ``pending`` is refused as it is in the inline records: a suppression whose
    reason is "nobody has looked" reads downstream exactly like one whose reason
    is a mechanism.
    """

    STATUSES = ("benign", "open")

    def __init__(self, data: dict, root: Path | None = None) -> None:
        section = data.get("positional", {})
        self.root = root if root is not None else ROOT
        self.floor = section.get("floor", {})
        self.shapes = section.get("shapes", [])
        self.parameters = section.get("parameters", [])
        self.constructors = section.get("constructors", [])
        self.used: set[str] = set()
        self.counted: dict[str, int] = {}
        self.covered_types: dict[str, set[str]] = {}
        self._patterns = [
            (
                shape,
                re.compile(shape["parameter_pattern"]),
                frozenset(shape["types"]) if shape.get("types") else None,
            )
            for shape in self.shapes
        ]

    @staticmethod
    def label(entry: dict) -> str:
        return entry.get("name") or entry.get("js") or f"{entry.get('file')}<{entry.get('arity')}>"

    def every_record(self) -> list[dict]:
        return self.shapes + self.parameters + self.constructors

    def missing_reasons(self) -> list[str]:
        return sorted(
            self.label(entry)
            for entry in self.every_record()
            if not str(entry.get("reason", "")).strip()
        )

    def unsupported_open(self) -> list[str]:
        """Open records that do not say what a wrapped value selects."""
        return sorted(
            self.label(entry)
            for entry in self.every_record()
            if entry.get("status") == "open" and not str(entry.get("selects", "")).strip()
        )

    def unsupported_benign(self) -> list[str]:
        """Benign records whose mechanism is missing, unreadable, or not there."""
        failures = []
        for entry in self.every_record():
            if entry.get("status") != "benign":
                continue
            citations = entry.get("mechanism") or []
            if not citations:
                failures.append(f"{self.label(entry)}: benign with no mechanism citation")
                continue
            for citation in citations:
                problem = read_citation(self.root, citation)
                if problem:
                    failures.append(f"{self.label(entry)}: {problem}")
        return sorted(failures)

    def shape_of(self, parameter: Positional) -> dict | None:
        for shape, pattern, types in self._patterns:
            if types is not None and parameter.type not in types:
                continue
            if pattern.search(parameter.name):
                return shape
        return None

    def _observe(self, key: str, declared_type: str) -> None:
        self.covered_types.setdefault(key, set()).add(declared_type)

    def covers(self, parameter: Positional) -> bool:
        shape = self.shape_of(parameter)
        if shape is not None:
            self.used.add(f"shape:{shape['name']}")
            self.counted[shape["name"]] = self.counted.get(shape["name"], 0) + 1
            self._observe(self.label(shape), parameter.type)
            return True
        for entry in self.parameters:
            if (entry["js"], entry["symbol"], entry["index"]) == parameter.key:
                self.used.add(f"parameter:{entry['js']}:{entry['index']}")
                self._observe(self.label(entry), parameter.type)
                return True
        return False

    def covers_constructor(
        self, file: str, arity: int, indices: list[int], types: list[str]
    ) -> bool:
        """Whether one `constructor<T...>()` is recorded with the same shape.

        Keyed on the arity rather than on the line, because a constructor names
        its types in place: there is no declaration to point at, and two
        overloads in one file are told apart by how many arguments they take.
        """
        for entry in self.constructors:
            if entry["file"] == file and entry["arity"] == arity:
                self.used.add(f"constructor:{file}:{arity}")
                for declared_type in types:
                    self._observe(self.label(entry), declared_type)
                return entry["indices"] == indices
        return False

    def misdeclared_defects(self) -> list[str]:
        """Records whose stated defect is not the one their types imply.

        Two failures in one, because they are the same mistake seen from either
        side: a ``defect`` that disagrees with the derivation, and a ``benign``
        on a population whose derived rule is not ``refuses``.  Neither can be
        argued out of by editing this file -- both read the C++ declarations.
        """
        problems = []
        for entry in self.every_record():
            label = self.label(entry)
            types = self.covered_types.get(label)
            if not types:
                continue
            derived = derive_defect(types)
            if derived is None:
                problems.append(
                    f"{label}: covers {sorted(types)}, which embind converts by "
                    "more than one rule, so one verdict cannot describe it"
                )
                continue
            if entry.get("defect") != derived:
                problems.append(
                    f"{label}: states defect {entry.get('defect')!r}, but "
                    f"{sorted(types)} is converted by the {derived!r} rule"
                )
            if entry.get("status") == "benign" and derived != "refuses":
                problems.append(
                    f"{label}: benign is only reachable where the conversion "
                    f"refuses, and {sorted(types)} {derived}"
                )
        return sorted(problems)

    def miscounted(self) -> list[str]:
        """Shapes whose population is not the size the record claims."""
        return sorted(
            f"{shape['name']}: recorded {shape['count']}, "
            f"found {self.counted.get(shape['name'], 0)}"
            for shape in self.shapes
            if shape["count"] != self.counted.get(shape["name"], 0)
        )

    def ungraded(self) -> list[str]:
        return sorted(
            f"{entry.get('name') or entry.get('js') or entry.get('file')}: status "
            f"{entry.get('status', 'absent')!r}"
            for entry in self.shapes + self.parameters + self.constructors
            if entry.get("status") not in self.STATUSES
        )

    def open_total(self) -> int:
        return (
            sum(shape["count"] for shape in self.shapes if shape.get("status") == "open")
            + sum(1 for entry in self.parameters if entry.get("status") == "open")
            + sum(
                len(entry["indices"])
                for entry in self.constructors
                if entry.get("status") == "open"
            )
        )

    def unused(self) -> list[str]:
        every = (
            [f"shape:{s['name']}" for s in self.shapes]
            + [f"parameter:{e['js']}:{e['index']}" for e in self.parameters]
            + [f"constructor:{e['file']}:{e['arity']}" for e in self.constructors]
        )
        return sorted(name for name in every if name not in self.used)


class FloatRecords:
    """The float population's pinned distribution and its passthrough roster.

    ``distribution`` is an exact per-file count, not a floor.  A floor alone
    would catch a scanner that had stopped matching and nothing else; this
    population is not one the work removes, so what is worth catching is a float
    parameter LANDING.  A count that moves either way fails naming the file,
    which puts the triage in the same change as the parameter.

    ``passthroughs`` name the parameters a saturated value is knowingly allowed
    to reach.  The verdict is the expensive one for the same reason ``benign``
    is next door: it costs a ``mechanism`` citation -- a path and text that must
    be readable at it -- so a claim that a conversion is total points at where
    that can be seen.  A record matching no live parameter is an error, because
    a stale one keeps a contract attached to a spelling the next parameter would
    inherit.

    There is deliberately no status meaning "refused elsewhere".  Every
    unrecorded parameter is in that state, and a record asserting it per site
    would turn a fact nothing here can check into a file full of unchecked
    claims -- read, downstream, exactly like the ones that are checked.
    """

    def __init__(self, data: dict, tree: Path | None = None, root: Path | None = None) -> None:
        section = data.get("float", {})
        self.tree = tree if tree is not None else WASM_TREE
        self.root = root if root is not None else ROOT
        self.floor = section.get("floor", {})
        self.distribution = section.get("distribution", [])
        self.passthroughs = section.get("passthroughs", [])
        self.used: set[str] = set()

    @staticmethod
    def label(entry: dict) -> str:
        return f"{entry.get('js')}({entry.get('name') or '#' + str(entry.get('index'))})"

    def covers(self, parameter: Positional) -> dict | None:
        for entry in self.passthroughs:
            if (entry["js"], entry["symbol"], entry["index"]) == parameter.key:
                self.used.add(self.label(entry))
                return entry
        return None

    def missing_reasons(self) -> list[str]:
        return sorted(
            self.label(entry)
            for entry in self.passthroughs
            if not str(entry.get("reason", "")).strip()
        )

    def unsupported(self) -> list[str]:
        """Passthroughs whose mechanism is missing, unreadable, or not there."""
        failures = []
        for entry in self.passthroughs:
            citations = entry.get("mechanism") or []
            if not citations:
                failures.append(f"{self.label(entry)}: no mechanism citation")
                continue
            for citation in citations:
                problem = read_citation(self.root, citation)
                if problem:
                    failures.append(f"{self.label(entry)}: {problem}")
        return sorted(failures)

    def unused(self) -> list[str]:
        return sorted(
            self.label(entry)
            for entry in self.passthroughs
            if self.label(entry) not in self.used
        )

    def miscounted(self, parameters: list[Positional]) -> list[str]:
        """Files whose float count is not the one the distribution records.

        Each disagreement is followed by the file's parameters, because a count
        alone leaves the reader to re-derive which one moved -- and finding that
        is the whole of the triage the count exists to force.
        """
        measured: dict[str, list[Positional]] = {}
        for parameter in parameters:
            key = _relative(parameter.registration.path, self.tree)
            measured.setdefault(key, []).append(parameter)
        recorded = {entry["file"]: entry["count"] for entry in self.distribution}
        problems = []
        for name in sorted(set(measured) | set(recorded)):
            found = measured.get(name, [])
            if len(found) == recorded.get(name, 0):
                continue
            problems.append(f"{name}: recorded {recorded.get(name, 0)}, found {len(found)}")
            problems.extend(
                f"    {p.registration.js_name}({p.name or '#' + str(p.index)})"
                for p in sorted(found, key=lambda p: (p.registration.js_name, p.index))
            )
        return problems


def _self_check(scan: Scan, floor: dict) -> list[str]:
    """The mandatory one. Two empty sets agree perfectly.

    With the cast pattern matching nothing, the orphan check and the
    reconciliation both pass and certify a scanner that has stopped working, so
    the population's own size is asserted before either of them is believed.
    """
    failures = []
    measured = {
        "files": len({site.path for site in scan.sites}),
        "containers": len(scan.containers),
        "narrowings": len(scan.sites),
        "readers": len(scan.readers),
    }
    for name, minimum in floor.items():
        if measured.get(name, 0) < minimum:
            failures.append(
                f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- "
                "the scan has stopped matching, and an empty population agrees "
                "with everything"
            )
    return failures


def _float_self_check(scan: PositionalScan, floor: dict) -> list[str]:
    """The float population's own floor, for the same reason as the other two.

    Redundant with the distribution on the day both are right, and not redundant
    on the day the distribution is emptied: an empty roster agrees with an empty
    scan, and the floor is what refuses that pair.
    """
    failures = []
    measured = {
        "parameters": len(scan.float_parameters),
        "functions": len({p.registration.js_name for p in scan.float_parameters}),
        "files": len({p.registration.path for p in scan.float_parameters}),
    }
    for name, minimum in floor.items():
        if measured.get(name, 0) < minimum:
            failures.append(
                f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- "
                "the scan has stopped matching, and an empty population agrees "
                "with everything"
            )
    return failures


def evaluate_floats(scan: PositionalScan, records: FloatRecords) -> list[tuple[str, list[str]]]:
    """Every failure class of the float population, as (heading, lines)."""
    failures: list[tuple[str, list[str]]] = []

    self_check = _float_self_check(scan, records.floor)
    if self_check:
        failures.append(
            ("The float scan no longer finds the population it is sized for", self_check)
        )

    # Walked before the roster is read for staleness, since this is what marks a
    # passthrough used.
    for parameter in scan.float_parameters:
        records.covers(parameter)

    missing_reasons = records.missing_reasons()
    if missing_reasons:
        failures.append(
            (
                ("These passthroughs carry no reason, so they allow a saturated "
                "value through without saying on what grounds"),
                [f"  {name}" for name in missing_reasons],
            )
        )

    unsupported = records.unsupported()
    if unsupported:
        failures.append(
            (
                ("These passthroughs claim a contract that cannot be read where "
                "they say it is. Letting a value through unrefused is the "
                "expensive verdict on purpose: it points at the text, and the "
                "text has to be there"),
                [f"  {line}" for line in unsupported],
            )
        )

    counts = records.miscounted(scan.float_parameters)
    if counts:
        failures.append(
            (
                ("These files no longer hold the float parameters the "
                "distribution records. The count moving either way is the "
                "report: a parameter landing needs its triage in this change, "
                "and one leaving needs its record deleted in the same one"),
                [f"  {line}" for line in counts],
            )
        )

    # The wrapping question's trip-wire. A float SATURATES rather than wrapping,
    # so the positional population excludes it by mechanism -- and by role, since
    # no float parameter here is a count or an index. Only the second half can
    # stop being true without anyone noticing.
    counted_floats = [p for p in scan.float_parameters if _COUNT_SHAPED.search(p.name)]
    if counted_floats:
        failures.append(
            (
                ("These float parameters are named like a count or an index, "
                "which is the one float shape the wrapping question reaches; the "
                "float exclusion from the positional population was written on "
                "there being none"),
                [f"  {p.display}" for p in sorted(counted_floats, key=lambda p: p.display)],
            )
        )

    stale = records.unused()
    if stale:
        failures.append(
            (
                ("These passthroughs matched no parameter. A record that allows "
                "nothing still asserts a contract about a spelling, so the next "
                "parameter to take it inherits permission unexamined"),
                [f"  {name}" for name in stale],
            )
        )
    return failures


def _positional_self_check(scan: PositionalScan, floor: dict) -> list[str]:
    """The positional population's own floor, for the same reason as the other."""
    failures = []
    measured = {
        "registrations": len(scan.registrations),
        "declarations": len(scan.declarations),
        "parameters": len(scan.route_r),
        "functions": len({p.registration.js_name for p in scan.route_r}),
        "files": len({p.registration.path for p in scan.route_r}),
    }
    for name, minimum in floor.items():
        if measured.get(name, 0) < minimum:
            failures.append(
                f"{name}: found {measured.get(name, 0)}, floor is {minimum} -- "
                "the scan has stopped matching, and an empty population agrees "
                "with everything"
            )
    return failures


def evaluate_positional(
    scan: PositionalScan, records: PositionalRecords
) -> list[tuple[str, list[str]]]:
    """Every failure class of the positional population, as (heading, lines)."""
    failures: list[tuple[str, list[str]]] = []

    ungraded = records.ungraded()
    if ungraded:
        failures.append(
            (
                ("These positional records carry no verdict, so they suppress a "
                "site on no stated mechanism and on no stated defect"),
                [f"  {line}" for line in ungraded],
            )
        )

    missing_reasons = records.missing_reasons()
    if missing_reasons:
        failures.append(
            (
                ("These positional records carry no reason, so they suppress a "
                "site without saying on what grounds"),
                [f"  {name}" for name in missing_reasons],
            )
        )

    unsupported_open = records.unsupported_open()
    if unsupported_open:
        failures.append(
            (
                ("These records are open without saying what a wrapped value "
                "selects, which is the whole content of the claim -- an open "
                "record that names no consequence is a pending one renamed"),
                [f"  {name}" for name in unsupported_open],
            )
        )

    unsupported_benign = records.unsupported_benign()
    if unsupported_benign:
        failures.append(
            (
                ("These records claim a mechanism that cannot be read where they "
                "say it is. Benign is the expensive verdict on purpose: it points "
                "at the text, and the text has to be there"),
                [f"  {line}" for line in unsupported_benign],
            )
        )

    self_check = _positional_self_check(scan, records.floor)
    if self_check:
        failures.append(
            ("The positional scan no longer finds the population it is sized for", self_check)
        )

    if scan.unresolved:
        failures.append(
            (
                ("These registrations bind a symbol no declaration in the tree "
                "matches, so nothing can say what their parameters are"),
                [f"  {r.display}  {r.js_name} -> {r.symbol}" for r in scan.unresolved],
            )
        )

    if scan.conflicting:
        failures.append(
            (
                ("These registrations resolve to declarations that disagree on a "
                "signature, so the parameter list read here is one of several"),
                [
                    f"  {r.display}  {r.js_name} -> "
                    + " | ".join(sorted({d.signature for d in candidates}))
                    for r, candidates in scan.conflicting
                ],
            )
        )

    disagreements = scan.reconcile()
    if disagreements:
        failures.append(
            (
                ("The two routes disagree on these parameters, so a registration "
                "resolved to a declaration the other route did not reach"),
                [f"  {line}" for line in disagreements],
            )
        )

    unrecorded = [p for p in scan.route_r if not records.covers(p)]
    if unrecorded:
        failures.append(
            (
                ("These positional parameters are declared as a narrow integer and "
                "are not recorded, so embind converts them by a rule -- wrapping "
                "modulo 2^32 -- that nothing here has graded"),
                [f"  {p.display}" for p in sorted(unrecorded, key=lambda p: p.display)],
            )
        )

    counts = records.miscounted()
    if counts:
        failures.append(
            (
                ("These shapes no longer cover the population they record. A shape "
                "is a pattern AND a count, because a pattern alone absorbs the "
                "next parameter to take a covered name"),
                [f"  {line}" for line in counts],
            )
        )

    unrecorded_constructors = [
        f"  {_display(path)}:{line}  constructor<{arity}> narrow at {indices}"
        for path, line, arity, indices, types in scan.constructor_parameters()
        if not records.covers_constructor(
            str(path.relative_to(scan.tree)), arity, indices, types
        )
    ]
    if unrecorded_constructors:
        failures.append(
            (
                ("These embind constructors take a narrow integer positionally and "
                "the record does not match. They name their types in place rather "
                "than pointing at a declaration, so the arity is the only key"),
                unrecorded_constructors,
            )
        )

    # After the coverage walks, which are what observe the declared types.
    misdeclared = records.misdeclared_defects()
    if misdeclared:
        failures.append(
            (
                ("These records state a conversion rule their parameters' types do "
                "not have. embind picks its glue by the C++ type, so the rule is "
                "derived here rather than believed"),
                [f"  {line}" for line in misdeclared],
            )
        )

    stale = records.unused()
    if stale:
        failures.append(
            (
                ("These positional records matched nothing. A record that "
                "suppresses nothing still asserts a reviewed decision about a "
                "spelling, so the next parameter to take it inherits the verdict"),
                [f"  {name}" for name in stale],
            )
        )
    return failures


def evaluate(scan: Scan, records: Records, floor: dict) -> list[tuple[str, list[str]]]:
    """Every failure class, as (heading, lines).

    Separated from ``main`` so the self-tests can revert one site at a time and
    require the matching class to fire with a count -- a class asserted through
    a reimplementation of this function would agree with itself.
    """
    failures: list[tuple[str, list[str]]] = []

    # A record marked pending says the site was enumerated and NOT graded, which
    # is a different claim from benign. Gated rather than printed: a suppression
    # whose reason is "nobody has looked" reads, to everything downstream, like
    # one whose reason is a mechanism.
    ungraded = [
        f"  {entry.get('file')}:{entry.get('receiver') or entry.get('symbol')}"
        for entry in records.narrowings + records.readers
        if entry.get("triage") == "pending"
    ]
    if ungraded:
        failures.append(
            (
                ("These records enumerate a site without grading what it does, so "
                "they suppress it on no stated mechanism"),
                sorted(ungraded),
            )
        )

    self_check = _self_check(scan, floor)
    if self_check:
        failures.append(("The scan no longer finds the population it is sized for", self_check))

    orphans = scan.orphans
    if orphans:
        failures.append(
            (
                ("These narrowings sit in no val-taking body, so the declaration "
                "pattern is too narrow to see the function they are in"),
                [f"  {site.display}  {site.receiver}.as<{site.type}>()" for site in orphans],
            )
        )

    disagreements = scan.reconcile()
    if disagreements:
        failures.append(
            (
                ("The two scans disagree on these bodies, so a body span was "
                "mis-measured and one of the counts is wrong"),
                [f"  {line}" for line in disagreements],
            )
        )

    local_readers = [
        container
        for container in scan.readers
        if scan.tree / SHARED_READER_SUBDIR not in container.path.parents
        and not records.covers_reader(container)
    ]
    if local_readers:
        failures.append(
            (
                ("These file-local readers convert a JS value outside the shared "
                "reader family, so a change to the conversion contract does not "
                "reach their call sites"),
                [
                    f"  {_display(c.path)}  {c.returns} {c.name}(val)"
                    for c in sorted(local_readers, key=lambda c: (str(c.path), c.name))
                ],
            )
        )

    unrecorded = [site for site in scan.sites if not records.covers(site)]
    if unrecorded:
        failures.append(
            (
                ("These narrowings are neither performed by the shared reader nor "
                "recorded with the mechanism that makes them harmless"),
                [
                    f"  {site.display}  {site.receiver}.as<{site.type}>()"
                    for site in unrecorded
                ],
            )
        )

    stale = records.unused()
    if stale:
        failures.append(
            (
                ("These records matched nothing. A record that suppresses nothing "
                "still asserts a reviewed decision about a spelling, so the next "
                "narrowing to take it inherits the blessing unexamined"),
                [f"  {name}" for name in stale],
            )
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, default=WASM_TREE)
    parser.add_argument("--records", type=Path, default=RECORDS)
    parser.add_argument(
        "--body-window",
        type=int,
        default=None,
        help="measure bodies with a fixed character window instead of brace "
        "balance, to show what the reconciliation catches",
    )
    parser.add_argument(
        "--no-qualified-val",
        action="store_true",
        help="stop matching an `emscripten::val` parameter, to show what the "
        "orphan check catches",
    )
    parser.add_argument(
        "--short-name-only",
        action="store_true",
        help="resolve a registered symbol on its short name alone, to show what "
        "the two positional routes catch",
    )
    args = parser.parse_args()

    global _VAL_PARAM
    if args.no_qualified_val:
        _VAL_PARAM = re.compile(r"\bval\b(?<!::val)")

    data = load_records(args.records)
    records = Records(data, args.tree)
    scan = Scan(args.tree, body_window=args.body_window)

    positional_records = PositionalRecords(data)
    positional = PositionalScan(args.tree, short_name_only=args.short_name_only)
    float_records = FloatRecords(data, args.tree)

    print(f"val-taking bodies: {len(scan.containers)}")
    print(f"integer narrowings: {len(scan.sites)}")
    print(f"val-taking numeric-returning functions: {len(scan.readers)}")
    print(f"embind registrations: {len(positional.registrations)}")
    print(
        f"narrow positional parameters: {len(positional.route_r)} "
        f"over {len({p.registration.js_name for p in positional.route_r})} functions, "
        f"{positional_records.open_total()} of them open"
    )
    print(
        f"float positional parameters: {len(positional.float_parameters)} "
        f"over {len({p.registration.js_name for p in positional.float_parameters})} functions "
        f"in {len({p.registration.path for p in positional.float_parameters})} files, "
        f"{len(float_records.passthroughs)} of them recorded as passthroughs "
        "(the rest are refused outside this tree, which this scan does not check)"
    )

    failures = evaluate(scan, records, data["floor"])
    failures += evaluate_positional(positional, positional_records)
    failures += evaluate_floats(positional, float_records)

    for heading, lines in failures:
        print(f"\n{heading}:", *lines, sep="\n", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
