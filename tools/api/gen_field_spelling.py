"""Generate the per-surface field-spelling tables the refusal translation points read.

A core refusal names its subject in the C-ABI spelling. Each surface respells it
the way its caller wrote it: Node and WASM by the mechanical snake->camel rule,
Python by identity. A row here is a subject whose facade binding departs from
that rule. Positional parameters and struct-mirroring records are paired by the
parity extractors (``tools/parity/facts.field_bindings``). A C subject those
leave unbound is looked up among the facade names on the same entry that pair
with no C subject, plus on Node and WASM the fields of the entry's object-typed
parameters (``ts_request_fields.cjs``), by canonical name
and then, on every surface, by the per-surface ruled renames in
``field_spelling_renames.toml``, which bind only where the entry carries the
renamed spelling unpaired. A subject still unbound is reported, not tabled, and a
ruled rename that binds nowhere fails the run.

Usage: python3 tools/api/gen_field_spelling.py [--check] [--report]
"""

from __future__ import annotations

import json
import subprocess
import sys
import tomllib
from collections import defaultdict
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/parity"))

CPP_TARGETS = {
    "node": ROOT / "bindings/node/src/addon/field_spelling.h",
    "wasm": ROOT / "src/wasm/field_spelling.h",
}
PY_TARGET = ROOT / "bindings/python/src/libsonare/_field_spelling.py"
RENAMES = ROOT / "tools/api/field_spelling_renames.toml"
SURFACES = ("node", "wasm", "python")


def mechanical(subject: str, surface: str) -> str:
    """The spelling a surface gets with no table row: camel for Node/WASM, identity for Python."""
    if surface == "python":
        return subject
    head, *rest = subject.split("_")
    return head + "".join(p[:1].upper() + p[1:] for p in rest)


@dataclass(frozen=True)
class Row:
    c_key: str
    c_subject: str
    spelling: str


@dataclass(frozen=True)
class Unbound:
    surface: str
    c_key: str
    c_subject: str
    kind: str
    location: str


def camel_to_snake(name: str) -> str:
    """The canonical snake_case form the parity extractors give a camelCase name."""
    out = []
    for i, c in enumerate(name):
        boundary = (
            c.isupper()
            and i > 0
            and (
                name[i - 1].islower()
                or name[i - 1].isdigit()
                or (i + 1 < len(name) and name[i + 1].islower() and name[i - 1].isupper())
            )
        )
        out.append(("_" if boundary else "") + c.lower())
    return "".join(out)


def entry_key(entry: str) -> str:
    """The canonical C key a facade entry (``fn`` or ``Class.method``) answers to."""
    return camel_to_snake(entry.replace(".", "_"))


def derive(
    bindings: Iterable,
    request_fields: dict[str, list[dict]] | None = None,
    renames: dict[str, dict[str, str]] | None = None,
) -> tuple[dict[str, list[Row]], list[Unbound], list[tuple[str, str]]]:
    """Table rows per surface, the subjects no facade binds, and the ruled renames that bound nothing."""
    bindings = list(bindings)
    request_fields = request_fields or {}
    renames = renames or {}
    names: dict[tuple[str, str], set[str]] = defaultdict(set)
    for surface, entries in request_fields.items():
        for row in entries:
            names[(surface, entry_key(row["entry"]))].update(row["fields"])
    for b in bindings:
        if b.c_subject is None:
            names[(b.surface, b.c_key)].add(b.surface_spelling)
    rows: dict[str, set[Row]] = {s: set() for s in SURFACES}
    unbound: list[Unbound] = []
    used: set[tuple[str, str]] = set()

    def bind(surface: str, key: str, subject: str, spelling: str) -> None:
        if spelling != mechanical(subject, surface):
            rows[surface].add(Row(key, subject, spelling))

    for b in bindings:
        if b.surface not in rows or b.c_subject is None:
            continue
        if b.surface_spelling is not None:
            bind(b.surface, b.c_key, b.c_subject, b.surface_spelling)
            continue
        known = names[(b.surface, b.c_key)]
        by_canonical = {camel_to_snake(f): f for f in known}
        renamed = renames.get(b.surface, {}).get(b.c_subject)
        if b.c_subject in by_canonical:
            bind(b.surface, b.c_key, b.c_subject, by_canonical[b.c_subject])
        elif renamed in known:
            bind(b.surface, b.c_key, b.c_subject, renamed)
            used.add((b.surface, b.c_subject))
        else:
            unbound.append(Unbound(b.surface, b.c_key, b.c_subject, b.kind, f"{b.file}:{b.line}"))
    unbound.sort(key=lambda u: (u.surface, u.c_key, u.c_subject))
    idle = sorted(
        (surface, subject)
        for surface, table in renames.items()
        for subject in table
        if (surface, subject) not in used
    )
    return (
        {s: sorted(r, key=lambda x: (x.c_key, x.c_subject)) for s, r in rows.items()},
        unbound,
        idle,
    )


