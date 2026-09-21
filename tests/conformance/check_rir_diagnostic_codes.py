"""Every warning code the RIR synthesis can emit must be named on every surface.

A diagnostic code is a string literal in one `.cpp` and a word in three doc
comments. Nothing links them, so a code added to the synthesis reaches a caller
only if whoever added it remembered three files -- and the failure is silent in
the direction that matters: a caller writing a `switch` over the documented set
falls through on a code that does occur, and the fall-through case is whichever
one nobody wrote down. That happened: `acoustic.rir_length_floored` was emitted
for as long as it existed and absent from all three room-morph docs.

The producer is the `Severity::Warning` push site, not the string literal alone.
The same file also emits Error codes, which reach a caller by a different route
(a morph raises rather than returning one), so a scan keyed on `"acoustic."`
would demand the morph docs name codes they must not name. Both directions are
checked: every warning present, and no error code pasted in to satisfy the first
half.

Two vacuity guards, because a scan that matches nothing exits 0 and reads as
coverage: the warning set must be non-empty and at least as large as the floor
recorded when this was written, and a surface file whose doc block cannot be
located is a failure rather than a skipped comparison.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

PRODUCER = ROOT / "src/acoustic/rir_synthesizer.cpp"

# The doc block each surface writes for the room-morph result. The anchor is the
# wording, not a line number, so the block survives being moved within its file.
SURFACES: dict[str, tuple[str, str]] = {
    "node": ("bindings/node/src/types_analysis.ts", "the morph went through a room"),
    "wasm": ("bindings/wasm/src/public_types_acoustic.ts", "the morph went through a room"),
    "python": ("bindings/python/src/libsonare/_types_acoustic.py", "Each says the morph"),
}

# What the producer held when this check was written. A warning set that shrinks
# below it means the extraction stopped reaching the push sites, which otherwise
# reports exactly like a repository with fewer codes.
WARNING_FLOOR = 4


def _flatten(text: str) -> str:
    """Collapse every run of whitespace, so a claim that wrapped still matches."""
    return re.sub(r"\s+", " ", text)


def emitted(source: str, severity: str) -> set[str]:
    """Codes pushed at @p severity, read from the push site rather than the literal."""
    pattern = r"Diagnostic::Severity::" + severity + r",\s*\"(acoustic\.[a-z_0-9]+)\""
    return set(re.findall(pattern, _flatten(source)))


def doc_block(text: str, anchor: str) -> str | None:
    """The comment or docstring containing @p anchor, or None when absent.

    Each blank-line-separated block is stripped of its comment markers and
    flattened BEFORE the anchor is looked for. Searching line by line would miss
    every anchor that wraps, which at this file's column width is most of them --
    and it would report the miss as "no such block", which is the same output as
    a surface that genuinely does not document the codes.
    """
    for raw in text.split("\n\n"):
        stripped = re.sub(r"(?m)^\s*(///|//|\*/|/\*\*|/\*|\*|#)\s?", "", raw)
        flat = _flatten(stripped)
        if anchor in flat:
            return flat
    return None


def scan(source: str, surfaces: dict[str, str]) -> tuple[list[str], int]:
    """Findings plus the number of (code, surface) pairs actually compared."""
    warnings = emitted(source, "Warning")
    errors = emitted(source, "Error")
    findings: list[str] = []
    comparisons = 0

    if len(warnings) < WARNING_FLOOR:
        findings.append(
            f"UNREACHED producer: {len(warnings)} warning push site(s) found, "
            f"at least {WARNING_FLOOR} expected -- the extraction stopped reaching them"
        )
        return findings, 0

    for side, text in surfaces.items():
        anchor = SURFACES[side][1]
        block = doc_block(text, anchor)
        if block is None:
            findings.append(f"NOT COMPARED [{side}]: no doc block contains {anchor!r}")
            continue
        for code in sorted(warnings):
            comparisons += 1
            if code not in block:
                findings.append(f"MISSING {code} [{side}]: emitted but not named in the doc")
        for code in sorted(errors):
            comparisons += 1
            if code in block:
                findings.append(
                    f"UNEXPECTED {code} [{side}]: an error code named in a warning-only doc"
                )
    return findings, comparisons


def main() -> int:
    source = PRODUCER.read_text()
    surfaces = {side: (ROOT / path).read_text() for side, (path, _) in SURFACES.items()}
    findings, comparisons = scan(source, surfaces)

    warnings = emitted(source, "Warning")
    print(
        f"warning codes: {len(warnings)} | surfaces: {len(surfaces)} | comparisons: {comparisons}"
    )
    if comparisons == 0:
        print("nothing was compared -- the producer or the doc anchors moved")
        for finding in findings:
            print(finding)
        return 1
    for finding in findings:
        print(finding)
    if findings:
        return 1
    print("every RIR warning code is named on every surface, and no error code is")
    return 0


if __name__ == "__main__":
    sys.exit(main())
