"""Which captures declare no source class, and which voices have no capture on their wanted layer.

A capture with no `source_class` is off-layer wherever its slot is aimed, so it can
never be a fit target (`policy.capture_layer`). A voice whose every reaching capture
is off-layer has no reference to be fitted against: `refit.py` refuses it unless
`--allow-off-layer` is given, and a fit run that way is recorded as provisional.

    python tools/voicematch/layer_report.py [--json]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bank
import policy
import refit
from capture import load_config


def undeclared_captures() -> list[dict]:
    """Every capture definition that declares no `source_class`."""
    out = []
    for path in sorted(bank.CAPTURE_DIR.glob("*.json")):
        if path.name.endswith(".local.json"):
            continue
        cfg = load_config(path)
        if not cfg.get("source_class"):
            out.append({"capture": cfg.get("id", path.stem), "definition": path.name})
    return out


def voices_without_on_layer() -> list[dict]:
    """Melodic voices reached by captures, none of them on the layer the voice's slot is aimed at."""
    rows = json.loads(refit.STATUS.read_text())["voices"]
    pool = bank.captures()
    classes = {c.id: c.source_class for c in pool}
    pol = policy.load()
    copies = refit.struct_copies()
    out = []
    for row in sorted(rows, key=lambda r: (r["bank"], r["program"])):
        if row["kit"] or not row.get("patch"):
            continue
        reaching = refit.capital_gates(row, rows, pool, refit.REFERENCE_DIR, copies)
        if not reaching:
            continue
        layers = {
            g["capture"]: policy.capture_layer(
                classes.get(g["capture"]), row["program"], row["bank"], policy=pol
            )
            for g in reaching
        }
        if any(layer == policy.LAYER_WANTED for layer in layers.values()):
            continue
        want = policy.wanted_layer(pol, row["program"], False, row["bank"])
        out.append(
            {
                "slug": row["slug"],
                "program": row["program"],
                "bank": row["bank"],
                "patch": row["patch"],
                "wanted": want.get("timbre"),
                "captures": [
                    {"capture": cap, "source_class": classes.get(cap), "layer": layer}
                    for cap, layer in layers.items()
                ],
            }
        )
    return out


def build() -> dict:
    undeclared = undeclared_captures()
    voices = voices_without_on_layer()
    return {
        "undeclared_count": len(undeclared),
        "undeclared_captures": undeclared,
        "no_on_layer_count": len(voices),
        "voices_without_on_layer": voices,
    }


def text(report: dict) -> str:
    lines = [f"captures with no source_class declared: {report['undeclared_count']}"]
    lines += [f"  {c['capture']} ({c['definition']})" for c in report["undeclared_captures"]]
    lines.append(f"voices with no on-layer capture: {report['no_on_layer_count']}")
    for v in report["voices_without_on_layer"]:
        have = ", ".join(
            f"{c['capture']}={c['source_class'] or 'unclassified'}" for c in v["captures"]
        )
        lines.append(f"  {v['slug']} (patch {v['patch']}): wanted {v['wanted']}, have {have}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--json", action="store_true", help="print the report as JSON instead of text")
    args = ap.parse_args()
    report = build()
    print(json.dumps(report, indent=2) if args.json else text(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
