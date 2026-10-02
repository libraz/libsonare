"""Independent fit/selection/final validation for shape fitting."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shape.__main__ import validation_partitions
from shape.loss import ShapeLoss, Terms
from shape.search import (
    KEEP_DB,
    prune,
    split_notes_three_way,
    split_velocities_three_way,
    validate_final,
)


def test_three_way_note_partitions_are_disjoint_and_independent():
    split = split_notes_three_way((21, 30, 42, 54, 66, 78, 90, 108, 120))
    groups = (set(split.fit), set(split.selection), set(split.final))
    assert split.independent
    assert not (groups[0] & groups[1] or groups[0] & groups[2] or groups[1] & groups[2])
    assert set.union(*groups) == {21, 30, 42, 54, 66, 78, 90, 108, 120}


def test_small_axis_does_not_duplicate_a_sample_for_final_validation():
    split = split_velocities_three_way((64, 127))
    groups = (set(split.fit), set(split.selection), set(split.final))
    assert not split.available
    assert not split.independent
    assert not split.final
    assert set.union(*groups) == {64, 127}
    assert "at least 3 distinct samples" in split.reason


def test_struck_partition_keeps_each_piece_on_every_velocity_split():
    partitions, fit, selection, final, fit_notes, select_notes, final_notes = validation_partitions(
        ShapeLoss(signals=None, pitched=False, velocities=(64, 88, 100, 127)), (36, 42)
    )
    assert partitions.independent
    assert fit.velocities == partitions.fit
    assert selection.velocities == partitions.selection
    assert final.velocities == partitions.final
    assert fit_notes == select_notes == final_notes == (36, 42)


class _ValidationLoss:
    def __init__(self):
        self.calls = []

    def score(self, overrides, notes=()):
        self.calls.append((overrides, tuple(notes)))
        total = 1.0 if not overrides else 1.0 + KEEP_DB + 0.01
        return Terms(total=total, parts={"spectrum": total}, per_note={}, gain_db=0.0)


def test_final_regression_restores_shipped_baseline_with_reason():
    loss = _ValidationLoss()
    written, report = validate_final(loss, {"voice.gain": 1.0}, {"voice.gain": 2.0}, (60,))
    assert written == {}
    assert report["fallback"] and not report["accepted"]
    assert "exceeding KEEP_DB" in report["reason"]
    assert len(loss.calls) == 2
    assert all(notes == (60,) for _overrides, notes in loss.calls)


class _PruneLoss:
    def __init__(self):
        self.notes = []

    def score(self, _overrides, notes=()):
        self.notes.append(tuple(notes))
        return Terms(total=1.0, parts={"spectrum": 1.0}, per_note={}, gain_db=0.0)


def test_prune_has_no_path_to_the_final_partition():
    loss = _PruneLoss()
    prune(loss, {"a": 1.0}, {"a": 2.0}, (60,), (62,), workers=1)
    assert loss.notes
    assert set(loss.notes) <= {(60,), (62,)}
    assert (66,) not in loss.notes
