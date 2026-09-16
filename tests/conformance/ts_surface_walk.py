#!/usr/bin/env python3
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


def named_type_bodies(whole: str, name: str) -> list[str]:
    """Bodies of `interface <name> {...}` / `type <name> = {...}`."""
    bodies = []
    pattern = re.compile(r"\b(?:interface|type)\s+" + re.escape(name) + r"\b[^{=]*[={]")
    for match in pattern.finditer(whole):
        brace = whole.find("{", match.start())
        if brace == -1:
            continue
        body = brace_body(whole, brace)
        if body is not None:
            bodies.append(body)
    return bodies


def property_bodies(text: str, prop: str, whole: str) -> list[str]:
    """Every object body a property may hold, across a union and through aliases."""
    bodies: list[str] = []
    header = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(prop) + r"\s*\??\s*:")
    for match in header.finditer(text):
        index = skip_trivia(text, match.end())
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
            bodies += named_type_bodies(whole, identifier.group(0).replace("[]", ""))
            index = skip_trivia(text, index + identifier.end())
    return bodies


def declares_leaf(body: str, leaf: str) -> bool:
    """Whether `body` declares `leaf` as a property of its own."""
    return (
        re.search(r"(?<![A-Za-z0-9_])" + re.escape(leaf) + r"\s*\??\s*:", body)
        is not None
    )
