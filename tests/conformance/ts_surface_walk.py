"""Resolve a dotted path through hand-written TypeScript declarations.

The core publishes several flat lists of dotted names -- parameter setter keys,
result schema paths -- and each has to be matched against a nested TS type that
nothing else compares to anything. The walk is shared by every one of those
checks because the hard part is the same each time and getting it wrong fails
towards a clean report: a leaf matched anywhere in the file resolves a generic
name (`mode`, `enabled`, `bpm`) against an unrelated block, and a walk that
cannot follow `foo?: boolean | Bar` silently stops comparing part of its
population while still printing a count.

So the two rules a caller inherits: resolve each segment inside the block its
parent named, and treat a path whose block could not be reached as a failure
rather than a pass.

A mapped type is expanded only when its keys are decidable: `Record<Union, T>`
over string literals becomes the object it stands for, while `Record<string, T>`
resolves to nothing, because no declaration says which keys such a value carries.
"""

from __future__ import annotations

import re


def camel(name: str) -> str:
    """`snake_case` or a bare word as the TS surfaces spell it."""
    head, *rest = name.split("_")
    return head + "".join(part.capitalize() for part in rest)


def brace_body(text: str, start: int) -> str | None:
    """The `{...}` block beginning at `start`, including both braces."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    return None


def skip_trivia(text: str, index: int) -> int:
    """Advance past whitespace and comments, which sit between union members."""
    while index < len(text):
        if text[index] in " \t\r\n":
            index += 1
        elif text.startswith("/*", index):
            close = text.find("*/", index)
            index = len(text) if close == -1 else close + 2
        elif text.startswith("//", index):
            newline = text.find("\n", index)
            index = len(text) if newline == -1 else newline + 1
        else:
            return index
    return index


def type_alias_rhs(whole: str, name: str) -> str | None:
    """The right-hand side of `type <name> = ...;`, or None."""
    match = re.search(r"\btype\s+" + re.escape(name) + r"\b\s*=\s*", whole)
    if match is None:
        return None
    end = whole.find(";", match.end())
    return whole[match.end() : end if end != -1 else len(whole)]


def union_literals(whole: str, expr: str, depth: int = 3) -> list[str]:
    """The string-literal members of a union, following alias hops."""
    expr = expr.strip()
    literals = re.findall(r"'([^']*)'", expr)
    if literals:
        return literals
    if depth <= 0 or re.fullmatch(r"[A-Za-z_]\w*", expr) is None:
        return []
    rhs = type_alias_rhs(whole, expr)
    return [] if rhs is None else union_literals(whole, rhs, depth - 1)


def _generic_args(text: str, start: int) -> tuple[list[str], int]:
    """The arguments of the `<...>` beginning at `start`, and the index past it."""
    depth = 0
    args: list[str] = []
    current: list[str] = []
    for index in range(start, len(text)):
        char = text[index]
        if char == "<":
            depth += 1
            if depth == 1:
                continue
        elif char == ">":
            depth -= 1
            if depth == 0:
                args.append("".join(current))
                return args, index + 1
        elif char == "," and depth == 1:
            args.append("".join(current))
            current = []
            continue
        if depth >= 1:
            current.append(char)
    return [], start


def record_body(whole: str, text: str, start: int) -> tuple[str | None, int]:
    """Expand `Record<K, V>` at `start` into an equivalent object body.

    Only a key type that resolves to a union of string literals can be expanded.
    A `Record<string, T>` has an open key set, so no path below it is decidable
    from the declarations -- the caller gets None and reports the path as one it
    could not compare, which is the honest answer rather than a pass.
    """
    index = skip_trivia(text, start)
    if index >= len(text) or text[index] != "<":
        return None, start
    args, end = _generic_args(text, index)
    if len(args) != 2:
        return None, end
    keys = union_literals(whole, args[0])
    if not keys:
        return None, end
    value = args[1].strip()
    return "{" + "".join(f"{key}: {value};" for key in keys) + "}", end


_DEPTH = 6


def expression_bodies(
    whole: str, text: str, index: int, depth: int = _DEPTH
) -> list[str]:
    """Every object body the type expression at `index` in `text` can take.

    One resolver for both a property's type and a type alias's right-hand side,
    so a shape either side can spell -- a union, an array, a named type, a mapped
    type -- is followed identically wherever it appears.
    """
    bodies: list[str] = []
    if depth <= 0:
        return bodies
    index = skip_trivia(text, index)
    while index < len(text):
        if text[index] == "|":
            index = skip_trivia(text, index + 1)
            continue
        if text[index] == "{":
            body = brace_body(text, index)
            if body is None:
                break
            bodies.append(body)
            index = skip_trivia(text, index + len(body))
            continue
        identifier = re.match(r"[A-Za-z_]\w*(\[\])?", text[index:])
        if identifier is None:
            break
        after = index + identifier.end()
        name = identifier.group(0).replace("[]", "")
        if name == "Record":
            body, after = record_body(whole, text, after)
            if body is not None:
                bodies.append(body)
        else:
            bodies += named_type_bodies(whole, name, depth - 1)
        index = skip_trivia(text, after)
    return bodies


def named_type_bodies(whole: str, name: str, depth: int = _DEPTH) -> list[str]:
    """Bodies of `interface <name> {...}` / `type <name> = ...`.

    An alias with no brace of its own is resolved through its right-hand side
    rather than by searching forward for a brace: the next one in the file
    belongs to an unrelated declaration, and adopting it reports a leaf as
    present under a name that never carried it.
    """
    bodies: list[str] = []
    pattern = re.compile(r"\b(?:interface|type)\s+" + re.escape(name) + r"\b[^{=]*[={]")
    for match in pattern.finditer(whole):
        if whole[match.end() - 1] == "{":
            body = brace_body(whole, match.end() - 1)
            if body is not None:
                bodies.append(body)
            # An interface's inherited members are its own, so each base's body counts too.
            header = whole[match.start() : match.end() - 1]
            heritage = re.search(r"\bextends\b(.*)$", header, re.DOTALL)
            if heritage is not None and depth > 1:
                bases = re.findall(
                    r"([A-Za-z_]\w*)\s*(?:<[^,]*>)?\s*(?:,|$)", heritage.group(1).strip()
                )
                for base in bases:
                    bodies += named_type_bodies(whole, base, depth - 1)
            continue
        terminator = whole.find(";", match.end())
        end = len(whole) if terminator == -1 else terminator
        bodies += expression_bodies(whole, whole[:end], match.end(), depth - 1)
    return bodies


def property_bodies(text: str, prop: str, whole: str) -> list[str]:
    """Every object body a property may hold, across a union and through aliases."""
    bodies: list[str] = []
    header = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(prop) + r"\s*\??\s*:")
    for match in header.finditer(text):
        bodies += expression_bodies(whole, text, match.end())
    return bodies


def declares_leaf(body: str, leaf: str) -> bool:
    """Whether `body` declares `leaf` as a property of its own."""
    return (
        re.search(r"(?<![A-Za-z0-9_])" + re.escape(leaf) + r"\s*\??\s*:", body)
        is not None
    )
