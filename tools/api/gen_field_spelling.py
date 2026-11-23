"""Generate the per-surface field-spelling tables the refusal translation points read.

A core refusal names its subject in the C-ABI spelling. Each surface respells it
the way its caller wrote it: Node and WASM by the mechanical snake->camel rule,
Python by identity. A row here is a subject whose facade binding departs from
that rule. Positional parameters and struct-mirroring records are paired by the
parity extractors (``tools/parity/facts.field_bindings``). A C subject those
leave unbound is looked up among the facade names on the same entry, plus on Node
and WASM the fields of the entry's object-typed parameters (``ts_request_fields.cjs``),
by canonical name and then, on every surface, by the per-surface ruled renames in
``field_spelling_renames.toml``, which bind only where the entry carries the renamed
spelling. A ``[<surface>.length_of]`` row binds an entry's count subject to the array
keyword whose length it is, and outranks a rename ruled for that subject; the refusal
names it as "<keyword> length" in Python and "<keyword>.length" on Node and WASM. It also
binds a count the parity extractors fold into its array, which carries no pairing,
when the C declaration takes it (a struct parameter's fields included) and the facade
takes the array. A name another C subject already pairs with counts: a second form folded
into the entry (an ``_ex``) can name the same argument differently. A subject still unbound is reported, not tabled, and a
ruled rename that binds nowhere fails the run. Every unbound parameter carries a
reason in ``field_spelling_unbound.toml``; unbound record fields are output records
and are counted only.

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
UNBOUND = ROOT / "tools/api/field_spelling_unbound.toml"
UNBOUND_KINDS = ("derived length", "output", "C-only plumbing", "folded into ", "not exposed")
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
    # True when the subject is the length of the array ``spelling`` names (a ``length_of`` row).
    length: bool = False


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
    renames: dict[str, dict[str, str | list[str]]] | None = None,
    c_params: dict[str, set[str]] | None = None,
) -> tuple[dict[str, list[Row]], list[Unbound], list[tuple[str, str, str]]]:
    """Table rows per surface, the subjects no facade binds, and the ruled renames that bound nothing.

    ``c_params`` maps an entry key to its C parameter names, for the counts a facade
    derives from an array and the extractors therefore leave unpaired.
    """
    bindings = list(bindings)
    request_fields = request_fields or {}
    renames = renames or {}
    c_params = c_params or {}
    names: dict[tuple[str, str], set[str]] = defaultdict(set)
    for surface, entries in request_fields.items():
        for row in entries:
            names[(surface, entry_key(row["entry"]))].update(row["fields"])
    for b in bindings:
        if b.surface_spelling is not None:
            names[(b.surface, b.c_key)].add(b.surface_spelling)
    rows: dict[str, set[Row]] = {s: set() for s in SURFACES}
    unbound: list[Unbound] = []
    used: set[tuple[str, str, str]] = set()

    lengths = {s: dict(renames.get(s, {}).get("length_of", {})) for s in SURFACES}
    renames = {
        s: {k: v for k, v in table.items() if k != "length_of"} for s, table in renames.items()
    }

    def spellings(surface: str, subject: str) -> list[str]:
        value = renames.get(surface, {}).get(subject, [])
        return [value] if isinstance(value, str) else list(value)

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
        renamed = [r for r in spellings(b.surface, b.c_subject) if r in known]
        array = lengths[b.surface].get(f"{b.c_key}.{b.c_subject}")
        if b.c_subject in by_canonical:
            bind(b.surface, b.c_key, b.c_subject, by_canonical[b.c_subject])
        elif array is not None and array in known:
            # An entry's own length_of row outranks a rename ruled for the subject on every entry.
            rows[b.surface].add(Row(b.c_key, b.c_subject, array, True))
            used.add((b.surface, f"length_of.{b.c_key}.{b.c_subject}", array))
        elif len(renamed) == 1:
            bind(b.surface, b.c_key, b.c_subject, renamed[0])
            used.add((b.surface, b.c_subject, renamed[0]))
        else:
            unbound.append(Unbound(b.surface, b.c_key, b.c_subject, b.kind, f"{b.file}:{b.line}"))
    paired = {(b.surface, b.c_key, b.c_subject) for b in bindings if b.c_subject is not None}
    for surface, table in lengths.items():
        for name, array in table.items():
            key, _, subject = name.partition(".")
            declared = c_params.get(key, set())
            # An array the extractors leave unpaired (a caller-filled record list) still counts
            # when the C declaration takes it under the spelling the surface gives it by rule.
            takes_array = array in names[(surface, key)] or any(
                mechanical(c, surface) == array for c in declared
            )
            if (surface, key, subject) not in paired and subject in declared and takes_array:
                rows[surface].add(Row(key, subject, array, True))
                used.add((surface, f"length_of.{name}", array))
    unbound.sort(key=lambda u: (u.surface, u.c_key, u.c_subject))
    idle = sorted(
        (surface, subject, spelling)
        for surface, table in renames.items()
        for subject in table
        for spelling in spellings(surface, subject)
        if (surface, subject, spelling) not in used
    ) + sorted(
        (surface, f"length_of.{name}", array)
        for surface, table in lengths.items()
        for name, array in table.items()
        if (surface, f"length_of.{name}", array) not in used
    )
    return (
        {s: sorted(r, key=lambda x: (x.c_key, x.c_subject)) for s, r in rows.items()},
        unbound,
        idle,
    )


def _cpp_array(rows: list[Row]) -> str:
    body = "".join(f'\n    {{"{r.c_key}", "{r.c_subject}", "{r.spelling}"}},' for r in rows)
    return body + ("\n" if rows else "")


def render_cpp(surface: str, rows: list[Row]) -> str:
    renames = [r for r in rows if not r.length]
    lengths = [r for r in rows if r.length]
    return f"""// Generated by tools/api/gen_field_spelling.py from the parity facts; do not edit.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "c_api/sonare_c_last_refusal.h"
