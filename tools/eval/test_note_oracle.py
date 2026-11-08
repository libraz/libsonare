"""Unit checks for the note-oracle scoring helpers."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import note_oracle


def note(onset: float, offset: float, hz: float) -> dict[str, float]:
    return {"onset_sec": onset, "offset_sec": offset, "hz": hz}


class MatchingTest(unittest.TestCase):
    def test_matching_is_maximum_rather_than_greedy(self) -> None:
        # The first reference can take either estimate; a greedy pass taking the
        # nearer one leaves the second reference without a partner.
        refs = [note(1.00, 1.5, 440.0), note(1.06, 1.5, 440.0)]
        ests = [note(1.03, 1.5, 440.0), note(0.96, 1.5, 440.0)]
        self.assertEqual(note_oracle.matched_count(refs, ests, "onset"), 2)

    def test_pitch_and_offset_criteria(self) -> None:
        ref = note(1.0, 2.0, 440.0)
        self.assertTrue(note_oracle.matches(ref, note(1.04, 2.0, 452.0), "note"))
        self.assertFalse(note_oracle.matches(ref, note(1.0, 2.0, 466.2), "note"))
        # Offset tolerance is 20% of the one-second reference duration.
        self.assertTrue(note_oracle.matches(ref, note(1.0, 2.19, 440.0), "offset"))
        self.assertFalse(note_oracle.matches(ref, note(1.0, 2.21, 440.0), "offset"))


class BoundaryProfileTest(unittest.TestCase):
    def test_split_merge_and_spurious_shapes(self) -> None:
        refs = [note(0.0, 1.0, 220.0), note(2.0, 2.4, 220.0), note(2.5, 3.0, 220.0)]
        ests = [
            note(0.0, 0.5, 220.0),
            note(0.5, 1.0, 220.0),
            note(2.0, 3.0, 220.0),
            note(5.0, 5.5, 220.0),
        ]
        self.assertEqual(
            note_oracle.boundary_profile(refs, ests),
            {
                "split_references": 1,
                "extra_estimates_from_splits": 1,
                "merged_estimates": 1,
                "uncovered_references": 0,
                "spurious_estimates": 1,
            },
        )


class SegmentLabelTest(unittest.TestCase):
    def test_median_of_voiced_frames_inside_the_span(self) -> None:
        f0 = [0.0, 220.0, 0.0, 230.0, 240.0, 0.0, 500.0]
        refs = [note(0.1, 0.45, 999.0), note(0.5, 0.55, 999.0)]
        labelled = note_oracle.segment_labels(refs, f0, 0.1)
        # Frames 1..4 hold 220, 230, 240; the unvoiced span yields no label.
        self.assertEqual([n["hz"] for n in labelled], [230.0])


class FrameMetricTest(unittest.TestCase):
    def test_octave_error_counts_for_chroma_only(self) -> None:
        contour = [(0.0, 220.0), (0.1, 220.0), (0.2, 0.0)]
        counts = note_oracle.frame_metrics([220.0, 440.0, 300.0], 0.1, contour)
        self.assertEqual(counts["ref_voiced"], 2)
        self.assertEqual(counts["pitch_hits"], 1)
        self.assertEqual(counts["chroma_hits"], 2)
        self.assertEqual(counts["false_alarms"], 1)


if __name__ == "__main__":
    unittest.main()
