"""Tests for the identity features — what each one is invariant to, and the grade's arithmetic.

A recording is a gain and a linear filter, so every feature here is built to
survive both. The tests apply exactly that to a synthetic row and require the
features not to move, then change the instrument and require that they do.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_idfeatures.py -q
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import idfeatures as F


def row(
    note=60, vel=80, *, b=3e-4, ladder=(0, -4, -9, -15, -18, -24, -28, -33), level=-12.0, decay=-5.0
):
    return {
        "note": note,
        "velocity": vel,
        "inharmonicity_reliable": True,
        "inharmonicity_b": b,
        "partials_db": list(ladder),
        "partial_decay_db_s": [decay * (1 + 0.1 * k) for k in range(8)],
        "decay_knee_s": 1.5,
        "decay_span_s": 6.0,
        "decay_early_db_s": -7.0,
        "decay_late_db_s": -3.0,
        "attack_ms": 12.0,
        "peak_dbfs": level,
        "decay_db_s": decay,
    }


def recorded(r: dict, gain_db: float, tilt_db_per_octave: float) -> dict:
    """The same note through a microphone: a gain and a log-linear filter."""
    ladder = [
        p + tilt_db_per_octave * math.log2(k) for k, p in enumerate(r["partials_db"], start=1)
    ]
    h1 = ladder[0]
    return {**r, "partials_db": [p - h1 for p in ladder], "peak_dbfs": r["peak_dbfs"] + gain_db}


def test_log2b_reads_doublings_and_needs_a_reliable_fit():
    assert F.cell_features(row(b=2**-12))["log2B"] == pytest.approx(-12.0)
    assert "log2B" not in F.cell_features({**row(), "inharmonicity_reliable": False})


def test_the_detilted_ladder_ignores_a_microphone_filter():
    plain = F.cell_features(row())
    through = F.cell_features(recorded(row(), gain_db=-9.0, tilt_db_per_octave=-4.0))
    for k in range(2, 9):
        assert through[f"ladder_detilt_h{k}"] == pytest.approx(
            plain[f"ladder_detilt_h{k}"], abs=1e-9
        )


def test_soft_to_hard_swings_ignore_a_gain_and_a_filter():
    soft, hard = (
        row(vel=40, level=-20.0),
        row(vel=110, ladder=(0, -2, -6, -10, -12, -16, -19, -22), level=-8.0),
    )
    plain = F.note_swings([soft, hard])[60]
    chain = F.note_swings([recorded(soft, -6.0, 3.0), recorded(hard, -6.0, 3.0)])[60]
    for k in ("~h2", "~h5", "~h8", "~level"):
        assert chain[k] == pytest.approx(plain[k], abs=1e-9)
    # A group cancels only to within the filter's spread across its partials.
    for k in ("~h2-3", "~h4-6"):
        assert chain[k] == pytest.approx(plain[k], abs=0.5)
    assert plain["~h4-6"] > 0.0


def test_another_instrument_moves_the_features():
    """The control that the invariances did not make the features blind."""
    a = [row(note=n, vel=v) for n in (48, 60, 72) for v in (40, 110)]
    b = [row(note=n, vel=v, b=3e-6, decay=-0.8) for n in (48, 60, 72) for v in (40, 110)]
    same = F.pair_distances(a, [recorded(r, -5.0, -2.0) for r in a])
    other = F.pair_distances(a, b)
    assert same["log2B"] == pytest.approx(0.0, abs=1e-9)
    assert other["log2B"] == pytest.approx(math.log2(100.0))
    assert other["pdecay_h1"] > same["pdecay_h1"]


def test_alignment_takes_the_nearest_velocity_within_reach():
    target = [row(vel=100)]
    pairs = F.align(target, [row(vel=80), row(vel=127)])
    assert pairs[0][1]["velocity"] == 80
    assert F.align(target, [row(vel=40)]) == []


def test_auc_counts_ties_as_half():
    assert F.auc([1.0, 2.0], [3.0, 4.0]) == 1.0
    assert F.auc([3.0], [3.0]) == 0.5
    assert F.auc([5.0], [1.0]) == 0.0
    assert F.auc([], [1.0]) is None


def test_split_halves_cut_by_alternation_and_by_register():
    rows = [row(note=n, vel=v) for n in (48, 52, 56, 60) for v in (40, 80, 120)]
    a, b = F.split_halves(rows, "note")
    assert {r["note"] for r in a} == {48, 56} and {r["note"] for r in b} == {52, 60}
    a, b = F.split_halves(rows, "register")
    assert {r["note"] for r in a} == {48, 52} and {r["note"] for r in b} == {56, 60}
    a, b = F.split_halves(rows, "velocity")
    assert {r["velocity"] for r in a} == {40, 120} and {r["velocity"] for r in b} == {80}
    assert len(a) + len(b) == len(rows)


def test_split_alignment_reaches_the_neighbouring_note():
    a = [row(note=48), row(note=56)]
    b = [row(note=52), row(note=72)]
    assert F.align(a, b) == []
    pairs = F.align(a, b, note_reach=None)
    assert [(x["note"], y["note"]) for x, y in pairs] == [(48, 52), (56, 52)]
    assert [(x["note"], y["note"]) for x, y in F.align(a, b, note_reach=4)] == [
        (48, 52),
        (56, 52),
    ]


def test_combined_rank_finds_the_own_half_and_ignores_a_missing_feature():
    scale = {f"f{i}": 1.0 for i in range(6)}
    own = {f"f{i}": 0.1 for i in range(6)}
    far = {f"f{i}": 2.0 for i in range(6)}
    # Lacks the one feature where own is far: compared on the shared five only.
    blind = {f"f{i}": 0.5 for i in range(5)}
    got = F.combined_hits({"c": (own, {"far": far, "blind": blind})}, scale)
    assert got["hit_rate"] == 1.0 and got["median_rank"] == 1.0
    lost = F.combined_hits({"c": ({**own, "f0": 9.0}, {"near": {**own, "f1": 0.05}})}, scale)
    assert lost["hit_rate"] == 0.0
