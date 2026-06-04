"""Render a parity Report as markdown (default) or JSON."""

from __future__ import annotations

import json
from dataclasses import asdict

from compare import Report

_CATEGORIES = (
    "coverage",
    "default",
    "core_default",
    "order",
    "input",
    "enum",
    "wasm_internal",
)
_CAT_TITLE = {
    "coverage": "Coverage gaps / surface-only symbols",
    "default": "Default drift (cross-facade)",
    "core_default": "Core-default drift (facade vs C++ core struct)",
    "order": "Argument order / count / name mismatch (vs C, config params)",
    "input": "Audio-input param naming consistency",
    "enum": "Enum value-set mismatch",
    "wasm_internal": "WASM-internal wiring (embind vs SonareModule vs index.ts)",
}


def to_json(rep: Report) -> str:
    payload = {
        "surfaces": rep.surfaces,
        "unparsed": rep.unparsed,
        "unparsed_notes": rep.unparsed_notes,
        "surface_only": rep.surface_only,
        "handle_keys": rep.handle_keys,
        "matrix": rep.matrix,
        "findings": [asdict(f) for f in rep.findings],
        "summary": _summary(rep),
    }
    return json.dumps(payload, indent=2, sort_keys=False)


def _summary(rep: Report) -> dict:
    active = rep.active()
    info = [f for f in rep.reported() if f.informational]
    by_cat_active = {c: 0 for c in _CATEGORIES}
    by_cat_info = {c: 0 for c in _CATEGORIES}
    for f in active:
        by_cat_active[f.category] = by_cat_active.get(f.category, 0) + 1
    for f in info:
        by_cat_info[f.category] = by_cat_info.get(f.category, 0) + 1
    allowlisted = [f for f in rep.findings if f.allowlisted]
    allow_by_cat = {c: 0 for c in _CATEGORIES}
    allow_by_surface: dict[str, int] = {}
    for f in allowlisted:
        allow_by_cat[f.category] = allow_by_cat.get(f.category, 0) + 1
        allow_by_surface[f.surface] = allow_by_surface.get(f.surface, 0) + 1
    return {
        "total_findings": len(rep.findings),
        "active_findings": len(active),
        "informational_findings": len(info),
        "allowlisted": len(allowlisted),
        "by_category_active": by_cat_active,
        "by_category_informational": by_cat_info,
        "allowlisted_by_category": allow_by_cat,
        "allowlisted_by_surface": allow_by_surface,
    }


def to_markdown(rep: Report) -> str:
    out: list[str] = []
    out.append("# libsonare cross-binding parity report\n")
    out.append(f"Surfaces compared (C is canonical): {', '.join(rep.surfaces)}\n")

    # Coverage matrix summary.
    out.append("## Coverage matrix summary\n")
    out.append("| surface | functions | of canonical C | surface-only |")
    out.append("|---|---|---|---|")
    c_keys = {k for k, row in rep.matrix.items() if row.get("c")}
    for s in rep.surfaces:
        present = [k for k, row in rep.matrix.items() if row.get(s)]
        of_c = sum(1 for k in present if k in c_keys)
        only = len(rep.surface_only.get(s, []))
        out.append(f"| {s} | {len(present)} | {of_c}/{len(c_keys)} | {only} |")
    out.append("")
    out.append(
        f"C functions classified as handle/class API (informational coverage): "
        f"**{len(rep.handle_keys)}** of {len(c_keys)}.\n"
    )

    # Self-confidence: unparsed counts.
    out.append("## Parser confidence (unparsed declarations)\n")
    out.append("| surface | unparsed |")
    out.append("|---|---|")
    for s in rep.surfaces:
        out.append(f"| {s} | {rep.unparsed.get(s, 0)} |")
    out.append("")

    summary = _summary(rep)
    out.append("## Findings summary\n")
    out.append(
        f"Active findings (count toward CI failure): **{summary['active_findings']}** | "
        f"informational: {summary['informational_findings']} | "
        f"allowlisted (suppressed): {summary['allowlisted']}\n"
    )
    out.append("| category | active | informational | allowlisted |")
    out.append("|---|---|---|---|")
    for c in _CATEGORIES:
        out.append(
            f"| {_CAT_TITLE[c]} | {summary['by_category_active'][c]} "
            f"| {summary['by_category_informational'][c]} "
            f"| {summary['allowlisted_by_category'][c]} |"
        )
    out.append("")

    # Intentional exclusions (allowlisted) per surface: these are the divergences
    # the allowlist deliberately suppresses (each with a documented reason in
    # allowlist.toml). Surfaced so the suppressed total stays auditable, not a
    # silent number.
    by_surface = summary["allowlisted_by_surface"]
    out.append(
        "Intentional exclusions (allowlisted, suppressed) by surface: "
        + (
            ", ".join(f"{s} {by_surface[s]}" for s in rep.surfaces if by_surface.get(s))
            or "_none_"
        )
        + "\n"
    )

    reported = rep.reported()

    # Per-category sections (active first, then informational subsection).
    for c in _CATEGORIES:
        active_items = [f for f in reported if f.category == c and not f.informational]
        info_items = [f for f in reported if f.category == c and f.informational]
        out.append(f"## {_CAT_TITLE[c]} — active ({len(active_items)})\n")
        if not active_items:
            out.append("_none_\n")
        else:
            for f in sorted(active_items, key=lambda x: (x.key, x.surface)):
                loc = f" — `{f.location}`" if f.location else ""
                out.append(f"- **{f.key}** [{f.surface}]: {f.message}{loc}")
            out.append("")
        if info_items:
            out.append(f"### {_CAT_TITLE[c]} — informational ({len(info_items)})\n")
            shown = sorted(info_items, key=lambda x: (x.key, x.surface))
            limit = 40
            for f in shown[:limit]:
                loc = f" — `{f.location}`" if f.location else ""
                out.append(f"- _{f.key}_ [{f.surface}]: {f.message}{loc}")
            if len(shown) > limit:
                out.append(f"- ... and {len(shown) - limit} more (see --json)")
            out.append("")

    # Handle/class API listing.
    out.append("## Handle/class API (informational)\n")
    out.append(
        "These C functions are handle/class-based (create/destroy/handle ops) and are "
        "exposed as class methods on the facades rather than free functions; they are "
        "excluded from free-function coverage-gap failures.\n"
    )
    out.append(", ".join(f"`{k}`" for k in rep.handle_keys) or "_none_")
    out.append("")

    # Coverage matrix (compact): only rows that are not fully present.
    out.append("## Coverage matrix (incomplete rows only)\n")
    cols = rep.surfaces
    out.append("| function | " + " | ".join(cols) + " |")
    out.append("|" + "---|" * (len(cols) + 1))
    rows_shown = 0
    for k in sorted(rep.matrix):
        row = rep.matrix[k]
        if all(row.get(s) for s in cols):
            continue
        cells = " | ".join("✓" if row.get(s) else "·" for s in cols)
        out.append(f"| {k} | {cells} |")
        rows_shown += 1
    if rows_shown == 0:
        out.append("_all functions present on all selected surfaces_")
    out.append("")

    return "\n".join(out)