#include "util/parameter_checks.h"

namespace sonare_field_spelling::{surface} {{

struct Override {{
  std::string_view entry;
  std::string_view subject;
  std::string_view spelling;
}};

// (C entry key, C-ABI subject) pairs the {surface} facade binds under another name, sorted.
inline constexpr std::array<Override, {len(renames)}> kOverrides = {{{{{_cpp_array(renames)}}}}};

// (C entry key, C-ABI count subject) pairs whose value is the length of the named array, sorted.
inline constexpr std::array<Override, {len(lengths)}> kLengths = {{{{{_cpp_array(lengths)}}}}};

template <std::size_t N>
inline const Override* Find(const std::array<Override, N>& table, std::string_view entry,
                            std::string_view subject) {{
  const auto it =
      std::lower_bound(table.begin(), table.end(), Override{{entry, subject, {{}}}},
                       [](const Override& a, const Override& b) {{
                         return a.entry != b.entry ? a.entry < b.entry : a.subject < b.subject;
                       }});
  return it != table.end() && it->entry == entry && it->subject == subject ? &*it : nullptr;
}}

// The {surface} caller's spelling of @p subject on C entry @p entry (no sonare_ prefix).
inline std::string SurfaceSpelling(std::string_view entry, std::string_view subject) {{
  if (const Override* length = Find(kLengths, entry, subject)) {{
    return std::string(length->spelling) + ".length";
  }}
  if (const Override* row = Find(kOverrides, entry, subject)) {{
    return std::string(row->spelling);
  }}
  return sonare::util::check::camel_path(std::string(subject));
}}

// The C roots naming an entry's object argument, whose keys the {surface} caller writes bare.
inline constexpr std::array<std::string_view, 2> kObjectRoots = {{"options.", "config."}};

// @p subject without a leading object-argument root, or without "<c_root>." when given.
inline std::string_view Unrooted(std::string_view subject, std::string_view c_root) {{
  for (const std::string_view root : kObjectRoots) {{
    if (subject.substr(0, root.size()) == root) return subject.substr(root.size());
  }}
  if (!c_root.empty() && subject.size() > c_root.size() && subject[c_root.size()] == '.' &&
      subject.substr(0, c_root.size()) == c_root) {{
    return subject.substr(c_root.size() + 1);
  }}
  return subject;
}}

// The last C refusal with each subject in the {surface} caller's spelling, looked up under the
// refused entry; @p c_root strips a "<c_root>." prefix beyond the object-argument roots.
inline std::optional<sonare::SonareParameterRefusal> SpelledLastRefusal(
    std::string_view c_root = {{}}) {{
  const sonare::SonareParameterRefusal* refusal = sonare_c_detail::last_parameter_refusal();
  if (refusal == nullptr) return std::nullopt;
  if (refusal->caller_spelled()) return *refusal;
  const char* entry = sonare_c_detail::last_refusal_entry();
  const std::string_view key = entry != nullptr ? entry : "";
  return refusal->respelled(
      refusal->code(),
      [key, c_root](const std::string& subject) {{
        return SurfaceSpelling(key, Unrooted(subject, c_root));
      }},
      true);
}}

}}  // namespace sonare_field_spelling::{surface}
"""


def _py_dict(rows: list[Row]) -> str:
    body = "".join(f'\n    ("{r.c_key}", "{r.c_subject}"): "{r.spelling}",' for r in rows)
    return body + ("\n" if rows else "")


def render_py(rows: list[Row]) -> str:
    renames = _py_dict([r for r in rows if not r.length])
    lengths = _py_dict([r for r in rows if r.length])
    return f'''"""Generated by tools/api/gen_field_spelling.py from the parity facts; do not edit."""

# (C entry key, C-ABI subject) pairs whose Python keyword is not the C spelling.
OVERRIDES: dict[tuple[str, str], str] = {{{renames}}}

# (C entry key, C-ABI count subject) pairs whose value is the length of the named array.
LENGTHS: dict[tuple[str, str], str] = {{{lengths}}}


def surface_spelling(entry: str, subject: str) -> str:
    """The Python caller's spelling of ``subject`` on C entry ``entry`` (no sonare_ prefix)."""
    length = LENGTHS.get((entry, subject))
    if length is not None:
        return f"{{length}} length"
    return OVERRIDES.get((entry, subject), subject)
