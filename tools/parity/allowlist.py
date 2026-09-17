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

from dataclasses import dataclass, field
from pathlib import Path

import tomllib

#: What a comparison concluded about one name, passed by every consult site.
#: The allowlist is asked only once the comparison has a verdict, so that "the
#: entry was looked up" cannot be read as "the entry suppressed something". A
#: check that cannot compare at all asks nothing: it records the declined
#: comparison on the report instead, where the derived fact expires by itself.
DIVERGED = "diverged"  # compared, and the surfaces disagreed
AGREED = "agreed"  # compared, and the surfaces matched

#: What the audit concludes about one declared entry. STALE and UNCONSULTED both
#: fail: an entry standing in front of a comparison that agrees, and one
#: standing in front of a comparison that never runs, are each a decision the
#: tool now derives on its own.
SUPPRESSED = "suppressed"
STALE = "stale"
UNCONSULTED = "unconsulted"


def _match(name: str, patterns: list[str], used: set[str] | None = None) -> str | None:
    """The first pattern matching @p name, recorded in @p used; None if none do.

    Recording is what makes a STALE entry visible, and it is recorded per
    verdict: a pattern a comparison reached and then AGREED with has outlived
    its reason, and a pattern that outlives its reason is not inert -- it
    silently pre-blesses whatever takes that name next, which is the failure
    mode an allowlist is least able to survive.
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
            return p
    return None


#: Sections held at zero entries, and the knob for letting one take its first.
#: Both describe what a facade computes or what it will accept, not how a name
#: is spelled, so an entry is far more often a surface that stopped agreeing
#: than the reviewed alias the section was written for. Both are empty today.
#: Taking a section off this tuple is how it stops being -- do that in the same
#: change as the entry, so the widening is reviewed rather than inherited.
RATCHETED_SECTIONS = ("core_default", "enum")


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
    # Patterns a comparison reached during the last run, bucketed by the verdict
    # it reached them with: verdict -> "<section>[.<surface>]" -> patterns. The
    # scope key lets a report name the TOML table an expired entry sits in.
    seen: dict[str, dict[str, set[str]]] = field(default_factory=dict)
    # What each pattern actually suppressed: (scope, pattern) -> the set of
    # divergences it excused. See :meth:`suppressed_divergences`.
    _suppressed: dict[tuple[str, str], set[str]] = field(default_factory=dict)

    def _mark(
        self,
        scope: str,
        name: str,
        patterns: list[str],
        verdict: str,
        divergence: str = "",
    ) -> bool:
        """Record @p name's verdict against @p patterns; True only when excused.

        A match under AGREED returns False: there is no divergence for the
        caller to suppress, only a pattern to account for.
        """
        bucket = self.seen.setdefault(verdict, {}).setdefault(scope, set())
        pattern = _match(name, patterns, bucket)
        if pattern is None or verdict != DIVERGED:
            return False
        self._suppressed.setdefault((scope, pattern), set()).add(divergence or name)
        return True

    def suppressed_divergences(self) -> dict[tuple[str, str], list[str]]:
        """(scope, pattern) -> the divergences that pattern excused, sorted.

        A pattern selects on the NAME; the reason beside it in ``allowlist.toml``
        argues about one divergence inside that name. The two are different
        properties, so an entry stays live while excusing something its reason
        never described -- and no checker can read the prose to tell. This is the
        material for reading the two against each other by hand; nothing here is
        matched against a reason automatically.

        The set is built to be comparable against a later run's, which is the
        form that would catch that drift mechanically: a pattern alone absorbs
        whatever takes the name next, and a count alone misses an identity that
        swaps at constant population. **No comparison is implemented here** --
        nothing reads a previous run's record, and no expected set is written to
        ``allowlist.toml``. So each divergence is spelled from names and declared
        values only: no file, no line number, no iteration order.
        """
        return {key: sorted(values) for key, values in self._suppressed.items()}

    def _declared_entries(self) -> list[tuple[str, str]]:
        """Every (scope, pattern) pair the file declares, in report order."""
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
        return declared

    def _pool(self, verdict: str, scope: str) -> set[str]:
        """Patterns reached under @p verdict that count toward @p scope."""
        by_scope = self.seen.get(verdict, {})
        if not scope.startswith("surface_only."):
            return by_scope.get(scope, set())
        # `surface_only.any` is consulted under the querying surface's scope, so
        # fold every surface_only scope together before judging one of them.
        folded: set[str] = set()
        for other, names in by_scope.items():
            if other.startswith("surface_only."):
                folded |= names
        return folded

    def classify(self) -> list[tuple[str, str, str]]:
        """(scope, pattern, state) for every declared entry, in report order.

        One comparison the entry excused keeps it alive whatever the others
        concluded, so SUPPRESSED is decided first.
        """
        out: list[tuple[str, str, str]] = []
        for scope, pattern in self._declared_entries():
            if pattern in self._pool(DIVERGED, scope):
                state = SUPPRESSED
            elif pattern in self._pool(AGREED, scope):
                state = STALE
            else:
                state = UNCONSULTED
            out.append((scope, pattern, state))
        return out

    def expired_entries(self) -> list[tuple[str, str, str]]:
        """Entries the audit fails on: compared and agreed, or never looked up."""
        return [e for e in self.classify() if e[2] in (STALE, UNCONSULTED)]

    def ratcheted_entries(self) -> list[tuple[str, str]]:
        """Entries in a section currently held at zero, as (scope, pattern)."""
        return [(f"{section}.params", pattern)
                for section in RATCHETED_SECTIONS
                for pattern in getattr(self, section)]

    # Each accessor below takes the verdict its caller reached and returns True
    # only for DIVERGED, so every consult site has to state what it compared
    # before it is told whether the divergence is excused. A check that could
    # not compare at all calls none of them.

    def coverage_ok(self, key: str, surface: str, verdict: str, div: str = "") -> bool:
        return self._mark(
            f"coverage.{surface}", key, self.coverage.get(surface, []), verdict, div
        )

    def input_naming_ok(self, key: str, verdict: str, div: str = "") -> bool:
        return self._mark("input_naming.keys", key, self.input_naming, verdict, div)

    def surface_only_ok(
        self, key: str, surface: str, verdict: str, div: str = ""
    ) -> bool:
        scope = f"surface_only.{surface}"
        if self._mark(scope, key, self.surface_only.get(surface, []), verdict, div):
            return True
        return self._mark(scope, key, self.surface_only.get("any", []), verdict, div)

    def order_ok(self, key: str, surface: str, verdict: str, div: str = "") -> bool:
        return self._mark(
            f"order.{surface}", key, self.order.get(surface, []), verdict, div
        )

    def default_ok(self, key: str, param: str, verdict: str, div: str = "") -> bool:
        return self._mark(
            "default.params", f"{key}.{param}", self.default, verdict, div
        )

    def core_default_ok(
        self, key: str, param: str, verdict: str, div: str = ""
    ) -> bool:
        return self._mark(
            "core_default.params", f"{key}.{param}", self.core_default, verdict, div
        )

    def enum_ok(self, key: str, param: str, verdict: str, div: str = "") -> bool:
        return self._mark("enum.params", f"{key}.{param}", self.enum, verdict, div)

    def wasm_internal_ok(self, name: str, verdict: str, div: str = "") -> bool:
        return self._mark(
            "wasm_internal.names", name, self.wasm_internal, verdict, div
        )

    def record_ok(self, key: str, surface: str, verdict: str, div: str = "") -> bool:
        scope = f"record.records.{surface}"
        return self._mark(
            scope, key, self.record.get(surface, []), verdict, div
        ) or self._mark(
            "record.records.any", key, self.record.get("any", []), verdict, div
        )

    def record_extra_ok(
        self, key: str, surface: str, verdict: str, div: str = ""
    ) -> bool:
        """True when EXTRA fields on this record are expected on ``surface``.

        Missing C fields on the same record still report — this is deliberately
        one-directional.
        """
        scope = f"record.extra_fields.{surface}"
        return self._mark(
            scope, key, self.record_extra.get(surface, []), verdict, div
        ) or self._mark(
            "record.extra_fields.any",
            key,
            self.record_extra.get("any", []),
            verdict,
            div,
        )

    def record_field_ok(
        self, key: str, field_name: str, verdict: str, div: str = ""
    ) -> bool:
        return self._mark(
            "record.fields", f"{key}.{field_name}", self.record_fields, verdict, div
        )


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