def render_cpp(surface: str, rows: list[Row]) -> str:
    body = "".join(f'\n    {{"{r.c_key}", "{r.c_subject}", "{r.spelling}"}},' for r in rows)
    body += "\n" if rows else ""
    return f"""// Generated by tools/api/gen_field_spelling.py from the parity facts; do not edit.
#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace sonare_field_spelling {{

struct Override {{
  std::string_view entry;
  std::string_view subject;
  std::string_view spelling;
}};

// (C entry key, C-ABI subject) pairs the {surface} facade binds under another name, sorted.
inline constexpr std::array<Override, {len(rows)}> kOverrides = {{{{{body}}}}};

// The {surface} caller's spelling of @p subject on C entry @p entry (no sonare_ prefix).
inline std::string SurfaceSpelling(std::string_view entry, std::string_view subject) {{
  const auto it =
      std::lower_bound(kOverrides.begin(), kOverrides.end(), Override{{entry, subject, {{}}}},
                       [](const Override& a, const Override& b) {{
                         return a.entry != b.entry ? a.entry < b.entry : a.subject < b.subject;
                       }});
  if (it != kOverrides.end() && it->entry == entry && it->subject == subject) {{
    return std::string(it->spelling);
  }}
  std::string out;
  bool upper = false;
  for (char c : subject) {{
    if (c == '_') {{
      upper = true;
      continue;
    }}
    out += upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    upper = false;
  }}
  return out;
}}

}}  // namespace sonare_field_spelling
"""


def render_py(rows: list[Row]) -> str:
    body = "".join(f'\n    ("{r.c_key}", "{r.c_subject}"): "{r.spelling}",' for r in rows)
    body += "\n" if rows else ""
    return f'''"""Generated by tools/api/gen_field_spelling.py from the parity facts; do not edit."""

# (C entry key, C-ABI subject) pairs whose Python keyword is not the C spelling.
OVERRIDES: dict[tuple[str, str], str] = {{{body}}}


def surface_spelling(entry: str, subject: str) -> str:
    """The Python caller's spelling of ``subject`` on C entry ``entry`` (no sonare_ prefix)."""
    return OVERRIDES.get((entry, subject), subject)
'''


def load_bindings() -> Callable[[Path], list]:
    try:
        from facts import field_bindings
    except ImportError as error:
        raise SystemExit(
            f"field spelling: tools/parity/facts.py is unavailable ({error})"
        ) from error
    return field_bindings


def load_request_fields() -> dict[str, list[dict]]:
    script = ROOT / "tools/api/ts_request_fields.cjs"
    return {
        surface: json.loads(
            subprocess.run(
                ["node", str(script), str(ROOT / "bindings" / surface)],
                check=True,
                capture_output=True,
                text=True,
            ).stdout
        )
        for surface in ("node", "wasm")
    }


def main(argv: list[str], field_bindings: Callable[[Path], list] | None = None) -> int:
    bindings = (field_bindings or load_bindings())(ROOT)
    if not bindings:
        print("field spelling: the parity facts paired no subject", file=sys.stderr)
        return 1
    renames = tomllib.loads(RENAMES.read_text(encoding="utf-8"))
    rows, unbound, idle = derive(bindings, load_request_fields(), renames)
    outputs = {CPP_TARGETS[s]: render_cpp(s, rows[s]) for s in CPP_TARGETS}
    outputs[PY_TARGET] = render_py(rows["python"])
    check = "--check" in argv
    stale = []
    for target, text in outputs.items():
        current = target.read_text(encoding="utf-8") if target.exists() else None
        if current == text:
            continue
        if check:
            stale.append(str(target.relative_to(ROOT)))
        else:
            target.write_text(text, encoding="utf-8")
    counts = " ".join(f"{s}={len(rows[s])}" for s in SURFACES)
    per_surface: dict[tuple[str, str], int] = defaultdict(int)
    for u in unbound:
        per_surface[(u.surface, u.kind)] += 1
    unbound_counts = " ".join(
        f"{s}={per_surface[(s, 'param')]}+{per_surface[(s, 'record_field')]}" for s in SURFACES
    )
    print(
        f"field spelling: overrides {counts}; subjects with no facade binding (param+record_field) {unbound_counts}"
    )
    if "--report" in argv:
        for u in unbound:
            print(f"  unbound {u.surface} {u.kind} {u.c_key}.{u.c_subject} ({u.location})")
    if idle:
        print(
            "field spelling: renames that bind no subject: "
            + ", ".join(f"{s}.{subject}" for s, subject in idle),
            file=sys.stderr,
        )
    if stale:
        print(
            f"field spelling: stale {', '.join(stale)}; run tools/api/gen_field_spelling.py",
            file=sys.stderr,
        )
        return 1
    return 1 if idle else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
