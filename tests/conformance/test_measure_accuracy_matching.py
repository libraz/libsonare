"""Regression and small exhaustive checks for benchmark beat matching."""

from __future__ import annotations

import itertools
import math
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "benchmarks"))
import measure_accuracy


def maximum_matching(
    reference: tuple[float, ...], estimated: tuple[float, ...], tolerance: float
) -> int:
    """Tiny exhaustive oracle, independent of the production two-pointer scan."""
    best = 0

    def visit(index: int, used: frozenset[int], matched: int) -> None:
        nonlocal best
        if index == len(estimated):
            best = max(best, matched)
            return
        visit(index + 1, used, matched)
        for ref_index, ref in enumerate(reference):
            if ref_index not in used and abs(estimated[index] - ref) <= tolerance:
                visit(index + 1, used | {ref_index}, matched + 1)

    visit(0, frozenset(), 0)
    return best


class BeatMatchingTest(unittest.TestCase):
    def test_matching_maximizes_one_to_one_matches(self) -> None:
        # Nearest-first consumes reference 0.1 for estimate 0.06, leaving 0.14
        # unmatched even though both estimates have distinct feasible refs.
        result = measure_accuracy.beat_f_measure([0.0, 0.1], [0.06, 0.14])
        self.assertEqual(result["matched"], 2)
        self.assertEqual(result["f1"], 1.0)

    def test_matching_sorts_inputs_preserves_duplicates_and_tolerance_boundary(self) -> None:
        result = measure_accuracy.beat_f_measure([0.1, 0.0, 0.0], [0.07, -0.07, 0.0])
        self.assertEqual(result["matched"], 3)
        shifted = measure_accuracy.beat_f_measure([1.0, 0.0], [1.07, 0.07], offset=0.07)
        self.assertEqual(shifted["matched"], 2)
        self.assertEqual(measure_accuracy.beat_f_measure([], [0.0])["precision"], 0.0)
        self.assertEqual(measure_accuracy.beat_f_measure([0.0], [])["recall"], 0.0)

    def test_non_finite_beats_remain_unmatched(self) -> None:
        result = measure_accuracy.beat_f_measure(
            [0.0, math.nan, math.inf], [0.0, math.nan, math.inf]
        )
        self.assertEqual(result["matched"], 1)
        self.assertEqual(result["precision"], 1 / 3)
        self.assertEqual(result["recall"], 1 / 3)

    def test_small_inputs_match_exhaustive_maximum_cardinality_oracle(self) -> None:
        values = (-0.04, 0.0, 0.04, 0.08)
        sequences = [
            items
            for size in range(4)
            for items in itertools.combinations_with_replacement(values, size)
        ]
        for reference in sequences:
            for estimated in sequences:
                with self.subTest(reference=reference, estimated=estimated):
                    expected = maximum_matching(
                        reference, estimated, measure_accuracy.BEAT_TOLERANCE_SEC
                    )
                    actual = measure_accuracy.beat_f_measure(reference, estimated)["matched"]
                    self.assertEqual(actual, expected)
