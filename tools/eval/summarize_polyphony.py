#!/usr/bin/env python3
"""Roll the multiple-F0 observations up into per-item and aggregate figures.

The observations come from the ``[polyphony_eval]`` case, which renders its own
material through the GM fallback bank. That makes this the one measurement here
that needs no corpus: the ground truth is the note list, so the figures are
reproducible from a clean checkout rather than from a dataset someone holds.

    ./build/bin/sonare_tests "[polyphony_eval]" | python3 tools/eval/summarize_polyphony.py

Two refusals it shares with summarize_accuracy.py: it does not score an empty
set, and it does not hide a partial one. A run that produced no observations
reports ``unmeasured`` and exits non-zero rather than averaging nothing into a
perfect score.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

MARKER = "POLYF0_EVAL "


def parse(stream: Any) -> list[dict[str, Any]]:
    """Every marked line, in the order the run produced them."""
    out = []
    for line in stream:
        index = line.find(MARKER)
        if index < 0:
            continue
        out.append(json.loads(line[index + len(MARKER) :]))
    return out


def micro(rows: list[dict[str, Any]]) -> dict[str, float]:
    """Pooled over frames, not averaged over items.

    An item mean weights a two-frame item the same as a thousand-frame one, and
    weights an easy item the same as the one item that discriminates. Pooling
    counts keeps every frame worth one frame.
    """
    tp = sum(r["tp"] for r in rows)
    fp = sum(r["fp"] for r in rows)
    fn = sum(r["fn"] for r in rows)
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    f = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    return {"tp": tp, "fp": fp, "fn": fn, "precision": precision, "recall": recall, "f": f}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path", nargs="?", help="run output; stdin when omitted")
    ap.add_argument(
        "--require",
        type=int,
        default=0,
        metavar="N",
        help="fail when fewer than N items were observed, so a corpus that "
        "silently shrank is not read as a corpus that passed",
    )
    ap.add_argument("--markdown", action="store_true", help="emit a markdown table")
    args = ap.parse_args()

    with open(args.path, encoding="utf-8") if args.path else sys.stdin as stream:
        rows = parse(stream)

    if not rows:
        print("multiple-F0: unmeasured (no observations in the input)", file=sys.stderr)
        return 1
    if len(rows) < args.require:
        print(
            f"multiple-F0: {len(rows)} items observed, {args.require} required",
            file=sys.stderr,
        )
        return 1

    if args.markdown:
        print("| item | voices | frames | precision | recall | F | polyphony error |")
        print("|---|---|---|---|---|---|---|")
        for r in rows:
            print(
                f"| {r['item']} | {r['voices']} | {r['frames']} | {r['precision']:.3f} "
                f"| {r['recall']:.3f} | {r['f_measure']:.3f} | {r['polyphony_error']:.2f} |"
            )
    else:
        width = max(len(r["item"]) for r in rows)
        print(f"{'item':<{width}}  {'voices':>6} {'frames':>6} {'P':>6} {'R':>6} {'F':>6} {'poly':>6}")
        for r in rows:
            print(
                f"{r['item']:<{width}}  {r['voices']:>6} {r['frames']:>6} "
                f"{r['precision']:>6.3f} {r['recall']:>6.3f} {r['f_measure']:>6.3f} "
                f"{r['polyphony_error']:>6.2f}"
            )

    agg = micro(rows)
    print()
    print(
        f"pooled over {len(rows)} items and {sum(r['frames'] for r in rows)} frames: "
        f"precision {agg['precision']:.3f}  recall {agg['recall']:.3f}  F {agg['f']:.3f}"
    )

    # An aggregate over a corpus this small says less than the spread does, and
    # a corpus whose items all score the same is measuring its own difficulty
    # rather than the model's accuracy.
    spread = [r["f_measure"] for r in rows]
    print(f"per-item F ranges {min(spread):.3f} to {max(spread):.3f}")
    if max(spread) - min(spread) < 0.05:
        print(
            "warning: every item scored within 0.05 of every other, so this "
            "corpus is not separating anything",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
