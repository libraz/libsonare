#!/usr/bin/env python3
"""Compare two metrics.json sets from render_corpus.py, plus the reference.

Usage:
  python tools/voicematch/compare_metrics.py /tmp/voicematch_corpus/baseline \
      /tmp/voicematch_corpus/improved

Prints per-file before -> after (-> ref where metrics_ref.json exists) for the
metrics that drive the "cheap" verdict: stereo width, spectral centroid,
centroid movement (cv), harmonic count, attack, rms.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

KEYS = ["rms", "stereo_width", "centroid_hz", "centroid_cv", "harmonics_mid", "attack_ms"]


def load(d: Path, name: str) -> dict:
    p = d / name
    return json.loads(p.read_text()) if p.exists() else {}


def fmt(v) -> str:
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.3f}" if v < 10 else f"{v:.0f}"
    return str(v)


def main() -> int:
    before_dir, after_dir = Path(sys.argv[1]), Path(sys.argv[2])
    before = load(before_dir, "metrics.json")
    after = load(after_dir, "metrics.json")
    ref = load(before_dir, "metrics_ref.json") or load(after_dir, "metrics_ref.json")

    for name in sorted(set(before) | set(after)):
        b, a, r = before.get(name, {}), after.get(name, {}), ref.get(name, {})
        print(f"\n{name}")
        for k in KEYS:
            line = f"  {k:14} {fmt(b.get(k)):>9} -> {fmt(a.get(k)):>9}"
            if r:
                line += f"   (ref {fmt(r.get(k)):>9})"
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
