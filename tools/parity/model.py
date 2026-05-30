"""Normalized data model shared by all extractors.

A ``FunctionSig`` is a single function/method as seen on one surface (C, Python,
Node, WASM, CLI). The ``key`` field is the canonical, surface-independent name
(see ``normalize.canonical_key``) so signatures from different surfaces can be
matched up against the C-API canonical reference.
"""

from __future__ import annotations

from dataclasses import dataclass, field

# Recognized surfaces. C is the canonical reference for name / order / types.
SURFACES = ("c", "python", "node", "wasm", "cli")


@dataclass
class Param:
    """A single parameter of a function on one surface.

    Attributes:
        name: Canonical snake_case parameter name.
        raw_name: Name exactly as written on the surface (camelCase, kebab, ...).
        default: Literal default as a normalized string, or ``None`` if the
            surface declares no default (C never has defaults; a required TS/py
            arg also has ``None``).
        type: Best-effort type string as written on the surface.
        enum_values: Sorted tuple of accepted enum/string-union values, when the
            parameter is an enum-like; empty otherwise.
        structural: True when the param is a known structural artifact (an
            options bag, the trailing ``validate`` flag, an out-pointer, length,
            sample-rate-by-convention, ...) that should not be diffed positionally.
    """

    name: str
    raw_name: str = ""
    default: str | None = None
    type: str = ""
    enum_values: tuple[str, ...] = ()
    structural: bool = False


@dataclass
class FunctionSig:
    """One function as seen on one surface."""

    key: str
    surface: str
    raw_name: str = ""
    params: list[Param] = field(default_factory=list)
    returns: str = ""
    file: str = ""
    line: int = 0
    # When True, this surface symbol has no canonical C counterpart by design /
    # by detection (surface-only helper). Compare against the allowlist.
    surface_only: bool = False

    def core_params(self) -> list[Param]:
        """Params with structural artifacts removed (for positional diffing)."""
        return [p for p in self.params if not p.structural]


@dataclass
class Extraction:
    """Result of parsing one surface."""

    surface: str
    functions: list[FunctionSig] = field(default_factory=list)
    # Number of declarations the parser saw but could not confidently parse.
    unparsed: int = 0
    # Human-readable notes about unparsed/skipped items (file:line + reason).
    unparsed_notes: list[str] = field(default_factory=list)
