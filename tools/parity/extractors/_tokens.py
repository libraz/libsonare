"""Shared C / C++ / TypeScript tokenization helpers for the parity extractors.

The C, C++ and TS extractors all need to split a comma-separated argument list
on its top-level commas (ignoring commas nested inside parentheses, brackets,
braces or template angle brackets) and to strip C-style comments. Keeping one
implementation of each here stops the extractors from drifting apart.
"""

from __future__ import annotations

import re


def split_top_level_commas(
    s: str, open_chars: str = "([{<", close_chars: str = ")]}>"
) -> list[str]:
    """Split ``s`` on commas that sit at bracket-nesting depth zero.

    ``open_chars`` / ``close_chars`` parameterize which characters raise and
    lower the nesting depth: the C extractor passes the narrower ``([<`` /
    ``)]>`` set (no braces) it has always used, while the C++ and TS extractors
    use the default full set. Returns the stripped, non-empty parts.
    """
    parts: list[str] = []
    depth = 0
    cur: list[str] = []
    for ch in s:
        if ch in open_chars:
            depth += 1
        elif ch in close_chars:
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def strip_c_comments(text: str) -> str:
    """Replace ``/* ... */`` and ``// ...`` comments with a single space each."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)
