"""Allowlist of intentional, non-drift divergences.

Loaded from ``allowlist.toml`` via the stdlib ``tomllib``. Entries fall into:

* ``[coverage]``  surface -> list of canonical keys allowed to be MISSING on
                  that surface (e.g. a C function deliberately not in WASM).
* ``[surface_only]`` surface -> list of surface-only symbols with no C
                  counterpart that are intentional (async/progress/platform).
  Glob-ish suffix/prefix wildcards (``*progress``) are supported.
* ``[order]``     surface -> list of keys whose param order legitimately differs.
* ``[default]``   list of ``"key.param"`` whose default is allowed to differ.
* ``[enum]``      list of ``"key.param"`` whose enum sets are allowed to differ.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path


def _match(name: str, patterns: list[str]) -> bool:
    for p in patterns:
        if p == name:
            return True
        if p.startswith("*") and name.endswith(p[1:]):
            return True
        if p.endswith("*") and name.startswith(p[:-1]):
            return True
    return False


@dataclass
class Allowlist:
    coverage: dict[str, list[str]] = field(default_factory=dict)
    surface_only: dict[str, list[str]] = field(default_factory=dict)
    order: dict[str, list[str]] = field(default_factory=dict)
    default: list[str] = field(default_factory=list)
    core_default: list[str] = field(default_factory=list)
    enum: list[str] = field(default_factory=list)
    input_naming: list[str] = field(default_factory=list)
    # Overrides for the central tuning knobs (empty -> use compare.py defaults).
    input_roles: list[str] = field(default_factory=list)
    handle_prefixes: list[str] = field(default_factory=list)

    def coverage_ok(self, key: str, surface: str) -> bool:
        return _match(key, self.coverage.get(surface, []))

    def input_naming_ok(self, key: str) -> bool:
        return _match(key, self.input_naming)

    def surface_only_ok(self, key: str, surface: str) -> bool:
        if _match(key, self.surface_only.get(surface, [])):
            return True
        return _match(key, self.surface_only.get("any", []))

    def order_ok(self, key: str, surface: str) -> bool:
        return _match(key, self.order.get(surface, []))

    def default_ok(self, key: str, param: str) -> bool:
        return _match(f"{key}.{param}", self.default)

    def core_default_ok(self, key: str, param: str) -> bool:
        return _match(f"{key}.{param}", self.core_default)

    def enum_ok(self, key: str, param: str) -> bool:
        return _match(f"{key}.{param}", self.enum)


def load(path: Path) -> Allowlist:
    if not path.exists():
        return Allowlist()
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    return Allowlist(
        coverage={k: list(v) for k, v in data.get("coverage", {}).items()},
        surface_only={k: list(v) for k, v in data.get("surface_only", {}).items()},
        order={k: list(v) for k, v in data.get("order", {}).items()},
        default=list(data.get("default", {}).get("params", [])),
        core_default=list(data.get("core_default", {}).get("params", [])),
        enum=list(data.get("enum", {}).get("params", [])),
        input_naming=list(data.get("input_naming", {}).get("keys", [])),
        input_roles=list(data.get("tuning", {}).get("input_roles", [])),
        handle_prefixes=list(data.get("tuning", {}).get("handle_prefixes", [])),
    )
