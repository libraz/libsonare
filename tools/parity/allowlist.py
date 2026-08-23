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
* ``[wasm_internal]`` list of WASM embind/SonareModule ``names`` whose
                  intra-binding wiring inconsistency is intentional.
* ``[record]``    three entry kinds, narrowest first:

  - ``fields``      a list of ``"record_key.field_name"`` for a single field that
                    legitimately differs. Surface-independent on purpose: a field
                    dropped for one facade's convention is nearly always dropped
                    for all of them, and a per-surface exception is the shape
                    drift looks like.
  - ``extra_fields`` surface -> record keys whose facade record is a RICHER read
                    model than the C struct, so fields it declares beyond the C
                    field list are expected. Suppresses only that direction — a C
                    field the facade FAILS to declare still reports. Use this
                    rather than ``records`` for a record under active
                    development, so the entry does not have to name (and thereby
                    pre-bless) fields that do not exist yet.
  - ``records``     surface -> record keys whose whole shape is intentionally not
                    mirrored there. This is the blunt one: it suppresses missing
                    C fields too, so it goes blind to exactly the drift this unit
                    exists to catch. Prefer ``extra_fields`` or ``fields``.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path


def _match(name: str, patterns: list[str], used: set[str] | None = None) -> bool:
    """Whether @p name matches any pattern, recording the pattern that did.

    Recording is what makes a STALE entry visible. An allowlist pattern only
    matches while the divergence it excuses still exists, so a pattern nothing
    consults has outlived its reason -- and a pattern that outlives its reason is
    not inert: it silently pre-blesses whatever takes that name next, which is
    the failure mode an allowlist is least able to survive.
    """
    for p in patterns:
        matched = (
            p == name
            or (p.startswith("*") and name.endswith(p[1:]))
            or (p.endswith("*") and name.startswith(p[:-1]))
        )
        if matched:
            if used is not None:
                used.add(p)
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
    wasm_internal: list[str] = field(default_factory=list)
    record: dict[str, list[str]] = field(default_factory=dict)
    record_extra: dict[str, list[str]] = field(default_factory=dict)
    record_fields: list[str] = field(default_factory=list)
    # Overrides for the central tuning knobs (empty -> use compare.py defaults).
    input_roles: list[str] = field(default_factory=list)
    handle_prefixes: list[str] = field(default_factory=list)
    # Patterns that actually suppressed something during the last comparison,
    # keyed by "<section>[.<surface>]" so a report can name the TOML table an
    # unused entry sits in.
    used: dict[str, set[str]] = field(default_factory=dict)

    def _mark(self, scope: str, name: str, patterns: list[str]) -> bool:
        return _match(name, patterns, self.used.setdefault(scope, set()))

    def unused_entries(self) -> list[tuple[str, str]]:
        """(scope, pattern) pairs no comparison consulted, in report order."""
        declared: list[tuple[str, str]] = []
        for surface, patterns in self.coverage.items():
            declared += [(f"coverage.{surface}", p) for p in patterns]
        for surface, patterns in self.surface_only.items():
            declared += [(f"surface_only.{surface}", p) for p in patterns]
        for surface, patterns in self.order.items():
            declared += [(f"order.{surface}", p) for p in patterns]
        declared += [("default.params", p) for p in self.default]
        declared += [("core_default.params", p) for p in self.core_default]
        declared += [("enum.params", p) for p in self.enum]
        declared += [("input_naming.keys", p) for p in self.input_naming]
        declared += [("wasm_internal.names", p) for p in self.wasm_internal]
        declared += [("record.fields", p) for p in self.record_fields]
        for surface, patterns in self.record.items():
            declared += [(f"record.records.{surface}", p) for p in patterns]
        for surface, patterns in self.record_extra.items():
            declared += [(f"record.extra_fields.{surface}", p) for p in patterns]
        # `surface_only.any` is consulted under the querying surface's scope, so
        # fold every surface_only scope together before deciding it is unused.
        surface_only_used: set[str] = set()
        for scope, names in self.used.items():
            if scope.startswith("surface_only."):
                surface_only_used |= names
        unused = []
        for scope, pattern in declared:
            pool = (
                surface_only_used
                if scope.startswith("surface_only.")
                else self.used.get(scope, set())
            )
            if pattern not in pool:
                unused.append((scope, pattern))
        return unused

    def coverage_ok(self, key: str, surface: str) -> bool:
        return self._mark(f"coverage.{surface}", key, self.coverage.get(surface, []))

    def input_naming_ok(self, key: str) -> bool:
        return self._mark("input_naming.keys", key, self.input_naming)

    def surface_only_ok(self, key: str, surface: str) -> bool:
        scope = f"surface_only.{surface}"
        if self._mark(scope, key, self.surface_only.get(surface, [])):
            return True
        return self._mark(scope, key, self.surface_only.get("any", []))

    def order_ok(self, key: str, surface: str) -> bool:
        return self._mark(f"order.{surface}", key, self.order.get(surface, []))

    def default_ok(self, key: str, param: str) -> bool:
        return self._mark("default.params", f"{key}.{param}", self.default)

    def core_default_ok(self, key: str, param: str) -> bool:
        return self._mark("core_default.params", f"{key}.{param}", self.core_default)

    def enum_ok(self, key: str, param: str) -> bool:
        return self._mark("enum.params", f"{key}.{param}", self.enum)

    def wasm_internal_ok(self, name: str) -> bool:
        return self._mark("wasm_internal.names", name, self.wasm_internal)

    def record_ok(self, key: str, surface: str) -> bool:
        scope = f"record.records.{surface}"
        return self._mark(scope, key, self.record.get(surface, [])) or self._mark(
            "record.records.any", key, self.record.get("any", [])
        )

    def record_extra_ok(self, key: str, surface: str) -> bool:
        """True when EXTRA fields on this record are expected on ``surface``.

        Missing C fields on the same record still report — this is deliberately
        one-directional.
        """
        scope = f"record.extra_fields.{surface}"
        return self._mark(scope, key, self.record_extra.get(surface, [])) or self._mark(
            "record.extra_fields.any", key, self.record_extra.get("any", [])
        )

    def record_field_ok(self, key: str, field_name: str) -> bool:
        return self._mark("record.fields", f"{key}.{field_name}", self.record_fields)


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
        wasm_internal=list(data.get("wasm_internal", {}).get("names", [])),
        record={
            k: list(v) for k, v in data.get("record", {}).get("records", {}).items()
        },
        record_extra={
            k: list(v)
            for k, v in data.get("record", {}).get("extra_fields", {}).items()
        },
        record_fields=list(data.get("record", {}).get("fields", [])),
        input_roles=list(data.get("tuning", {}).get("input_roles", [])),
        handle_prefixes=list(data.get("tuning", {}).get("handle_prefixes", [])),
    )