'''


def classify(
    unbound: list[Unbound],
    table: dict[str, str],
    pending: dict[str, str] | None = None,
    recorded: dict[str, int] | None = None,
) -> list[str]:
    """Problems with the unbound-parameter classification: missing, stale or of no known kind.

    ``recorded`` holds, per surface, how many unbound parameters are still awaiting a
    classification. A surface whose count matches exactly reports none of them; any other
    count, higher or lower, fails and names them, so the record moves with every change.
    """
    pending = pending or {}
    recorded = recorded or {}
    problems = []
    live = {f"{u.c_key}.{u.c_subject}" for u in unbound if u.kind == "param"}
    waiting: dict[str, list[str]] = defaultdict(list)
    for u in unbound:
        name = f"{u.c_key}.{u.c_subject}"
        if u.kind == "param" and name not in table and name not in pending:
            waiting[u.surface].append(
                f"unclassified unbound {u.surface} parameter {name} ({u.location})"
            )
    for surface in sorted(set(waiting) | set(recorded)):
        if len(waiting[surface]) != recorded.get(surface, 0):
            problems.extend(waiting[surface])
            problems.append(
                f"{len(waiting[surface])} unclassified unbound {surface} parameter(s), "
                f"[unclassified] records {recorded.get(surface, 0)}; update the record"
            )
    for name in sorted(pending):
        if name not in live:
            problems.append(f"{name} is pending but every surface binds it; drop the entry")
    for name, reason in sorted(table.items()):
        if name not in live:
            problems.append(f"{name} is classified but every surface binds it; drop the entry")
        elif not reason.startswith(UNBOUND_KINDS):
            problems.append(f"{name}: '{reason}' starts with none of {', '.join(UNBOUND_KINDS)}")
    return sorted(set(problems))


def struct_entries_without_bindings(bindings: list) -> list[str]:
    try:
        from facts import struct_entries_without_bindings as check
    except ImportError as error:
        raise SystemExit(
            f"field spelling: tools/parity/facts.py is unavailable ({error})"
        ) from error
    return check(ROOT, bindings)


def load_bindings() -> Callable[[Path], list]:
    try:
        from facts import field_bindings
    except ImportError as error:
        raise SystemExit(
            f"field spelling: tools/parity/facts.py is unavailable ({error})"
        ) from error
    return field_bindings


def load_c_params(root: Path) -> dict[str, set[str]]:
    """Entry key -> the parameter names its C declaration takes, folded counts included.

    A struct parameter contributes its fields too, since a facade that takes the struct as
    an object or as keywords is refused under the field's name.
    """
    import check_parity
    from facts import entry_key as facts_entry_key

    facts = check_parity._EXTRACTORS["c"](root)
    fields = {r.raw_name: [f.raw_name for f in r.fields] for r in facts.records}
    params: dict[str, set[str]] = defaultdict(set)
    for function in facts.functions:
        names = params[facts_entry_key(function.key)]
        for p in function.params:
            names.add(p.raw_name)
            names.update(fields.get(p.type.replace("const ", "").rstrip("*").strip(), ()))
    return params


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
    rows, unbound, idle = derive(bindings, load_request_fields(), renames, load_c_params(ROOT))
    classified = tomllib.loads(UNBOUND.read_text(encoding="utf-8"))
    problems = classify(
        unbound, classified["params"], classified["pending"], classified.get("unclassified")
    )
    problems += [
        f"{key} takes a caller-filled struct and no surface binds any of its members"
        for key in struct_entries_without_bindings(bindings)
    ]
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
        f"field spelling: overrides {counts}; subjects with no facade binding (param+record_field) "
        f"{unbound_counts}; the record fields are output records a refusal never names; "
        f"{len(classified['pending'])} pending a rename"
    )
    if "--report" in argv:
        for u in unbound:
            print(f"  unbound {u.surface} {u.kind} {u.c_key}.{u.c_subject} ({u.location})")
    for problem in problems:
        print(f"field spelling: {problem}", file=sys.stderr)
    if idle:
        print(
            "field spelling: renames that bind no subject: "
            + ", ".join(f"{s}.{subject}->{spelling}" for s, subject, spelling in idle),
            file=sys.stderr,
        )
    if stale:
        print(
            f"field spelling: stale {', '.join(stale)}; run tools/api/gen_field_spelling.py",
            file=sys.stderr,
        )
        return 1
    return 1 if idle or problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
