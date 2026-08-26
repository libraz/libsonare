"""Which terms a score is entitled to, given what it was asked about."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shape.loss import STRUCK_WEIGHTS, compares_notes  # noqa: E402


def test_one_piece_at_several_velocities_still_only_holds_one_note():
    """The shape a kit fit takes, and the one the guard exists for.

    Every piece of a kit is its own patch, so a fit that moves one piece's
    coordinates can only be scored on that piece — and the velocity layers that
    make the score look broad are all the same note.
    """
    assert not compares_notes([(42, 88), (42, 100), (42, 112), (42, 127)])


def test_two_pieces_are_enough_to_ask_what_they_have_in_common():
    assert compares_notes([(42, 100), (49, 100)])


def test_an_empty_score_compares_nothing():
    assert not compares_notes([])


def test_both_across_note_terms_are_ones_a_struck_piece_is_scored_on():
    """So withholding them changes the total rather than being cosmetic.

    If either dropped out of the struck weight set the guard would be guarding
    a term nothing reads, and this test would be the only thing that noticed.
    """
    assert "invariance" in STRUCK_WEIGHTS
    assert "recurrence" in STRUCK_WEIGHTS
